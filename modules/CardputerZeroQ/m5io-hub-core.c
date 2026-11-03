// SPDX-License-Identifier: GPL-2.0
/*
 * M5Stack M5IO-HUB 核心驱动
 *
 * 分层结构：
 *   1) 框架层：spi_driver 注册、struct m5io_hub 句柄、DT 子设备注册、OTA misc。
 *   2) 序列化层：按 m5io-hub-api.h 顶部的 v2 协议，把 TOKEN 请求编码成
 *      “2 B 头 + payload + 1 B CRC-8/ATM”，把 DATA 消息编码成
 *      “3 B 头 + payload + 1 B CRC-8/ATM”，DATA 消息按 CH/SEQ/STA 分片。
 *   3) 操作队列层：token_queue / data_queue 两个先进先出队列。每个对外 API
 *      调用在这里被序列化成“一个 token 操作”或“一个 data 逻辑消息操作”，
 *      入队后阻塞等待完成（带超时）。
 *   4) 收发状态机层（下一步实现）：从两个队列取操作，负责 TID/PID 窗口、
 *      SPI 收发、WINDOW_ACK、重试与超时，并把结果写回操作项、唤醒调用者。
 *      本文件已经提供队列 pop 与 complete 接口，状态机直接接上即可。
 *
 * 调用约定：本文件导出的所有 m5io_hub_* API 都可能睡眠（mutex、
 * kzalloc(GFP_KERNEL)、wait_for_completion），必须在可睡眠的进程上下文调用。
 * uart_ops.stop_tx/start_tx/tx_empty 这类可能持有 port->lock 或处于原子上下文的
 * 回调不能直接调这些 API，需由子驱动用 workqueue 中转后转发。
 *
 * 注意：状态机尚未接入时，所有会入队的 API 都会等满各自超时后返回
 * -ETIMEDOUT；这是当前开发阶段的预期行为。
 */

#include <linux/atomic.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/i2c.h>
#include <linux/jiffies.h>
#include <linux/list.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/spi/spi.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <linux/wait.h>

#include "m5io-hub.h"
#include "m5io-hub-uapi.h"

/* =====================================================================
 * 协议常量与帧工具（见 m5io-hub-api.h 顶部“协议简述”）
 * ===================================================================== */

#define M5IO_HUB_TYPE_TOKEN 0x00U
#define M5IO_HUB_TYPE_DATA  0x01U
/* TOKEN 头：TYPE(bit0) | P(bit1) | TID(bits3:2) | FUNC(bits7:4)；
 * byte1：LEN(low 4) | ARG(high 4)。 */
#define M5IO_HUB_TOKEN_P_BIT      BIT(1)
#define M5IO_HUB_TOKEN_PAYLOAD_MAX 15U
#define M5IO_HUB_TOKEN_FRAME_MAX  (2U + M5IO_HUB_TOKEN_PAYLOAD_MAX + 1U)
/* DATA 头：TYPE(bit0) | PID(bits2:1) | CHN(bits7:3)；byte1：STA(bits7:6) | SEQ(bits5:0)；
 * byte2：LEN。帧尾 1 B CRC。 */
#define M5IO_HUB_DATA_PAYLOAD_MAX 255U
#define M5IO_HUB_DATA_FRAME_MAX   (3U + M5IO_HUB_DATA_PAYLOAD_MAX + 1U)
/* 一个 data 操作 = 一条逻辑消息，最多拆成 4 个 255 B 的 DATA 帧。 */
#define M5IO_HUB_DATA_OP_MAX_FRAMES 4U
#define M5IO_HUB_DATA_OP_MAX_BYTES                                            \
  (M5IO_HUB_DATA_OP_MAX_FRAMES * M5IO_HUB_DATA_PAYLOAD_MAX)

#define M5IO_HUB_TID_NR 4U
#define M5IO_HUB_CHN_MAX M5IO_HUB_LOG_CH

#define M5IO_HUB_TOKEN_TIMEOUT_MS 2000U
#define M5IO_HUB_DATA_TIMEOUT_MS  30000U

/* 分区号 -> DATA 通道号（见 m5io-hub-api.h 的通道分配）。 */
#define M5IO_HUB_I2C_CHN(bus) ((u8)(bus))
#define M5IO_HUB_SPI_CHN(bus) ((u8)(M5IO_HUB_SPI_CH1 + (bus)))
#define M5IO_HUB_UART_CHN(bus)                                          \
  ((u8)(M5IO_HUB_UART_CH1 + (u8)((bus) - 1U)))
#define M5IO_HUB_IR_CHN       ((u8)M5IO_HUB_IR_CH)

struct m5io_hub_data_frame_desc {
  u16 offset;      /* 序列化帧在 op->buf 里的偏移 */
  u16 len;         /* 序列化帧长度：3 + payload + 1（最大 259，不能用 u8） */
  u8 payload_len;  /* 该帧 payload 长度，状态机 set_pid() 改 CRC 时用 */
};

/* 一个 TOKEN 操作：一个已序列化、可直接上线的帧。 */
struct m5io_hub_token_op {
  struct list_head node;
  u8 frame[M5IO_HUB_TOKEN_FRAME_MAX];
  u8 frame_len;
  u8 tid;
  u8 func;
  u8 arg;
  u8 reply_cap; /* 期望的应答 payload 上限，0 表示只关心 status */
  u8 reply[M5IO_HUB_TOKEN_PAYLOAD_MAX];
  u8 reply_len;
  int status; /* 状态机写回：0 成功，负 errno */
  struct completion done;
};

/* 一个 DATA 操作：一条逻辑消息，含 1..N 个已序列化 DATA 帧（CH/SEQ/STA 已定）。
 * PID 由共享窗口在发送前分配，状态机发送前用 m5io_hub_data_frame_set_pid()
 * 改写帧头并重算 CRC；应答数据由状态机重组进 rx[]。 */
struct m5io_hub_data_op {
  struct list_head node;
  u8 chn;
  u8 frame_count;
  struct m5io_hub_data_frame_desc frames[M5IO_HUB_DATA_OP_MAX_FRAMES];
  u16 buf_len;
  u8 buf[M5IO_HUB_DATA_OP_MAX_FRAMES * M5IO_HUB_DATA_FRAME_MAX];
  u16 rx_cap;
  u16 rx_len;
  u8 rx[M5IO_HUB_DATA_OP_MAX_BYTES];
  int status; /* 状态机写回：0 成功，负 errno */
  struct completion done;
};

struct m5io_hub {
  struct device *dev;
  struct spi_device *spi;
  int irq;

  /* TOKEN 操作队列 */
  struct mutex token_lock;
  struct list_head token_queue;
  wait_queue_head_t token_wait;
  unsigned int token_count;

  /* DATA 操作队列；两个队列分开，状态机按各自的窗口节奏推进 */
  struct mutex data_lock;
  struct list_head data_queue;
  wait_queue_head_t data_wait;
  unsigned int data_count;
  u8 data_seq[M5IO_HUB_CHN_MAX + 1]; /* 每通道独立的 6 bit SEQ 分配器 */

  /* TOKEN TID 池：0..3，WINDOW_ACK 固定 TID=0 不占池 */
  struct mutex tid_lock;
  unsigned long tid_busy;
  wait_queue_head_t tid_wait;

  /* 通道 DATA 接收回调（UART/IR 等） */
  struct mutex chn_lock;
  m5io_hub_chn_data_handler_t chn_handler[M5IO_HUB_CHN_MAX + 1];
  void *chn_handler_data[M5IO_HUB_CHN_MAX + 1];

  /* GPIO 中断事件回调：按逻辑引脚注册 */
  struct mutex gpio_irq_lock;
  m5io_hub_gpio_irq_handler_t gpio_irq_handler[M5IO_HUB_NGPIO];
  void *gpio_irq_data[M5IO_HUB_NGPIO];

  /* OTA / 维护态：持有期间其它 API 一律 -EBUSY */
  struct mutex ota_lock;
  bool ota_active;
  struct task_struct *bus_owner;
  atomic_t active_apis;
  struct miscdevice ota_misc;
  char ota_misc_name[32];
  struct completion ota_ready; /* HOST_READY 中断唤醒 IAP 命令等待 */
};

/* =====================================================================
 * 帧序列化与 CRC-8/ATM
 * ===================================================================== */

static u8 m5io_hub_crc8(const u8 *data, unsigned int len)
{
  u8 crc = 0x00;
  unsigned int i;
  unsigned int bit;

  for (i = 0; i < len; i++) {
    crc ^= data[i];
    for (bit = 0; bit < 8; bit++)
      crc = (crc & 0x80U) ? (u8)((crc << 1) ^ 0x07U) : (u8)(crc << 1);
  }
  return crc;
}

static unsigned int m5io_hub_token_frame_build(u8 *frame, u8 tid, u8 func,
                                               u8 arg, const u8 *payload,
                                               u8 len)
{
  frame[0] = M5IO_HUB_TYPE_TOKEN | M5IO_HUB_TOKEN_P_BIT |
             ((tid & 0x3U) << 2) | ((func & 0xfU) << 4);
  frame[1] = (len & 0xfU) | ((arg & 0xfU) << 4);
  if (len && payload)
    memcpy(&frame[2], payload, len);
  frame[2 + len] = m5io_hub_crc8(frame, 2 + len);
  return 3 + len;
}

static unsigned int m5io_hub_data_frame_build(u8 *frame, u8 chn, u8 seq,
                                              u8 sta, const u8 *payload,
                                              u8 len)
{
  frame[0] = M5IO_HUB_TYPE_DATA | ((chn & 0x1fU) << 3); /* PID 由状态机填 */
  frame[1] = (u8)(((sta & 0x3U) << M5IO_HUB_DATA_STA_SHIFT) |
                  (seq & M5IO_HUB_DATA_SEQ_MASK));
  frame[2] = len;
  if (len && payload)
    memcpy(&frame[3], payload, len);
  frame[3 + len] = m5io_hub_crc8(frame, 3 + len);
  return 4 + len;
}

/* 状态机在共享窗口里分到 PID 后，发送前调用：改写帧头并重算 CRC。 */
static inline void m5io_hub_data_frame_set_pid(u8 *frame, u8 payload_len,
                                               u8 pid)
{
  frame[0] = (u8)((frame[0] & ~(0x3U << 1)) | ((pid & 0x3U) << 1));
  frame[3 + payload_len] = m5io_hub_crc8(frame, 3 + payload_len);
}

/* 协议 status -> errno（api.h：0=OK 1=INPROGRESS 2=EOPNOTSUPP 3=EINVAL
 * 4=BUSY 7=EIO，其余=EPROTO）。 */
static __maybe_unused int m5io_hub_status_to_errno(u8 status)
{
  switch (status) {
  case 0:
    return 0;
  case 1:
    return -EINPROGRESS;
  case 2:
    return -EOPNOTSUPP;
  case 3:
    return -EINVAL;
  case 4:
    return -EBUSY;
  case 7:
    return -EIO;
  default:
    return -EPROTO;
  }
}

/* =====================================================================
 * TOKEN TID 池
 * ===================================================================== */

