/*
 * M5Stack M5IO-HUB MFD 核心驱动
 *
 * 通过 SPI 与 M5IO-HUB 芯片通信，向上提供寄存器读写、RPC 事务调度
 * （TX/RX FIFO + 工作队列）以及 Arduino 风格的 GPIO 接口，并按 MFD
 * 框架注册 GPIO / UART / I2C / SPI 四个子设备。
 */

#include <linux/bitops.h>
#include <linux/completion.h>
#include <linux/crc-ccitt.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/jiffies.h>
#include <linux/kfifo.h>
#include <linux/mfd/core.h>
#include <linux/mfd/m5io-hub.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/slab.h>
#include <linux/spi/spi.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/workqueue.h>

#define M5IO_HUB_FIFO_SIZE 4096
#define M5IO_HUB_GPIO_IRQ_FIFO_SIZE 256
#define M5IO_HUB_RPC_XFER_ALIGN 8
#define M5IO_HUB_RPC_FILL_BYTE 0xff
#define M5IO_HUB_RPC_MAX_TRANSACTION_BYTES (64U * 1024U)
#define M5IO_HUB_RPC_TID_NR 4
#define M5_RPC_TOKEN_HDR_LEN 2
#define M5_RPC_CRC_LEN 2
#define M5_RPC_TOKEN_MAX_PLEN 0xf
#define M5_RPC_TOKEN_MAX_LEN                                                       \
  (M5_RPC_TOKEN_HDR_LEN + M5_RPC_TOKEN_MAX_PLEN + M5_RPC_CRC_LEN)
#define M5_RPC_TYPE_TOKEN 0
#define M5_RPC_CMD_GPIO 0x1
#define M5_RPC_CMD_GPIO_INTERRUPT_REPORT 0xa
#define M5_RPC_CMD_GPIO_PINMODE 0x1
#define M5_RPC_CMD_GPIO_DIGITALWRITE 0x2
#define M5_RPC_CMD_GPIO_DIGITALREAD 0x3
#define M5_RPC_CMD_GPIO_ATTACHINTERRUPT 0x4
#define M5_RPC_CMD_GPIO_DETACHINTERRUPT 0x5
#define M5_RPC_TOKEN_TIMEOUT_MS 2000

/*
 * Token 协议帧头（16 bit，小端按字节拆分）：
 * header = TYPE | (P << 1) | (TID << 2) | (CMD << 4)
 *        | (LEN << 8) | (ARG << 12)
 * data[] 为变长载荷，长度由 len 给出。
 */
struct m5_rpc_token_frame {
  u8 type : 1;
  u8 p : 1;
  u8 tid : 2;
  u8 cmd : 4;
  u8 len : 4;
  u8 arg : 4;
  u8 data[];
} __packed;

struct m5io_hub_core;

typedef void (*m5_rpc_cb_t)(struct m5io_hub_core *mcore, int tid, int status);

struct m5_rpc_data {
  struct mutex tid_lock[M5IO_HUB_RPC_TID_NR];
  m5_rpc_cb_t cb[M5IO_HUB_RPC_TID_NR];
  wait_queue_head_t tid_wq;
};

struct m5_rpc_token {
  struct mutex tid_lock[M5IO_HUB_RPC_TID_NR];
  m5_rpc_cb_t cb[M5IO_HUB_RPC_TID_NR];
  wait_queue_head_t tid_wq;
  spinlock_t state_lock;
  bool waiting[M5IO_HUB_RPC_TID_NR];
  u8 expected_cmd[M5IO_HUB_RPC_TID_NR];
  struct completion done[M5IO_HUB_RPC_TID_NR];
  int status[M5IO_HUB_RPC_TID_NR];
  u8 rx[M5IO_HUB_RPC_TID_NR][M5_RPC_TOKEN_MAX_LEN];
  u8 rx_len[M5IO_HUB_RPC_TID_NR];
};

struct m5_rpc {
  struct m5_rpc_data data;
  struct m5_rpc_token token;
};

struct m5_rpc_rx_stream {
  u8 frame[M5_RPC_TOKEN_MAX_LEN];
  u8 len;
  u8 expect_len;
};

struct m5io_hub_gpio_irq_event {
  u32 generation;
  u8 pin;
};

struct m5io_hub_core {
  struct m5io_hub hub;

  struct m5_rpc rpc;

  struct work_struct rpc_work;
  struct mutex spi_lock;
  atomic_t active_reqs;
  struct m5_rpc_rx_stream rx_stream;

  struct kfifo rx_fifo;
  spinlock_t rx_lock;
  wait_queue_head_t rx_wq;

  struct kfifo tx_fifo;
  spinlock_t tx_lock;
  wait_queue_head_t tx_wq;

  struct work_struct gpio_irq_work;
  spinlock_t gpio_irq_pending_lock;
  DECLARE_KFIFO(gpio_irq_fifo, struct m5io_hub_gpio_irq_event,
                M5IO_HUB_GPIO_IRQ_FIFO_SIZE);
  bool gpio_irq_work_scheduled;
  struct mutex gpio_irq_handler_lock;
  m5io_hub_gpio_irq_handler_t gpio_irq_handler;
  void *gpio_irq_handler_data;
  u32 gpio_irq_handler_generation;
};

static void m5io_hub_gpio_irq_worker(struct work_struct *work) {
  struct m5io_hub_core *mcore;
  struct m5io_hub_gpio_irq_event event;
  unsigned long flags;
  bool have_event;

  mcore = container_of(work, struct m5io_hub_core, gpio_irq_work);

  for (;;) {
    spin_lock_irqsave(&mcore->gpio_irq_pending_lock, flags);
    have_event = kfifo_get(&mcore->gpio_irq_fifo, &event);
    if (!have_event)
      mcore->gpio_irq_work_scheduled = false;
    spin_unlock_irqrestore(&mcore->gpio_irq_pending_lock, flags);

    if (!have_event)
      break;

    mutex_lock(&mcore->gpio_irq_handler_lock);
    if (mcore->gpio_irq_handler &&
        event.generation == mcore->gpio_irq_handler_generation)
      mcore->gpio_irq_handler(mcore->gpio_irq_handler_data, event.pin);
    mutex_unlock(&mcore->gpio_irq_handler_lock);
  }
}

