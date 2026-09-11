/*
 * M5Stack M5IO-HUB MFD 核心驱动
 *
 * 通过 SPI 与 M5IO-HUB 芯片通信，向上提供寄存器读写、RPC 事务调度
 * （TX/RX FIFO + 工作队列）以及 Arduino 风格的 GPIO 接口，并按 MFD
 * 框架注册 GPIO / UART / I2C / SPI 四个子设备。
 */

#include <linux/bitops.h>
#include <linux/completion.h>
#include <linux/crc-itu-t.h>
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
#define M5_RPC_FRAME_HDR_LEN 2
#define M5_RPC_CRC_LEN 2
#define M5_RPC_TOKEN_HDR_LEN M5_RPC_FRAME_HDR_LEN
#define M5_RPC_TOKEN_MAX_PLEN 0xf
#define M5_RPC_TOKEN_MAX_LEN                                                       \
  (M5_RPC_TOKEN_HDR_LEN + M5_RPC_TOKEN_MAX_PLEN + M5_RPC_CRC_LEN)
#define M5_RPC_TYPE_TOKEN 0
#define M5_RPC_FUNC_PROTOCOL_CONTROL 0x0
#define M5_RPC_FUNC_GPIO 0x1
#define M5_RPC_FUNC_DATA_ACK 0xb
#define M5_RPC_ARG_GPIO_INTERRUPT_EVENT 0x7
#define M5_RPC_ARG_GPIO_PINMODE 0x1
#define M5_RPC_ARG_GPIO_DIGITALWRITE 0x2
#define M5_RPC_ARG_GPIO_DIGITALREAD 0x3
#define M5_RPC_ARG_GPIO_ATTACHINTERRUPT 0x4
#define M5_RPC_ARG_GPIO_DETACHINTERRUPT 0x5
#define M5_RPC_TOKEN_TIMEOUT_MS 2000

#define M5_RPC_TYPE_DATA 1
#define M5_RPC_CHN_UART1 M5IO_HUB_CHN_UART1
#define M5_RPC_CHN_UART2 M5IO_HUB_CHN_UART2
#define M5_RPC_CHN_IIC1 M5IO_HUB_CHN_IIC1
#define M5_RPC_CHN_IIC2 M5IO_HUB_CHN_IIC2
#define M5_RPC_CHN_SPI M5IO_HUB_CHN_SPI
#define M5_RPC_DATA_HDR_LEN M5_RPC_FRAME_HDR_LEN
#define M5_RPC_DATA_MAX_PLEN 0xff
#define M5_RPC_DATA_MAX_LEN                                                        \
  (M5_RPC_DATA_HDR_LEN + M5_RPC_DATA_MAX_PLEN + M5_RPC_CRC_LEN)
#define M5_RPC_MAX_FRAME_LEN M5_RPC_DATA_MAX_LEN
#define M5_RPC_DATA_ACK_TID_MASK 0x03
#define M5_RPC_DATA_ACK_CHN_SHIFT 2
#define M5_RPC_DATA_ACK_RESERVED_MASK BIT(7)

#define M5_RPC_TOKEN_TYPE_MASK BIT(0)
#define M5_RPC_TOKEN_P_MASK BIT(1)
#define M5_RPC_TOKEN_TID_SHIFT 2
#define M5_RPC_TOKEN_TID_MASK 0x3
#define M5_RPC_TOKEN_FUNC_SHIFT 4
#define M5_RPC_TOKEN_FUNC_MASK 0xf
#define M5_RPC_TOKEN_LEN_MASK 0xf
#define M5_RPC_TOKEN_ARG_SHIFT 4
#define M5_RPC_TOKEN_ARG_MASK 0xf

#define M5_RPC_DATA_TID_SHIFT 1
#define M5_RPC_DATA_TID_MASK 0x3
#define M5_RPC_DATA_CHN_SHIFT 3
#define M5_RPC_DATA_CHN_MASK 0x1f

/*
 * Token 协议帧头（16 bit，小端按字节拆分）：
 * header = TYPE | (P << 1) | (TID << 2) | (FUNC << 4)
 *        | (LEN << 8) | (ARG << 12)
 * data[] 为变长载荷，长度由 len 给出。
 */
struct m5_rpc_token_frame {
  u8 header[M5_RPC_TOKEN_HDR_LEN];
  u8 data[];
} __packed;

/* DATA header = TYPE | (TID << 1) | (CHN << 3) | (LEN << 8). */
struct m5_rpc_data_frame {
  u8 header[M5_RPC_DATA_HDR_LEN];
  u8 data[];
} __packed;

static u8 m5_rpc_token_type(const struct m5_rpc_token_frame *frame) {
  return frame->header[0] & M5_RPC_TOKEN_TYPE_MASK;
}

static u8 m5_rpc_token_p(const struct m5_rpc_token_frame *frame) {
  return !!(frame->header[0] & M5_RPC_TOKEN_P_MASK);
}