static int m5io_hub_tid_alloc(struct m5io_hub *hub, unsigned int timeout_ms)
{
  unsigned long deadline = jiffies + msecs_to_jiffies(timeout_ms);

  for (;;) {
    int tid;

    mutex_lock(&hub->tid_lock);
    tid = find_first_zero_bit(&hub->tid_busy, M5IO_HUB_TID_NR);
    if (tid < (int)M5IO_HUB_TID_NR) {
      set_bit(tid, &hub->tid_busy);
      mutex_unlock(&hub->tid_lock);
      return tid;
    }
    mutex_unlock(&hub->tid_lock);

    if (time_after_eq(jiffies, deadline))
      return -EBUSY;
    wait_event_timeout(hub->tid_wait,
                       find_first_zero_bit(&hub->tid_busy,
                                           M5IO_HUB_TID_NR) <
                           M5IO_HUB_TID_NR,
                       msecs_to_jiffies(50));
  }
}

static void m5io_hub_tid_free(struct m5io_hub *hub, int tid)
{
  if (tid < 0 || tid >= (int)M5IO_HUB_TID_NR)
    return;
  mutex_lock(&hub->tid_lock);
  clear_bit(tid, &hub->tid_busy);
  mutex_unlock(&hub->tid_lock);
  wake_up(&hub->tid_wait);
}

/* =====================================================================
 * 操作队列层
 * ===================================================================== */

/* 把操作从队列里摘下来（若还在队列里）。返回 true 表示摘下来了。 */
static bool m5io_hub_token_op_cancel(struct m5io_hub *hub,
                                     struct m5io_hub_token_op *op)
{
  bool removed = false;

  mutex_lock(&hub->token_lock);
  if (!list_empty(&op->node)) {
    list_del_init(&op->node);
    hub->token_count--;
    removed = true;
  }
  mutex_unlock(&hub->token_lock);
  return removed;
}

static bool m5io_hub_data_op_cancel(struct m5io_hub *hub,
                                    struct m5io_hub_data_op *op)
{
  bool removed = false;

  mutex_lock(&hub->data_lock);
  if (!list_empty(&op->node)) {
    list_del_init(&op->node);
    hub->data_count--;
    removed = true;
  }
  mutex_unlock(&hub->data_lock);
  return removed;
}

/* 提交一个 TOKEN 操作并等待完成。payload/reply 均可为 NULL。 */
static int m5io_hub_token_submit(struct m5io_hub *hub, u8 func, u8 arg,
                                 const u8 *payload, u8 len, u8 reply_cap,
                                 u8 *reply, u8 *reply_len,
                                 unsigned int timeout_ms)
{
  struct m5io_hub_token_op *op;
  unsigned long wait;
  int tid;
  int ret;

  if (!hub || !hub->spi)
    return -ENODEV;
  if (len > M5IO_HUB_TOKEN_PAYLOAD_MAX || (len && !payload))
    return -EINVAL;
  if (reply_len)
    *reply_len = 0;

  tid = m5io_hub_tid_alloc(hub, timeout_ms);
  if (tid < 0)
    return tid;

  op = kzalloc(sizeof(*op), GFP_KERNEL);
  if (!op) {
    m5io_hub_tid_free(hub, tid);
    return -ENOMEM;
  }
  init_completion(&op->done);
  INIT_LIST_HEAD(&op->node);
  op->tid = (u8)tid;
  op->func = func;
  op->arg = arg;
  op->reply_cap = min_t(u8, reply_cap, M5IO_HUB_TOKEN_PAYLOAD_MAX);
  op->frame_len =
      (u8)m5io_hub_token_frame_build(op->frame, (u8)tid, func, arg, payload,
                                     len);

  mutex_lock(&hub->token_lock);
  list_add_tail(&op->node, &hub->token_queue);
  hub->token_count++;
  mutex_unlock(&hub->token_lock);
  wake_up(&hub->token_wait);

  wait = wait_for_completion_timeout(&op->done,
                                     msecs_to_jiffies(timeout_ms));
  if (!wait) {
    if (m5io_hub_token_op_cancel(hub, op)) {
      /* 状态机还没取走：直接回收。 */
      m5io_hub_tid_free(hub, tid);
      kfree(op);
      return -ETIMEDOUT;
    }
    /* 状态机已经取走，按契约它必须最终 complete 该操作。 */
    wait_for_completion(&op->done);
  }

  ret = op->status;
  if (!ret && reply) {
    u8 copy_len = min_t(u8, op->reply_len, reply_cap);

    if (copy_len)
      memcpy(reply, op->reply, copy_len);
    if (reply_len)
      *reply_len = copy_len;
  }

  m5io_hub_tid_free(hub, tid);
  kfree(op);
  return ret;
}

/* 提交一个 DATA 逻辑消息并等待完成；payload 按 chunk 拆成 1..N 帧。 */
static int m5io_hub_data_submit(struct m5io_hub *hub, u8 chn,
                                const u8 *payload, unsigned int len,
                                unsigned int chunk, u8 *rx, u16 rx_cap,
                                u16 *rx_len, unsigned int timeout_ms)
{
  struct m5io_hub_data_op *op;
  unsigned int frame_count;
  unsigned int i;
  unsigned long wait;
  int ret;

  if (!hub || !hub->spi)
    return -ENODEV;
  if (!chn || chn > M5IO_HUB_CHN_MAX || !len || (len && !payload))
    return -EINVAL;
  if (!chunk || chunk > M5IO_HUB_DATA_PAYLOAD_MAX)
    return -EINVAL;
  frame_count = DIV_ROUND_UP(len, chunk);
  if (frame_count > M5IO_HUB_DATA_OP_MAX_FRAMES)
    return -E2BIG;
  if (rx_len)
    *rx_len = 0;

  op = kzalloc(sizeof(*op), GFP_KERNEL);
  if (!op)
    return -ENOMEM;
  init_completion(&op->done);
  INIT_LIST_HEAD(&op->node);
  op->chn = chn;
  op->rx_cap = min_t(u16, rx_cap, sizeof(op->rx));

  mutex_lock(&hub->data_lock);
  op->frame_count = (u8)frame_count;
  op->buf_len = 0;
  for (i = 0; i < frame_count; i++) {
    unsigned int off = i * chunk;
    unsigned int chunk_len = min_t(unsigned int, chunk, len - off);
    u8 seq = hub->data_seq[chn];
    u8 sta;

    /* SoC -> MCU 方向没有首包/引导帧：多帧消息从第一个数据帧开始就是
     * STA=0（传输包），最后一帧 STA=2，单包消息 STA=3。引导帧（STA=1）只由
     * MCU 上行发送，见 m5io-hub-api.h 的定义。 */
    if (frame_count == 1)
      sta = M5IO_HUB_DATA_STA_START_END;
    else if (i == frame_count - 1)
      sta = M5IO_HUB_DATA_STA_END;
    else
      sta = M5IO_HUB_DATA_STA_MID;

    hub->data_seq[chn] = (u8)((seq + 1) & M5IO_HUB_DATA_SEQ_MASK);
    op->frames[i].offset = op->buf_len;
    op->frames[i].len =
        (u16)m5io_hub_data_frame_build(&op->buf[op->buf_len], chn, seq, sta,
                                       &payload[off], (u8)chunk_len);
    op->frames[i].payload_len = (u8)chunk_len;
    op->buf_len += op->frames[i].len;
  }
  list_add_tail(&op->node, &hub->data_queue);
  hub->data_count++;
  mutex_unlock(&hub->data_lock);
  wake_up(&hub->data_wait);

  wait = wait_for_completion_timeout(&op->done,
                                     msecs_to_jiffies(timeout_ms));
  if (!wait) {
    if (m5io_hub_data_op_cancel(hub, op)) {
      kfree(op);
      return -ETIMEDOUT;
    }
    wait_for_completion(&op->done);
  }

  ret = op->status;
  if (!ret && rx && op->rx_len) {
    u16 copy_len = min_t(u16, op->rx_len, rx_cap);

    memcpy(rx, op->rx, copy_len);
    if (rx_len)
      *rx_len = copy_len;
  } else if (rx_len && !ret) {
    *rx_len = 0;
  }

  kfree(op);
  return ret;
}

/* ---- 供下一步收发状态机使用的队列接口 ---- */

static __maybe_unused struct m5io_hub_token_op *
m5io_hub_token_queue_pop(struct m5io_hub *hub)
{
  struct m5io_hub_token_op *op = NULL;

  mutex_lock(&hub->token_lock);
  if (!list_empty(&hub->token_queue)) {
    op = list_first_entry(&hub->token_queue, struct m5io_hub_token_op, node);
    list_del_init(&op->node);
    hub->token_count--;
  }
  mutex_unlock(&hub->token_lock);
  return op;
}

static __maybe_unused struct m5io_hub_data_op *
m5io_hub_data_queue_pop(struct m5io_hub *hub)
{
  struct m5io_hub_data_op *op = NULL;

  mutex_lock(&hub->data_lock);
  if (!list_empty(&hub->data_queue)) {
    op = list_first_entry(&hub->data_queue, struct m5io_hub_data_op, node);
    list_del_init(&op->node);
    hub->data_count--;
  }
  mutex_unlock(&hub->data_lock);
  return op;
}

static __maybe_unused void
m5io_hub_token_op_complete(struct m5io_hub_token_op *op, int status,
                           const u8 *reply, u8 reply_len)
{
  op->status = status;
  if (!status && reply && reply_len) {
    op->reply_len = min_t(u8, reply_len, op->reply_cap);
    memcpy(op->reply, reply, op->reply_len);
  }
  complete(&op->done);
}

static __maybe_unused void
m5io_hub_data_op_complete(struct m5io_hub_data_op *op, int status,
                          const u8 *rx, u16 rx_len)
{
  op->status = status;
  if (!status && rx && rx_len) {
    op->rx_len = min_t(u16, rx_len, op->rx_cap);
    memcpy(op->rx, rx, op->rx_len);
  }
  complete(&op->done);
}

static __maybe_unused bool m5io_hub_queue_has_work(struct m5io_hub *hub)
{
  bool work;

  mutex_lock(&hub->token_lock);
  work = !list_empty(&hub->token_queue);
  mutex_unlock(&hub->token_lock);
  if (work)
    return true;
  mutex_lock(&hub->data_lock);
  work = !list_empty(&hub->data_queue);
  mutex_unlock(&hub->data_lock);
  return work;
}

static __maybe_unused unsigned int m5io_hub_token_queue_len(struct m5io_hub *hub)
{
  unsigned int count;

  mutex_lock(&hub->token_lock);
  count = hub->token_count;
  mutex_unlock(&hub->token_lock);
  return count;
}

static __maybe_unused unsigned int m5io_hub_data_queue_len(struct m5io_hub *hub)
{
  unsigned int count;

  mutex_lock(&hub->data_lock);
  count = hub->data_count;
  mutex_unlock(&hub->data_lock);
  return count;
}