static void m5io_hub_gpio_irq_report(struct m5io_hub_core *mcore,
                                     unsigned int pin) {
  struct m5io_hub_gpio_irq_event event;
  unsigned long flags;
  bool inserted;
  bool queue = false;

  if (!smp_load_acquire(&mcore->gpio_irq_handler))
    return;

  event.generation = READ_ONCE(mcore->gpio_irq_handler_generation);
  event.pin = pin;

  spin_lock_irqsave(&mcore->gpio_irq_pending_lock, flags);
  inserted = kfifo_put(&mcore->gpio_irq_fifo, event);
  if (inserted && !mcore->gpio_irq_work_scheduled) {
    mcore->gpio_irq_work_scheduled = true;
    queue = true;
  }
  spin_unlock_irqrestore(&mcore->gpio_irq_pending_lock, flags);

  if (!inserted) {
    dev_warn_ratelimited(mcore->hub.dev,
                         "GPIO irq event FIFO full, drop pin %u\n", pin);
    return;
  }

  if (queue)
    queue_work(system_unbound_wq, &mcore->gpio_irq_work);
}

/**
 * m5_rpc_token_frame_fill - 按协议位域填充 token 帧头，并可选拷贝载荷
 * @frame: 待填充的 token 帧，调用方需保证尾部有 @len 字节空间
 * @type: TYPE，1 bit
 * @p: P，1 bit
 * @tid: 事务 ID，2 bit
 * @cmd: 命令字，4 bit
 * @len: 后续 data 长度，4 bit
 * @arg: 参数，4 bit
 * @data: 载荷指针；为 NULL 或 @len 为 0 时不拷贝
 *
 * 编码关系：
 * header = TYPE | (P << 1) | (TID << 2) | (CMD << 4)
 *        | (LEN << 8) | (ARG << 12)
 */
static void m5_rpc_token_frame_fill(struct m5_rpc_token_frame *frame, u8 type,
                                    u8 p, u8 tid, u8 cmd, u8 len, u8 arg,
                                    const u8 *data) {
  if (!frame)
    return;

  frame->type = type & 0x1;
  frame->p = p & 0x1;
  frame->tid = tid & 0x3;
  frame->cmd = cmd & 0xf;
  frame->len = len & 0xf;
  frame->arg = arg & 0xf;

  if (data && frame->len)
    memcpy(frame->data, data, frame->len);
}

/**
 * m5_rpc_token_frame_crc_check - 按协议校验一帧 TOKEN NSS 事务
 * @buf: 完整 NSS 事务缓冲，线上顺序为 Header | Payload | CRC
 * @nss_len: 实际收到的 NSS 事务长度
 *
 * 处理顺序：
 * 1. 读取完整 NSS 长度
 * 2. 读取前两字节 Header
 * 3. 由 LEN 计算期望长度：2 + LEN + 2
 * 4. 比较期望长度与 @nss_len
 * 5. 解析帧头字段
 * 6. 计算 CRC-CCITT-FALSE(Header + Payload)
 * 7. 与帧末大端 CRC（高字节在前）比较
 * 8. 仅校验成功时返回 0
 *
 * Return: 0 表示校验通过；-EINVAL 参数非法；-EBADMSG 长度或 CRC 不匹配。
 */
static int m5_rpc_token_frame_crc_check(const u8 *buf, unsigned int nss_len) {
  const struct m5_rpc_token_frame *frame;
  unsigned int expect_len;
  u16 crc_calc;
  u16 crc_rx;

  if (!buf)
    return -EINVAL;

  if (nss_len < M5_RPC_TOKEN_HDR_LEN + M5_RPC_CRC_LEN)
    return -EINVAL;

  frame = (const struct m5_rpc_token_frame *)buf;
  expect_len = M5_RPC_TOKEN_HDR_LEN + frame->len + M5_RPC_CRC_LEN;
  if (expect_len != nss_len)
    return -EBADMSG;

  crc_calc = crc_ccitt(0xffff, buf, M5_RPC_TOKEN_HDR_LEN + frame->len);
  crc_rx = ((u16)buf[M5_RPC_TOKEN_HDR_LEN + frame->len] << 8) |
           buf[M5_RPC_TOKEN_HDR_LEN + frame->len + 1];
  if (crc_calc != crc_rx)
    return -EBADMSG;

  return 0;
}

/**
 * m5_rpc_token_frame_crc_fill - 计算并写入 token 帧末尾的大端 CRC
 * @frame: 已填好 Header/Payload 的帧，调用方需在 data[len] 后预留 2 字节
 */
static void m5_rpc_token_frame_crc_fill(struct m5_rpc_token_frame *frame) {
  u16 crc;
  unsigned int n;

  if (!frame)
    return;

  n = M5_RPC_TOKEN_HDR_LEN + frame->len;
  crc = crc_ccitt(0xffff, (const u8 *)frame, n);
  frame->data[frame->len] = crc >> 8;
  frame->data[frame->len + 1] = crc & 0xff;
}

/**
 * m5_rpc_token_frame_put - 把 token 帧写入 TX FIFO 并排队一次 RPC 传输
 * @mcore: hub 核心上下文
 * @buf: 完整线上帧（Header + Payload + CRC）
 * @len: @buf 长度
 *
 * Return: 0 已入队；-ENOSPC FIFO 空间不足；其余为 m5io_hub_rpc_transfer() 错误码。
 */
static int m5_rpc_token_frame_put(struct m5io_hub_core *mcore, const u8 *buf,
                                  unsigned int len) {
  unsigned long flags;
  unsigned int copied;

  if (!mcore || !buf || !len)
    return -EINVAL;

  spin_lock_irqsave(&mcore->tx_lock, flags);
  copied = kfifo_avail(&mcore->tx_fifo) >= len
               ? kfifo_in(&mcore->tx_fifo, buf, len)
               : 0;
  spin_unlock_irqrestore(&mcore->tx_lock, flags);
  if (copied != len)
    return -ENOSPC;

  return m5io_hub_rpc_transfer(&mcore->hub);
}

/**
 * m5_rpc_token_done_cb - token 应答到达时结束等待
 * @mcore: hub 核心上下文
 * @tid: 事务 ID
 * @status: 0 成功，负值为错误
 */
static void m5_rpc_token_done_cb(struct m5io_hub_core *mcore, int tid,
                                 int status) {
  if (!mcore || tid < 0 || tid >= M5IO_HUB_RPC_TID_NR)
    return;

  mcore->rpc.token.status[tid] = status;
  complete(&mcore->rpc.token.done[tid]);
}