static u8 m5_rpc_token_tid(const struct m5_rpc_token_frame *frame) {
  return (frame->header[0] >> M5_RPC_TOKEN_TID_SHIFT) &
         M5_RPC_TOKEN_TID_MASK;
}

static u8 m5_rpc_token_func(const struct m5_rpc_token_frame *frame) {
  return (frame->header[0] >> M5_RPC_TOKEN_FUNC_SHIFT) &
         M5_RPC_TOKEN_FUNC_MASK;
}

static u8 m5_rpc_token_len(const struct m5_rpc_token_frame *frame) {
  return frame->header[1] & M5_RPC_TOKEN_LEN_MASK;
}

static u8 m5_rpc_token_arg(const struct m5_rpc_token_frame *frame) {
  return (frame->header[1] >> M5_RPC_TOKEN_ARG_SHIFT) &
         M5_RPC_TOKEN_ARG_MASK;
}

static u8 m5_rpc_data_tid(const struct m5_rpc_data_frame *frame) {
  return (frame->header[0] >> M5_RPC_DATA_TID_SHIFT) & M5_RPC_DATA_TID_MASK;
}

static u8 m5_rpc_data_chn(const struct m5_rpc_data_frame *frame) {
  return (frame->header[0] >> M5_RPC_DATA_CHN_SHIFT) & M5_RPC_DATA_CHN_MASK;
}

static u8 m5_rpc_data_len(const struct m5_rpc_data_frame *frame) {
  return frame->header[1];
}

struct m5io_hub_core;

typedef void (*m5_rpc_cb_t)(struct m5io_hub_core *mcore, int tid, int status);

struct m5_rpc_data {
  spinlock_t tx_window_lock;
  unsigned long tx_windows;
  u8 tx_chn[M5IO_HUB_RPC_TID_NR];
  struct kfifo rx_fifo;
  spinlock_t rx_lock;
  wait_queue_head_t rx_wq;
  bool rx_work_scheduled;
  struct mutex handler_lock;
  m5io_hub_chn_data_handler_t handler[M5IO_HUB_CHN_MAX + 1];
  void *handler_data[M5IO_HUB_CHN_MAX + 1];
};

struct m5_rpc_token {
  struct mutex tid_lock[M5IO_HUB_RPC_TID_NR];
  m5_rpc_cb_t cb[M5IO_HUB_RPC_TID_NR];
  wait_queue_head_t tid_wq;
  spinlock_t state_lock;
  bool waiting[M5IO_HUB_RPC_TID_NR];
  u8 expected_func[M5IO_HUB_RPC_TID_NR];
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
  u8 frame[M5_RPC_MAX_FRAME_LEN];
  unsigned int len;
  unsigned int expect_len;
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

  struct kfifo tx_fifo;
  spinlock_t tx_lock;
  wait_queue_head_t tx_wq;