/* 中止并清空两个队列：OTA 等维护态接管总线前使用，唤醒所有等待者。 */
static void m5io_hub_queues_abort(struct m5io_hub *hub, int error)
{
  struct m5io_hub_token_op *top;
  struct m5io_hub_token_op *tmp_t;
  struct m5io_hub_data_op *dop;
  struct m5io_hub_data_op *tmp_d;
  LIST_HEAD(tokens);
  LIST_HEAD(datas);

  mutex_lock(&hub->token_lock);
  list_splice_init(&hub->token_queue, &tokens);
  hub->token_count = 0;
  mutex_unlock(&hub->token_lock);

  mutex_lock(&hub->data_lock);
  list_splice_init(&hub->data_queue, &datas);
  hub->data_count = 0;
  mutex_unlock(&hub->data_lock);

  list_for_each_entry_safe(top, tmp_t, &tokens, node) {
    list_del_init(&top->node);
    m5io_hub_token_op_complete(top, error, NULL, 0);
  }
  list_for_each_entry_safe(dop, tmp_d, &datas, node) {
    list_del_init(&dop->node);
    m5io_hub_data_op_complete(dop, error, NULL, 0);
  }
}

/* =====================================================================
 * API 调用入口：OTA 独占检查 + 活跃调用计数
 * ===================================================================== */

static int m5io_hub_api_enter(struct m5io_hub *hub)
{
  if (!hub || !hub->spi)
    return -ENODEV;
  /* 维护态（OTA）期间只允许持有总线所有权的线程继续下发。 */
  if (READ_ONCE(hub->ota_active) && READ_ONCE(hub->bus_owner) != current)
    return -EBUSY;
  atomic_inc(&hub->active_apis);
  return 0;
}

static void m5io_hub_api_leave(struct m5io_hub *hub)
{
  atomic_dec(&hub->active_apis);
}

static int m5io_hub_api_token(struct m5io_hub *hub, u8 func, u8 arg,
                              const void *payload, u8 len)
{
  int ret = m5io_hub_api_enter(hub);

  if (ret)
    return ret;
  ret = m5io_hub_token_submit(hub, func, arg, payload, len, 0, NULL, NULL,
                              M5IO_HUB_TOKEN_TIMEOUT_MS);
  m5io_hub_api_leave(hub);
  return ret;
}

static int m5io_hub_api_token_reply(struct m5io_hub *hub, u8 func, u8 arg,
                                    const void *payload, u8 len, u8 *reply,
                                    u8 reply_cap, u8 *reply_len)
{
  int ret = m5io_hub_api_enter(hub);

  if (ret)
    return ret;
  ret = m5io_hub_token_submit(hub, func, arg, payload, len, reply_cap, reply,
                              reply_len, M5IO_HUB_TOKEN_TIMEOUT_MS);
  m5io_hub_api_leave(hub);
  return ret;
}

static int m5io_hub_api_data(struct m5io_hub *hub, u8 chn, const void *payload,
                             unsigned int len, unsigned int chunk, u8 *rx,
                             u16 rx_cap, u16 *rx_len, unsigned int timeout_ms)
{
  int ret = m5io_hub_api_enter(hub);

  if (ret)
    return ret;
  ret = m5io_hub_data_submit(hub, chn, payload, len, chunk, rx, rx_cap,
                             rx_len, timeout_ms);
  m5io_hub_api_leave(hub);
  return ret;
}

/* =====================================================================
 * GPIO API（FUNC=1，全部 TOKEN）
 * ===================================================================== */

int m5io_hub_gpio_pinMode(struct m5io_hub *hub, u8 pin, u8 mode)
{
  u8 payload[2] = {pin, mode};

  if (pin >= M5IO_HUB_NGPIO)
    return -EINVAL;
  return m5io_hub_api_token(hub, M5IO_HUB_GPIO_FUN, M5IO_HUB_GPIO_pinMode,
                            payload, sizeof(payload));
}
EXPORT_SYMBOL_GPL(m5io_hub_gpio_pinMode);

int m5io_hub_gpio_funMode(struct m5io_hub *hub, u8 pin, u8 mode)
{
  u8 payload[3] = {pin, mode, 0};
  u8 reply[5] = {};
  u8 reply_len = 0;

  if (pin >= M5IO_HUB_NGPIO)
    return -EINVAL;
  return m5io_hub_api_token_reply(hub, M5IO_HUB_GPIO_FUN,
                                  M5IO_HUB_GPIO_funMode, payload,
                                  sizeof(payload), reply, sizeof(reply),
                                  &reply_len);
}
EXPORT_SYMBOL_GPL(m5io_hub_gpio_funMode);

int m5io_hub_gpio_digitalWrite(struct m5io_hub *hub, u8 pin, u8 value)
{
  u8 payload[2] = {pin, value ? 1 : 0};

  if (pin >= M5IO_HUB_NGPIO)
    return -EINVAL;
  return m5io_hub_api_token(hub, M5IO_HUB_GPIO_FUN,
                            M5IO_HUB_GPIO_digitalWrite, payload,
                            sizeof(payload));
}
EXPORT_SYMBOL_GPL(m5io_hub_gpio_digitalWrite);

int m5io_hub_gpio_digitalRead(struct m5io_hub *hub, u8 pin)
{
  u8 payload[1] = {pin};
  u8 reply[1] = {};
  u8 reply_len = 0;
  int ret;

  if (pin >= M5IO_HUB_NGPIO)
    return -EINVAL;
  ret = m5io_hub_api_token_reply(hub, M5IO_HUB_GPIO_FUN,
                                 M5IO_HUB_GPIO_digitalRead, payload,
                                 sizeof(payload), reply, sizeof(reply),
                                 &reply_len);
  if (ret)
    return ret;
  if (reply_len != 1)
    return -EPROTO;
  return reply[0] ? 1 : 0;
}
EXPORT_SYMBOL_GPL(m5io_hub_gpio_digitalRead);

int m5io_hub_gpio_attachInterrupt(struct m5io_hub *hub, u8 pin, u8 type,
                                  m5io_hub_gpio_irq_handler_t handler,
                                  void *p)
{
  u8 payload[2] = {pin, type};
  int ret;

  if (pin >= M5IO_HUB_NGPIO || !handler)
    return -EINVAL;
  ret = m5io_hub_api_token(hub, M5IO_HUB_GPIO_FUN,
                           M5IO_HUB_GPIO_attachInterrupt, payload,
                           sizeof(payload));
  if (ret)
    return ret;

  mutex_lock(&hub->gpio_irq_lock);
  hub->gpio_irq_handler[pin] = handler;
  hub->gpio_irq_data[pin] = p;
  mutex_unlock(&hub->gpio_irq_lock);
  return 0;
}
EXPORT_SYMBOL_GPL(m5io_hub_gpio_attachInterrupt);

int m5io_hub_gpio_detachInterrupt(struct m5io_hub *hub, u8 pin)
{
  u8 payload[1] = {pin};
  int ret;

  if (pin >= M5IO_HUB_NGPIO)
    return -EINVAL;
  ret = m5io_hub_api_token(hub, M5IO_HUB_GPIO_FUN,
                           M5IO_HUB_GPIO_detachInterrupt, payload,
                           sizeof(payload));
  if (ret)
    return ret;

  mutex_lock(&hub->gpio_irq_lock);
  hub->gpio_irq_handler[pin] = NULL;
  hub->gpio_irq_data[pin] = NULL;
  mutex_unlock(&hub->gpio_irq_lock);
  return 0;
}
EXPORT_SYMBOL_GPL(m5io_hub_gpio_detachInterrupt);

/* =====================================================================
 * I2C API（FUNC=2；配置/复位走 TOKEN，报文本走 DATA CH=bus）
 * ===================================================================== */

int m5io_hub_i2c_config(struct m5io_hub *hub, u8 bus, u8 clk)
{
  u8 payload[2] = {bus, clk};

  if (bus < 1 || bus > 2 || clk > 1)
    return -EINVAL;
  return m5io_hub_api_token(hub, M5IO_HUB_IIC_FUN, M5IO_HUB_I2C_config,
                            payload, sizeof(payload));
}
EXPORT_SYMBOL_GPL(m5io_hub_i2c_config);

int m5io_hub_i2c_reset(struct m5io_hub *hub, u8 bus)
{
  u8 payload[1] = {bus};

  if (bus < 1 || bus > 2)
    return -EINVAL;
  return m5io_hub_api_token(hub, M5IO_HUB_IIC_FUN, M5IO_HUB_I2C_reset,
                            payload, sizeof(payload));
}
EXPORT_SYMBOL_GPL(m5io_hub_i2c_reset);

int m5io_hub_i2c_transfer(struct m5io_hub *hub, u8 bus, struct i2c_msg *msgs,
                          int num)
{
  u8 payload[M5IO_HUB_DATA_PAYLOAD_MAX];
  u8 rx[2 + M5IO_HUB_DATA_PAYLOAD_MAX];
  u16 rx_len = 0;
  unsigned int payload_len;
  int ret;

  if (!hub || !msgs || num < 1 || num > 2 || bus < 1 || bus > 2)
    return -EINVAL;

  if (num == 1) {
    struct i2c_msg *m = &msgs[0];

    if (m->addr < 0x08 || m->addr > 0x77)
      return -EINVAL;
    if (m->flags & ~(I2C_M_RD | I2C_M_DMA_SAFE))
      return -EOPNOTSUPP;
    if (m->flags & I2C_M_RD) {
      if (!m->len || m->len > 253 || !m->buf)
        return -EINVAL;
      payload[0] = M5IO_HUB_I2C_OP_READ;
      payload[1] = m->addr;
      payload[2] = (u8)m->len;
      payload_len = 3;
    } else {
      if (!m->len || m->len > 253 || !m->buf)
        return -EINVAL;
      payload[0] = M5IO_HUB_I2C_OP_WRITE;
      payload[1] = m->addr;
      memcpy(&payload[2], m->buf, m->len);
      payload_len = 2 + m->len;
    }
  } else {
    struct i2c_msg *w = &msgs[0];
    struct i2c_msg *r = &msgs[1];

    if ((w->flags & ~(I2C_M_DMA_SAFE)) ||
        (r->flags & ~(I2C_M_RD | I2C_M_DMA_SAFE)) ||
        !(r->flags & I2C_M_RD) ||
        w->addr != r->addr || w->addr < 0x08 || w->addr > 0x77 ||
        !w->len || w->len > 252 || !r->len || r->len > 253 || !w->buf ||
        !r->buf)
      return -EINVAL;
    payload[0] = M5IO_HUB_I2C_OP_READ;
    payload[1] = w->addr;
    memcpy(&payload[2], w->buf, w->len);
    payload[2 + w->len] = (u8)r->len;
    payload_len = 3 + w->len;
  }

  ret = m5io_hub_api_data(hub, M5IO_HUB_I2C_CHN(bus), payload, payload_len,
                          M5IO_HUB_DATA_PAYLOAD_MAX, rx, sizeof(rx), &rx_len,
                          M5IO_HUB_DATA_TIMEOUT_MS);
  if (ret)
    return ret;

  if (rx_len) {
    u8 addr = msgs[0].addr;

    if (rx_len < 2 || rx[0] != M5IO_HUB_I2C_OP_READ || rx[1] != addr)
      return -EPROTO;
    if (num == 1) {
      if (rx_len != 2U + msgs[0].len)
        return -EPROTO;
      memcpy(msgs[0].buf, &rx[2], msgs[0].len);
    } else {
      if (rx_len != 2U + msgs[1].len)
        return -EPROTO;
      memcpy(msgs[1].buf, &rx[2], msgs[1].len);
    }
  } else if (num == 1 && (msgs[0].flags & I2C_M_RD)) {
    return -EPROTO;
  } else if (num == 2) {
    return -EPROTO;
  }

  return num;
}
EXPORT_SYMBOL_GPL(m5io_hub_i2c_transfer);