/**
 * m5_rpc_token_wait - 等待指定 TID 的应答回调，超时则返回
 * @mcore: hub 核心上下文
 * @tid: 由 token_tid_alloc() 得到的事务 ID
 * @timeout: 等待超时（jiffies）
 *
 * 应答回调会 complete 对应完成量；超时同样让调用方继续做 tid_free。
 *
 * Return: 0 或回调写入的 status；超时返回 -ETIMEDOUT。
 */
static int m5_rpc_token_wait(struct m5io_hub_core *mcore, int tid,
                             unsigned long timeout) {
  if (!mcore || tid < 0 || tid >= M5IO_HUB_RPC_TID_NR)
    return -EINVAL;

  if (!wait_for_completion_timeout(&mcore->rpc.token.done[tid], timeout))
    return -ETIMEDOUT;

  return mcore->rpc.token.status[tid];
}

/**
 * m5_rpc_token_frame_parse - 解析已通过 CRC 的 token 帧头
 * @buf: Header | Payload | CRC
 * @nss_len: 缓冲长度
 * @out: 可选，拷贝 2 字节帧头
 *
 * Return: 0 成功；-EINVAL 参数非法。
 */
static int m5_rpc_token_frame_parse(const u8 *buf, unsigned int nss_len,
                                    struct m5_rpc_token_frame *out) {
  if (!buf || nss_len < M5_RPC_TOKEN_HDR_LEN)
    return -EINVAL;

  if (out)
    memcpy(out, buf, M5_RPC_TOKEN_HDR_LEN);

  return 0;
}

/**
 * m5_rpc_token_rx_dispatch - 分发同步应答或识别设备主动上报 token
 * @mcore: hub 核心上下文
 * @buf: SPI 读回数据
 * @len: 读回长度
 *
 * 首先用 TID 和 CMD 匹配当前正在等待的同步请求。未匹配时再识别设备
 * 主动上报的 GPIO 中断 token：CMD=10、ARG=0、data[0]=pin；主动上报的
 * TID 由设备决定，不参与匹配。
 *
 */
static void m5_rpc_token_rx_dispatch(struct m5io_hub_core *mcore, const u8 *buf,
                                     unsigned int len) {
  const struct m5_rpc_token_frame *frame;
  unsigned long flags;
  unsigned int expect_len;
  m5_rpc_cb_t cb;
  bool matched = false;
  int tid;

  if (!mcore || !buf || len < M5_RPC_TOKEN_HDR_LEN + M5_RPC_CRC_LEN)
    return;

  frame = (const struct m5_rpc_token_frame *)buf;
  if (frame->type != M5_RPC_TYPE_TOKEN)
    return;

  expect_len = M5_RPC_TOKEN_HDR_LEN + frame->len + M5_RPC_CRC_LEN;
  if (expect_len > len || expect_len > M5_RPC_TOKEN_MAX_LEN)
    return;

  if (m5_rpc_token_frame_crc_check(buf, expect_len))
    return;

  tid = frame->tid;
  cb = NULL;

  spin_lock_irqsave(&mcore->rpc.token.state_lock, flags);
  if (mcore->rpc.token.waiting[tid] &&
      mcore->rpc.token.expected_cmd[tid] == frame->cmd) {
    mcore->rpc.token.waiting[tid] = false;
    memcpy(mcore->rpc.token.rx[tid], buf, expect_len);
    mcore->rpc.token.rx_len[tid] = expect_len;
    cb = mcore->rpc.token.cb[tid];
    matched = true;
  }
  spin_unlock_irqrestore(&mcore->rpc.token.state_lock, flags);

  if (matched) {
    if (cb)
      cb(mcore, tid, 0);
    return;
  }

  if (frame->cmd != M5_RPC_CMD_GPIO_INTERRUPT_REPORT || frame->arg != 0 ||
      frame->len < 1 || frame->data[0] >= M5IO_HUB_NGPIO) {
    dev_dbg_ratelimited(mcore->hub.dev,
                        "drop unmatched token: tid=%u cmd=%u arg=%u len=%u\n",
                        frame->tid, frame->cmd, frame->arg, frame->len);
    return;
  }

  dev_dbg(mcore->hub.dev, "gpio interrupt report: pin=%u\n", frame->data[0]);
  m5io_hub_gpio_irq_report(mcore, frame->data[0]);
}

/**
 * m5_rpc_rx_frame_put - 将一帧完整的接收数据写入 RX FIFO
 * @mcore: hub 核心上下文
 * @buf: 完整协议帧
 * @len: 协议帧长度
 *
 * RX FIFO 仅保存非 token 数据帧。空间不足时整帧丢弃，避免向 FIFO
 * 留下无法解析的半帧。
 */
static void m5_rpc_rx_frame_put(struct m5io_hub_core *mcore, const u8 *buf,
                                unsigned int len) {
  unsigned long flags;
  unsigned int copied = 0;

  spin_lock_irqsave(&mcore->rx_lock, flags);
  if (kfifo_avail(&mcore->rx_fifo) >= len)
    copied = kfifo_in(&mcore->rx_fifo, buf, len);
  spin_unlock_irqrestore(&mcore->rx_lock, flags);

  if (copied != len) {
    dev_warn_ratelimited(mcore->hub.dev,
                         "rx_fifo overflow: drop %u-byte rpc frame\n", len);
    return;
  }

  wake_up_interruptible(&mcore->rx_wq);
}

/**
 * m5_rpc_rx_stream_feed - 将一次 SPI 读回的数据送入协议流解析器
 * @mcore: hub 核心上下文
 * @buf: SPI 读回数据
 * @len: 读回长度
 * @slave_empty: 返回时为 true 表示在帧边界读到了 0xff 空白
 *
 * LEN 位只描述 payload 长度，因此完整帧长度为 2 + LEN + 2。SPI 传输
 * 可能停在帧中间，未完成数据保存在 rx_stream 中供下一轮继续拼接。0xff
 * 只有出现在帧边界时才表示从机已经没有数据，payload 或 CRC 内的 0xff
 * 仍按普通帧数据处理。
 *
 * Return: 当前半帧还缺少的字节数；没有半帧时返回 0。
 */