  struct work_struct data_rx_work;

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
 * @func: 功能索引，4 bit
 * @len: 后续 data 长度，4 bit
 * @arg: 参数，4 bit
 * @data: 载荷指针；为 NULL 或 @len 为 0 时不拷贝
 *
 * 编码关系：
 * header = TYPE | (P << 1) | (TID << 2) | (FUNC << 4)
 *        | (LEN << 8) | (ARG << 12)
 */
static void m5_rpc_token_frame_fill(struct m5_rpc_token_frame *frame, u8 type,
                                    u8 p, u8 tid, u8 func, u8 len, u8 arg,
                                    const u8 *data) {
  u8 payload_len;

  if (!frame)
    return;

  payload_len = len & M5_RPC_TOKEN_LEN_MASK;
  frame->header[0] = (type & 0x1) | ((p & 0x1) << 1) |
                     ((tid & M5_RPC_TOKEN_TID_MASK)
                      << M5_RPC_TOKEN_TID_SHIFT) |
                     ((func & M5_RPC_TOKEN_FUNC_MASK)
                      << M5_RPC_TOKEN_FUNC_SHIFT);
  frame->header[1] = payload_len |
                     ((arg & M5_RPC_TOKEN_ARG_MASK)
                      << M5_RPC_TOKEN_ARG_SHIFT);

  if (data && payload_len)
    memcpy(frame->data, data, payload_len);
}

static void m5_rpc_data_frame_fill(struct m5_rpc_data_frame *frame, u8 tid,
                                   u8 chn, u8 len, const u8 *data) {
  frame->header[0] = M5_RPC_TYPE_DATA |
                     ((tid & M5_RPC_DATA_TID_MASK)
                      << M5_RPC_DATA_TID_SHIFT) |
                     ((chn & M5_RPC_DATA_CHN_MASK)
                      << M5_RPC_DATA_CHN_SHIFT);
  frame->header[1] = len;

  if (data && len)
    memcpy(frame->data, data, len);
}

/**
 * m5_rpc_frame_crc_check - 按协议校验一帧 RPC 数据
 * @buf: 完整 NSS 事务缓冲，线上顺序为 Header | Payload | CRC
 * @nss_len: 实际收到的 NSS 事务长度
 * @payload_len: 从帧头解析出的 Payload 长度
 *
 * 使用 Linux crc_itu_t() 计算 Header + Payload，初值为 0xffff，并与
 * 帧末小端 CRC（低字节在前）比较。
 *
 * Return: 0 表示校验通过；-EINVAL 参数非法；-EBADMSG 长度或 CRC 不匹配。
 */
static int m5_rpc_frame_crc_check(const u8 *buf, unsigned int nss_len,
                                  unsigned int payload_len) {
  unsigned int expect_len;
  u16 crc_calc;
  u16 crc_rx;

  if (!buf)
    return -EINVAL;

  if (nss_len < M5_RPC_FRAME_HDR_LEN + M5_RPC_CRC_LEN)
    return -EINVAL;

  expect_len = M5_RPC_FRAME_HDR_LEN + payload_len + M5_RPC_CRC_LEN;
  if (expect_len != nss_len)
    return -EBADMSG;

  crc_calc = crc_itu_t(0xffff, buf, M5_RPC_FRAME_HDR_LEN + payload_len);
  crc_rx = buf[M5_RPC_FRAME_HDR_LEN + payload_len] |
           ((u16)buf[M5_RPC_FRAME_HDR_LEN + payload_len + 1] << 8);
  if (crc_calc != crc_rx)
    return -EBADMSG;

  return 0;
}

static int m5_rpc_token_frame_crc_check(const u8 *buf, unsigned int nss_len) {
  const struct m5_rpc_token_frame *frame;

  if (!buf || nss_len < M5_RPC_TOKEN_HDR_LEN + M5_RPC_CRC_LEN)
    return -EINVAL;

  frame = (const struct m5_rpc_token_frame *)buf;
  return m5_rpc_frame_crc_check(buf, nss_len, m5_rpc_token_len(frame));
}

static int m5_rpc_data_frame_crc_check(const u8 *buf, unsigned int nss_len) {
  const struct m5_rpc_data_frame *frame;

  if (!buf || nss_len < M5_RPC_DATA_HDR_LEN + M5_RPC_CRC_LEN)
    return -EINVAL;

  frame = (const struct m5_rpc_data_frame *)buf;
  return m5_rpc_frame_crc_check(buf, nss_len, m5_rpc_data_len(frame));
}

/**
 * m5_rpc_token_frame_crc_fill - 计算并写入 token 帧末尾的小端 CRC
 * @frame: 已填好 Header/Payload 的帧，调用方需在 data[len] 后预留 2 字节
 */
static void m5_rpc_token_frame_crc_fill(struct m5_rpc_token_frame *frame) {
  u16 crc;
  unsigned int n;
  u8 len;

  if (!frame)
    return;

  len = m5_rpc_token_len(frame);
  n = M5_RPC_TOKEN_HDR_LEN + len;
  crc = crc_itu_t(0xffff, (const u8 *)frame, n);
  frame->data[len] = crc & 0xff;
  frame->data[len + 1] = crc >> 8;
}

static void m5_rpc_data_frame_crc_fill(struct m5_rpc_data_frame *frame) {
  u16 crc;
  unsigned int n;
  u8 len;

  if (!frame)
    return;

  len = m5_rpc_data_len(frame);
  n = M5_RPC_DATA_HDR_LEN + len;
  crc = crc_itu_t(0xffff, (const u8 *)frame, n);
  frame->data[len] = crc & 0xff;
  frame->data[len + 1] = crc >> 8;
}

/**
 * m5_rpc_frame_put - 把完整帧写入 TX FIFO 并排队一次 RPC 传输
 * @mcore: hub 核心上下文
 * @buf: 完整线上帧（Header + Payload + CRC）
 * @len: @buf 长度
 *
 * Return: 0 已入队；-ENOSPC FIFO 空间不足；其余为 m5io_hub_rpc_transfer() 错误码。
 */
static int m5_rpc_frame_put(struct m5io_hub_core *mcore, const u8 *buf,
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

/* DATA ACK TOKENs are control traffic and do not allocate a TOKEN window. */
static int m5_rpc_data_ack_send(struct m5io_hub_core *mcore, u8 tid, u8 chn) {
  struct m5_rpc_token_frame *frame;
  u8 tx_buf[M5_RPC_TOKEN_HDR_LEN + 1 + M5_RPC_CRC_LEN];
  u8 payload;

  payload = (chn << M5_RPC_DATA_ACK_CHN_SHIFT) |
            (tid & M5_RPC_DATA_ACK_TID_MASK);
  frame = (struct m5_rpc_token_frame *)tx_buf;
  m5_rpc_token_frame_fill(frame, M5_RPC_TYPE_TOKEN, 1, 0,
                          M5_RPC_FUNC_DATA_ACK, 1, 0, &payload);
  m5_rpc_token_frame_crc_fill(frame);

  return m5_rpc_frame_put(mcore, tx_buf, sizeof(tx_buf));
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

static bool m5_rpc_data_window_release(struct m5io_hub_core *mcore, u8 tid,
                                       u8 chn) {
  unsigned long flags;
  bool matched = false;

  spin_lock_irqsave(&mcore->rpc.data.tx_window_lock, flags);
  if (test_bit(tid, &mcore->rpc.data.tx_windows) &&
      mcore->rpc.data.tx_chn[tid] == chn) {
    clear_bit(tid, &mcore->rpc.data.tx_windows);
    mcore->rpc.data.tx_chn[tid] = 0;
    matched = true;
  }
  spin_unlock_irqrestore(&mcore->rpc.data.tx_window_lock, flags);

  return matched;
}

static int m5_rpc_response_status_to_errno(u8 status) {
  switch (status) {
  case 0x0:
    return 0;
  case 0x1:
    return -EINPROGRESS;
  case 0x2:
    return -EOPNOTSUPP;
  case 0x3:
    return -EINVAL;
  case 0x4:
    return -EBUSY;
  case 0x5:
    return -EALREADY;
  case 0x6:
    return -EBADMSG;
  case 0x7:
    return -EIO;
  default:
    return -EPROTO;
  }
}

/**
 * m5_rpc_token_rx_dispatch - 分发同步应答或识别设备主动上报 token
 * @mcore: hub 核心上下文
 * @buf: SPI 读回数据
 * @len: 读回长度
 *
 * GPIO 中断事件与普通 GPIO 应答使用相同 FUNC，因此先按 P/FUNC/ARG/LEN
 * 识别事件并忽略其 TID。其余帧先匹配同步请求，再识别不占 TOKEN 窗口的
 * DATA ACK。
 *
 */
static void m5_rpc_token_rx_dispatch(struct m5io_hub_core *mcore, const u8 *buf,
                                     unsigned int len) {
  const struct m5_rpc_token_frame *frame;
  unsigned long flags;
  unsigned int expect_len;
  m5_rpc_cb_t cb;
  bool matched = false;
  bool pending = false;
  u8 ack_chn;
  u8 ack_tid;
  u8 arg;
  u8 func;
  u8 payload_len;
  int status = 0;
  int tid;

  if (!mcore || !buf || len < M5_RPC_TOKEN_HDR_LEN + M5_RPC_CRC_LEN)
    return;

  frame = (const struct m5_rpc_token_frame *)buf;
  if (m5_rpc_token_type(frame) != M5_RPC_TYPE_TOKEN)
    return;

  tid = m5_rpc_token_tid(frame);
  func = m5_rpc_token_func(frame);
  arg = m5_rpc_token_arg(frame);
  payload_len = m5_rpc_token_len(frame);
  expect_len = M5_RPC_TOKEN_HDR_LEN + payload_len + M5_RPC_CRC_LEN;
  if (expect_len > len || expect_len > M5_RPC_TOKEN_MAX_LEN)
    return;

  if (m5_rpc_token_frame_crc_check(buf, expect_len))
    return;

  if (m5_rpc_token_p(frame) == 1 && func == M5_RPC_FUNC_GPIO &&
      arg == M5_RPC_ARG_GPIO_INTERRUPT_EVENT && payload_len == 1) {
    if (frame->data[0] >= M5IO_HUB_NGPIO) {
      dev_dbg_ratelimited(mcore->hub.dev,
                          "drop gpio interrupt event for invalid pin %u\n",
                          frame->data[0]);
      return;
    }

    dev_dbg(mcore->hub.dev, "gpio interrupt event: pin=%u\n",
            frame->data[0]);
    m5io_hub_gpio_irq_report(mcore, frame->data[0]);
    return;
  }

  cb = NULL;

  spin_lock_irqsave(&mcore->rpc.token.state_lock, flags);
  if (mcore->rpc.token.waiting[tid] &&
      mcore->rpc.token.expected_func[tid] == func) {
    status = func == M5_RPC_FUNC_PROTOCOL_CONTROL
                 ? 0
                 : m5_rpc_response_status_to_errno(arg);
    if (status == -EINPROGRESS) {
      pending = true;
    } else {
      mcore->rpc.token.waiting[tid] = false;
      memcpy(mcore->rpc.token.rx[tid], buf, expect_len);
      mcore->rpc.token.rx_len[tid] = expect_len;
      cb = mcore->rpc.token.cb[tid];
      matched = true;
    }
  }
  spin_unlock_irqrestore(&mcore->rpc.token.state_lock, flags);

  if (pending)
    return;

  if (matched) {
    if (cb)
      cb(mcore, tid, status);
    return;
  }

  if (func == M5_RPC_FUNC_DATA_ACK) {
    if (arg != 0 || payload_len != 1) {
      dev_dbg_ratelimited(mcore->hub.dev,
                          "drop invalid data ack: token_tid=%u arg=%u len=%u\n",
                          tid, arg, payload_len);
      return;
    }

    ack_tid = frame->data[0] & M5_RPC_DATA_ACK_TID_MASK;
    ack_chn = frame->data[0] >> M5_RPC_DATA_ACK_CHN_SHIFT;
    if (frame->data[0] & M5_RPC_DATA_ACK_RESERVED_MASK || !ack_chn ||
        ack_chn > M5IO_HUB_CHN_MAX) {
      dev_dbg_ratelimited(mcore->hub.dev,
                          "drop invalid data ack payload: token_tid=%u data=%#x\n",
                          tid, frame->data[0]);
      return;
    }

    if (!m5_rpc_data_window_release(mcore, ack_tid, ack_chn))
      dev_dbg_ratelimited(mcore->hub.dev,
                          "drop unmatched data ack: data_tid=%u chn=%u\n",
                          ack_tid, ack_chn);
    return;
  }

  dev_dbg_ratelimited(mcore->hub.dev,
                      "drop unmatched token: tid=%u func=%u arg=%u len=%u\n",
                      tid, func, arg, payload_len);
}

static void m5_rpc_data_rx_worker(struct work_struct *work) {
  struct m5io_hub_core *mcore;
  const struct m5_rpc_data_frame *frame;
  m5io_hub_chn_data_handler_t handler;
  u8 frame_buf[M5_RPC_DATA_MAX_LEN];
  u8 header[M5_RPC_DATA_HDR_LEN];
  unsigned long flags;
  unsigned int frame_len;
  unsigned int fifo_len;
  unsigned int copied;
  void *handler_data;
  u8 chn;
  u8 payload_len;

  mcore = container_of(work, struct m5io_hub_core, data_rx_work);

  for (;;) {
    spin_lock_irqsave(&mcore->rpc.data.rx_lock, flags);
    fifo_len = kfifo_len(&mcore->rpc.data.rx_fifo);
    if (!fifo_len) {
      mcore->rpc.data.rx_work_scheduled = false;
      spin_unlock_irqrestore(&mcore->rpc.data.rx_lock, flags);
      break;
    }

    if (fifo_len < M5_RPC_DATA_HDR_LEN ||
        kfifo_out_peek(&mcore->rpc.data.rx_fifo, header,
                       sizeof(header)) != sizeof(header)) {
      kfifo_reset(&mcore->rpc.data.rx_fifo);
      mcore->rpc.data.rx_work_scheduled = false;
      spin_unlock_irqrestore(&mcore->rpc.data.rx_lock, flags);
      dev_warn_ratelimited(mcore->hub.dev,
                           "reset malformed data rx_fifo\n");
      break;
    }

    frame_len = M5_RPC_DATA_HDR_LEN + header[1] + M5_RPC_CRC_LEN;
    if (fifo_len < frame_len) {
      kfifo_reset(&mcore->rpc.data.rx_fifo);
      mcore->rpc.data.rx_work_scheduled = false;
      spin_unlock_irqrestore(&mcore->rpc.data.rx_lock, flags);
      dev_warn_ratelimited(mcore->hub.dev,
                           "reset truncated data rx_fifo frame\n");
      break;
    }

    copied = kfifo_out(&mcore->rpc.data.rx_fifo, frame_buf, frame_len);
    spin_unlock_irqrestore(&mcore->rpc.data.rx_lock, flags);
    if (copied != frame_len)
      continue;

    frame = (const struct m5_rpc_data_frame *)frame_buf;
    chn = m5_rpc_data_chn(frame);
    payload_len = m5_rpc_data_len(frame);
    mutex_lock(&mcore->rpc.data.handler_lock);
    handler = mcore->rpc.data.handler[chn];
    handler_data = mcore->rpc.data.handler_data[chn];
    if (handler)
      handler(handler_data, frame->data, payload_len);
    else
      dev_dbg_ratelimited(mcore->hub.dev,
                          "drop data for unregistered channel %u\n",
                          chn);
    mutex_unlock(&mcore->rpc.data.handler_lock);
  }
}

/**
 * m5_rpc_data_rx_frame_put - 将完整 DATA 帧写入 DATA RX FIFO
 * @mcore: hub 核心上下文
 * @buf: 完整 DATA 帧（Header + Payload + CRC）
 * @len: 协议帧长度
 * @queue_work: 返回是否需要排队 DATA RX 工作项
 *
 * 空间不足时整帧丢弃，避免向 FIFO 留下无法解析的半帧。
 */
static bool m5_rpc_data_rx_frame_put(struct m5io_hub_core *mcore,
                                     const u8 *buf, unsigned int len,
                                     bool *queue_work) {
  unsigned long flags;
  unsigned int copied = 0;

  *queue_work = false;
  spin_lock_irqsave(&mcore->rpc.data.rx_lock, flags);
  if (kfifo_avail(&mcore->rpc.data.rx_fifo) >= len) {
    copied = kfifo_in(&mcore->rpc.data.rx_fifo, buf, len);
    if (copied == len && !mcore->rpc.data.rx_work_scheduled) {
      mcore->rpc.data.rx_work_scheduled = true;
      *queue_work = true;
    }
  }
  spin_unlock_irqrestore(&mcore->rpc.data.rx_lock, flags);

  if (copied != len) {
    dev_warn_ratelimited(mcore->hub.dev,
                         "data rx_fifo overflow: drop %u-byte frame\n", len);
    return false;
  }

  wake_up_interruptible(&mcore->rpc.data.rx_wq);
  return true;
}

/**
 * m5_rpc_rx_stream_feed - 将一次 SPI 读回的数据送入协议流解析器
 * @mcore: hub 核心上下文
 * @buf: SPI 读回数据
 * @len: 读回长度
 * @slave_empty: 返回时为 true 表示在帧边界读到了 0xff 空白
 *
 * TOKEN 的 LEN 是第二字节低 4 bit，DATA 的 LEN 是完整第二字节；两种帧
 * 的完整长度均为 2 + LEN + 2。SPI 传输可能停在帧中间，未完成数据保存
 * 在 rx_stream 中供下一轮继续拼接。帧边界的 0xff 会被作为上行前导空白
 * 跳过，不计入 CRC；若本次剩余字节全为空白，则表示从机已经没有数据。
 * Payload 或 CRC 内的 0xff 仍按普通帧数据处理。
 *
 * Return: 当前半帧还缺少的字节数；没有半帧时返回 0。
 */
static unsigned int m5_rpc_rx_stream_feed(struct m5io_hub_core *mcore,
                                          const u8 *buf, unsigned int len,
                                          bool *slave_empty) {
  struct m5_rpc_rx_stream *stream = &mcore->rx_stream;
  const struct m5_rpc_data_frame *data_frame;
  unsigned int copy_len;
  unsigned int pos = 0;
  bool queue_data_work;
  int ret;
  u8 chn;
  u8 data_tid;
  u8 payload_len;
  u8 type;

  *slave_empty = false;

  while (pos < len) {
    if (!stream->len) {
      while (pos < len && buf[pos] == M5IO_HUB_RPC_FILL_BYTE)
        pos++;
      if (pos == len) {
        *slave_empty = true;
        break;
      }
    }

    if (stream->len < M5_RPC_FRAME_HDR_LEN) {
      copy_len = min_t(unsigned int, M5_RPC_FRAME_HDR_LEN - stream->len,
                       len - pos);
      memcpy(stream->frame + stream->len, buf + pos, copy_len);
      stream->len += copy_len;
      pos += copy_len;

      if (stream->len < M5_RPC_FRAME_HDR_LEN)
        break;

      type = stream->frame[0] & 0x1;
      if (type == M5_RPC_TYPE_TOKEN)
        stream->expect_len = M5_RPC_TOKEN_HDR_LEN +
                             (stream->frame[1] & M5_RPC_TOKEN_MAX_PLEN) +
                             M5_RPC_CRC_LEN;
      else
        stream->expect_len = M5_RPC_DATA_HDR_LEN + stream->frame[1] +
                             M5_RPC_CRC_LEN;
    }

    copy_len = min_t(unsigned int, stream->expect_len - stream->len,
                     len - pos);
    memcpy(stream->frame + stream->len, buf + pos, copy_len);
    stream->len += copy_len;
    pos += copy_len;

    if (stream->len < stream->expect_len)
      break;

    type = stream->frame[0] & M5_RPC_TOKEN_TYPE_MASK;
    if (type == M5_RPC_TYPE_TOKEN) {
      m5_rpc_token_rx_dispatch(mcore, stream->frame, stream->expect_len);
    } else {
      data_frame = (const struct m5_rpc_data_frame *)stream->frame;
      data_tid = m5_rpc_data_tid(data_frame);
      chn = m5_rpc_data_chn(data_frame);
      payload_len = m5_rpc_data_len(data_frame);
      if (m5_rpc_data_frame_crc_check(stream->frame, stream->expect_len)) {
        dev_dbg_ratelimited(mcore->hub.dev,
                            "drop invalid data frame: tid=%u chn=%u len=%u\n",
                            data_tid, chn, payload_len);
      } else if (!chn || chn > M5IO_HUB_CHN_MAX) {
        dev_dbg_ratelimited(mcore->hub.dev,
                            "drop data frame for invalid channel %u\n",
                            chn);
      } else if (m5_rpc_data_rx_frame_put(mcore, stream->frame,
                                           stream->expect_len,
                                           &queue_data_work)) {
        ret = m5_rpc_data_ack_send(mcore, data_tid, chn);
        if (ret)
          dev_warn_ratelimited(mcore->hub.dev,
                               "failed to send data ack: tid=%u chn=%u: %d\n",
                               data_tid, chn, ret);
        if (queue_data_work)
          queue_work(system_unbound_wq, &mcore->data_rx_work);
      }
    }

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

static int m5_rpc_data_window_alloc(struct m5io_hub_core *mcore, u8 chn) {
  unsigned long flags;
  unsigned int tid;

  spin_lock_irqsave(&mcore->rpc.data.tx_window_lock, flags);
  for (tid = 0; tid < M5IO_HUB_RPC_TID_NR; tid++) {
    if (test_bit(tid, &mcore->rpc.data.tx_windows))
      continue;

    set_bit(tid, &mcore->rpc.data.tx_windows);
    mcore->rpc.data.tx_chn[tid] = chn;
    spin_unlock_irqrestore(&mcore->rpc.data.tx_window_lock, flags);
    return tid;
  }
  spin_unlock_irqrestore(&mcore->rpc.data.tx_window_lock, flags);

  return -EAGAIN;
}

/**
 * token_tid_alloc - 从 token 池占用一个 TID 槽位并绑定回调
 * @mcore: hub 核心上下文
 * @cb: 本次事务完成时使用的回调，可为 NULL
 * @expected_func: 本次同步请求期望应答的 FUNC
 *
 * 无空闲槽位时在 token 池的等待队列上等待。
 *
 * Context: 可睡眠的进程上下文。
 *
 * Return: 成功返回已加锁的 token tid_lock 指针；@mcore 为 NULL 时返回
 *         NULL。
 */
static struct mutex *token_tid_alloc(struct m5io_hub_core *mcore,
                                     m5_rpc_cb_t cb, u8 expected_func) {
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
  mcore->rpc.token.expected_func[tid] = expected_func;
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
  mcore->rpc.token.expected_func[tid] = 0;
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
 * 只有在跳过帧边界的上行 0xff 后没有读到新帧、没有待拼接的半帧且 TX
 * FIFO 为空时，事务才结束。整个逻辑事务持有 spi_lock，从而串行化调用
 * 方和 IRQ 发起的事务。
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
 * m5io_hub_SendChnData - 非阻塞发送一个 DATA 帧
 * @hub: hub 句柄
 * @chn: DATA 通道，取值 M5IO_HUB_CHN_UART1 到 M5IO_HUB_CHN_SPI
 * @data: 待发送的 Payload，@len 为 0 时可为 NULL
 * @len: Payload 长度，最大 255 字节
 *
 * DATA 发送池有 4 个全局共享的 TID 窗口。成功入队后立即返回，设备用
 * FUNC=11、ARG=0、LEN=1 的 TOKEN 确认接收，其 data[0] bit[1:0] 为
 * DATA TID、bit[6:2] 为 CHN；TOKEN 头部 TID 不参与匹配。DATA TID 和
 * CHN 均匹配时异步释放窗口。
 *
 * Context: 可在原子上下文调用。
 *
 * Return: 0 已入队；无空闲窗口返回 -EAGAIN；参数非法返回 -EINVAL；
 *         无设备返回 -ENODEV；发送队列空间不足返回 -ENOSPC。
 */
int m5io_hub_SendChnData(struct m5io_hub *hub, unsigned int chn,
                         const u8 *data, unsigned int len) {
  struct m5io_hub_core *mcore;
  struct m5_rpc_data_frame *frame;
  u8 tx_buf[M5_RPC_DATA_MAX_LEN];
  unsigned int tx_len;
  int tid;
  int ret;

  if (!hub || !hub->spi)
    return -ENODEV;

  if (!chn || chn > M5IO_HUB_CHN_MAX || len > M5_RPC_DATA_MAX_PLEN ||
      (len && !data))
    return -EINVAL;

  mcore = container_of(hub, struct m5io_hub_core, hub);

  tid = m5_rpc_data_window_alloc(mcore, chn);
  if (tid < 0)
    return tid;

  frame = (struct m5_rpc_data_frame *)tx_buf;
  m5_rpc_data_frame_fill(frame, tid, chn, len, data);
  m5_rpc_data_frame_crc_fill(frame);

  tx_len = M5_RPC_DATA_HDR_LEN + len + M5_RPC_CRC_LEN;
  ret = m5_rpc_frame_put(mcore, tx_buf, tx_len);
  if (ret)
    m5_rpc_data_window_release(mcore, tid, chn);
  return ret;
}
EXPORT_SYMBOL_GPL(m5io_hub_SendChnData);

/**
 * m5io_hub_register_chn_data_handler - 注册通道 DATA 接收处理函数
 * @hub: hub 句柄
 * @chn: DATA 通道
 * @handler: 裸 Payload 接收处理函数
 * @data: 传给 @handler 的私有数据
 *
 * 每个通道只允许一个处理函数。处理函数在可睡眠的工作队列上下文调用，
 * @buf 只在本次回调期间有效。
 */
int m5io_hub_register_chn_data_handler(
    struct m5io_hub *hub, unsigned int chn,
    m5io_hub_chn_data_handler_t handler, void *data) {
  struct m5io_hub_core *mcore;
  int ret = 0;

  if (!hub || !hub->spi || !handler || !chn || chn > M5IO_HUB_CHN_MAX)
    return -EINVAL;

  mcore = container_of(hub, struct m5io_hub_core, hub);
  mutex_lock(&mcore->rpc.data.handler_lock);
  if (mcore->rpc.data.handler[chn]) {
    ret = -EBUSY;
  } else {
    mcore->rpc.data.handler_data[chn] = data;
    mcore->rpc.data.handler[chn] = handler;
  }
  mutex_unlock(&mcore->rpc.data.handler_lock);

  return ret;
}
EXPORT_SYMBOL_GPL(m5io_hub_register_chn_data_handler);

/**
 * m5io_hub_unregister_chn_data_handler - 注销通道 DATA 接收处理函数
 * @hub: hub 句柄
 * @chn: DATA 通道
 * @handler: 注册时使用的处理函数
 * @data: 注册时使用的私有数据
 *
 * 返回时已经没有该处理函数正在执行。
 */
void m5io_hub_unregister_chn_data_handler(
    struct m5io_hub *hub, unsigned int chn,
    m5io_hub_chn_data_handler_t handler, void *data) {
  struct m5io_hub_core *mcore;

  if (!hub || !chn || chn > M5IO_HUB_CHN_MAX)
    return;

  mcore = container_of(hub, struct m5io_hub_core, hub);
  mutex_lock(&mcore->rpc.data.handler_lock);
  if (mcore->rpc.data.handler[chn] == handler &&
      mcore->rpc.data.handler_data[chn] == data) {
    mcore->rpc.data.handler[chn] = NULL;
    mcore->rpc.data.handler_data[chn] = NULL;
  }
  mutex_unlock(&mcore->rpc.data.handler_lock);
}
EXPORT_SYMBOL_GPL(m5io_hub_unregister_chn_data_handler);

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
  tid_lock = token_tid_alloc(mcore, m5_rpc_token_done_cb, M5_RPC_FUNC_GPIO);
  if (!tid_lock)
    return -ENODEV;

  tid = tid_lock - mcore->rpc.token.tid_lock;
  frame = (struct m5_rpc_token_frame *)tx_buf;
  m5_rpc_token_frame_fill(frame, M5_RPC_TYPE_TOKEN, 1, tid, M5_RPC_FUNC_GPIO,
                          plen, subcmd, payload);
  m5_rpc_token_frame_crc_fill(frame);

  tx_len = M5_RPC_TOKEN_HDR_LEN + plen + M5_RPC_CRC_LEN;
  ret = m5_rpc_frame_put(mcore, tx_buf, tx_len);
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
  return m5_rpc_token_gpio_exec(hub, M5_RPC_ARG_GPIO_PINMODE, cmd, sizeof(cmd), NULL,
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
  return m5_rpc_token_gpio_exec(hub, M5_RPC_ARG_GPIO_DIGITALWRITE, cmd, sizeof(cmd),
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
  ret = m5_rpc_token_gpio_exec(hub, M5_RPC_ARG_GPIO_DIGITALREAD, cmd, sizeof(cmd),
                               rx, &rx_len);
  if (ret)
    return ret;

  frame = (const struct m5_rpc_token_frame *)rx;
  if (m5_rpc_token_len(frame) >= 1)
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
  return m5_rpc_token_gpio_exec(hub, M5_RPC_ARG_GPIO_ATTACHINTERRUPT, cmd,
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
  return m5_rpc_token_gpio_exec(hub, M5_RPC_ARG_GPIO_DETACHINTERRUPT, cmd,
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
  cancel_work_sync(&mcore->data_rx_work);
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
    INIT_WORK(&mcore->data_rx_work, m5_rpc_data_rx_worker);
    INIT_WORK(&mcore->gpio_irq_work, m5io_hub_gpio_irq_worker);
    mutex_init(&mcore->spi_lock);
    mutex_init(&mcore->rpc.data.handler_lock);
    mutex_init(&mcore->gpio_irq_handler_lock);
    spin_lock_init(&mcore->gpio_irq_pending_lock);
    INIT_KFIFO(mcore->gpio_irq_fifo);
    spin_lock_init(&mcore->rpc.data.tx_window_lock);
    spin_lock_init(&mcore->rpc.token.state_lock);
    atomic_set(&mcore->active_reqs, 0);
    for (i = 0; i < ARRAY_SIZE(mcore->rpc.token.tid_lock); i++) {
      mutex_init(&mcore->rpc.token.tid_lock[i]);
      init_completion(&mcore->rpc.token.done[i]);
    }
    init_waitqueue_head(&mcore->rpc.token.tid_wq);
  }

  /* DATA RX/TX kfifo 及等待队列，用于缓存 RPC 帧 */
  {
    spin_lock_init(&mcore->rpc.data.rx_lock);
    init_waitqueue_head(&mcore->rpc.data.rx_wq);

    fifo_buf = devm_kmalloc(&spi->dev, M5IO_HUB_FIFO_SIZE, GFP_KERNEL);
    if (!fifo_buf)
      return -ENOMEM;

    ret = kfifo_init(&mcore->rpc.data.rx_fifo, fifo_buf,
                     M5IO_HUB_FIFO_SIZE);
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