int m5io_hub_i2c_quick(struct m5io_hub *hub, u8 bus, u8 addr, bool read)
{
  u8 payload[3] = {M5IO_HUB_I2C_OP_QUICK, addr, read ? 1 : 0};
  u8 rx[3] = {};
  u16 rx_len = 0;
  int ret;

  if (bus < 1 || bus > 2 || addr < 0x08 || addr > 0x77)
    return -EINVAL;

  ret = m5io_hub_api_data(hub, M5IO_HUB_I2C_CHN(bus), payload,
                          sizeof(payload), M5IO_HUB_DATA_PAYLOAD_MAX, rx,
                          sizeof(rx), &rx_len, M5IO_HUB_DATA_TIMEOUT_MS);
  if (ret)
    return ret;
  if (rx_len != sizeof(rx) || rx[0] != M5IO_HUB_I2C_OP_QUICK ||
      rx[1] != addr)
    return -EPROTO;
  return rx[2] ? 0 : -ENXIO;
}
EXPORT_SYMBOL_GPL(m5io_hub_i2c_quick);

int m5io_hub_i2c_quick_write(struct m5io_hub *hub, u8 bus, u8 addr)
{
  return m5io_hub_i2c_quick(hub, bus, addr, false);
}
EXPORT_SYMBOL_GPL(m5io_hub_i2c_quick_write);

int m5io_hub_i2c_quick_read(struct m5io_hub *hub, u8 bus, u8 addr)
{
  return m5io_hub_i2c_quick(hub, bus, addr, true);
}
EXPORT_SYMBOL_GPL(m5io_hub_i2c_quick_read);

/* =====================================================================
 * UART API（FUNC=4；配置/控制走 TOKEN，字节流走 DATA CH）
 * ===================================================================== */

int m5io_hub_uart_config(struct m5io_hub *hub, u8 bus, unsigned int baudrate,
                         u8 dbit, u8 parity, u8 sbit, u8 mode)
{
  /* payload[10]={port, baud(u32 LE), dbit, parity, sbit, watermark(u16 LE)}；
   * mode 在协议里没有对应字段，暂不下发。 */
  u8 payload[10];

  (void)mode;
  if (bus < 1 || bus > 2 || baudrate < 1200 || baudrate > 1000000 ||
      (dbit != 7 && dbit != 8) || parity > 2 || (sbit != 1 && sbit != 2))
    return -EINVAL;

  payload[0] = bus;
  payload[1] = (u8)(baudrate & 0xffU);
  payload[2] = (u8)((baudrate >> 8) & 0xffU);
  payload[3] = (u8)((baudrate >> 16) & 0xffU);
  payload[4] = (u8)((baudrate >> 24) & 0xffU);
  payload[5] = dbit;
  payload[6] = parity;
  payload[7] = sbit;
  payload[8] = 0x00; /* watermark = 0x0100（LE） */
  payload[9] = 0x01;

  return m5io_hub_api_token(hub, M5IO_HUB_UART_FUN, M5IO_HUB_UART_config,
                            payload, sizeof(payload));
}
EXPORT_SYMBOL_GPL(m5io_hub_uart_config);

int m5io_hub_uart_reset(struct m5io_hub *hub, u8 bus)
{
  u8 payload[1] = {bus};

  if (bus < 1 || bus > 2)
    return -EINVAL;
  return m5io_hub_api_token(hub, M5IO_HUB_UART_FUN, M5IO_HUB_UART_reset,
                            payload, sizeof(payload));
}
EXPORT_SYMBOL_GPL(m5io_hub_uart_reset);

int m5io_hub_uart_send_data(struct m5io_hub *hub, u8 bus, const u8 *data,
                            unsigned int len)
{
  if (bus < 1 || bus > 2 || !data || !len ||
      len > M5IO_HUB_DATA_OP_MAX_BYTES)
    return -EINVAL;
  return m5io_hub_api_data(hub, M5IO_HUB_UART_CHN(bus), data, len,
                           M5IO_HUB_DATA_PAYLOAD_MAX, NULL, 0, NULL,
                           M5IO_HUB_DATA_TIMEOUT_MS);
}
EXPORT_SYMBOL_GPL(m5io_hub_uart_send_data);

int m5io_hub_uart_register_rx(struct m5io_hub *hub, u8 bus,
                              m5io_hub_chn_data_handler_t handler, void *p)
{
  if (!hub || bus < 1 || bus > 2 || !handler)
    return -EINVAL;

  mutex_lock(&hub->chn_lock);
  hub->chn_handler[M5IO_HUB_UART_CHN(bus)] = handler;
  hub->chn_handler_data[M5IO_HUB_UART_CHN(bus)] = p;
  mutex_unlock(&hub->chn_lock);
  return 0;
}
EXPORT_SYMBOL_GPL(m5io_hub_uart_register_rx);

void m5io_hub_uart_unregister_rx(struct m5io_hub *hub, u8 bus)
{
  if (!hub || bus < 1 || bus > 2)
    return;

  mutex_lock(&hub->chn_lock);
  hub->chn_handler[M5IO_HUB_UART_CHN(bus)] = NULL;
  hub->chn_handler_data[M5IO_HUB_UART_CHN(bus)] = NULL;
  mutex_unlock(&hub->chn_lock);
}
EXPORT_SYMBOL_GPL(m5io_hub_uart_unregister_rx);

int m5io_hub_uart_tx_empty(struct m5io_hub *hub, u8 bus)
{
  u8 payload[1] = {bus};
  u8 reply[1] = {};
  u8 reply_len = 0;
  int ret;

  if (bus < 1 || bus > 2)
    return -EINVAL;
  ret = m5io_hub_api_token_reply(hub, M5IO_HUB_UART_FUN,
                                 M5IO_HUB_UART_is_tx_empty, payload,
                                 sizeof(payload), reply, sizeof(reply),
                                 &reply_len);
  if (ret)
    return ret;
  if (reply_len != 1)
    return -EPROTO;
  return reply[0] ? 1 : 0;
}
EXPORT_SYMBOL_GPL(m5io_hub_uart_tx_empty);

int m5io_hub_uart_stop_tx(struct m5io_hub *hub, u8 bus)
{
  u8 payload[1] = {bus};

  if (bus < 1 || bus > 2)
    return -EINVAL;
  return m5io_hub_api_token(hub, M5IO_HUB_UART_FUN,
                            M5IO_HUB_UART_stop_tx, payload, sizeof(payload));
}
EXPORT_SYMBOL_GPL(m5io_hub_uart_stop_tx);

int m5io_hub_uart_start_tx(struct m5io_hub *hub, u8 bus)
{
  u8 payload[1] = {bus};

  if (bus < 1 || bus > 2)
    return -EINVAL;
  return m5io_hub_api_token(hub, M5IO_HUB_UART_FUN,
                            M5IO_HUB_UART_start_tx, payload, sizeof(payload));
}
EXPORT_SYMBOL_GPL(m5io_hub_uart_start_tx);

int m5io_hub_uart_stop_rx(struct m5io_hub *hub, u8 bus)
{
  u8 payload[1] = {bus};

  if (bus < 1 || bus > 2)
    return -EINVAL;
  return m5io_hub_api_token(hub, M5IO_HUB_UART_FUN, M5IO_HUB_UART_stop_rx,
                            payload, sizeof(payload));
}
EXPORT_SYMBOL_GPL(m5io_hub_uart_stop_rx);

/* =====================================================================
 * SPI API（FUNC=3；CONFIG/BEGIN/END/FLUSH 走 TOKEN，交换数据走 DATA CH）
 * ===================================================================== */

static int m5io_hub_spi_token(struct m5io_hub *hub, u8 arg, u8 bus,
                              unsigned int len)
{
  u8 payload[3] = {bus, (u8)(len & 0xffU), (u8)((len >> 8) & 0xffU)};

  if (bus >= 4 || len > 512)
    return -EINVAL;
  return m5io_hub_api_token(hub, M5IO_HUB_SPI_FUN, arg, payload,
                            sizeof(payload));
}

int m5io_hub_spi_config(struct m5io_hub *hub, u8 bus, unsigned int clk,
                        u8 direction, u8 datasize, u8 mode, u8 nss,
                        u8 firstbit, u8 wmode)
{
  /* payload[7]={bus, mode, datasize, u16 clk_khz(LE), u16 watermark(LE)=0x0100}；
   * direction/nss/firstbit/wmode 在当前 payload 布局里没有字段，暂忽略。 */
  u8 payload[7];
  unsigned int clk_khz = clk / 1000U;

  (void)direction;
  (void)nss;
  (void)firstbit;
  (void)wmode;
  if (bus >= 4 || clk < 250000 || clk > 20000000 ||
      (datasize != 8 && datasize != 16) || clk_khz > 65535)
    return -EINVAL;

  payload[0] = bus;
  payload[1] = mode;
  payload[2] = datasize;
  payload[3] = (u8)(clk_khz & 0xffU);
  payload[4] = (u8)((clk_khz >> 8) & 0xffU);
  payload[5] = 0x00; /* watermark = 0x0100（LE） */
  payload[6] = 0x01;

  return m5io_hub_api_token(hub, M5IO_HUB_SPI_FUN, M5IO_HUB_SPI_config,
                            payload, sizeof(payload));
}
EXPORT_SYMBOL_GPL(m5io_hub_spi_config);

int m5io_hub_spi_reset(struct m5io_hub *hub, u8 bus)
{
  u8 payload[1] = {bus};

  if (bus >= 4)
    return -EINVAL;
  return m5io_hub_api_token(hub, M5IO_HUB_SPI_FUN, M5IO_HUB_SPI_reset,
                            payload, sizeof(payload));
}
EXPORT_SYMBOL_GPL(m5io_hub_spi_reset);

int m5io_hub_spi_begin(struct m5io_hub *hub, u8 bus, unsigned int len)
{
  return m5io_hub_spi_token(hub, M5IO_HUB_SPI_begin, bus, len);
}
EXPORT_SYMBOL_GPL(m5io_hub_spi_begin);

int m5io_hub_spi_begin_txonly(struct m5io_hub *hub, u8 bus, unsigned int len)
{
  return m5io_hub_spi_token(hub, M5IO_HUB_SPI_begin_txonly, bus, len);
}
EXPORT_SYMBOL_GPL(m5io_hub_spi_begin_txonly);

int m5io_hub_spi_end(struct m5io_hub *hub, u8 bus, unsigned int len)
{
  return m5io_hub_spi_token(hub, M5IO_HUB_SPI_end, bus, len);
}
EXPORT_SYMBOL_GPL(m5io_hub_spi_end);