static unsigned int m5_rpc_rx_stream_feed(struct m5io_hub_core *mcore,
                                          const u8 *buf, unsigned int len,
                                          bool *slave_empty) {
  struct m5_rpc_rx_stream *stream = &mcore->rx_stream;
  const struct m5_rpc_token_frame *frame;
  unsigned int copy_len;
  unsigned int pos = 0;

  *slave_empty = false;

  while (pos < len) {
    if (!stream->len && buf[pos] == M5IO_HUB_RPC_FILL_BYTE) {
      *slave_empty = true;
      break;
    }

    if (stream->len < M5_RPC_TOKEN_HDR_LEN) {
      copy_len = min_t(unsigned int, M5_RPC_TOKEN_HDR_LEN - stream->len,
                       len - pos);
      memcpy(stream->frame + stream->len, buf + pos, copy_len);
      stream->len += copy_len;
      pos += copy_len;

      if (stream->len < M5_RPC_TOKEN_HDR_LEN)
        break;

      stream->expect_len = M5_RPC_TOKEN_HDR_LEN +
                           (stream->frame[1] & M5_RPC_TOKEN_MAX_PLEN) +
                           M5_RPC_CRC_LEN;
    }

    copy_len = min_t(unsigned int, stream->expect_len - stream->len,
                     len - pos);
    memcpy(stream->frame + stream->len, buf + pos, copy_len);
    stream->len += copy_len;
    pos += copy_len;

    if (stream->len < stream->expect_len)
      break;

    frame = (const struct m5_rpc_token_frame *)stream->frame;
    if (frame->type == M5_RPC_TYPE_TOKEN)
      m5_rpc_token_rx_dispatch(mcore, stream->frame, stream->expect_len);
    else
      m5_rpc_rx_frame_put(mcore, stream->frame, stream->expect_len);

    stream->len = 0;
    stream->expect_len = 0;
  }

  if (!stream->expect_len)
    return 0;

  return stream->expect_len - stream->len;
}

/**
 * m5_rpc_tid_try_alloc - 非阻塞地占用一个空闲 TID 槽位并绑定回调
 * @locks: TID mutex 数组，每个元素代表一个 RPC 事务槽位
 * @cbs: 与 @locks 一一对应的完成回调数组
 * @n: 槽位数量，即 @locks 与 @cbs 的元素个数
 * @cb: 本次事务完成时使用的回调，可为 NULL
 *
 * 依次对每个槽位调用 mutex_trylock()，抢占第一个空闲槽位，并把 @cb
 * 写入 @cbs 中对应的下标处。
 *
 * Return: 成功返回已加锁的 mutex 指针；没有空闲槽位时返回 NULL。
 */
static struct mutex *m5_rpc_tid_try_alloc(struct mutex *locks, m5_rpc_cb_t *cbs,
                                          unsigned int n, m5_rpc_cb_t cb) {
  unsigned int i;

  for (i = 0; i < n; i++) {
    if (!mutex_trylock(&locks[i]))
      continue;

    cbs[i] = cb;
    return &locks[i];
  }

  return NULL;
}

/**
 * m5_rpc_tid_alloc - 等待空闲 TID 槽位后占用并绑定回调
 * @locks: TID mutex 数组
 * @cbs: 与 @locks 一一对应的完成回调数组
 * @n: 槽位数量
 * @cb: 本次事务完成时使用的回调，可为 NULL
 * @wq: 槽位释放时被唤醒的等待队列
 *
 * 当前无空闲槽位时会在 @wq 上等待，直到 m5_rpc_tid_free() 释放出
 * 一个槽位。
 *
 * Context: 可睡眠的进程上下文，不可在原子上下文调用。
 *
 * Return: 已加锁的 mutex 指针，不会返回 NULL。
 */
static struct mutex *m5_rpc_tid_alloc(struct mutex *locks, m5_rpc_cb_t *cbs,
                                      unsigned int n, m5_rpc_cb_t cb,
                                      wait_queue_head_t *wq) {
  struct mutex *lock;

  wait_event(*wq, (lock = m5_rpc_tid_try_alloc(locks, cbs, n, cb)) != NULL);
  return lock;
}

/**
 * m5_rpc_tid_free - 释放指定 TID 槽位并唤醒等待者
 * @locks: TID mutex 数组
 * @cbs: 与 @locks 一一对应的完成回调数组
 * @n: 槽位数量
 * @lock: 由 m5_rpc_tid_alloc() 返回的 mutex 指针
 * @wq: 槽位释放时被唤醒的等待队列
 *
 * 清除回调、解锁槽位并唤醒一个等待者。@lock 为 NULL 或不属于 @locks
 * 时直接返回，因此调用方可以无条件地做清理。
 */
static void m5_rpc_tid_free(struct mutex *locks, m5_rpc_cb_t *cbs,
                            unsigned int n, struct mutex *lock,
                            wait_queue_head_t *wq) {
  unsigned int i;

  if (!lock || lock < locks)
    return;

  i = lock - locks;
  if (i >= n)
    return;

  cbs[i] = NULL;
  mutex_unlock(lock);
  wake_up(wq);
}

/**
 * token_tid_alloc - 从 token 池占用一个 TID 槽位并绑定回调
 * @mcore: hub 核心上下文
 * @cb: 本次事务完成时使用的回调，可为 NULL
 * @expected_cmd: 本次同步请求期望应答的 CMD
 *
 * 无空闲槽位时在 token 池的等待队列上等待。
 *
 * Context: 可睡眠的进程上下文。
 *
 * Return: 成功返回已加锁的 token tid_lock 指针；@mcore 为 NULL 时返回
 *         NULL。
 */
static struct mutex *token_tid_alloc(struct m5io_hub_core *mcore,
                                     m5_rpc_cb_t cb, u8 expected_cmd) {
  struct mutex *lock;
  unsigned long flags;
  unsigned int tid;

  if (!mcore)
    return NULL;

  lock = m5_rpc_tid_alloc(mcore->rpc.token.tid_lock, mcore->rpc.token.cb,
                          ARRAY_SIZE(mcore->rpc.token.tid_lock), cb,
                          &mcore->rpc.token.tid_wq);
  tid = lock - mcore->rpc.token.tid_lock;
  mcore->rpc.token.status[tid] = 0;
  mcore->rpc.token.rx_len[tid] = 0;
  reinit_completion(&mcore->rpc.token.done[tid]);

  spin_lock_irqsave(&mcore->rpc.token.state_lock, flags);
  mcore->rpc.token.expected_cmd[tid] = expected_cmd;
  mcore->rpc.token.waiting[tid] = true;
  spin_unlock_irqrestore(&mcore->rpc.token.state_lock, flags);
  return lock;
}

/**
 * token_tid_free - 释放 token 池中的 TID 槽位
 * @mcore: hub 核心上下文
 * @lock: 由 token_tid_alloc() 返回的 mutex 指针
 *
 * @mcore 为 NULL 或 @lock 不属于 token 池时直接返回。
 */
static void token_tid_free(struct m5io_hub_core *mcore, struct mutex *lock) {
  unsigned long flags;
  unsigned int tid;

  if (!mcore)
    return;

  if (!lock || lock < mcore->rpc.token.tid_lock ||
      lock >= mcore->rpc.token.tid_lock +
                  ARRAY_SIZE(mcore->rpc.token.tid_lock))
    return;

  tid = lock - mcore->rpc.token.tid_lock;
  spin_lock_irqsave(&mcore->rpc.token.state_lock, flags);
  mcore->rpc.token.waiting[tid] = false;
  mcore->rpc.token.expected_cmd[tid] = 0;
  spin_unlock_irqrestore(&mcore->rpc.token.state_lock, flags);

  m5_rpc_tid_free(mcore->rpc.token.tid_lock, mcore->rpc.token.cb,
                  ARRAY_SIZE(mcore->rpc.token.tid_lock), lock,
                  &mcore->rpc.token.tid_wq);
}

/**
 * m5io_hub_tx_fifo_len - 获取当前待发送字节数
 * @mcore: hub 核心上下文
 */
static unsigned int m5io_hub_tx_fifo_len(struct m5io_hub_core *mcore) {
  unsigned long flags;
  unsigned int len;

  spin_lock_irqsave(&mcore->tx_lock, flags);
  len = kfifo_len(&mcore->tx_fifo);
  spin_unlock_irqrestore(&mcore->tx_lock, flags);

  return len;
}

/**
 * m5io_hub_rpc_transaction - 激活并排空一次 SPI RPC 事务
 * @mcore: hub 核心上下文
 *
 * 首次传输优先使用 TX FIFO 的实际长度；TX FIFO 为空时发送 8 字节 0xff
 * 以读取从机。收到不完整帧后，依据 LEN 计算剩余长度，并向上对齐到 8
 * 字节继续传输。后续传输也会捎带排空新进入 TX FIFO 的数据。
 *
 * 只有在帧边界收到 0xff、没有待拼接的半帧且 TX FIFO 为空时，事务才
 * 结束。整个逻辑事务持有 spi_lock，从而串行化调用方和 IRQ 发起的事务。
 * 为避免异常从机永久占用工作线程，单次逻辑事务最多传输 64 KiB。
 *
 * Return: 0 表示双方数据均已排空；负值为错误码。
 */
static int m5io_hub_rpc_transaction(struct m5io_hub_core *mcore) {
  struct spi_transfer transfer = {0};
  unsigned int transaction_len = 0;
  unsigned int rx_needed = 0;
  unsigned int tx_avail;
  unsigned int xfer_len;
  bool first = true;
  bool slave_empty;
  u8 *tx_buf;
  u8 *rx_buf;
  int ret = 0;

  tx_buf = kmalloc(M5IO_HUB_FIFO_SIZE, GFP_KERNEL);
  if (!tx_buf)
    return -ENOMEM;

  rx_buf = kmalloc(M5IO_HUB_FIFO_SIZE, GFP_KERNEL);
  if (!rx_buf) {
    ret = -ENOMEM;
    goto out_free_tx;
  }

  mutex_lock(&mcore->spi_lock);

  for (;;) {
    tx_avail = m5io_hub_tx_fifo_len(mcore);

    if (first) {
      xfer_len = tx_avail ? tx_avail : M5IO_HUB_RPC_XFER_ALIGN;
    } else {
      xfer_len = max(tx_avail, rx_needed);
      if (!xfer_len)
        xfer_len = M5IO_HUB_RPC_XFER_ALIGN;
      xfer_len = ALIGN(xfer_len, M5IO_HUB_RPC_XFER_ALIGN);
    }

    if (xfer_len > M5IO_HUB_FIFO_SIZE ||
        transaction_len > M5IO_HUB_RPC_MAX_TRANSACTION_BYTES - xfer_len) {
      ret = -EOVERFLOW;
      dev_warn_ratelimited(mcore->hub.dev,
                           "rpc transaction exceeded %u bytes\n",
                           M5IO_HUB_RPC_MAX_TRANSACTION_BYTES);
      break;
    }

    memset(tx_buf, M5IO_HUB_RPC_FILL_BYTE, xfer_len);
    if (tx_avail)
      tx_avail = kfifo_out_spinlocked(&mcore->tx_fifo, tx_buf, xfer_len,
                                      &mcore->tx_lock);

    memset(rx_buf, M5IO_HUB_RPC_FILL_BYTE, xfer_len);
    transfer.tx_buf = tx_buf;
    transfer.rx_buf = rx_buf;
    transfer.len = xfer_len;

    ret = spi_sync_transfer(mcore->hub.spi, &transfer, 1);
    if (ret)
      break;

    transaction_len += xfer_len;
    rx_needed =
        m5_rpc_rx_stream_feed(mcore, rx_buf, xfer_len, &slave_empty);

    if (slave_empty && !rx_needed && !m5io_hub_tx_fifo_len(mcore))
      break;

    first = false;
    cond_resched();
  }

  mutex_unlock(&mcore->spi_lock);
  kfree(rx_buf);
out_free_tx:
  kfree(tx_buf);
  return ret;
}

/**
 * m5io_hub_rpc_transfer - 发起一次主机侧 RPC 传输请求
 * @hub: 对外公开的 hub 句柄
 *
 * 只累加请求计数并排队 @mcore->rpc_work，实际的 SPI 传输在 RPC worker
 * 中完成。
 *
 * Context: 可在原子上下文调用。
 *
 * Return: 0 表示请求已排队；-ENODEV 表示 @hub 或 @hub->spi 无效。
 */
int m5io_hub_rpc_transfer(struct m5io_hub *hub) {
  struct m5io_hub_core *mcore;

  if (!hub || !hub->spi)
    return -ENODEV;

  mcore = container_of(hub, struct m5io_hub_core, hub);
  atomic_inc(&mcore->active_reqs);
  queue_work(system_wq, &mcore->rpc_work);
  return 0;
}
EXPORT_SYMBOL_GPL(m5io_hub_rpc_transfer);

/**
 * m5io_hub_rpc_worker - 主机侧 RPC 工作项处理函数
 * @work: 嵌入在 m5io_hub_core.rpc_work 中的工作项
 *
 * 循环消费 active_reqs，每个请求激活一次会持续排空双方数据的 SPI
 * 逻辑事务。IRQ 事务在线程化中断里完成。单次事务失败只记录调试日志，
 * 不影响后续请求。
 *
 * Context: 运行在 system_wq 的进程上下文，可以睡眠。
 */