int m5io_hub_spi_flush(struct m5io_hub *hub, u8 bus)
{
  u8 payload[1] = {bus};
  u8 reply[4] = {};
  u8 reply_len = 0;
  int ret;

  if (bus >= 4)
    return -EINVAL;
  ret = m5io_hub_api_token_reply(hub, M5IO_HUB_SPI_FUN,
                                 M5IO_HUB_SPI_flush, payload,
                                 sizeof(payload), reply, sizeof(reply),
                                 &reply_len);
  if (ret)
    return ret;
  if (reply_len != sizeof(reply) || reply[0])
    return -EPROTO;
  return 0;
}
EXPORT_SYMBOL_GPL(m5io_hub_spi_flush);

/* 校验一条 SPI message；不在总线上产生任何动作。 */
static int m5io_hub_spi_validate_msg(u8 bus, struct spi_message *msg)
{
  struct spi_transfer *t;
  unsigned int speed = 0;
  unsigned int bits = 0;
  unsigned int group = 0;

  if (bus >= 4 || !msg || !msg->spi)
    return -EINVAL;
  if (msg->spi->mode & ~(SPI_CPOL | SPI_CPHA))
    return -EINVAL;
  if (msg->spi->word_delay.value || msg->spi->cs_setup.value ||
      msg->spi->cs_hold.value || msg->spi->cs_inactive.value)
    return -EOPNOTSUPP;

  list_for_each_entry(t, &msg->transfers, transfer_list) {
    if (!t->len || t->len > 512)
      return -EINVAL;
    if (t->bits_per_word != 8 && t->bits_per_word != 16)
      return -EINVAL;
    if (t->bits_per_word == 16 && (t->len & 1U))
      return -EINVAL;
    if (!t->speed_hz || t->speed_hz < 250000 || t->speed_hz > 20000000)
      return -EINVAL;
    if (t->cs_off || t->delay.value || t->word_delay.value ||
        t->tx_nbits > SPI_NBITS_SINGLE || t->rx_nbits > SPI_NBITS_SINGLE)
      return -EOPNOTSUPP;

    if (!group) {
      speed = t->speed_hz;
      bits = t->bits_per_word;
    } else if (speed != t->speed_hz || bits != t->bits_per_word) {
      return -EOPNOTSUPP;
    }
    group += t->len;
    if (group > 512)
      return -EMSGSIZE;
    if (t->cs_change)
      group = 0;
  }
  return 0;
}

int m5io_hub_spi_transfer(struct m5io_hub *hub, u8 bus, struct spi_message *msg)
{
  struct spi_transfer *t;
  struct spi_transfer *group_first = NULL;
  u8 *txbuf = NULL;
  u8 *rxbuf = NULL;
  unsigned int group_len = 0;
  unsigned int offset = 0;
  unsigned int group_bits = 8;
  u8 chn = M5IO_HUB_SPI_CHN(bus);
  bool window_open = false;
  int ret = 0;

  if (!hub || !hub->spi || !msg || !msg->spi) {
    if (msg)
      msg->status = -EINVAL;
    return -EINVAL;
  }

  ret = m5io_hub_spi_validate_msg(bus, msg);
  if (ret) {
    msg->status = ret;
    return ret;
  }

  txbuf = kmalloc(512, GFP_KERNEL);
  rxbuf = kmalloc(512, GFP_KERNEL);
  if (!txbuf || !rxbuf) {
    ret = -ENOMEM;
    goto out_free;
  }

  msg->actual_length = 0;
  msg->status = 0;

  list_for_each_entry(t, &msg->transfers, transfer_list) {
    struct spi_transfer *u;
    u8 cfg[7];
    u8 len_le[3];
    unsigned int clk_khz;
    bool group_done;

    if (!group_first) {
      group_first = t;
      group_len = 0;
      offset = 0;
      group_bits = t->bits_per_word;
    }

    /* 拼接本组 MOSI；tx_buf 为 NULL 的段补 0，16 bit 按大端字序上线。 */
    if (group_bits == 8) {
      if (t->tx_buf)
        memcpy(&txbuf[offset], t->tx_buf, t->len);
      else
        memset(&txbuf[offset], 0, t->len);
    } else if (t->tx_buf) {
      unsigned int i;

      for (i = 0; i < t->len; i += 2) {
        const u8 *src = (const u8 *)t->tx_buf + i;

        txbuf[offset + i] = src[1];
        txbuf[offset + i + 1] = src[0];
      }
    } else {
      memset(&txbuf[offset], 0, t->len);
    }
    offset += t->len;
    group_len += t->len;

    group_done = list_is_last(&t->transfer_list, &msg->transfers) ||
                 t->cs_change;
    if (!group_done)
      continue;

    /* 每组：CONFIG -> BEGIN -> DATA -> END。 */
    clk_khz = group_first->speed_hz / 1000U;
    cfg[0] = bus;
    cfg[1] = msg->spi->mode & (SPI_CPOL | SPI_CPHA);
    cfg[2] = (u8)group_bits;
    cfg[3] = (u8)(clk_khz & 0xffU);
    cfg[4] = (u8)((clk_khz >> 8) & 0xffU);
    cfg[5] = 0x00; /* watermark = 0x0100（LE） */
    cfg[6] = 0x01;

    ret = m5io_hub_api_token(hub, M5IO_HUB_SPI_FUN, M5IO_HUB_SPI_config, cfg,
                             sizeof(cfg));
    if (ret)
      goto out_free;

    len_le[0] = bus;
    len_le[1] = (u8)(group_len & 0xffU);
    len_le[2] = (u8)((group_len >> 8) & 0xffU);
    ret = m5io_hub_api_token(hub, M5IO_HUB_SPI_FUN, M5IO_HUB_SPI_begin,
                             len_le, sizeof(len_le));
    if (ret)
      goto out_free;
    window_open = true;

    {
      u16 rx_len = 0;

      ret = m5io_hub_api_data(hub, chn, txbuf, group_len,
                              group_bits == 16 ? 254 : 255, rxbuf,
                              (u16)group_len, &rx_len,
                              M5IO_HUB_DATA_TIMEOUT_MS);
      if (ret)
        goto out_free;
      if (rx_len != group_len) {
        ret = -EPROTO;
        goto out_free;
      }
    }

    ret = m5io_hub_api_token(hub, M5IO_HUB_SPI_FUN, M5IO_HUB_SPI_end, len_le,
                             sizeof(len_le));
    if (ret)
      goto out_free;
    window_open = false;

    /* MISO 按各段 len 切回 rx_buf；16 bit 按大端还原成宿主字序。 */
    offset = 0;
    for (u = group_first;; u = list_next_entry(u, transfer_list)) {
      if (u->rx_buf) {
        if (group_bits == 8) {
          memcpy(u->rx_buf, &rxbuf[offset], u->len);
        } else {
          unsigned int i;

          for (i = 0; i < u->len; i += 2) {
            u8 *dst = (u8 *)u->rx_buf + i;

            dst[0] = rxbuf[offset + i + 1];
            dst[1] = rxbuf[offset + i];
          }
        }
      }
      offset += u->len;
      if (u == t)
        break;
    }

    msg->actual_length += group_len;
    group_first = NULL;
    group_len = 0;
    offset = 0;
  }

out_free:
  /* BEGIN 成功后任何失败都可能把 MCU 留在 BUSY 窗口，按协议用 FLUSH 收尾。 */
  if (ret && window_open) {
    int flush_ret = m5io_hub_spi_flush(hub, bus);

    if (flush_ret)
      dev_dbg_ratelimited(hub->dev, "SPI bus %u flush after failure: %d\n",
                          bus, flush_ret);
  }
  kfree(txbuf);
  kfree(rxbuf);
  if (ret)
    msg->status = ret;
  return ret;
}
EXPORT_SYMBOL_GPL(m5io_hub_spi_transfer);

/* =====================================================================
 * IR API（FUNC=6；时序/开关走 TOKEN，波形走 DATA CH=9）
 * ===================================================================== */

int m5io_hub_ir_config(struct m5io_hub *hub, unsigned int clk)
{
  /* 15 B 时序块：carrier(u16 LE)、duty%、leader mark/space、bit mark、
   * bit0/bit1 space、frame gap（各 u16 LE）。API 只有载波，其余按 NEC 补齐，
   * 与 m5io-hub-ir.c 里换算波形用的常量保持一致。 */
  u8 cfg[15];
  u16 carrier = (u16)clk;

  if (!clk || clk > 0xffffU)
    return -EINVAL;
  cfg[0] = (u8)(carrier & 0xffU);
  cfg[1] = (u8)((carrier >> 8) & 0xffU);
  cfg[2] = 33; /* duty % */
  cfg[3] = (u8)(9000 & 0xff);
  cfg[4] = (u8)(9000 >> 8);
  cfg[5] = (u8)(4500 & 0xff);
  cfg[6] = (u8)(4500 >> 8);
  cfg[7] = (u8)(560 & 0xff);
  cfg[8] = (u8)(560 >> 8);
  cfg[9] = (u8)(560 & 0xff);
  cfg[10] = (u8)(560 >> 8);
  cfg[11] = (u8)(1690 & 0xff);
  cfg[12] = (u8)(1690 >> 8);
  cfg[13] = (u8)(30000 & 0xff);
  cfg[14] = (u8)(30000 >> 8);

  return m5io_hub_api_token(hub, M5IO_HUB_IR_FUN, M5IO_HUB_IR_config, cfg,
                            sizeof(cfg));
}
EXPORT_SYMBOL_GPL(m5io_hub_ir_config);

int m5io_hub_ir_reset(struct m5io_hub *hub)
{
  return m5io_hub_api_token(hub, M5IO_HUB_IR_FUN, M5IO_HUB_IR_reset, NULL, 0);
}
EXPORT_SYMBOL_GPL(m5io_hub_ir_reset);

int m5io_hub_ir_start_reace(struct m5io_hub *hub)
{
  u8 payload[1] = {1};

  return m5io_hub_api_token(hub, M5IO_HUB_IR_FUN, M5IO_HUB_IR_receive,
                            payload, sizeof(payload));
}
EXPORT_SYMBOL_GPL(m5io_hub_ir_start_reace);

int m5io_hub_ir_stop_reace(struct m5io_hub *hub)
{
  u8 payload[1] = {0};

  return m5io_hub_api_token(hub, M5IO_HUB_IR_FUN, M5IO_HUB_IR_receive,
                            payload, sizeof(payload));
}
EXPORT_SYMBOL_GPL(m5io_hub_ir_stop_reace);

int m5io_hub_ir_send_data(struct m5io_hub *hub, const u8 *data,
                          unsigned int len)
{
  if (!data || !len || len > M5IO_HUB_DATA_PAYLOAD_MAX)
    return -EINVAL;
  return m5io_hub_api_data(hub, M5IO_HUB_IR_CHN, data, len,
                           M5IO_HUB_DATA_PAYLOAD_MAX, NULL, 0, NULL,
                           M5IO_HUB_DATA_TIMEOUT_MS);
}
EXPORT_SYMBOL_GPL(m5io_hub_ir_send_data);