static void m5io_hub_rpc_worker(struct work_struct *work) {
  struct m5io_hub_core *mcore;
  int ret;

  mcore = container_of(work, struct m5io_hub_core, rpc_work);

  while (atomic_dec_if_positive(&mcore->active_reqs) >= 0) {
    ret = m5io_hub_rpc_transaction(mcore);
    if (ret)
      dev_dbg(mcore->hub.dev, "rpc transaction failed: %d\n", ret);
  }
}

/**
 * m5io_hub_irq_thread - 从设备中断的线程化处理函数
 * @irq: 中断号
 * @data: probe 时注册的 m5io_hub_core 指针
 *
 * 在 IRQF_ONESHOT 屏蔽期间直接激活一次 SPI 事务，持续读取至从机返回
 * 空白且 TX FIFO 为空。线程返回前中断保持屏蔽。
 *
 * Return: IRQ_HANDLED。
 */
static irqreturn_t m5io_hub_irq_thread(int irq, void *data) {
  struct m5io_hub_core *mcore = data;
  int ret;

  ret = m5io_hub_rpc_transaction(mcore);
  if (ret)
    dev_dbg(mcore->hub.dev, "rpc irq transaction failed: %d\n", ret);

  return IRQ_HANDLED;
}

/**
 * m5io_hub_check_pin - 校验 GPIO 编号是否在芯片支持范围内
 * @pin: GPIO 编号
 *
 * Return: 0 表示合法；-EINVAL 表示 @pin 超出 M5IO_HUB_NGPIO。
 */
static int m5io_hub_check_pin(unsigned int pin) {
  if (pin >= M5IO_HUB_NGPIO)
    return -EINVAL;

  return 0;
}

/* -------------------------------------------------------------------------
 * Arduino 风格的用户接口
 * ------------------------------------------------------------------------- */

/**
 * m5_rpc_token_gpio_exec - 发送一帧 GPIO token 并等待应答
 * @hub: hub 句柄
 * @subcmd: 写入 ARG 的 GPIO 子命令
 * @payload: 载荷，可为 NULL
 * @plen: 载荷长度
 * @rx: 可选，拷贝完整应答帧
 * @rx_len: 可选，应答长度输出
 *
 * Return: 0 成功；负值为错误码。
 */
static int m5_rpc_token_gpio_exec(struct m5io_hub *hub, u8 subcmd,
                                  const u8 *payload, u8 plen, u8 *rx,
                                  unsigned int *rx_len) {
  struct m5io_hub_core *mcore;
  struct m5_rpc_token_frame *frame;
  struct mutex *tid_lock;
  u8 tx_buf[M5_RPC_TOKEN_MAX_LEN];
  unsigned int tid;
  unsigned int tx_len;
  int ret;

  if (!hub || !hub->spi)
    return -ENODEV;

  if (plen > M5_RPC_TOKEN_MAX_PLEN)
    return -EINVAL;

  mcore = container_of(hub, struct m5io_hub_core, hub);
  tid_lock = token_tid_alloc(mcore, m5_rpc_token_done_cb, M5_RPC_CMD_GPIO);
  if (!tid_lock)
    return -ENODEV;

  tid = tid_lock - mcore->rpc.token.tid_lock;
  frame = (struct m5_rpc_token_frame *)tx_buf;
  m5_rpc_token_frame_fill(frame, M5_RPC_TYPE_TOKEN, 1, tid, M5_RPC_CMD_GPIO,
                          plen, subcmd, payload);
  m5_rpc_token_frame_crc_fill(frame);

  tx_len = M5_RPC_TOKEN_HDR_LEN + plen + M5_RPC_CRC_LEN;
  ret = m5_rpc_token_frame_put(mcore, tx_buf, tx_len);
  if (ret)
    goto out_free;

  ret = m5_rpc_token_wait(mcore, tid, msecs_to_jiffies(M5_RPC_TOKEN_TIMEOUT_MS));
  if (ret)
    goto out_free;

  ret = m5_rpc_token_frame_crc_check(mcore->rpc.token.rx[tid],
                                     mcore->rpc.token.rx_len[tid]);
  if (ret)
    goto out_free;

  ret = m5_rpc_token_frame_parse(mcore->rpc.token.rx[tid],
                                 mcore->rpc.token.rx_len[tid], NULL);
  if (!ret && rx && rx_len) {
    *rx_len = mcore->rpc.token.rx_len[tid];
    memcpy(rx, mcore->rpc.token.rx[tid], *rx_len);
  }

out_free:
  token_tid_free(mcore, tid_lock);
  return ret;
}

/**
 * m5io_hub_pinMode - 配置引脚方向（对应 Arduino pinMode）
 * @hub: hub 句柄
 * @pin: 引脚号，取值 0 ~ M5IO_HUB_NGPIO-1
 * @mode: 引脚模式，原样写入载荷（如 INPUT / OUTPUT / INPUT_PULLUP 等）
 *
 * Context: 可睡眠的进程上下文。
 *
 * Return: 0 成功；-ENODEV/@pin/@mode 非法为 -EINVAL；超时 -ETIMEDOUT；
 *         应答 CRC 失败 -EBADMSG。
 */
int m5io_hub_pinMode(struct m5io_hub *hub, unsigned int pin, int mode) {
  u8 cmd[2];
  int ret;

  if (!hub || !hub->spi)
    return -ENODEV;

  ret = m5io_hub_check_pin(pin);
  if (ret)
    return ret;

  if (mode < 0 || mode > 0xff)
    return -EINVAL;

  cmd[0] = pin;
  cmd[1] = mode;
  return m5_rpc_token_gpio_exec(hub, M5_RPC_CMD_GPIO_PINMODE, cmd, sizeof(cmd), NULL,
                                NULL);
}
EXPORT_SYMBOL_GPL(m5io_hub_pinMode);

/**
 * m5io_hub_digitalWrite - 设置输出引脚电平（对应 Arduino digitalWrite）
 * @hub: hub 句柄
 * @pin: 引脚号，取值 0 ~ M5IO_HUB_NGPIO-1
 * @value: 非 0 输出高电平，0 输出低电平
 *
 * Context: 可睡眠的进程上下文。
 *
 * Return: 0 成功；负值为错误码。
 */
int m5io_hub_digitalWrite(struct m5io_hub *hub, unsigned int pin, int value) {
  u8 cmd[2];
  int ret;

  if (!hub || !hub->spi)
    return -ENODEV;

  ret = m5io_hub_check_pin(pin);
  if (ret)
    return ret;

  cmd[0] = pin;
  cmd[1] = value ? 1 : 0;
  return m5_rpc_token_gpio_exec(hub, M5_RPC_CMD_GPIO_DIGITALWRITE, cmd, sizeof(cmd),
                                NULL, NULL);
}
EXPORT_SYMBOL_GPL(m5io_hub_digitalWrite);

/**
 * m5io_hub_digitalRead - 读取引脚输入电平（对应 Arduino digitalRead）
 * @hub: hub 句柄
 * @pin: 引脚号，取值 0 ~ M5IO_HUB_NGPIO-1
 *
 * Context: 可睡眠的进程上下文。
 *
 * Return: 成功返回 0 或 1；负值为错误码。
 */
int m5io_hub_digitalRead(struct m5io_hub *hub, unsigned int pin) {
  const struct m5_rpc_token_frame *frame;
  u8 cmd[1];
  u8 rx[M5_RPC_TOKEN_MAX_LEN];
  unsigned int rx_len = 0;
  int ret;

  if (!hub || !hub->spi)
    return -ENODEV;

  ret = m5io_hub_check_pin(pin);
  if (ret)
    return ret;

  cmd[0] = pin;
  ret = m5_rpc_token_gpio_exec(hub, M5_RPC_CMD_GPIO_DIGITALREAD, cmd, sizeof(cmd),
                               rx, &rx_len);
  if (ret)
    return ret;

  frame = (const struct m5_rpc_token_frame *)rx;
  if (frame->len >= 1)
    return !!frame->data[0];

  return 0;
}
EXPORT_SYMBOL_GPL(m5io_hub_digitalRead);

/**
 * m5io_hub_attachInterrupt - 使能引脚边沿中断
 * @hub: hub 句柄
 * @pin: 引脚号，取值 0 ~ M5IO_HUB_NGPIO-1
 * @type: 触发类型，取 IRQ_TYPE_EDGE_RISING / FALLING / BOTH
 *
 * Context: 可睡眠的进程上下文。
 *
 * Return: 0 成功；-EINVAL 表示 @pin 越界或 @type 不受支持；其余为传输错误。
 */
int m5io_hub_attachInterrupt(struct m5io_hub *hub, unsigned int pin,
                             unsigned int type) {
  u8 cmd[2];
  int ret;

  if (!hub || !hub->spi)
    return -ENODEV;

  ret = m5io_hub_check_pin(pin);
  if (ret)
    return ret;

  switch (type) {
  case IRQ_TYPE_EDGE_RISING:
  case IRQ_TYPE_EDGE_FALLING:
  case IRQ_TYPE_EDGE_BOTH:
    break;
  default:
    return -EINVAL;
  }

  cmd[0] = pin;
  cmd[1] = type;
  return m5_rpc_token_gpio_exec(hub, M5_RPC_CMD_GPIO_ATTACHINTERRUPT, cmd,
                                sizeof(cmd), NULL, NULL);
}
EXPORT_SYMBOL_GPL(m5io_hub_attachInterrupt);

/**
 * m5io_hub_detachInterrupt - 关闭引脚中断
 * @hub: hub 句柄
 * @pin: 引脚号，取值 0 ~ M5IO_HUB_NGPIO-1
 *
 * Context: 可睡眠的进程上下文。
 *
 * Return: 0 成功；负值为错误码。
 */
int m5io_hub_detachInterrupt(struct m5io_hub *hub, unsigned int pin) {
  u8 cmd[1];
  int ret;

  if (!hub || !hub->spi)
    return -ENODEV;

  ret = m5io_hub_check_pin(pin);
  if (ret)
    return ret;

  cmd[0] = pin;
  return m5_rpc_token_gpio_exec(hub, M5_RPC_CMD_GPIO_DETACHINTERRUPT, cmd,
                                sizeof(cmd), NULL, NULL);
}
EXPORT_SYMBOL_GPL(m5io_hub_detachInterrupt);

int m5io_hub_register_gpio_irq_handler(
    struct m5io_hub *hub, m5io_hub_gpio_irq_handler_t handler, void *data) {
  struct m5io_hub_core *mcore;
  int ret = 0;

  if (!hub || !hub->spi || !handler)
    return -EINVAL;

  mcore = container_of(hub, struct m5io_hub_core, hub);
  mutex_lock(&mcore->gpio_irq_handler_lock);
  if (mcore->gpio_irq_handler)
    ret = -EBUSY;
  else {
    WRITE_ONCE(mcore->gpio_irq_handler_generation,
               mcore->gpio_irq_handler_generation + 1);
    mcore->gpio_irq_handler_data = data;
    smp_store_release(&mcore->gpio_irq_handler, handler);
  }
  mutex_unlock(&mcore->gpio_irq_handler_lock);

  return ret;
}
EXPORT_SYMBOL_GPL(m5io_hub_register_gpio_irq_handler);

void m5io_hub_unregister_gpio_irq_handler(
    struct m5io_hub *hub, m5io_hub_gpio_irq_handler_t handler, void *data) {
  struct m5io_hub_core *mcore;

  if (!hub)
    return;

  mcore = container_of(hub, struct m5io_hub_core, hub);
  mutex_lock(&mcore->gpio_irq_handler_lock);
  if (mcore->gpio_irq_handler == handler &&
      mcore->gpio_irq_handler_data == data) {
    WRITE_ONCE(mcore->gpio_irq_handler_generation,
               mcore->gpio_irq_handler_generation + 1);
    smp_store_release(&mcore->gpio_irq_handler, NULL);
    mcore->gpio_irq_handler_data = NULL;
  }
  mutex_unlock(&mcore->gpio_irq_handler_lock);
}
EXPORT_SYMBOL_GPL(m5io_hub_unregister_gpio_irq_handler);

static void m5io_hub_cancel_works(void *data) {
  struct m5io_hub_core *mcore = data;

  cancel_work_sync(&mcore->rpc_work);
  cancel_work_sync(&mcore->gpio_irq_work);
}

/* -------------------------------------------------------------------------
 * MFD 子设备定义
 *
 * 与设备树 m5io-hub 节点下的 4 个子节点一一对应。
 * ------------------------------------------------------------------------- */