int m5io_hub_ir_register_rx(struct m5io_hub *hub,
                            m5io_hub_chn_data_handler_t handler, void *p)
{
  if (!hub || !handler)
    return -EINVAL;

  mutex_lock(&hub->chn_lock);
  hub->chn_handler[M5IO_HUB_IR_CHN] = handler;
  hub->chn_handler_data[M5IO_HUB_IR_CHN] = p;
  mutex_unlock(&hub->chn_lock);
  return 0;
}
EXPORT_SYMBOL_GPL(m5io_hub_ir_register_rx);

void m5io_hub_ir_unregister_rx(struct m5io_hub *hub)
{
  if (!hub)
    return;

  mutex_lock(&hub->chn_lock);
  hub->chn_handler[M5IO_HUB_IR_CHN] = NULL;
  hub->chn_handler_data[M5IO_HUB_IR_CHN] = NULL;
  mutex_unlock(&hub->chn_lock);
}
EXPORT_SYMBOL_GPL(m5io_hub_ir_unregister_rx);

/* =====================================================================
 * PWM/背光 API（FUNC=5，无 DATA 通道）
 * ===================================================================== */

int m5io_hub_pwm_config(struct m5io_hub *hub, unsigned int frequency,
                        unsigned int brightness)
{
  u8 cfg[2];
  u8 set[1] = {(u8)brightness};
  int ret;

  if (frequency < 1 || frequency > 65535 || brightness > 255)
    return -EINVAL;
  cfg[0] = (u8)(frequency & 0xffU);
  cfg[1] = (u8)((frequency >> 8) & 0xffU);

  ret = m5io_hub_api_token(hub, M5IO_HUB_PWM_FUN, M5IO_HUB_PWM_config, cfg,
                           sizeof(cfg));
  if (ret)
    return ret;
  return m5io_hub_api_token(hub, M5IO_HUB_PWM_FUN, M5IO_HUB_PWM_set, set,
                            sizeof(set));
}
EXPORT_SYMBOL_GPL(m5io_hub_pwm_config);

int m5io_hub_pwm_reset(struct m5io_hub *hub)
{
  /* 协议没有独立的“复位”子命令：按 api.h 的建议发 SET 0 关闭背光。 */
  u8 set[1] = {0};

  return m5io_hub_api_token(hub, M5IO_HUB_PWM_FUN, M5IO_HUB_PWM_set, set,
                            sizeof(set));
}
EXPORT_SYMBOL_GPL(m5io_hub_pwm_reset);

/* =====================================================================
 * 内部协议辅助（OTA 等使用，不对外导出）
 * ===================================================================== */

static int m5io_hub_hello(struct m5io_hub *hub, u8 payload[15])
{
  int ret;

  if (!hub || !payload)
    return -EINVAL;
  ret = m5io_hub_token_submit(hub, M5IO_HUB_PROTOCOL_FUN,
                              M5IO_HUB_PROTOCOL_hello, NULL, 0, 15, payload,
                              NULL, M5IO_HUB_TOKEN_TIMEOUT_MS);
  if (ret)
    return ret;
  return 0;
}

/* 请求 APP 复位；wait_for_app 时再轮询 HELLO 等应用重新就绪。 */
static int m5io_hub_mcu_reset_request(struct m5io_hub *hub, bool wait_for_app)
{
  unsigned long deadline;
  int ret;
  int hello_ret;

  ret = m5io_hub_token_submit(hub, M5IO_HUB_PROTOCOL_FUN,
                              M5IO_HUB_PROTOCOL_mcuReset, NULL, 0, 0, NULL,
                              NULL, M5IO_HUB_TOKEN_TIMEOUT_MS);
  if (ret || !wait_for_app)
    return ret;

  /* MCU 复位后从 MCU_RESET 应答结束开始重建会话；轮询 HELLO 等它启动。 */
  deadline = jiffies + msecs_to_jiffies(5000);
  do {
    u8 hello[15];

    hello_ret = m5io_hub_hello(hub, hello);
    if (!hello_ret)
      return 0;
    if (hello_ret == -ENODEV || hello_ret == -EBUSY)
      return hello_ret;
    msleep(100);
  } while (time_before(jiffies, deadline));

  dev_warn(hub->dev, "MCU_RESET: application HELLO timeout: %d\n", hello_ret);
  return hello_ret ? hello_ret : -ETIMEDOUT;
}


/* =====================================================================
 * OTA 升级（misc 设备 + 裸 IAP 命令流）
 *
 * 升级期间独占数据线：ota_active/bus_owner 让普通 API 返回 -EBUSY，
 * 队列里已有的操作用 m5io_hub_queues_abort() 取消掉，然后 MCU 复位进
 * bootloader、按 IAP 协议直接下发镜像。普通 API 的收发状态机接入后，
 * OTA 前的 MCU_RESET 会通过 TOKEN 队列正常完成。
 * ===================================================================== */

/* ---- STM32 IAP 命令流常量（与 bootloader 的协议，和 m5io-hub 数据帧无关） ---- */
#define M5IO_HUB_IAP_READ 0x03
#define M5IO_HUB_IAP_WREN 0x06
#define M5IO_HUB_IAP_ENTRY 0x5a
#define M5IO_HUB_IAP_START 0x77
#define M5IO_HUB_IAP_RESPONSE_SIZE 8
#define M5IO_HUB_IAP_HEADER_SIZE 8
#define M5IO_HUB_IAP_DEFAULT_TIMEOUT_MS 2000
#define M5IO_HUB_IAP_MIN_TIMEOUT_MS 100
#define M5IO_HUB_IAP_MAX_TIMEOUT_MS 30000
#define M5IO_HUB_IAP_REARM_DELAY_US 10000
/*
 * The APP only runs a requested reset once its transport has clocked the
 * acknowledgement out.  Dummy bytes keep the bus moving without upsetting the
 * APP frame parser, and they are also the way to tell whether the bootloader
 * is up: a listening bootloader rejects them with BAD_COMMAND, whereas the APP
 * simply ignores them.  The entry loop therefore alternates dummy bytes and a
 * read until the bootloader answers, and only then sends OPC_IAP.
 */
#define M5IO_HUB_IAP_POKE_DELAY_US 2000
#define M5IO_HUB_IAP_ENTRY_REPROBE_DELAY_US 6000
#define M5IO_HUB_IAP_ENTRY_PROBE_TIMEOUT_MS 10
#define M5IO_HUB_IAP_ENTRY_ATTEMPTS 24
#define M5IO_HUB_IAP_ENTRY_DRAIN_EVERY 4
#define M5IO_HUB_IAP_POLL_DELAY_US 1000

/* 独占数据线：普通 API 立即 -EBUSY，已入队/等待中的操作被取消。 */
static void m5io_hub_ota_bus_lock(struct m5io_hub *hub)
{
  unsigned int i;

  WARN_ON_ONCE(READ_ONCE(hub->bus_owner));
  mutex_lock(&hub->ota_lock);
  WRITE_ONCE(hub->bus_owner, current);
  WRITE_ONCE(hub->ota_active, true);
  /*
   * 反复清队列直到不再有活跃 API：已经进入的复合操作（例如 pwm_config 的
   * 第二步、spi_transfer 的下一段）可能在一次 abort 之后又提交了新操作。
   */
  for (i = 0; i < 2000; i++) {
    m5io_hub_queues_abort(hub, -ECANCELED);
    if (!atomic_read(&hub->active_apis))
      break;
    msleep(1);
  }
}

static void m5io_hub_ota_bus_unlock(struct m5io_hub *hub)
{
  WRITE_ONCE(hub->ota_active, false);
  WRITE_ONCE(hub->bus_owner, NULL);
  mutex_unlock(&hub->ota_lock);
}

/* 进入/离开裸 IAP 命令流；普通操作队列在独占开始时已经清空。 */
static void m5io_hub_ota_begin(struct m5io_hub *hub)
{
  m5io_hub_queues_abort(hub, -ECANCELED);
}

static void m5io_hub_ota_end(struct m5io_hub *hub)
{
  m5io_hub_queues_abort(hub, -ECANCELED);
}

static int m5io_hub_ota_spi_xfer(struct m5io_hub *hub,
                                 const void *tx, void *rx, unsigned int len)
{
  struct spi_transfer transfer = {
    .tx_buf = tx,
    .rx_buf = rx,
    .len = len,
  };

  return spi_sync_transfer(hub->spi, &transfer, 1);
}

static int m5io_hub_ota_drain(struct m5io_hub *hub,
                              struct m5io_hub_ota_request *result)
{
  u8 read_command[M5IO_HUB_IAP_RESPONSE_SIZE];
  u8 response[M5IO_HUB_IAP_RESPONSE_SIZE];
  int ret;

  memset(read_command, 0xff, sizeof(read_command));
  read_command[0] = M5IO_HUB_IAP_READ;
  memset(response, 0xff, sizeof(response));

  ret = m5io_hub_ota_spi_xfer(hub, read_command, response,
                              sizeof(response));
  usleep_range(M5IO_HUB_IAP_REARM_DELAY_US,
               M5IO_HUB_IAP_REARM_DELAY_US + 2000);
  if (ret)
    return ret;

  if (response[0] == 0xff)
    return 0;

  result->response_command = response[0];
  result->response_status = response[1];
  result->response_address = ((u32)response[2] << 24) |
                             ((u32)response[3] << 16) |
                             ((u32)response[4] << 8) | response[5];
  result->response_length = ((u16)response[6] << 8) | response[7];
  dev_info(hub->dev,
           "OTA: drained pending reply cmd=0x%02x status=%u addr=0x%08x\n",
           result->response_command, result->response_status,
           result->response_address);
  return 0;
}

/*
 * Clock eight dummy bytes out of the MCU.  The APP consumes them one by one,
 * which advances the physical TX cursor that its deferred reset waits on, and
 * it forms no incomplete candidate, so the APP's parser cannot take the early
 * return that would postpone the reset again.  A bootloader that is listening
 * treats 0xff as an unknown opcode and queues a BAD_COMMAND reply, which the
 * following read picks up.
 */
static int m5io_hub_ota_poke(struct m5io_hub *hub)
{
  u8 dummy[M5IO_HUB_IAP_RESPONSE_SIZE];
  u8 discard[M5IO_HUB_IAP_RESPONSE_SIZE];
  int ret;

  memset(dummy, 0xff, sizeof(dummy));
  memset(discard, 0, sizeof(discard));

  ret = m5io_hub_ota_spi_xfer(hub, dummy, discard, sizeof(dummy));
  usleep_range(M5IO_HUB_IAP_POKE_DELAY_US,
               M5IO_HUB_IAP_POKE_DELAY_US + 500);
  return ret;
}

/*
 * Clock one eight-byte OPC_READ and report whatever the MCU drives back.
 * The bootloader treats these eight bytes as a valid read and answers with its
 * pending response, so a non-0xff byte proves the bootloader is listening.
 * The APP sees an unknown header, resynchronises, and -- importantly -- its TX
 * wire cursor advances, which is the condition it waits for before running a
 * reset it has already been asked for.  One probe therefore both inspects the
 * MCU and releases that deferred reset.
 */