static const struct mfd_cell m5io_hub_devs[] = {
    {
        .name = "m5io-hub-gpio",
        .of_compatible = "m5stack,m5io-hub-gpio",
    },
    {
        .name = "m5io-hub-uart",
        .of_compatible = "m5stack,m5io-hub-uart",
    },
    {
        .name = "m5io-hub-i2c",
        .of_compatible = "m5stack,m5io-hub-i2c",
    },
    {
        .name = "m5io-hub-spi",
        .of_compatible = "m5stack,m5io-hub-spi",
    },
};

/**
 * m5io_hub_probe - SPI 设备探测
 * @spi: 绑定到 m5io-hub 的 SPI 设备
 *
 * 分配并初始化核心上下文（RPC 槽位、RX/TX FIFO、等待队列），配置 SPI
 * 模式后申请线程化 IRQ，最后按 MFD 框架注册 GPIO / UART / I2C / SPI
 * 四个子设备。资源均通过 devm 接口申请，unbind 时自动释放。
 *
 * Return: 0 表示成功；负值为错误码（内存分配、spi_setup()、IRQ 申请或
 *         MFD 子设备注册失败）。
 */
static int m5io_hub_probe(struct spi_device *spi) {
  struct m5io_hub_core *mcore;
  void *fifo_buf;
  unsigned long irqflags;
  unsigned int irq_type;
  int irq;
  int ret;

  /* 分配驱动的私有核心状态（devm 管理，unbind 时自动释放） */
  mcore = devm_kzalloc(&spi->dev, sizeof(struct m5io_hub_core), GFP_KERNEL);
  if (!mcore)
    return -ENOMEM;

  /* 把对外公开的 hub 句柄绑定到本 SPI 设备 */
  mcore->hub.dev = &spi->dev;
  mcore->hub.spi = spi;
  mutex_init(&mcore->hub.lock);

  /* RPC 工作项：调用方走 worker；IRQ 在线程化中断中激活排空事务 */
  {
    int i;

    INIT_WORK(&mcore->rpc_work, m5io_hub_rpc_worker);
    INIT_WORK(&mcore->gpio_irq_work, m5io_hub_gpio_irq_worker);
    mutex_init(&mcore->spi_lock);
    mutex_init(&mcore->gpio_irq_handler_lock);
    spin_lock_init(&mcore->gpio_irq_pending_lock);
    INIT_KFIFO(mcore->gpio_irq_fifo);
    spin_lock_init(&mcore->rpc.token.state_lock);
    atomic_set(&mcore->active_reqs, 0);
    for (i = 0; i < ARRAY_SIZE(mcore->rpc.data.tid_lock); i++)
      mutex_init(&mcore->rpc.data.tid_lock[i]);
    for (i = 0; i < ARRAY_SIZE(mcore->rpc.token.tid_lock); i++) {
      mutex_init(&mcore->rpc.token.tid_lock[i]);
      init_completion(&mcore->rpc.token.done[i]);
    }
    init_waitqueue_head(&mcore->rpc.data.tid_wq);
    init_waitqueue_head(&mcore->rpc.token.tid_wq);
  }

  /* RX/TX kfifo 及等待队列，用于缓存 RPC 载荷 */
  {
    spin_lock_init(&mcore->rx_lock);
    init_waitqueue_head(&mcore->rx_wq);

    fifo_buf = devm_kmalloc(&spi->dev, M5IO_HUB_FIFO_SIZE, GFP_KERNEL);
    if (!fifo_buf)
      return -ENOMEM;

    ret = kfifo_init(&mcore->rx_fifo, fifo_buf, M5IO_HUB_FIFO_SIZE);
    if (ret)
      return ret;

    spin_lock_init(&mcore->tx_lock);
    init_waitqueue_head(&mcore->tx_wq);

    fifo_buf = devm_kmalloc(&spi->dev, M5IO_HUB_FIFO_SIZE, GFP_KERNEL);
    if (!fifo_buf)
      return -ENOMEM;

    ret = kfifo_init(&mcore->tx_fifo, fifo_buf, M5IO_HUB_FIFO_SIZE);
    if (ret)
      return ret;
  }

  /* SPI 模式 0，8 位字长；时钟频率由设备树给出 */
  spi->mode = SPI_MODE_0;
  spi->bits_per_word = 8;
  ret = spi_setup(spi);
  if (ret)
    return ret;

  spi_set_drvdata(spi, &mcore->hub);

  /* Runs after MFD children and the devm IRQ have been removed. */
  ret = devm_add_action_or_reset(&spi->dev, m5io_hub_cancel_works, mcore);
  if (ret)
    return ret;

  /*
   * DT: interrupt-parent = <&tlmm>; interrupts = <28 IRQ_TYPE_LEVEL_LOW>;
   * 线拉低后进入线程化 IRQ，ONESHOT 屏蔽期间直接调用
   * m5io_hub_rpc_transaction() 排空从机数据。
   */
  irq = spi->irq;
  if (irq <= 0 && spi->dev.of_node)
    irq = of_irq_get(spi->dev.of_node, 0);
  if (irq == -EPROBE_DEFER)
    return irq;
  if (irq < 0)
    irq = 0;

  mcore->hub.irq = irq;
  spi->irq = irq;

  if (irq > 0) {
    irqflags = IRQF_ONESHOT;
    irq_type = irq_get_trigger_type(irq);
    irqflags |= irq_type ? irq_type : IRQF_TRIGGER_LOW;

    ret = devm_request_threaded_irq(&spi->dev, irq, NULL, m5io_hub_irq_thread,
                                    irqflags, "m5io-hub-rpc", mcore);
    if (ret) {
      dev_err(&spi->dev, "failed to request irq %d: %d\n", irq, ret);
      return ret;
    }
  } else {
    dev_warn(&spi->dev, "no irq from DT, rpc irq sampling disabled\n");
  }

  /* 依据设备树注册 GPIO/UART/I2C/SPI 子设备 */
  ret = devm_mfd_add_devices(&spi->dev, PLATFORM_DEVID_AUTO, m5io_hub_devs,
                             ARRAY_SIZE(m5io_hub_devs), NULL, 0, NULL);
  if (ret) {
    dev_err(&spi->dev, "failed to add mfd devices: %d\n", ret);
    return ret;
  }

  dev_info(&spi->dev, "m5io-hub probed, irq=%d\n", mcore->hub.irq);
  return 0;
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
    .id_table = m5io_hub_spi_id,
};
module_spi_driver(m5io_hub_driver);

MODULE_DESCRIPTION("M5Stack M5IO-HUB MFD core driver");
MODULE_LICENSE("GPL");