static int m5io_hub_ota_probe(struct m5io_hub *hub,
                              struct m5io_hub_ota_request *result)
{
  u8 read_command[M5IO_HUB_IAP_RESPONSE_SIZE];
  u8 response[M5IO_HUB_IAP_RESPONSE_SIZE];
  int ret;

  memset(read_command, 0xff, sizeof(read_command));
  read_command[0] = M5IO_HUB_IAP_READ;
  memset(response, 0, sizeof(response));

  ret = m5io_hub_ota_spi_xfer(hub, read_command, response,
                              sizeof(response));
  usleep_range(M5IO_HUB_IAP_REARM_DELAY_US,
               M5IO_HUB_IAP_REARM_DELAY_US + 2000);
  if (ret)
    return ret;

  result->response_command = response[0];
  result->response_status = response[1];
  result->response_address = ((u32)response[2] << 24) |
                             ((u32)response[3] << 16) |
                             ((u32)response[4] << 8) | response[5];
  result->response_length = ((u16)response[6] << 8) | response[7];
  return 0;
}

static int m5io_hub_ota_command(struct m5io_hub *hub,
                                const u8 *command, unsigned int command_len,
                                u8 expected_command, u32 expected_address,
                                u16 expected_length,
                                unsigned int timeout_ms,
                                struct m5io_hub_ota_request *result)
{
  u8 read_command[M5IO_HUB_IAP_RESPONSE_SIZE];
  u8 response[M5IO_HUB_IAP_RESPONSE_SIZE];
  unsigned long timeout = 0;
  unsigned int poll;
  bool ready;
  int ret;

  reinit_completion(&hub->ota_ready);
  ret = m5io_hub_ota_spi_xfer(hub, command, NULL, command_len);
  if (ret)
    return ret;

  if (hub->irq > 0) {
    timeout = wait_for_completion_timeout(&hub->ota_ready,
                                          msecs_to_jiffies(timeout_ms));
    ready = timeout != 0;
  } else {
    /* 本阶段还没有申请 HOST_READY 中断：直接走下面的轮询路径。 */
    ready = false;
  }

  /*
   * HOST_READY 中断未接入或应答丢失时，直接轮询 IAP 响应。erase/program
   * 期间 IAP 的 DMA 是停的，这些短读会被忽略；响应发布后读回正常的
   * 八字节应答。
   */
  for (poll = 0; !ready && poll < timeout_ms; poll++) {
    memset(read_command, 0xff, sizeof(read_command));
    read_command[0] = M5IO_HUB_IAP_READ;
    memset(response, 0xff, sizeof(response));
    ret = m5io_hub_ota_spi_xfer(hub, read_command, response,
                                sizeof(read_command));
    if (ret)
      return ret;
    if (response[0] != 0xff) {
      ready = true;
      break;
    }
    usleep_range(M5IO_HUB_IAP_POLL_DELAY_US,
                 M5IO_HUB_IAP_POLL_DELAY_US + 500);
  }
  if (!ready) {
    dev_warn(hub->dev,
             "OTA command 0x%02x timed out waiting for HOST_READY (irq=%d)\n",
             expected_command, hub->irq);
    return -ETIMEDOUT;
  }

  if (timeout) {
    memset(read_command, 0xff, sizeof(read_command));
    read_command[0] = M5IO_HUB_IAP_READ;
    memset(response, 0, sizeof(response));
    ret = m5io_hub_ota_spi_xfer(hub, read_command, response,
                                sizeof(read_command));
    if (ret)
      return ret;
  }

  /* Allow the IAP loop to re-arm its DMA before the next NSS assertion. */
  usleep_range(M5IO_HUB_IAP_REARM_DELAY_US,
               M5IO_HUB_IAP_REARM_DELAY_US + 2000);

  result->response_command = response[0];
  result->response_status = response[1];
  result->response_address = ((u32)response[2] << 24) |
                             ((u32)response[3] << 16) |
                             ((u32)response[4] << 8) | response[5];
  result->response_length = ((u16)response[6] << 8) | response[7];

  if (response[0] != expected_command || response[1] != 0 ||
      result->response_address != expected_address ||
      result->response_length != expected_length) {
    dev_warn(hub->dev,
             "OTA response mismatch: got cmd=0x%02x status=%u addr=0x%08x len=%u, "
             "expected cmd=0x%02x addr=0x%08x len=%u\n",
             response[0], response[1], result->response_address,
             result->response_length, expected_command, expected_address,
             expected_length);
    return response[1] ? -EIO : -EPROTO;
  }

  return 0;
}

/*
 * Firmware upgrade entry.  The APP is first asked to reset into its short IAP
 * entry window through the normal TOKEN queue, then the raw IAP command stream
 * below owns the bus until the upgrade finishes.
 *
 * Timing invariants that must not be broken:
 *  1. The bus has to be frozen the moment the reset is acknowledged: the
 *     bootloader only listens for OPC_IAP during 300 ms after the reset.
 *  2. Bytes sent after the reset request postpone the reset itself; the probe
 *     transaction below is what promotes it, so retries are kept minimal.
 *  3. A dropped command and an absent MCU look identical; the bootloader
 *     answers an unknown opcode with BAD_COMMAND (whose response command byte
 *     is also 0xff), so the probe distinguishes them by status, and OPC_IAP is
 *     only sent after the bootloader has been detected.
 */
static int m5io_hub_ota_update(struct m5io_hub *hub,
                               struct m5io_hub_ota_request *request)
{
  u8 *image = NULL;
  u8 *packet = NULL;
  u32 page;
  unsigned int timeout_ms;
  unsigned long entry_start;
  u8 hello_payload[15];
  int reset_ret = -1;
  unsigned int attempt;
  unsigned int sample;
  int hello_ret;
  int ret;

  if ((request->image_len != M5IO_HUB_OTA_IMAGE_SIZE &&
       request->image_len != M5IO_HUB_OTA_APP_SIZE) ||
      (request->reserved & ~M5IO_HUB_OTA_WAIT_FOR_IAP) ||
      !request->image_ptr)
    return -EINVAL;

  timeout_ms = request->timeout_ms;
  if (!timeout_ms)
    timeout_ms = M5IO_HUB_IAP_DEFAULT_TIMEOUT_MS;
  if (timeout_ms < M5IO_HUB_IAP_MIN_TIMEOUT_MS ||
      timeout_ms > M5IO_HUB_IAP_MAX_TIMEOUT_MS)
    return -EINVAL;

  /*
   * Lock the m5hub data line for the complete upgrade.  Every other m5hub
   * user is refused with -EBUSY from now until the unlock at the end of this
   * function, so neither the APP reset request below nor the IAP command
   * stream that follows can be interleaved with another request.
   */
  m5io_hub_ota_bus_lock(hub);

  /* The image is copied with kmalloc semantics, so kfree() is the matching
   * release operation.  vfree() on this pointer triggers the kernel warning
   * seen when the first ioctl fails. */
  image = memdup_user(u64_to_user_ptr(request->image_ptr),
                      request->image_len);
  if (IS_ERR(image)) {
    ret = PTR_ERR(image);
    goto out_unlock_bus;
  }

  packet = kmalloc(M5IO_HUB_IAP_HEADER_SIZE + M5IO_HUB_OTA_PAGE_SIZE,
                   GFP_KERNEL);
  if (!packet) {
    ret = -ENOMEM;
    goto out_free_image;
  }

  request->completed_pages = 0;
  request->response_command = 0;
  request->response_status = 0xff;
  request->response_address = 0;
  request->response_length = 0;

  dev_info(hub->dev, "OTA start: image=%u bytes timeout=%u ms irq=%d\n",
           request->image_len, timeout_ms, hub->irq);

  packet[0] = M5IO_HUB_IAP_ENTRY;
  entry_start = jiffies;

  /*
   * Step 1: ask the running APP to reset into its short IAP entry window.
   * No raw IAP byte may be sent before this: the APP parses every received
   * byte as part of its own framed protocol, so IAP traffic issued while the
   * APP still runs desynchronises that parser and can make the reset request
   * itself be dropped.
   */
  if (request->reserved & M5IO_HUB_OTA_WAIT_FOR_IAP) {
    dev_info(hub->dev, "OTA: waiting up to 60 seconds for external MCU reset\n");
    reset_ret = -EOPNOTSUPP;
  } else {
    /* OTA must enter the bootloader window immediately after the reset
     * response.  Do not use the public recovery wrapper's app-ready delay. */
    reset_ret = m5io_hub_mcu_reset_request(hub, false);
    /* A definite refusal means the APP is still running: never inject raw
     * IAP bytes after BUSY/UNSUPPORTED/INVALID.  A timeout is uncertain and
     * may mean the MCU already entered IAP. */
    if (reset_ret && reset_ret != -ETIMEDOUT) {
      ret = reset_ret;
      goto out_free_packet;
    }
  }
  if (reset_ret)
    dev_info(hub->dev,
             "OTA: APP reset not acknowledged after %ums (%d), probing for a "
             "bootloader already parked in IAP\n",
             jiffies_to_msecs(jiffies - entry_start), reset_ret);
  else
    dev_info(hub->dev, "OTA: APP reset acknowledged after %ums\n",
             jiffies_to_msecs(jiffies - entry_start));

  /*
   * Step 2: freeze the bus at once.  The bootloader only listens for OPC_IAP
   * for 300 ms after the reset, so every millisecond spent clocking the bus
   * between here and the command below is taken away from that window.
   * Step 1 cannot succeed once an upgrade has been interrupted: the IAP keeps
   * iap_active set until a real reset, so the MCU stays in the bootloader and
   * no longer answers normal TOKEN requests.  The static command stream below
   * works in both states, which is what makes the interrupted case
   * recoverable.
   */
  m5io_hub_ota_begin(hub);

  /*
   * Watch the reset land.  The APP only runs the reset once the master has
   * clocked its acknowledgement out completely, so a single eight-byte read is
   * both the observation and the go-ahead; repeating it also catches the
   * bootloader window if the reset takes a few attempts to be honoured.
   */
  /*
   * Watch for the bootloader while keeping the bus moving, so the APP can
   * finish the reset it accepted and the bootloader's answer becomes visible.
   */
  for (sample = 1; ; sample++) {
    bool external = request->reserved & M5IO_HUB_OTA_WAIT_FOR_IAP;
    if (signal_pending(current)) {
      ret = -EINTR;
      goto out_unlock_ota;
    }
    if ((external && time_after_eq(jiffies, entry_start + msecs_to_jiffies(60000))) ||
        (!external && sample > M5IO_HUB_IAP_ENTRY_ATTEMPTS)) {
      ret = -ETIMEDOUT;
      goto out_unlock_ota;
    }
    if (m5io_hub_ota_poke(hub) || m5io_hub_ota_probe(hub, request)) {
      dev_warn(hub->dev, "OTA probe transfer failed\n");
      ret = -EIO;
      goto out_unlock_ota;
    }
    dev_dbg(hub->dev,
             "OTA probe %u/%u: %ums after OTA start, resp cmd=0x%02x "
             "status=%u addr=0x%08x\n",
             sample, M5IO_HUB_IAP_ENTRY_ATTEMPTS,
             jiffies_to_msecs(jiffies - entry_start),
             request->response_command, request->response_status,
             request->response_address);
    /*
     * Any real answer proves the bootloader is up.  It replies to a dummy
     * command with BAD_COMMAND, whose response command byte is 0xff, so the
     * status is what distinguishes "answered" (small status code) from "nobody
     * on the bus" (0xff).  The read above already drained that reply, so
     * OPC_IAP goes out with no pending response to be discarded.
     */
    if (request->response_command == 0xff && request->response_status == 2 &&
        request->response_address == 0 && request->response_length == 0) {
      dev_info(hub->dev,
               "OTA: bootloader detected (cmd=0x%02x status=%u), entering IAP\n",
               request->response_command, request->response_status);
      break;
    }
    usleep_range(M5IO_HUB_IAP_ENTRY_REPROBE_DELAY_US,
                 M5IO_HUB_IAP_ENTRY_REPROBE_DELAY_US + 500);
  }

  /*
   * The bootloader only listens for OPC_IAP for 300 ms after the reset, and
   * that window is absolute: it is armed once and never extended by an arriving
   * command.  So the command goes out with no bus traffic in front of it.  A
   * reset MCU leaves no reply pending, which is why no drain runs on the happy
   * path -- the drain only happens between attempts, where it is needed.
   *
   * The bootloader may still be arming its DMA when the first command arrives,
   * and a dropped command produces no answer at all.  Retrying is therefore the
   * only way to tell "not listening yet" from "already gone"; each retry drains
   * first, because a pending reply makes the bootloader discard anything that is
   * not an exact eight-byte OPC_READ.  A rejection is final and stops the loop.
   */
  for (attempt = 1; attempt <= M5IO_HUB_IAP_ENTRY_ATTEMPTS; attempt++) {
    dev_info(hub->dev,
             "OTA: OPC_IAP attempt %u/%u, %ums after OTA start "
             "(bootloader window is 300 ms)\n",
             attempt, M5IO_HUB_IAP_ENTRY_ATTEMPTS,
             jiffies_to_msecs(jiffies - entry_start));

    ret = m5io_hub_ota_command(hub, packet, 1, M5IO_HUB_IAP_ENTRY,
                               M5IO_HUB_OTA_START_ADDRESS, 0,
                               M5IO_HUB_IAP_ENTRY_PROBE_TIMEOUT_MS, request);
    if (!ret)
      break;
    if (ret != -ETIMEDOUT) {
      dev_warn(hub->dev,
               "OTA: IAP entry rejected, reply cmd=0x%02x status=%u "
               "addr=0x%08x\n",
               request->response_command, request->response_status,
               request->response_address);
      break;
    }
    if (((attempt % M5IO_HUB_IAP_ENTRY_DRAIN_EVERY) == 0) &&
        m5io_hub_ota_drain(hub, request))
      break;
  }

  if (ret) {
    if (ret == -ETIMEDOUT)
      dev_warn(hub->dev,
               "OTA: OPC_IAP got no answer in %u attempts over %ums; the MCU "
               "answers neither the APP reset nor the bootloader, power-cycle "
               "the device to recover\n",
               M5IO_HUB_IAP_ENTRY_ATTEMPTS,
               jiffies_to_msecs(jiffies - entry_start));
    goto out_unlock_ota;
  }

  dev_info(hub->dev, "OTA: IAP entered, writing %u pages\n",
           M5IO_HUB_OTA_APP_PAGES);

  for (page = 0; page < M5IO_HUB_OTA_APP_PAGES; page++) {
    u32 address = M5IO_HUB_OTA_START_ADDRESS +
                  page * M5IO_HUB_OTA_PAGE_SIZE;

    packet[0] = M5IO_HUB_IAP_WREN;
    packet[1] = address >> 24;
    packet[2] = address >> 16;
    packet[3] = address >> 8;
    packet[4] = address;
    packet[5] = (u8)(M5IO_HUB_OTA_PAGE_SIZE >> 8);
    packet[6] = (u8)(M5IO_HUB_OTA_PAGE_SIZE & 0xff);
    packet[7] = 0;
    memcpy(packet + M5IO_HUB_IAP_HEADER_SIZE,
           image + page * M5IO_HUB_OTA_PAGE_SIZE,
           M5IO_HUB_OTA_PAGE_SIZE);

    ret = m5io_hub_ota_command(hub, packet,
                               M5IO_HUB_IAP_HEADER_SIZE +
                                   M5IO_HUB_OTA_PAGE_SIZE,
                               M5IO_HUB_IAP_WREN, address,
                               M5IO_HUB_OTA_PAGE_SIZE, timeout_ms, request);
    if (ret)
      goto out_unlock_ota;
    request->completed_pages = page + 1;
  }

  packet[0] = M5IO_HUB_IAP_START;
  ret = m5io_hub_ota_command(hub, packet, 1, M5IO_HUB_IAP_START,
                             M5IO_HUB_OTA_START_ADDRESS, 0, timeout_ms,
                             request);

out_unlock_ota:
  m5io_hub_ota_end(hub);
out_free_packet:
  kfree(packet);
out_free_image:
  kfree(image);
out_unlock_bus:
  m5io_hub_ota_bus_unlock(hub);

  /*
   * Diagnostic for a failed entry whose reset was acknowledged: measure how
   * long the MCU stops answering the APP protocol.  A bootloader that is
   * present holds the bus for its ~300 ms entry window before falling back to
   * the APP, so the APP stays silent for about that long; a reset that never
   * took effect, or a device whose flash carries no bootloader, answers again
   * almost immediately.  HELLO is read-only and carries no 0x5a, so it cannot
   * pull the bootloader into upgrade mode.
   */
  if (ret && (reset_ret == 0)) {
    for (sample = 1; sample <= 16; sample++) {
      msleep(25);
      hello_ret = m5io_hub_hello(hub, hello_payload);
      dev_info(hub->dev,
               "OTA: APP %s at %ums after OTA start (%d)\n",
               hello_ret ? "silent" : "answers",
               jiffies_to_msecs(jiffies - entry_start), hello_ret);
    }
  }
  return ret;
}

static long m5io_hub_misc_ioctl(struct file *file, unsigned int command,
                                unsigned long argument)
{
  struct miscdevice *misc = file->private_data;
  struct m5io_hub *hub = container_of(misc, struct m5io_hub,
                                              ota_misc);
  struct m5io_hub_ota_request request;
  long ret;

  if (command != M5IO_HUB_IOCTL_OTA)
    return -ENOTTY;
  if (copy_from_user(&request, (void __user *)argument, sizeof(request)))
    return -EFAULT;

  /* 并发 OTA 由 m5io_hub_ota_bus_lock() 里的 ota_lock 串行化。 */
  ret = m5io_hub_ota_update(hub, &request);

  if (copy_to_user((void __user *)argument, &request, sizeof(request)))
    return -EFAULT;
  return ret;
}

static const struct file_operations m5io_hub_misc_fops = {
  .owner = THIS_MODULE,
  .unlocked_ioctl = m5io_hub_misc_ioctl,
#ifdef CONFIG_COMPAT
  .compat_ioctl = m5io_hub_misc_ioctl,
#endif
  .llseek = noop_llseek,
};

static void m5io_hub_misc_deregister(void *data)
{
  struct m5io_hub *hub = data;

  misc_deregister(&hub->ota_misc);
}

/* =====================================================================
 * 框架层：SPI driver 注册与子设备
 * ===================================================================== */

static int m5io_hub_probe(struct spi_device *spi)
{
  struct m5io_hub *hub;
  int ret;

  hub = devm_kzalloc(&spi->dev, sizeof(*hub), GFP_KERNEL);
  if (!hub)
    return -ENOMEM;

  hub->dev = &spi->dev;
  hub->spi = spi;
  /* HOST_READY 中断留到收发状态机那一步再接；在此之前 OTA 走轮询路径。 */
  hub->irq = 0;

  mutex_init(&hub->token_lock);
  INIT_LIST_HEAD(&hub->token_queue);
  init_waitqueue_head(&hub->token_wait);
  mutex_init(&hub->data_lock);
  INIT_LIST_HEAD(&hub->data_queue);
  init_waitqueue_head(&hub->data_wait);
  mutex_init(&hub->tid_lock);
  init_waitqueue_head(&hub->tid_wait);
  mutex_init(&hub->chn_lock);
  mutex_init(&hub->gpio_irq_lock);
  mutex_init(&hub->ota_lock);
  init_completion(&hub->ota_ready);
  atomic_set(&hub->active_apis, 0);
  WRITE_ONCE(hub->ota_active, false);
  WRITE_ONCE(hub->bus_owner, NULL);

  spi->bits_per_word = 8;
  ret = spi_setup(spi);
  if (ret)
    return dev_err_probe(&spi->dev, ret, "failed to setup SPI\n");

  /* 子驱动从父设备的 drvdata 取 hub 句柄。 */
  dev_set_drvdata(&spi->dev, hub);

  snprintf(hub->ota_misc_name, sizeof(hub->ota_misc_name), "m5io-hub-ota");
  hub->ota_misc.minor = MISC_DYNAMIC_MINOR;
  hub->ota_misc.name = hub->ota_misc_name;
  hub->ota_misc.fops = &m5io_hub_misc_fops;
  hub->ota_misc.parent = &spi->dev;
  ret = misc_register(&hub->ota_misc);
  if (ret)
    return dev_err_probe(&spi->dev, ret, "failed to register OTA misc\n");
  ret = devm_add_action_or_reset(&spi->dev, m5io_hub_misc_deregister, hub);
  if (ret)
    return ret;

  ret = devm_of_platform_populate(&spi->dev);
  if (ret)
    return dev_err_probe(&spi->dev, ret, "failed to populate DT children\n");

  dev_info(&spi->dev,
           "m5io-hub core probed: token/data queues ready, irq=%d\n",
           hub->irq);
  return 0;
}

static void m5io_hub_remove(struct spi_device *spi)
{
  /* 所有资源都由 devm 管理，子设备由 of_platform_populate 自动解绑。 */
}

static const struct of_device_id m5io_hub_of_match[] = {
    {.compatible = "m5stack,m5io-hub"}, {}};
MODULE_DEVICE_TABLE(of, m5io_hub_of_match);

static const struct spi_device_id m5io_hub_spi_id[] = {{"m5io-hub", 0}, {}};
MODULE_DEVICE_TABLE(spi, m5io_hub_spi_id);

static struct spi_driver m5io_hub_driver = {
  .driver =
    {
      .name = "m5io-hub",
      .of_match_table = m5io_hub_of_match,
    },
  .probe = m5io_hub_probe,
  .remove = m5io_hub_remove,
  .id_table = m5io_hub_spi_id,
};
module_spi_driver(m5io_hub_driver);

MODULE_DESCRIPTION("M5Stack M5IO-HUB core driver");
MODULE_LICENSE("GPL");
