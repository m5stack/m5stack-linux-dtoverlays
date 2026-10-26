/*
 * M5Stack M5IO-HUB 核心驱动
 *
 * 通过 SPI 与 M5IO-HUB 芯片通信，向上提供寄存器读写、RPC 事务调度
 * （TX/RX FIFO + 工作队列）以及 Arduino 风格的 GPIO 接口，并按设备树
 * 注册 GPIO / UART / I2C / SPI 子设备。
 */

#include <linux/bitops.h>
#include <linux/completion.h>
#include <linux/crc-itu-t.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/jiffies.h>
#include <linux/kfifo.h>
#include "m5io-hub.h"
#include "m5io-hub-uapi.h"
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/of_platform.h>
#include <linux/proc_fs.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/spi/spi.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/uaccess.h>
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
#define M5_RPC_FUNC_DATA_ACK M5_RPC_FUNC_PROTOCOL_CONTROL
#define M5_RPC_ARG_DATA_ACK 0x4
#define M5_RPC_ARG_DATA_STATUS 0x5
#define M5_RPC_ARG_GPIO_INTERRUPT_EVENT 0x7
#define M5_RPC_ARG_GPIO_PINMODE 0x1
#define M5_RPC_ARG_GPIO_DIGITALWRITE 0x2
#define M5_RPC_ARG_GPIO_DIGITALREAD 0x3
#define M5_RPC_ARG_GPIO_ATTACHINTERRUPT 0x4
#define M5_RPC_ARG_GPIO_DETACHINTERRUPT 0x5
#define M5_RPC_GPIO_EDGE_FALLING 0x0
#define M5_RPC_GPIO_EDGE_RISING  0x1
#define M5_RPC_GPIO_EDGE_BOTH    0x2
#define M5_RPC_TOKEN_TIMEOUT_MS 2000
#define M5_RPC_UART_TOKEN_TIMEOUT_MS 30000

#define M5_RPC_TYPE_DATA 1
#define M5_RPC_CHN_UART1 M5IO_HUB_CHN_UART1
#define M5_RPC_CHN_UART2 M5IO_HUB_CHN_UART2
#define M5_RPC_CHN_IIC1 M5IO_HUB_CHN_IIC1
#define M5_RPC_CHN_IIC2 M5IO_HUB_CHN_IIC2
#define M5_RPC_CHN_SPI M5IO_HUB_CHN_SPI
#define M5_RPC_CHN_LOG M5IO_HUB_CHN_LOG
/* MCU log ring exposed through /proc; large enough for a boot-time burst. */
#define M5IO_HUB_LOG_BUF_SIZE 8192
#define M5IO_HUB_LOG_PROC_NAME "m5io-hub-log"
#define M5_RPC_DATA_HDR_LEN 3
#define M5_RPC_DATA_MAX_PLEN 0xff
#define M5_RPC_DATA_MAX_LEN 260
#define M5IO_HUB_SPI_TX_BUF_LEN                                                    \
  (M5_RPC_TOKEN_MAX_LEN + M5IO_HUB_SPI_MAX_LEN +                                  \
   DIV_ROUND_UP(M5IO_HUB_SPI_MAX_LEN, M5IO_HUB_SPI_CHUNK_LEN) *                    \
   (M5_RPC_DATA_HDR_LEN + M5_RPC_CRC_LEN))
#define M5_RPC_UART_MAX_TRANSACTION_LEN 1024U
#define M5_RPC_UART_DATA_FRAME_COUNT \
  DIV_ROUND_UP(M5_RPC_UART_MAX_TRANSACTION_LEN, M5_RPC_DATA_MAX_PLEN)
#define M5_RPC_EXEC_TX_BUF_LEN \
  (M5_RPC_TOKEN_MAX_LEN + M5_RPC_UART_DATA_FRAME_COUNT * \
   (M5_RPC_DATA_HDR_LEN + M5_RPC_DATA_MAX_PLEN + M5_RPC_CRC_LEN))
#define M5_RPC_MAX_FRAME_LEN \
  (M5_RPC_DATA_HDR_LEN + M5_RPC_DATA_MAX_PLEN + M5_RPC_CRC_LEN)
#define M5_RPC_DATA_ACK_PID_MASK 0x03
#define M5_RPC_WIRE_REVISION 9
#define M5_RPC_DATA_ACK_CHN_MAX M5IO_HUB_CHN_MAX
#define M5_RPC_DATA_ACK_RETRY_FIFO_SIZE 128
#define M5_RPC_DATA_ACK_RETRY_PENDING_BITS \
  (M5IO_HUB_RPC_TID_NR * (M5IO_HUB_CHN_MAX + 1))
#define M5_RPC_DATA_ACK_RETRY_DELAY_MS 20
#define M5_RPC_TX_OP_NR 16

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
 * acknowledgement out, so the master has to keep the bus moving for the reset
 * to land.  Dummy bytes do that without upsetting the APP's frame parser, and
 * they are also the way to tell whether the bootloader is up: a bootloader that
 * is listening rejects them with BAD_COMMAND, whereas the APP simply ignores
 * them.  The entry loop therefore alternates dummy bytes and a read until the
 * bootloader answers, and only then sends OPC_IAP.  The bootloader listens for
 * 300 ms from the reset, so the loop also keeps the window under observation.
 */
#define M5IO_HUB_IAP_POKE_DELAY_US 2000
#define M5IO_HUB_IAP_ENTRY_REPROBE_DELAY_US 6000
#define M5IO_HUB_IAP_ENTRY_PROBE_TIMEOUT_MS 10
#define M5IO_HUB_IAP_ENTRY_ATTEMPTS 24
/* Refresh a pending bootloader reply occasionally rather than every attempt,
 * so the retry loop spends its time inside the entry window. */
#define M5IO_HUB_IAP_ENTRY_DRAIN_EVERY 4
#define M5IO_HUB_IAP_POLL_DELAY_US 1000

#define M5_RPC_TOKEN_TYPE_MASK BIT(0)
#define M5_RPC_TOKEN_P_MASK BIT(1)
#define M5_RPC_TOKEN_TID_SHIFT 2
#define M5_RPC_TOKEN_TID_MASK 0x3
#define M5_RPC_TOKEN_FUNC_SHIFT 4
#define M5_RPC_TOKEN_FUNC_MASK 0xf
#define M5_RPC_TOKEN_LEN_MASK 0xf
#define M5_RPC_TOKEN_ARG_SHIFT 4
#define M5_RPC_TOKEN_ARG_MASK 0xf

#define M5_RPC_DATA_PID_SHIFT 1
#define M5_RPC_DATA_PID_MASK 0x3
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

/* DATA header = TYPE | (PID << 1) | (CHN << 3), followed by SEQ and LEN. */
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

static u8 m5_rpc_data_pid(const struct m5_rpc_data_frame *frame) {
  return (frame->header[0] >> M5_RPC_DATA_PID_SHIFT) & M5_RPC_DATA_PID_MASK;
}
static u8 m5_rpc_data_chn(const struct m5_rpc_data_frame *frame) {
  return (frame->header[0] >> M5_RPC_DATA_CHN_SHIFT) & M5_RPC_DATA_CHN_MASK;
}
static u8 m5_rpc_data_seq(const struct m5_rpc_data_frame *frame) {
  return frame->header[1];
}
static u8 m5_rpc_data_len(const struct m5_rpc_data_frame *frame) {
  return frame->header[2];
}

struct m5io_hub_core;


struct m5_rpc_data_ack {
  u8 chn;
  u8 pid;
  u8 seq;
  u8 status;
};

struct m5_rpc_data {
  spinlock_t tx_window_lock;
  unsigned long tx_windows;
  u8 tx_chn[M5IO_HUB_RPC_TID_NR];
  u8 tx_seq[M5IO_HUB_RPC_TID_NR];
  int channel_error[M5IO_HUB_CHN_MAX + 1];
  u8 next_seq;
  struct {
    bool valid;
    u8 seq;
  } tx_history[M5IO_HUB_RPC_TID_NR];
  struct {
    bool valid;
    bool held;
    u8 chn;
    u8 seq;
    u16 crc;
  } rx_dedup[M5IO_HUB_RPC_TID_NR];
  spinlock_t ack_retry_lock;
  DECLARE_KFIFO(ack_retry_fifo, struct m5_rpc_data_ack,
                M5_RPC_DATA_ACK_RETRY_FIFO_SIZE);
  DECLARE_BITMAP(ack_retry_pending, M5_RPC_DATA_ACK_RETRY_PENDING_BITS);
  /* WINDOW_ACK is control traffic.  Keep it out of the ordinary DATA FIFO so
   * a burst of downlink I2C/SPI frames cannot delay the credit return that
   * releases the MCU's uplink window. */
  spinlock_t ack_tx_lock;
  DECLARE_KFIFO(ack_tx_fifo, struct m5_rpc_data_ack,
                M5_RPC_DATA_ACK_RETRY_FIFO_SIZE);
  struct kfifo rx_fifo;
  spinlock_t rx_lock;
  wait_queue_head_t rx_wq;
  bool rx_work_scheduled;
  struct mutex handler_lock;
  /* Protect callback lifetime while handler_lock is not held in callbacks. */
  wait_queue_head_t handler_wq[M5IO_HUB_CHN_MAX + 1];
  atomic_t handler_inflight[M5IO_HUB_CHN_MAX + 1];
  m5io_hub_chn_data_handler_t handler[M5IO_HUB_CHN_MAX + 1];
  void *handler_data[M5IO_HUB_CHN_MAX + 1];
};

enum m5_rpc_token_state {
  M5_TOKEN_FREE, M5_TOKEN_WAIT, M5_TOKEN_DONE, M5_TOKEN_QUARANTINED,
};

struct m5_rpc_token_slot {
  enum m5_rpc_token_state state;
  bool caller, queued, in_flight, sent, final_received;
  u32 generation;
  unsigned long deadline, send_deadline;
  struct completion done;
  int status;
  u8 tx[M5_RPC_TOKEN_MAX_LEN], rx[M5_RPC_TOKEN_MAX_LEN];
  u8 tx_len, rx_len;
};

struct m5_rpc_token {
  spinlock_t state_lock;
  struct m5_rpc_token_slot slot[M5IO_HUB_RPC_TID_NR];
  unsigned int next_tx;
  unsigned long late_responses;
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

/* DATA frames identify their I2C channel, so each bus owns its state. */
struct m5_rpc_i2c_channel {
  struct mutex lock;
  int tid; /* legacy control-TID context for CONFIG/CLOSE only */
  u8 pid, seq;
  u8 chn;
  bool data_waiting;
  unsigned int expected_len;
  unsigned int rx_len;
  u8 rx[M5_RPC_DATA_MAX_PLEN];
  int error;
  wait_queue_head_t wq;
};

struct m5_rpc_i2c {
  struct m5_rpc_i2c_channel chan[2];
};

/* FUNC3 has one transaction context; DATA PID is a sequence, not a TID. */
struct m5_rpc_spi {
  struct mutex lock;
  /* Fixed maximum FUNC3 TOKEN+DATA stream, allocated during probe. */
  u8 *tx_buf;
  bool data_waiting; /* independent of the four TOKEN TIDs */
  unsigned int expected_len;
  unsigned int rx_len;
  u8 next_pid;
  u8 rx[M5IO_HUB_SPI_MAX_LEN];
  int error;
  wait_queue_head_t wq;
};

struct m5io_hub_core {
  struct m5io_hub hub;

  struct m5_rpc rpc;
  struct m5_rpc_i2c i2c;
  struct m5_rpc_spi spi_rpc;

  /* One fixed-size transaction buffer; protected while building/enqueuing. */
  /* SPI transfer scratch buffers, allocated during probe and spi_lock-held. */
  u8 *rpc_xfer_tx_buf;
  u8 *rpc_xfer_rx_buf;

  struct work_struct rpc_work;
  spinlock_t rpc_schedule_lock;
  bool rpc_work_kick;
  bool rpc_work_running;
  struct delayed_work data_ack_retry_work;
  bool data_ack_retry_work_scheduled;
  struct mutex spi_lock;
  /* TOKEN has four firmware TIDs.  Synchronous callers must reserve one
   * transaction at a time so a burst of CONFIG/RESET requests cannot fill
   * all slots and make the recovery command itself return BUSY. */
  struct mutex exec_lock;
  /* Serializes userspace OTA with all normal RPC users. */
  struct mutex ota_lock;
  /*
   * Owner of the m5hub data line, NULL when the line is free.  An upgrade
   * holds it from its first command until it has finished, so no other
   * context can put an RPC frame on the bus while the firmware is being
   * replaced.  Written under tx_lock; read locklessly as a fast-path hint.
   */
  struct task_struct *bus_owner;
  struct completion ota_ready;
  bool ota_active;
  struct miscdevice ota_misc;
  char ota_misc_name[32];
  struct m5_rpc_rx_stream rx_stream;

  /* MCU -> SoC diagnostic text from CH=M5IO_HUB_CHN_LOG.  A mutex, not a
   * spinlock: the only writers are the RPC worker and the threaded IRQ, and
   * the reader copies to userspace while holding it. */
  struct mutex log_lock;
  struct proc_dir_entry *log_proc;
  unsigned int log_len;
  unsigned int log_dropped;
  u8 log_buf[M5IO_HUB_LOG_BUF_SIZE];

  struct kfifo tx_fifo;
  /* Synchronous I2C/SPI DATA must not be blocked by best-effort UART DATA. */
  struct kfifo sync_tx_fifo;
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

/* RPC wakeups are level-triggered: one queued worker drains all currently
 * visible TX/RX state.  Coalesce IRQ and synchronous polling wakeups so a
 * missed HOST_READY edge cannot build an unbounded work counter. */
static void m5io_hub_rpc_kick(struct m5io_hub_core *mcore);

static void m5io_hub_log_rx(struct m5io_hub_core *mcore, const u8 *buf,
                            unsigned int len);

static void m5_rpc_rx_stream_reset(struct m5io_hub_core *mcore) {
  mcore->rx_stream.len = 0;
  mcore->rx_stream.expect_len = 0;
}

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

static void m5_rpc_data_frame_fill(struct m5_rpc_data_frame *frame, u8 pid,
                                   u8 chn, u8 seq, u8 len, const u8 *data) {
  frame->header[0] = M5_RPC_TYPE_DATA |
                     ((pid & M5_RPC_DATA_PID_MASK) << M5_RPC_DATA_PID_SHIFT) |
                     ((chn & M5_RPC_DATA_CHN_MASK) << M5_RPC_DATA_CHN_SHIFT);
  frame->header[1] = seq;
  frame->header[2] = len;
  if (data && len)
    memcpy(frame->data, data, len);
}

static int m5_rpc_frame_crc_check(const u8 *buf, unsigned int nss_len,
                                  unsigned int header_len,
                                  unsigned int payload_len) {
  unsigned int expect_len;
  u16 crc_calc;
  u16 crc_rx;

  if (!buf)
    return -EINVAL;

  if (nss_len < header_len + M5_RPC_CRC_LEN)
    return -EINVAL;

  expect_len = header_len + payload_len + M5_RPC_CRC_LEN;
  if (expect_len != nss_len)
    return -EBADMSG;

  crc_calc = crc_itu_t(0xffff, buf, header_len + payload_len);
  crc_rx = buf[header_len + payload_len] |
           ((u16)buf[header_len + payload_len + 1] << 8);
  if (crc_calc != crc_rx)
    return -EBADMSG;

  return 0;
}

static int m5_rpc_token_frame_crc_check(const u8 *buf, unsigned int nss_len) {
  const struct m5_rpc_token_frame *frame;

  if (!buf || nss_len < M5_RPC_TOKEN_HDR_LEN + M5_RPC_CRC_LEN)
    return -EINVAL;

  frame = (const struct m5_rpc_token_frame *)buf;
  return m5_rpc_frame_crc_check(buf, nss_len, M5_RPC_TOKEN_HDR_LEN,
                                m5_rpc_token_len(frame));
}

static int m5_rpc_data_frame_crc_check(const u8 *buf, unsigned int nss_len) {
  const struct m5_rpc_data_frame *frame;

  if (!buf || nss_len < M5_RPC_DATA_HDR_LEN + M5_RPC_CRC_LEN)
    return -EINVAL;

  frame = (const struct m5_rpc_data_frame *)buf;
  return m5_rpc_frame_crc_check(buf, nss_len, M5_RPC_DATA_HDR_LEN,
                                m5_rpc_data_len(frame));
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
 * @buf: 一帧或多帧完整线上帧（Header + Payload + CRC），原子入队
 * @len: @buf 长度
 *
 * Return: 0 已入队；-ENOSPC FIFO 空间不足；其余为 m5io_hub_rpc_transfer() 错误码。
 */
/*
 * True when another context owns the m5hub data line, i.e. an upgrade is in
 * progress.  The owning thread itself is let through, so an upgrade can still
 * issue the APP reset request before it switches to the raw IAP command
 * stream.
 */
static bool m5io_hub_bus_held_by_other(const struct m5io_hub_core *mcore)
{
  struct task_struct *owner = READ_ONCE(mcore->bus_owner);

  return owner && owner != current;
}

void m5io_hub_data_abort_channel(struct m5io_hub *hub, unsigned int chn)
{
  struct m5io_hub_core *mcore;
  unsigned long flags;
  unsigned int pid;
  unsigned int kept_count = 0;
  struct m5_rpc_data_ack ack;
  struct m5_rpc_data_ack kept[M5_RPC_DATA_ACK_RETRY_FIFO_SIZE];

  if (!hub || !hub->spi || !chn || chn > M5IO_HUB_CHN_MAX)
    return;
  mcore = container_of(hub, struct m5io_hub_core, hub);

  spin_lock_irqsave(&mcore->rpc.data.tx_window_lock, flags);
  for (pid = 0; pid < M5IO_HUB_RPC_TID_NR; pid++) {
    if (test_bit(pid, &mcore->rpc.data.tx_windows) &&
        mcore->rpc.data.tx_chn[pid] == chn) {
      clear_bit(pid, &mcore->rpc.data.tx_windows);
      mcore->rpc.data.tx_chn[pid] = 0;
      mcore->rpc.data.tx_seq[pid] = 0;
      mcore->rpc.data.tx_history[pid].valid = false;
      mcore->rpc.data.tx_history[pid].seq = 0;
    }
  }
  WRITE_ONCE(mcore->rpc.data.channel_error[chn], 0);
  spin_unlock_irqrestore(&mcore->rpc.data.tx_window_lock, flags);

  /* Drop stale ACK retries for this channel while preserving other channels. */
  cancel_delayed_work_sync(&mcore->data_ack_retry_work);
  spin_lock_irqsave(&mcore->rpc.data.ack_retry_lock, flags);
  while (kfifo_out(&mcore->rpc.data.ack_retry_fifo, &ack, 1)) {
    if (ack.chn != chn && kept_count < ARRAY_SIZE(kept))
      kept[kept_count++] = ack;
  }
  kfifo_reset(&mcore->rpc.data.ack_retry_fifo);
  bitmap_zero(mcore->rpc.data.ack_retry_pending,
              M5_RPC_DATA_ACK_RETRY_PENDING_BITS);
  for (pid = 0; pid < kept_count; pid++) {
    if (kfifo_put(&mcore->rpc.data.ack_retry_fifo, kept[pid]))
      set_bit(kept[pid].chn * M5IO_HUB_RPC_TID_NR + kept[pid].pid,
              mcore->rpc.data.ack_retry_pending);
  }
  mcore->data_ack_retry_work_scheduled = false;
  if (kept_count) {
    mcore->data_ack_retry_work_scheduled = true;
    queue_delayed_work(system_wq, &mcore->data_ack_retry_work, 0);
  }
  spin_unlock_irqrestore(&mcore->rpc.data.ack_retry_lock, flags);

  /* Drop already-built ACK frames for this channel as well.  A reset/flush
   * invalidates the MCU generation, so delivering one later could ACK a
   * reused PID from the new exchange. */
  kept_count = 0;
  spin_lock_irqsave(&mcore->rpc.data.ack_tx_lock, flags);
  while (kfifo_out(&mcore->rpc.data.ack_tx_fifo, &ack, 1)) {
    if (ack.chn != chn && kept_count < ARRAY_SIZE(kept))
      kept[kept_count++] = ack;
  }
  kfifo_reset(&mcore->rpc.data.ack_tx_fifo);
  for (pid = 0; pid < kept_count; pid++)
    kfifo_put(&mcore->rpc.data.ack_tx_fifo, kept[pid]);
  spin_unlock_irqrestore(&mcore->rpc.data.ack_tx_lock, flags);

  /* Release de-duplication records so a post-FLUSH generation can reuse a
   * PID without being mistaken for the timed-out frame. */
  spin_lock_irqsave(&mcore->rpc.data.rx_lock, flags);
  for (pid = 0; pid < M5IO_HUB_RPC_TID_NR; pid++) {
    if (mcore->rpc.data.rx_dedup[pid].valid &&
        mcore->rpc.data.rx_dedup[pid].chn == chn)
      memset(&mcore->rpc.data.rx_dedup[pid], 0,
             sizeof(mcore->rpc.data.rx_dedup[pid]));
  }
  spin_unlock_irqrestore(&mcore->rpc.data.rx_lock, flags);
  wake_up(&mcore->rpc.data.rx_wq);
  wake_up(&mcore->i2c.chan[0].wq);
  wake_up(&mcore->i2c.chan[1].wq);
  wake_up(&mcore->spi_rpc.wq);
}
EXPORT_SYMBOL_GPL(m5io_hub_data_abort_channel);

static int m5_rpc_frame_put(struct m5io_hub_core *mcore, const u8 *buf,
                            unsigned int len, bool synchronous) {
  unsigned long flags;
  unsigned int copied;
  int ret;
  struct kfifo *fifo;

  if (!mcore || !buf || !len)
    return -EINVAL;

  fifo = synchronous ? &mcore->sync_tx_fifo : &mcore->tx_fifo;

  spin_lock_irqsave(&mcore->tx_lock, flags);
  /*
   * Test the data-line owner, enqueue and queue the transfer in one critical
   * section.  An upgrade takes the line and resets the FIFO under this same
   * lock, so a frame can neither be slipped in behind its back nor be dropped
   * after this function already reported the request as accepted.
   */
  if (mcore->bus_owner && mcore->bus_owner != current) {
    spin_unlock_irqrestore(&mcore->tx_lock, flags);
    return -EBUSY;
  }
  copied = kfifo_avail(fifo) >= len
               ? kfifo_in(fifo, buf, len)
               : 0;
  if (copied != len) {
    spin_unlock_irqrestore(&mcore->tx_lock, flags);
    return -ENOSPC;
  }
  ret = m5io_hub_rpc_transfer(&mcore->hub);
  spin_unlock_irqrestore(&mcore->tx_lock, flags);

  return ret;
}

/* DATA ACK TOKENs are control traffic and do not allocate a TOKEN window. */
static int m5_rpc_data_ack_send(struct m5io_hub_core *mcore, u8 chn, u8 pid,
                                u8 seq, u8 status) {
  struct m5_rpc_data_ack ack = {
    .chn = chn,
    .pid = pid,
    .seq = seq,
    .status = status,
  };
  unsigned long flags;
  int ret = 0;

  if (!chn || chn > M5IO_HUB_CHN_MAX || pid > 3 ||
      (status != 0 && status != 2 && status != 3 && status != 7))
    return -EINVAL;
  spin_lock_irqsave(&mcore->rpc.data.ack_tx_lock, flags);
  if (!kfifo_put(&mcore->rpc.data.ack_tx_fifo, ack))
    ret = -ENOSPC;
  spin_unlock_irqrestore(&mcore->rpc.data.ack_tx_lock, flags);
  if (!ret)
    m5io_hub_rpc_kick(mcore);
  return ret;
}

/* Build and remove one high-priority WINDOW_ACK for the next SPI transfer. */
static int m5_rpc_data_ack_take_tx(struct m5io_hub_core *mcore, u8 *buf,
                                   struct m5_rpc_data_ack *ack_out) {
  struct m5_rpc_data_ack ack;
  struct m5_rpc_token_frame *frame;
  unsigned long flags;
  u8 payload[4];

  spin_lock_irqsave(&mcore->rpc.data.ack_tx_lock, flags);
  if (!kfifo_get(&mcore->rpc.data.ack_tx_fifo, &ack)) {
    spin_unlock_irqrestore(&mcore->rpc.data.ack_tx_lock, flags);
    return 0;
  }
  spin_unlock_irqrestore(&mcore->rpc.data.ack_tx_lock, flags);

  payload[0] = ack.chn;
  payload[1] = ack.pid;
  payload[2] = ack.seq;
  payload[3] = ack.status;
  frame = (struct m5_rpc_token_frame *)buf;
  m5_rpc_token_frame_fill(frame, M5_RPC_TYPE_TOKEN, 0, 0,
                          M5_RPC_FUNC_DATA_ACK, sizeof(payload),
                          M5_RPC_ARG_DATA_ACK, payload);
  m5_rpc_token_frame_crc_fill(frame);
  if (ack_out)
    *ack_out = ack;
  return M5_RPC_TOKEN_HDR_LEN + sizeof(payload) + M5_RPC_CRC_LEN;
}

static bool m5_rpc_data_ack_tx_pending(struct m5io_hub_core *mcore) {
  unsigned long flags;
  bool pending;

  spin_lock_irqsave(&mcore->rpc.data.ack_tx_lock, flags);
  pending = !kfifo_is_empty(&mcore->rpc.data.ack_tx_fifo);
  spin_unlock_irqrestore(&mcore->rpc.data.ack_tx_lock, flags);
  return pending;
}

/*
 * Keep a failed DATA ACK until the TX FIFO can accept it.  The MCU stops
 * advancing its DATA window until this ACK arrives, so dropping it here can
 * eventually stall both directions of DATA traffic.
 */
static void m5_rpc_data_ack_retry_work(struct work_struct *work) {
  struct m5io_hub_core *mcore;
  struct m5_rpc_data_ack ack;
  unsigned long flags;
  bool have_ack;
  int ret;

  mcore = container_of(to_delayed_work(work), struct m5io_hub_core,
                       data_ack_retry_work);

  for (;;) {
    spin_lock_irqsave(&mcore->rpc.data.ack_retry_lock, flags);
    have_ack = kfifo_peek(&mcore->rpc.data.ack_retry_fifo, &ack);
    if (!have_ack) {
      mcore->data_ack_retry_work_scheduled = false;
      spin_unlock_irqrestore(&mcore->rpc.data.ack_retry_lock, flags);
      return;
    }
    spin_unlock_irqrestore(&mcore->rpc.data.ack_retry_lock, flags);

    ret = m5_rpc_data_ack_send(mcore, ack.chn, ack.pid, ack.seq,
                               ack.status);
    if (ret) {
      mod_delayed_work(system_wq, &mcore->data_ack_retry_work,
                       max_t(unsigned long, 1,
                             msecs_to_jiffies(M5_RPC_DATA_ACK_RETRY_DELAY_MS)));
      return;
    }

    spin_lock_irqsave(&mcore->rpc.data.ack_retry_lock, flags);
    kfifo_skip(&mcore->rpc.data.ack_retry_fifo);
    clear_bit(ack.chn * M5IO_HUB_RPC_TID_NR + ack.pid,
              mcore->rpc.data.ack_retry_pending);
    spin_unlock_irqrestore(&mcore->rpc.data.ack_retry_lock, flags);
  }
}

static void m5_rpc_data_ack_retry_queue(struct m5io_hub_core *mcore, u8 chn,
                                        u8 pid, u8 seq, u8 status) {
  struct m5_rpc_data_ack ack = {.pid = pid, .chn = chn, .seq = seq,
                                .status = status};
  unsigned long flags;
  bool queue = false;

  spin_lock_irqsave(&mcore->rpc.data.ack_retry_lock, flags);
  if (test_and_set_bit(chn * M5IO_HUB_RPC_TID_NR + pid,
                       mcore->rpc.data.ack_retry_pending)) {
    spin_unlock_irqrestore(&mcore->rpc.data.ack_retry_lock, flags);
    return;
  }
  if (!kfifo_put(&mcore->rpc.data.ack_retry_fifo, ack)) {
    clear_bit(chn * M5IO_HUB_RPC_TID_NR + pid,
              mcore->rpc.data.ack_retry_pending);
    spin_unlock_irqrestore(&mcore->rpc.data.ack_retry_lock, flags);
    dev_warn_ratelimited(mcore->hub.dev,
                         "DATA ACK retry queue full: pid=%u chn=%u\n", pid,
                         chn);
    return;
  }
  if (!mcore->data_ack_retry_work_scheduled) {
    mcore->data_ack_retry_work_scheduled = true;
    queue = true;
  }
  spin_unlock_irqrestore(&mcore->rpc.data.ack_retry_lock, flags);

  if (queue)
    queue_delayed_work(system_wq, &mcore->data_ack_retry_work, 0);
}

static bool m5_rpc_data_window_release(struct m5io_hub_core *mcore, u8 tid,
                                       u8 chn, u8 seq, int error) {
  unsigned long flags;
  bool matched = false;

  spin_lock_irqsave(&mcore->rpc.data.tx_window_lock, flags);
  if (test_bit(tid, &mcore->rpc.data.tx_windows) &&
      mcore->rpc.data.tx_chn[tid] == chn &&
      mcore->rpc.data.tx_seq[tid] == seq) {
    WRITE_ONCE(mcore->rpc.data.channel_error[chn], error);
    clear_bit(tid, &mcore->rpc.data.tx_windows);
    mcore->rpc.data.tx_history[tid].valid = true;
    mcore->rpc.data.tx_history[tid].seq = seq;
    mcore->rpc.data.tx_chn[tid] = 0;
    matched = true;
  }
  spin_unlock_irqrestore(&mcore->rpc.data.tx_window_lock, flags);

  if (matched) {
    m5io_dbg("m5io-hub-core: DATA window release chn=%u pid=%u seq=%u error=%d\n",
             chn, tid, seq, error);
    wake_up(&mcore->rpc.data.rx_wq);
    wake_up(&mcore->i2c.chan[0].wq);
    wake_up(&mcore->i2c.chan[1].wq);
    wake_up(&mcore->spi_rpc.wq);
  }

  return matched;
}

static bool m5_rpc_data_channel_idle(struct m5io_hub_core *mcore, u8 chn)
{
  unsigned long flags;
  unsigned int pid;
  bool idle = true;
  spin_lock_irqsave(&mcore->rpc.data.tx_window_lock, flags);
  for (pid = 0; pid < M5IO_HUB_RPC_TID_NR; pid++) {
    if (test_bit(pid, &mcore->rpc.data.tx_windows) &&
        mcore->rpc.data.tx_chn[pid] == chn) {
      idle = false;
      break;
    }
  }
  spin_unlock_irqrestore(&mcore->rpc.data.tx_window_lock, flags);
  return idle;
}

/* Reset all transport-owned DATA state after a broken SPI session or MCU
 * reset.  No stale PID/ACK/SEQ state may survive into the next HELLO session. */
static void m5_rpc_data_state_reset(struct m5io_hub_core *mcore)
{
  unsigned long flags;

  /* No DATA consumer may still be looking at rx_fifo while the session is
   * being replaced.  In particular, cancel_work_sync() is required here for
   * MCU_RESET: the old firmware can leave a complete frame queued just before
   * it acknowledges the reset command, and that work item would otherwise run
   * against handlers belonging to the new session. */
  cancel_work_sync(&mcore->data_rx_work);

  spin_lock_irqsave(&mcore->rpc.data.tx_window_lock, flags);
  mcore->rpc.data.tx_windows = 0;
  memset(mcore->rpc.data.tx_chn, 0, sizeof(mcore->rpc.data.tx_chn));
  memset(mcore->rpc.data.tx_seq, 0, sizeof(mcore->rpc.data.tx_seq));
  memset(mcore->rpc.data.tx_history, 0, sizeof(mcore->rpc.data.tx_history));
  memset(mcore->rpc.data.channel_error, 0,
         sizeof(mcore->rpc.data.channel_error));
  mcore->rpc.data.next_seq = 0;
  spin_unlock_irqrestore(&mcore->rpc.data.tx_window_lock, flags);

  /* rx_dedup is consumed by the parser and DATA worker under rx_lock.  It
   * must never be protected by tx_window_lock: doing so permits a parser to
   * race the reset and retain a stale HELD PID into the new MCU session. */
  spin_lock_irqsave(&mcore->rpc.data.rx_lock, flags);
  memset(mcore->rpc.data.rx_dedup, 0, sizeof(mcore->rpc.data.rx_dedup));
  kfifo_reset(&mcore->rpc.data.rx_fifo);
  mcore->rpc.data.rx_work_scheduled = false;
  spin_unlock_irqrestore(&mcore->rpc.data.rx_lock, flags);
  wake_up(&mcore->rpc.data.rx_wq);

  spin_lock_irqsave(&mcore->rpc.data.ack_retry_lock, flags);
  kfifo_reset(&mcore->rpc.data.ack_retry_fifo);
  bitmap_zero(mcore->rpc.data.ack_retry_pending,
              M5_RPC_DATA_ACK_RETRY_PENDING_BITS);
  mcore->data_ack_retry_work_scheduled = false;
  spin_unlock_irqrestore(&mcore->rpc.data.ack_retry_lock, flags);

  spin_lock_irqsave(&mcore->rpc.data.ack_tx_lock, flags);
  kfifo_reset(&mcore->rpc.data.ack_tx_fifo);
  spin_unlock_irqrestore(&mcore->rpc.data.ack_tx_lock, flags);
}

/*
 * Cancel a pending DATA ACK retry and clear its bookkeeping.  The retry worker
 * only clears data_ack_retry_work_scheduled itself when it drains the queue,
 * so a plain cancel_delayed_work_sync() would leave the flag set and silently
 * stop every later ACK from being scheduled.  Queued retries are dropped
 * because the MCU is about to be reset and can no longer consume them.
 */
static void m5io_hub_data_ack_retry_cancel(struct m5io_hub_core *mcore)
{
  unsigned long flags;

  cancel_delayed_work_sync(&mcore->data_ack_retry_work);

  spin_lock_irqsave(&mcore->rpc.data.ack_retry_lock, flags);
  kfifo_reset(&mcore->rpc.data.ack_retry_fifo);
  bitmap_zero(mcore->rpc.data.ack_retry_pending,
              M5_RPC_DATA_ACK_RETRY_PENDING_BITS);
  mcore->data_ack_retry_work_scheduled = false;
  spin_unlock_irqrestore(&mcore->rpc.data.ack_retry_lock, flags);

  spin_lock_irqsave(&mcore->rpc.data.ack_tx_lock, flags);
  kfifo_reset(&mcore->rpc.data.ack_tx_fifo);
  spin_unlock_irqrestore(&mcore->rpc.data.ack_tx_lock, flags);
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
  case 0x7:
    return -EIO;
  default:
    return -EPROTO;
  }
}

/* TOKEN slots own every byte until both the caller and physical transport have
 * detached. An uncertain request has no time-based reclamation. All helpers
 * ending in _locked require rpc.token.state_lock. */
static void m5_rpc_token_release_locked(struct m5_rpc_token_slot *slot)
{
  if (!slot->caller && !slot->queued && !slot->in_flight &&
      (slot->state == M5_TOKEN_DONE ||
       (slot->state == M5_TOKEN_QUARANTINED && slot->final_received)))
    slot->state = M5_TOKEN_FREE;
}

static void m5_rpc_token_fail_locked(struct m5_rpc_token_slot *slot, int error)
{
  if (slot->state != M5_TOKEN_WAIT)
    return;
  slot->queued = false; /* synchronously cancel any unclaimed first send */
  slot->status = error;
  slot->state = slot->sent ? M5_TOKEN_QUARANTINED : M5_TOKEN_DONE;
  complete(&slot->done);
}

/* Validate the response against the original command, not FUNC alone. */
static int m5_rpc_token_response_status(const struct m5_rpc_token_slot *slot,
                                        const struct m5_rpc_token_frame *rx)
{
  const struct m5_rpc_token_frame *tx = (const void *)slot->tx;
  u8 func = m5_rpc_token_func(tx), command = m5_rpc_token_arg(tx);
  u8 arg = m5_rpc_token_arg(rx), len = m5_rpc_token_len(rx);
  int expected_len = -1, status;
  bool fixed_arg = false;

  if (arg == 1 && !len)
    return -EINPROGRESS;
  if ((arg == 2 || arg == 3) && !len)
    return m5_rpc_response_status_to_errno(arg);

  switch (func) {
  case 0:
    switch (command) {
    case 1: case 5: expected_len = 15; break;
    case 2: case 3: expected_len = 0; break;
    case 6: expected_len = 8; fixed_arg = true; break;
    }
    break;
  case 1:
    if (command >= 1 && command <= 5)
      expected_len = command == 3 ? 1 : 0;
    else if (command == 6) {
      fixed_arg = true;
      expected_len = m5_rpc_token_len(tx) == 3 && tx->data[1] == 0xfe ? 5 : 3;
    }
    break;
  case 2:
    if (command == 1 || command == 6 || command == 7) expected_len = 0;
    break;
  case 3:
    if (command == 1 || command == 2 || command == 6) expected_len = 0;
    else if (command == 8) { expected_len = 4; fixed_arg = true; }
    break;
  case 4:
    if (command == 1) expected_len = 10;
    else if (command == 6) expected_len = 0;
    break;
  case 5:
    if (command == 1) expected_len = 2;
    else if (command == 2) expected_len = 0;
    break;
  case 6:
    if (command == 1 || command == 3 || command == 5 || command == 7)
      expected_len = 0;
    break;
  }
  if (fixed_arg) {
    if (arg != command || len != expected_len)
      return -EPROTO;
    status = m5_rpc_response_status_to_errno(rx->data[0]);
    return status == -EINPROGRESS ? -EPROTO : status;
  }
  status = m5_rpc_response_status_to_errno(arg);
  if (!status)
    return expected_len >= 0 && len == expected_len ? 0 : -EPROTO;
  if (status == -EINPROGRESS)
    return -EPROTO;
  if (status == -EBUSY && func == 3 && (command == 1 || command == 6))
    return len == 5 ? status : -EPROTO;
  return !len ? status : -EPROTO;
}

static bool m5_rpc_token_response_locked(struct m5io_hub_core *mcore, u8 tid,
                                          const u8 *buf, unsigned int len)
{
  struct m5_rpc_token_slot *slot = &mcore->rpc.token.slot[tid];
  const struct m5_rpc_token_frame *rx = (const void *)buf;
  const struct m5_rpc_token_frame *tx = (const void *)slot->tx;
  int status;

  if ((slot->state != M5_TOKEN_WAIT && slot->state != M5_TOKEN_QUARANTINED) ||
      !slot->sent || m5_rpc_token_func(rx) != m5_rpc_token_func(tx))
    return false;
  if (time_after_eq(jiffies, slot->deadline))
    m5_rpc_token_fail_locked(slot, -ETIMEDOUT);
  status = m5_rpc_token_response_status(slot, rx);
  if (status == -EINPROGRESS)
    return true;
  if (status == -EPROTO) {
    m5_rpc_token_fail_locked(slot, status);
    return true;
  }
  slot->final_received = true;
  if (slot->state == M5_TOKEN_QUARANTINED) {
    ++mcore->rpc.token.late_responses;
    m5_rpc_token_release_locked(slot);
    return true;
  }
  memcpy(slot->rx, buf, len);
  slot->rx_len = len;
  slot->status = status;
  slot->state = M5_TOKEN_DONE;
  complete(&slot->done);
  return true;
}

static int m5_rpc_token_submit(struct m5io_hub_core *mcore,
                               const struct m5io_hub_request *request)
{
  struct m5_rpc_token_slot *slot;
  struct m5_rpc_token_frame *frame;
  unsigned long flags;
  unsigned int tid;
  int ret = -EBUSY;

  /* Lock ordering: tx_lock -> token.state_lock. Admission and OTA ownership
   * changes are atomic; the four control buffers cannot be filled by DATA. */
  spin_lock_irqsave(&mcore->tx_lock, flags);
  if (READ_ONCE(mcore->ota_active) || m5io_hub_bus_held_by_other(mcore))
    goto out;
  spin_lock(&mcore->rpc.token.state_lock);
  for (tid = 0; tid < M5IO_HUB_RPC_TID_NR; ++tid) {
    slot = &mcore->rpc.token.slot[tid];
    if (slot->state != M5_TOKEN_FREE)
      continue;
    if (!++slot->generation) ++slot->generation;
    reinit_completion(&slot->done);
    slot->state = M5_TOKEN_WAIT;
    slot->caller = slot->queued = true;
    slot->in_flight = slot->sent = slot->final_received = false;
    slot->rx_len = 0;
    slot->status = -EINPROGRESS;
    slot->deadline = jiffies + msecs_to_jiffies(
        request->func == M5IO_HUB_FUNC_UART ?
        M5_RPC_UART_TOKEN_TIMEOUT_MS : M5_RPC_TOKEN_TIMEOUT_MS);
    slot->send_deadline = jiffies + msecs_to_jiffies(100);
    frame = (void *)slot->tx;
    m5_rpc_token_frame_fill(frame, M5_RPC_TYPE_TOKEN, 1, tid, request->func,
                            request->payload_len, request->arg, request->payload);
    m5_rpc_token_frame_crc_fill(frame);
    slot->tx_len = M5_RPC_TOKEN_HDR_LEN + request->payload_len + M5_RPC_CRC_LEN;
    ret = tid;
    break;
  }
  spin_unlock(&mcore->rpc.token.state_lock);
  if (ret >= 0) {
    m5io_hub_rpc_kick(mcore);
  }
out:
  spin_unlock_irqrestore(&mcore->tx_lock, flags);
  return ret;
}

static int m5_rpc_token_take_tx(struct m5io_hub_core *mcore, u8 *buf,
                                int *tid_out, u32 *generation)
{
  struct m5_rpc_token_slot *slot;
  unsigned long flags;
  unsigned int i, tid;
  int len = 0;

  spin_lock_irqsave(&mcore->rpc.token.state_lock, flags);
  for (i = 0; i < M5IO_HUB_RPC_TID_NR; ++i) {
    tid = (mcore->rpc.token.next_tx + i) % M5IO_HUB_RPC_TID_NR;
    slot = &mcore->rpc.token.slot[tid];
    if (!slot->queued)
      continue;
    if (time_after_eq(jiffies, slot->send_deadline)) {
      m5_rpc_token_fail_locked(slot, -ETIMEDOUT);
      continue;
    }
    slot->queued = false;
    slot->in_flight = true;
    memcpy(buf, slot->tx, slot->tx_len);
    len = slot->tx_len;
    *tid_out = tid;
    *generation = slot->generation;
    mcore->rpc.token.next_tx = (tid + 1) % M5IO_HUB_RPC_TID_NR;
    break;
  }
  spin_unlock_irqrestore(&mcore->rpc.token.state_lock, flags);
  return len;
}

/* A claimed copy may still be cancelled before it is committed to SPI. */
static bool m5_rpc_token_start_tx(struct m5io_hub_core *mcore, unsigned int tid,
                                  u32 generation)
{
  struct m5_rpc_token_slot *slot = &mcore->rpc.token.slot[tid];
  unsigned long flags;
  bool send = false;

  spin_lock_irqsave(&mcore->rpc.token.state_lock, flags);
  if (slot->generation == generation && slot->in_flight &&
      slot->state == M5_TOKEN_WAIT) {
    if (time_after_eq(jiffies, slot->send_deadline))
      m5_rpc_token_fail_locked(slot, -ETIMEDOUT);
    else
      send = slot->sent = true;
  }
  spin_unlock_irqrestore(&mcore->rpc.token.state_lock, flags);
  return send;
}

static void m5_rpc_token_tx_done(struct m5io_hub_core *mcore, unsigned int tid,
                                 u32 generation, int error)
{
  struct m5_rpc_token_slot *slot = &mcore->rpc.token.slot[tid];
  unsigned long flags;

  spin_lock_irqsave(&mcore->rpc.token.state_lock, flags);
  if (slot->generation == generation && slot->in_flight) {
    slot->in_flight = false;
    if (error)
      m5_rpc_token_fail_locked(slot, error);
    m5_rpc_token_release_locked(slot);
  }
  spin_unlock_irqrestore(&mcore->rpc.token.state_lock, flags);
}

static bool m5_rpc_token_tx_pending(struct m5io_hub_core *mcore)
{
  unsigned long flags;
  unsigned int i;
  bool pending = false;

  spin_lock_irqsave(&mcore->rpc.token.state_lock, flags);
  for (i = 0; i < M5IO_HUB_RPC_TID_NR; ++i)
    pending |= mcore->rpc.token.slot[i].queued;
  spin_unlock_irqrestore(&mcore->rpc.token.state_lock, flags);
  return pending;
}

static int m5_rpc_token_wait_result(struct m5io_hub_core *mcore, unsigned int tid,
                                    struct m5io_hub_request *request)
{
  struct m5_rpc_token_slot *slot = &mcore->rpc.token.slot[tid];
  const struct m5_rpc_token_frame *rx = (const void *)slot->rx;
  unsigned long flags, left;
  int ret;

  for (;;) {
    spin_lock_irqsave(&mcore->rpc.token.state_lock, flags);
    if (slot->state != M5_TOKEN_WAIT)
      break;
    if (time_after_eq(jiffies, slot->deadline)) {
      m5_rpc_token_fail_locked(slot, -ETIMEDOUT);
      break;
    }
    /* The token result may only become visible after another SPI clock.
     * Do not sleep for the whole token deadline: if HOST_READY was missed,
     * the RPC worker must be kicked periodically so it can drain the MCU's
     * response.  Keep the wake interval short enough to preserve the token
     * budget while avoiding a busy loop. */
    left = min_t(unsigned long, slot->deadline - jiffies,
                 msecs_to_jiffies(10));
    spin_unlock_irqrestore(&mcore->rpc.token.state_lock, flags);
    if (!wait_for_completion_timeout(&slot->done, left))
      m5io_hub_rpc_kick(mcore);
  }
  ret = slot->status;
  if (slot->rx_len) {
    request->status = m5_rpc_token_arg(rx);
    request->reply_len = m5_rpc_token_len(rx);
    memcpy(request->reply, rx->data, request->reply_len);
  }
  slot->caller = false;
  m5_rpc_token_release_locked(slot);
  spin_unlock_irqrestore(&mcore->rpc.token.state_lock, flags);
  return ret;
}

static void m5_rpc_token_cancel_all(struct m5io_hub_core *mcore, int error)
{
  unsigned long flags;
  unsigned int i;

  spin_lock_irqsave(&mcore->rpc.token.state_lock, flags);
  for (i = 0; i < M5IO_HUB_RPC_TID_NR; ++i)
    m5_rpc_token_fail_locked(&mcore->rpc.token.slot[i], error);
  spin_unlock_irqrestore(&mcore->rpc.token.state_lock, flags);
}

static void m5_rpc_token_session_reset(struct m5io_hub_core *mcore)
{
  unsigned long flags;
  unsigned int i;

  /* A successful MCU_RESET invalidates every response still owned by the old
   * firmware.  A timed-out HELLO from that old session is intentionally
   * quarantined during normal operation, but the reset guarantees that it can
   * never arrive later.  Reclaim only detached slots; a caller/in-flight slot
   * is left intact so an unrelated operation cannot be completed underneath
   * its waiter. */
  spin_lock_irqsave(&mcore->rpc.token.state_lock, flags);
  for (i = 0; i < M5IO_HUB_RPC_TID_NR; ++i) {
    struct m5_rpc_token_slot *slot = &mcore->rpc.token.slot[i];

    if (slot->caller || slot->queued || slot->in_flight)
      continue;
    slot->state = M5_TOKEN_FREE;
    slot->sent = false;
    slot->final_received = false;
    slot->rx_len = 0;
    slot->status = -ECONNRESET;
    if (!++slot->generation)
      slot->generation = 1;
  }
  spin_unlock_irqrestore(&mcore->rpc.token.state_lock, flags);
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
  u8 func, arg, payload_len, tid;
  bool matched = false;

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
  if (expect_len != len || expect_len > M5_RPC_TOKEN_MAX_LEN ||
      m5_rpc_token_frame_crc_check(buf, expect_len))
    return;

  /* WINDOW_ACK is the only DATA acknowledgement.  It is a response/event
   * frame, never a TOKEN transaction, and carries CH/PID/SEQ/status. */
  if (!m5_rpc_token_p(frame) &&
      func == M5_RPC_FUNC_PROTOCOL_CONTROL && arg == M5_RPC_ARG_DATA_ACK &&
      payload_len == 4) {
    u8 chn, pid, seq, ack_status;
    if (tid != 0)
      return;
    chn = frame->data[0];
    pid = frame->data[1];
    seq = frame->data[2];
    ack_status = frame->data[3];
    if (!chn || chn > M5IO_HUB_CHN_MAX || pid > 3 ||
        (ack_status != 0 && ack_status != 2 && ack_status != 3 &&
         ack_status != 7))
      return;
    if (!m5_rpc_data_window_release(mcore, pid, chn, seq,
          ack_status == 0 ? 0 : ack_status == 2 ? -EOPNOTSUPP :
          ack_status == 3 ? -EINVAL : -EIO))
      dev_dbg_ratelimited(mcore->hub.dev,
                          "unmatched WINDOW_ACK ch=%u pid=%u seq=%u status=%u\n",
                          chn, pid, seq, ack_status);
    return;
  }

  /* GPIO and watermark events are unsolicited and have no TID wait. */
  if (!m5_rpc_token_p(frame) && tid == 0 && func == M5_RPC_FUNC_GPIO &&
      arg == M5_RPC_ARG_GPIO_INTERRUPT_EVENT && payload_len == 1) {
    if (frame->data[0] < M5IO_HUB_NGPIO)
      m5io_hub_gpio_irq_report(mcore, frame->data[0]);
    return;
  }
  if (!m5_rpc_token_p(frame) && tid == 0 && func == M5IO_HUB_FUNC_SPI &&
      arg == 7 && payload_len == 2)
    return;
  if (!m5_rpc_token_p(frame) && tid == 0 && func == M5IO_HUB_FUNC_UART &&
      arg == 7 && payload_len == 3)
    return;

  /* Events and WINDOW_ACK were consumed above. Completion, deadline and
   * quarantine transitions share one lock; no callback escapes its lifetime. */
  if (m5_rpc_token_p(frame))
    return;
  spin_lock_irqsave(&mcore->rpc.token.state_lock, flags);
  matched = m5_rpc_token_response_locked(mcore, tid, buf, expect_len);
  spin_unlock_irqrestore(&mcore->rpc.token.state_lock, flags);
  if (!matched)
    dev_dbg_ratelimited(mcore->hub.dev,
                        "unmatched token response tid=%u func=%u arg=%u len=%u\n",
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

    /* DATA byte 1 is SEQ; byte 2 is the payload length. */
    frame_len = M5_RPC_DATA_HDR_LEN + header[2] + M5_RPC_CRC_LEN;
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
      atomic_inc(&mcore->rpc.data.handler_inflight[chn]);
    mutex_unlock(&mcore->rpc.data.handler_lock);

    if (handler) {
      handler(handler_data, frame->data, payload_len);

      if (atomic_dec_and_test(&mcore->rpc.data.handler_inflight[chn]))
        wake_up_all(&mcore->rpc.data.handler_wq[chn]);
    } else {
      dev_dbg_ratelimited(mcore->hub.dev,
                          "drop data for unregistered channel %u\n",
                          chn);
    }
  }
}

/* Serialize DATA de-duplication by PID.  A repeated CH/PID/SEQ with the same
 * CRC is replayed without invoking a consumer; a different generation while
 * the previous PID is held is a protocol fault. */
static int m5_rpc_data_rx_dedup(struct m5io_hub_core *mcore, u8 chn, u8 pid,
                                u8 seq, u16 crc)
{
  unsigned long flags;
  int result = 0;
  spin_lock_irqsave(&mcore->rpc.data.rx_lock, flags);
  if (mcore->rpc.data.rx_dedup[pid].valid) {
    if (mcore->rpc.data.rx_dedup[pid].seq == seq) {
      if (mcore->rpc.data.rx_dedup[pid].chn != chn ||
          mcore->rpc.data.rx_dedup[pid].crc != crc)
        result = -1;
      else
        /* HELD duplicates must wait for the first copy's release ACK. */
        result = mcore->rpc.data.rx_dedup[pid].held ? 2 : 1;
    } else if (mcore->rpc.data.rx_dedup[pid].held) {
      result = -1;
    }
  }
  if (result == 0) {
    mcore->rpc.data.rx_dedup[pid].valid = true;
    mcore->rpc.data.rx_dedup[pid].held = true;
    mcore->rpc.data.rx_dedup[pid].chn = chn;
    mcore->rpc.data.rx_dedup[pid].seq = seq;
    mcore->rpc.data.rx_dedup[pid].crc = crc;
  }
  spin_unlock_irqrestore(&mcore->rpc.data.rx_lock, flags);
  return result;
}

static void m5_rpc_data_rx_release_dedup(struct m5io_hub_core *mcore, u8 chn,
                                         u8 pid, u8 seq)
{
  unsigned long flags;
  spin_lock_irqsave(&mcore->rpc.data.rx_lock, flags);
  if (mcore->rpc.data.rx_dedup[pid].valid &&
      mcore->rpc.data.rx_dedup[pid].held &&
      mcore->rpc.data.rx_dedup[pid].chn == chn &&
      mcore->rpc.data.rx_dedup[pid].seq == seq)
    mcore->rpc.data.rx_dedup[pid].held = false;
  spin_unlock_irqrestore(&mcore->rpc.data.rx_lock, flags);
  wake_up(&mcore->rpc.data.rx_wq);
}

/* An upstream DATA frame remains held until its WINDOW_ACK has actually
 * crossed the SPI wire.  Synchronous bus calls must wait for that release
 * before starting the next exchange; otherwise a new FUNC=3/2 command can
 * overtake the ACK and leave the MCU's four uplink PID slots occupied. */
static bool m5_rpc_data_rx_channel_idle(struct m5io_hub_core *mcore, u8 chn)
{
  unsigned long flags;
  unsigned int pid;
  bool idle = true;

  spin_lock_irqsave(&mcore->rpc.data.rx_lock, flags);
  for (pid = 0; pid < M5IO_HUB_RPC_TID_NR; pid++) {
    if (mcore->rpc.data.rx_dedup[pid].valid &&
        mcore->rpc.data.rx_dedup[pid].held &&
        mcore->rpc.data.rx_dedup[pid].chn == chn) {
      idle = false;
      break;
    }
  }
  spin_unlock_irqrestore(&mcore->rpc.data.rx_lock, flags);
  return idle;
}

/* Release an MCU DATA receive slot only after its WINDOW_ACK was clocked out
 * in a successful SPI transaction.  Queueing the ACK is not enough: a new
 * generation must not reuse the PID while the old ACK is still pending. */
static void m5_rpc_data_ack_tx_done(struct m5io_hub_core *mcore,
                                    const u8 *buf, unsigned int len)
{
  u8 chn, pid, seq, status;

  if (!mcore || !buf || len != M5_RPC_TOKEN_HDR_LEN + 4 + M5_RPC_CRC_LEN ||
      buf[0] != 0 || (buf[1] & M5_RPC_TOKEN_LEN_MASK) != 4 ||
      (buf[1] >> M5_RPC_TOKEN_ARG_SHIFT) != M5_RPC_ARG_DATA_ACK)
    return;
  chn = buf[2];
  pid = buf[3];
  seq = buf[4];
  status = buf[5];
  if (!chn || chn > M5IO_HUB_CHN_MAX || pid > 3 ||
      (status != 0 && status != 2 && status != 3 && status != 7))
    return;
  m5io_dbg("m5io-hub-core: uplink ACK sent chn=%u pid=%u seq=%u status=%u\n",
           chn, pid, seq, status);
  m5_rpc_data_rx_release_dedup(mcore, chn, pid, seq);
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

/* Called inline by the stream parser, before a following completion TOKEN. */
static void m5_rpc_i2c_data_rx(struct m5io_hub_core *mcore,
                              const struct m5_rpc_data_frame *frame) {
  struct m5_rpc_i2c_channel *i2c;
  unsigned long flags;
  bool accepted = false;
  int error = 0;
  int tid;
  u8 chn = m5_rpc_data_chn(frame);
  unsigned int expected_len;

  if (chn < M5_RPC_CHN_IIC1 || chn > M5_RPC_CHN_IIC2)
    return;
  i2c = &mcore->i2c.chan[chn - M5_RPC_CHN_IIC1];

  spin_lock_irqsave(&mcore->rpc.token.state_lock, flags);
  tid = i2c->tid;
  expected_len = i2c->expected_len;
  if (i2c->data_waiting && m5_rpc_data_len(frame) >= 2 &&
      m5_rpc_data_len(frame) <= i2c->expected_len && !i2c->rx_len) {
    memcpy(i2c->rx, frame->data, m5_rpc_data_len(frame));
    i2c->rx_len = m5_rpc_data_len(frame);
    accepted = true;
    wake_up(&i2c->wq);
  } else if (i2c->data_waiting) {
    i2c->error = -EPROTO;
    error = i2c->error;
    wake_up(&i2c->wq);
  }
  spin_unlock_irqrestore(&mcore->rpc.token.state_lock, flags);

  if (accepted)
    m5io_dbg("m5io-hub-core: i2c DATA rx tid=%d chn=%u len=%u\n",
            tid, chn, expected_len);
  else
    m5io_dbg("m5io-hub-core: i2c DATA rx rejected tid=%d chn=%u len=%u expected=%u error=%d\n",
            tid, chn, m5_rpc_data_len(frame), expected_len, error);
}

static void m5_rpc_spi_data_rx(struct m5io_hub_core *mcore,
                              const struct m5_rpc_data_frame *frame) {
  struct m5_rpc_spi *rpc = &mcore->spi_rpc;
  unsigned long flags;
  unsigned int len = m5_rpc_data_len(frame);

  spin_lock_irqsave(&mcore->rpc.token.state_lock, flags);
  if (rpc->data_waiting && !rpc->error) {
    /* Upstream PID is allocated from the independent MCU->SoC window; it
     * need not mirror the downstream PID sequence.  Ordering is provided by
     * the SPI channel queue, so accept any legal PID here. */
    if (!len || len > rpc->expected_len - rpc->rx_len) {
      rpc->error = -EPROTO;
    } else {
      memcpy(rpc->rx + rpc->rx_len, frame->data, len);
      rpc->rx_len += len;
      wake_up(&rpc->wq);
    }
  }
  spin_unlock_irqrestore(&mcore->rpc.token.state_lock, flags);
}

/**
 * m5_rpc_rx_stream_feed - 将一次 SPI 读回的数据送入协议流解析器
 * @mcore: hub 核心上下文
 * @buf: SPI 读回数据
 * @len: 读回长度
 * @slave_empty: 返回时为 true 表示在帧边界读到了 0xff 空白
 *
 * TOKEN 的 LEN 是第二字节低 4 bit，DATA 的 LEN 是第三字节；两种帧
 * 的完整长度分别为 2 + LEN + 2 和 3 + LEN + 2。SPI 传输可能停在帧中间，未完成数据保存
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
  u8 data_pid;
  u8 payload_len;
  u8 type;
  u16 wire_crc;
  int dedup;
  bool queued;

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

    if (!stream->expect_len) {
      copy_len = stream->len < M5_RPC_FRAME_HDR_LEN ?
                   min_t(unsigned int, M5_RPC_FRAME_HDR_LEN - stream->len,
                         len - pos) : 0;
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
      else {
        /* DATA has a third header byte.  Keep the parser in the header state
         * until it is available; byte 1 is SEQ, never LEN. */
        if (stream->len < M5_RPC_DATA_HDR_LEN) {
          copy_len = min_t(unsigned int, M5_RPC_DATA_HDR_LEN - stream->len,
                           len - pos);
          memcpy(stream->frame + stream->len, buf + pos, copy_len);
          stream->len += copy_len;
          pos += copy_len;
          if (stream->len < M5_RPC_DATA_HDR_LEN)
            break;
        }
        stream->expect_len = M5_RPC_DATA_HDR_LEN + stream->frame[2] +
                             M5_RPC_CRC_LEN;
      }
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
      data_pid = m5_rpc_data_pid(data_frame);
      chn = m5_rpc_data_chn(data_frame);
      payload_len = m5_rpc_data_len(data_frame);
      if (m5_rpc_data_frame_crc_check(stream->frame, stream->expect_len)) {
        dev_dbg_ratelimited(mcore->hub.dev,
                            "drop invalid data frame: tid=%u chn=%u len=%u\n",
                            data_pid, chn, payload_len);
      } else if (!chn || chn > M5IO_HUB_CHN_MAX) {
        dev_dbg_ratelimited(mcore->hub.dev,
                            "drop data frame for invalid channel %u\n",
                            chn);
      } else {
        wire_crc = stream->frame[M5_RPC_DATA_HDR_LEN + payload_len] |
                   ((u16)stream->frame[M5_RPC_DATA_HDR_LEN + payload_len + 1]
                    << 8);
        dedup = m5_rpc_data_rx_dedup(mcore, chn, data_pid,
                                     m5_rpc_data_seq(data_frame), wire_crc);
        if (dedup == 1) {
          /* A released duplicate only replays the fixed ACK; it never reaches
           * a consumer a second time. */
          ret = m5_rpc_data_ack_send(mcore, chn, data_pid,
                                     m5_rpc_data_seq(data_frame), 0);
          if (ret)
            m5_rpc_data_ack_retry_queue(mcore, chn, data_pid,
                                        m5_rpc_data_seq(data_frame), 0);
        } else if (dedup == 2) {
          /* HELD duplicate: do not acknowledge before the first copy is
           * released. The sender retries on its fixed 1 s schedule. */
          dev_dbg_ratelimited(mcore->hub.dev,
                              "ignore HELD duplicate data: chn=%u pid=%u seq=%u\n",
                              chn, data_pid, m5_rpc_data_seq(data_frame));
        } else if (dedup < 0) {
          /* A conflicting generation must not overwrite the old PID record or
           * be turned into a normal BUSY ACK. */
          dev_warn_ratelimited(mcore->hub.dev,
                               "DATA PID conflict: chn=%u pid=%u seq=%u\n",
                               chn, data_pid, m5_rpc_data_seq(data_frame));
      } else if (chn == M5_RPC_CHN_IIC1 || chn == M5_RPC_CHN_IIC2) {
        /* FUNC=2 has a completion TOKEN; DATA still needs its window ACK. */
        m5_rpc_i2c_data_rx(mcore, data_frame);
        ret = m5_rpc_data_ack_send(mcore, chn, data_pid,
                                   m5_rpc_data_seq(data_frame), 0);
        if (ret)
          m5_rpc_data_ack_retry_queue(mcore, chn, data_pid,
                                      m5_rpc_data_seq(data_frame),
                                      0);
      } else if (chn == M5_RPC_CHN_SPI) {
        /* FUNC3 runs a real exchange on the MCU's SPI2 master and uplinks the
         * MISO bytes it clocked in; it does not echo MOSI.  The uplink arrives
         * as ordinary DATA and is completed by the synchronous waiter, so this
         * channel needs no asynchronous DATA ACK window of its own.
         */
        m5_rpc_spi_data_rx(mcore, data_frame);
        ret = m5_rpc_data_ack_send(mcore, chn, data_pid,
                                   m5_rpc_data_seq(data_frame), 0);
        if (ret)
          m5_rpc_data_ack_retry_queue(mcore, chn, data_pid,
                                      m5_rpc_data_seq(data_frame), 0);
      } else if (chn == M5_RPC_CHN_LOG) {
        /*
         * Diagnostic text from the MCU.  The frame is consumed here and the
         * The ACK is released only after the SPI transaction carrying it
         * completes; losing log bytes is fine, stalling the protocol is not.
         */
        m5io_hub_log_rx(mcore, data_frame->data, payload_len);
        ret = m5_rpc_data_ack_send(mcore, chn, data_pid,
                                   m5_rpc_data_seq(data_frame), 0);
        if (ret)
          m5_rpc_data_ack_retry_queue(mcore, chn, data_pid,
                                      m5_rpc_data_seq(data_frame), 0);
      } else {
        /*
         * The ACK is queued even when the payload cannot be queued.  The MCU
         * has no timer that frees a PID, so the ACK must reach the wire before
         * that slot can be reused.  Losing the bytes is survivable; losing the
         * window is not.  The header CRC already passed here, so the PID is
         * trustworthy.
         */
        queued = m5_rpc_data_rx_frame_put(mcore, stream->frame,
                                          stream->expect_len,
                                          &queue_data_work);

        ret = m5_rpc_data_ack_send(mcore, chn, data_pid,
                                   m5_rpc_data_seq(data_frame),
                                   queued ? 0 : 7);
        if (ret) {
          dev_warn_ratelimited(mcore->hub.dev,
                               "failed to send data ack, retrying: pid=%u "
                               "chn=%u: %d\n",
                               data_pid, chn, ret);
          m5_rpc_data_ack_retry_queue(mcore, chn, data_pid,
                                      m5_rpc_data_seq(data_frame), 0);
        }
        if (queued && queue_data_work)
          queue_work(system_unbound_wq, &mcore->data_rx_work);
      }
      }
    }

    stream->len = 0;
    stream->expect_len = 0;
  }

  if (!stream->expect_len)
    return 0;

  return stream->expect_len - stream->len;
}

static int m5_rpc_data_window_alloc(struct m5io_hub_core *mcore, u8 chn,
                                    u8 *pid_out, u8 *seq_out) {
  unsigned long flags;
  unsigned int pid, active = 0;
  u8 seq;

  spin_lock_irqsave(&mcore->rpc.data.tx_window_lock, flags);
  seq = mcore->rpc.data.next_seq;
  /* Keep one direction-wide DATA PID available for synchronous I2C/SPI.
   * UART/IR are best-effort producers and can otherwise occupy all four
   * windows while a synchronous transfer is waiting for its first frame.
   * The reservation is released naturally when any window is ACKed. */
  if (chn != M5_RPC_CHN_IIC1 && chn != M5_RPC_CHN_IIC2 &&
      chn != M5_RPC_CHN_SPI) {
    for (pid = 0; pid < M5IO_HUB_RPC_TID_NR; pid++)
      active += test_bit(pid, &mcore->rpc.data.tx_windows) ? 1 : 0;
    if (active >= M5IO_HUB_RPC_TID_NR - 1) {
      spin_unlock_irqrestore(&mcore->rpc.data.tx_window_lock, flags);
      return -EAGAIN;
    }
  }
  /* One outstanding frame per channel; global SEQ must also be unique
   * among active windows, including when the byte counter wraps. */
  for (pid = 0; pid < M5IO_HUB_RPC_TID_NR; pid++) {
    if (test_bit(pid, &mcore->rpc.data.tx_windows) &&
        (mcore->rpc.data.tx_chn[pid] == chn ||
         mcore->rpc.data.tx_seq[pid] == seq)) {
      spin_unlock_irqrestore(&mcore->rpc.data.tx_window_lock, flags);
      return -EAGAIN;
    }
  }
  for (pid = 0; pid < M5IO_HUB_RPC_TID_NR; pid++) {
    if (test_bit(pid, &mcore->rpc.data.tx_windows))
      continue;
    if (mcore->rpc.data.tx_history[pid].valid &&
        mcore->rpc.data.tx_history[pid].seq == seq)
      continue;
    set_bit(pid, &mcore->rpc.data.tx_windows);
    mcore->rpc.data.tx_chn[pid] = chn;
    mcore->rpc.data.tx_seq[pid] = seq;
    WRITE_ONCE(mcore->rpc.data.channel_error[chn], 0);
    mcore->rpc.data.next_seq++;
    if (pid_out)
      *pid_out = pid;
    if (seq_out)
      *seq_out = seq;
    spin_unlock_irqrestore(&mcore->rpc.data.tx_window_lock, flags);
    return 0;
  }
  spin_unlock_irqrestore(&mcore->rpc.data.tx_window_lock, flags);

  dev_dbg_ratelimited(mcore->hub.dev,
                       "no free DATA window for chn=%u, tx_windows=%#lx "
                       "chn_map=%u,%u,%u,%u\n",
                       chn, mcore->rpc.data.tx_windows,
                       mcore->rpc.data.tx_chn[0], mcore->rpc.data.tx_chn[1],
                       mcore->rpc.data.tx_chn[2], mcore->rpc.data.tx_chn[3]);
  m5io_dbg("m5io-hub-core: DATA window busy chn=%u windows=%#lx "
           "map=%u,%u,%u,%u seq=%u history=%u,%u,%u,%u\n",
           chn, mcore->rpc.data.tx_windows, mcore->rpc.data.tx_chn[0],
           mcore->rpc.data.tx_chn[1], mcore->rpc.data.tx_chn[2],
           mcore->rpc.data.tx_chn[3], seq,
           mcore->rpc.data.tx_history[0].valid ?
             mcore->rpc.data.tx_history[0].seq : 0xff,
           mcore->rpc.data.tx_history[1].valid ?
             mcore->rpc.data.tx_history[1].seq : 0xff,
           mcore->rpc.data.tx_history[2].valid ?
             mcore->rpc.data.tx_history[2].seq : 0xff,
           mcore->rpc.data.tx_history[3].valid ?
             mcore->rpc.data.tx_history[3].seq : 0xff);
  return -EAGAIN;
}

/**
 * m5io_hub_tx_fifo_len - 获取当前待发送字节数
 * @mcore: hub 核心上下文
 */
static unsigned int m5io_hub_tx_fifo_len(struct m5io_hub_core *mcore,
                                         struct kfifo *fifo) {
  unsigned long flags;
  unsigned int len;

  spin_lock_irqsave(&mcore->tx_lock, flags);
  len = kfifo_len(fifo);
  spin_unlock_irqrestore(&mcore->tx_lock, flags);

  return len;
}

/* HOST_READY is a level-like, active-low interrupt.  A low level can remain
 * asserted after the threaded IRQ has run, so a worker must not conclude that
 * the slave is empty solely from an all-0xff SPI sample.  Some irqchips do not
 * expose the raw line state; in that case the normal IRQ retrigger path is the
 * fallback and this helper conservatively reports false. */
static bool m5io_hub_host_ready_low(struct m5io_hub_core *mcore)
{
  bool line_high;

  if (!mcore->hub.irq ||
      irq_get_irqchip_state(mcore->hub.irq, IRQCHIP_STATE_LINE_LEVEL,
                            &line_high))
    return false;
  return !line_high;
}

/* Every downstream NSS transaction must contain exactly one complete frame. */
static int m5io_hub_tx_frame_len(struct m5io_hub_core *mcore,
                                 struct kfifo *fifo) {
  unsigned long flags;
  unsigned int available;
  u8 header[M5_RPC_DATA_HDR_LEN];
  int len = 0;

  spin_lock_irqsave(&mcore->tx_lock, flags);
  available = kfifo_len(fifo);
  if (!available)
    goto unlock;
  if (available < M5_RPC_FRAME_HDR_LEN ||
      kfifo_out_peek(fifo, header, M5_RPC_FRAME_HDR_LEN) !=
          M5_RPC_FRAME_HDR_LEN) {
    len = -EPROTO;
    goto unlock;
  }
  if (header[0] & M5_RPC_TOKEN_TYPE_MASK) {
    if (available < M5_RPC_DATA_HDR_LEN ||
        kfifo_out_peek(fifo, header, M5_RPC_DATA_HDR_LEN) !=
            M5_RPC_DATA_HDR_LEN) {
      len = -EPROTO;
      goto unlock;
    }
    len = M5_RPC_DATA_HDR_LEN + header[2] + M5_RPC_CRC_LEN;
  } else {
    len = M5_RPC_TOKEN_HDR_LEN + (header[1] & M5_RPC_TOKEN_LEN_MASK) +
          M5_RPC_CRC_LEN;
  }
  if ((unsigned int)len > available)
    len = -EPROTO;
unlock:
  spin_unlock_irqrestore(&mcore->tx_lock, flags);
  return len;
}

/**
 * m5io_hub_rpc_transaction - 激活并排空一次 SPI RPC 事务
 * @mcore: hub 核心上下文
 *
 * 每次发送严格取某个 TX FIFO 中的一帧，不拼接帧，也不在命令后添加填充。
 * 只有两个 TX FIFO 都为空的读取事务才按 8 字节对齐并发送 0xff dummy。
 * 上行允许分次读取，rx_stream 保存尚未接收完整的帧。
 *
 * 只有在跳过帧边界的上行 0xff 后没有读到新帧、没有待拼接的半帧且两个
 * TX FIFO 都为空时，事务才结束。整个逻辑事务持有 spi_lock，从而串行化调用
 * 方和 IRQ 发起的事务。
 * 为避免异常从机永久占用工作线程，单次逻辑事务最多传输 64 KiB。
 *
 * Return: 0 表示双方数据均已排空；负值为错误码。
 */
static int m5io_hub_rpc_transaction_mode(struct m5io_hub_core *mcore,
                                           bool irq_context) {
  struct spi_transfer transfer = {0};
  unsigned int transaction_len = 0;
  unsigned int rx_needed = 0;
  int tx_len;
  unsigned int xfer_len;
  bool i2c_token;
  bool ack_tx;
  struct kfifo *data_tx_fifo;
  struct m5_rpc_data_ack ack_current = {0};
  bool slave_empty;
  int token_tid;
  u32 token_generation = 0;
  unsigned long transaction_deadline;
  u8 *tx_buf;
  u8 *rx_buf;
  int ret = 0;

  if (READ_ONCE(mcore->ota_active))
    return -EBUSY;
  mutex_lock(&mcore->spi_lock);
  if (READ_ONCE(mcore->ota_active)) {
    ret = -EBUSY;
    goto out_unlock;
  }
  tx_buf = mcore->rpc_xfer_tx_buf;
  rx_buf = mcore->rpc_xfer_rx_buf;
  /* A malformed or stale MISO byte stream must not monopolize spi_lock.
   * Keep one logical drain bounded in wall-clock time; callers with pending
   * work will queue another transaction after the parser is reset. */
  transaction_deadline = jiffies + msecs_to_jiffies(250);
  if (!tx_buf || !rx_buf) {
    ret = -ENOMEM;
    goto out_unlock;
  }

  for (;;) {
    /*
     * An OTA freeze can start while this transaction is still draining, and
     * the MCU is reset at the beginning of an upgrade.  Without this check the
     * loop keeps clocking the bus until it fills the whole transaction budget:
     * a resetting MCU leaves MISO floating, so slave_empty never becomes true.
     * That costs up to a second of bus traffic, and every millisecond of it is
     * taken away from the 300 ms window the bootloader listens for OPC_IAP.
     */
    if (READ_ONCE(mcore->ota_active)) {
      ret = -EBUSY;
      m5_rpc_rx_stream_reset(mcore);
      break;
    }
    if (time_after_eq(jiffies, transaction_deadline)) {
      ret = -ETIMEDOUT;
      m5_rpc_rx_stream_reset(mcore);
      dev_dbg_ratelimited(mcore->hub.dev,
                          "rpc transaction time budget exhausted\n");
      break;
    }
    token_tid = -1;
    tx_len = 0;
    ack_tx = false;
    /* WINDOW_ACK is a separate high-priority queue.  It must go out before
     * ordinary downlink DATA and before a newly submitted TOKEN; otherwise a
     * burst of I2C/SPI traffic can hold the MCU's uplink PID window. */
    tx_len = m5_rpc_data_ack_take_tx(mcore, tx_buf, &ack_current);
    if (tx_len)
      ack_tx = true;
    else
      tx_len = m5_rpc_token_take_tx(mcore, tx_buf, &token_tid,
                                   &token_generation);
    data_tx_fifo = NULL;
    if (!tx_len) {
      /* Synchronous I2C/SPI DATA has its own queue.  UART DATA is deliberately
       * lower priority so an open-ended tty writer cannot fill the shared
       * FIFO and make a synchronous bus transaction time out. */
      tx_len = m5io_hub_tx_frame_len(mcore, &mcore->sync_tx_fifo);
      if (tx_len > 0)
        data_tx_fifo = &mcore->sync_tx_fifo;
      else if (!tx_len) {
        tx_len = m5io_hub_tx_frame_len(mcore, &mcore->tx_fifo);
        if (tx_len > 0)
          data_tx_fifo = &mcore->tx_fifo;
      }
    }
    if (tx_len < 0) {
      ret = tx_len;
      break;
    }
    xfer_len = tx_len ? (unsigned int)tx_len :
        ALIGN(max(rx_needed, M5IO_HUB_RPC_XFER_ALIGN), M5IO_HUB_RPC_XFER_ALIGN);

    if (xfer_len > M5IO_HUB_FIFO_SIZE ||
        transaction_len > M5IO_HUB_RPC_MAX_TRANSACTION_BYTES - xfer_len) {
      ret = -EOVERFLOW;
      if (token_tid >= 0)
        m5_rpc_token_tx_done(mcore, token_tid, token_generation, ret);
      dev_warn_ratelimited(mcore->hub.dev,
                           "rpc transaction exceeded %u bytes\n",
                           M5IO_HUB_RPC_MAX_TRANSACTION_BYTES);
      break;
    }

    if (token_tid < 0 && !ack_tx)
      memset(tx_buf, M5IO_HUB_RPC_FILL_BYTE, xfer_len);
    if (token_tid < 0 && !ack_tx && tx_len && data_tx_fifo &&
        kfifo_out_spinlocked(data_tx_fifo, tx_buf, tx_len,
                                       &mcore->tx_lock) != (unsigned int)tx_len) {
      ret = -EPROTO;
      break;
    }

    if (tx_len && token_tid < 0 && !ack_tx &&
        (tx_buf[0] & M5_RPC_TOKEN_TYPE_MASK)) {
      const struct m5_rpc_data_frame *data_frame =
          (const struct m5_rpc_data_frame *)tx_buf;
      m5io_dbg("m5io-hub-core: DATA tx chn=%u pid=%u seq=%u len=%u\n",
               m5_rpc_data_chn(data_frame), m5_rpc_data_pid(data_frame),
               m5_rpc_data_seq(data_frame), m5_rpc_data_len(data_frame));
    }

    memset(rx_buf, M5IO_HUB_RPC_FILL_BYTE, xfer_len);
    transfer.tx_buf = tx_buf;
    transfer.rx_buf = rx_buf;
    transfer.len = xfer_len;

    i2c_token = tx_len >= M5_RPC_TOKEN_HDR_LEN &&
                !(tx_buf[0] & M5_RPC_TOKEN_TYPE_MASK) &&
                ((tx_buf[0] >> M5_RPC_TOKEN_FUNC_SHIFT) &
                 M5_RPC_TOKEN_FUNC_MASK) == M5IO_HUB_FUNC_I2C;
    if (i2c_token)
      m5io_dbg("m5io-hub-core: SPI I2C TOKEN tx_len=%d xfer_len=%u\n",
              tx_len, xfer_len);

    if (token_tid >= 0 &&
        !m5_rpc_token_start_tx(mcore, token_tid, token_generation)) {
      m5_rpc_token_tx_done(mcore, token_tid, token_generation, 0);
      continue;
    }
    ret = spi_sync_transfer(mcore->hub.spi, &transfer, 1);
    if (i2c_token)
      m5io_dbg("m5io-hub-core: SPI I2C TOKEN transfer ret=%d\n", ret);
    if (ret) {
      if (ack_tx)
        m5_rpc_data_ack_retry_queue(mcore, ack_current.chn,
                                    ack_current.pid, ack_current.seq,
                                    ack_current.status);
      if (token_tid >= 0)
        m5_rpc_token_tx_done(mcore, token_tid, token_generation, ret);
      m5_rpc_rx_stream_reset(mcore);
      break;
    }

    /* Give the MCU main loop a bounded scheduling gap between NSS
     * transactions.  The slave publishes HOST_READY and parses the just
     * completed frame outside the DMA/EXTI path; issuing the next transfer
     * back-to-back can otherwise outrun that bookkeeping and lose an edge. */
    /* Keep NSS high long enough for the MCU's EXTI0 rising-edge handler to
     * publish the completed RX boundary before the next transfer starts.
     * Without this gap a delayed/coalesced edge can be serviced while NSS is
     * already low, publishing a partial frame and eventually losing the
     * WINDOW_ACK under sustained DATA traffic. */
    usleep_range(8000, 10000);

    m5_rpc_data_ack_tx_done(mcore, tx_buf, tx_len);

    dev_dbg(mcore->hub.dev, "rpc tx=%*ph rx=%*ph\n",
            (int)xfer_len, tx_buf, (int)xfer_len, rx_buf);

    transaction_len += xfer_len;
    rx_needed =
        m5_rpc_rx_stream_feed(mcore, rx_buf, xfer_len, &slave_empty);

    if (token_tid >= 0)
      m5_rpc_token_tx_done(mcore, token_tid, token_generation, 0);
    /* Threaded HOST_READY IRQs can continuously drain UART DATA while a
     * synchronous I2C/SPI caller is waiting for the shared bus.  Once such a
     * request is queued, finish this exchange and release spi_lock so the
     * rpc worker can make progress.  Without this hand-off an IRQ stream can
     * repeatedly reacquire the lock and starve the queued DATA transaction
     * until its userspace timeout. */
    if (irq_context)
      break;

    if (slave_empty && !rx_needed &&
        !m5io_hub_tx_fifo_len(mcore, &mcore->sync_tx_fifo) &&
        !m5io_hub_tx_fifo_len(mcore, &mcore->tx_fifo) &&
        !m5_rpc_data_ack_tx_pending(mcore) &&
        !m5_rpc_token_tx_pending(mcore) &&
        !m5io_hub_host_ready_low(mcore))
      break;

    cond_resched();
  }

out_unlock:
  mutex_unlock(&mcore->spi_lock);
  return ret;
}

static int m5io_hub_rpc_transaction(struct m5io_hub_core *mcore)
{
  return m5io_hub_rpc_transaction_mode(mcore, false);
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
 * Return: 0 表示请求已排队；-ENODEV 表示 @hub 或 @hub->spi 无效；
 *         -EBUSY 表示裸 IAP 阶段或升级已独占数据线。
 */
int m5io_hub_rpc_transfer(struct m5io_hub *hub) {
  struct m5io_hub_core *mcore;

  if (!hub || !hub->spi)
    return -ENODEV;

  mcore = container_of(hub, struct m5io_hub_core, hub);
  if (READ_ONCE(mcore->ota_active))
    return -EBUSY;
  /* An upgrade owns the data line until it has finished. */
  if (m5io_hub_bus_held_by_other(mcore))
    return -EBUSY;
  m5io_hub_rpc_kick(mcore);
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

  for (;;) {
    unsigned long flags;

    spin_lock_irqsave(&mcore->rpc_schedule_lock, flags);
    if (!mcore->rpc_work_kick) {
      /* Publish the idle state under the same lock used by the producer.
       * A producer which arrives after this check will see running=false and
       * queue a fresh work item, so the final wakeup cannot be lost in the
       * small window before this function returns. */
      mcore->rpc_work_running = false;
      spin_unlock_irqrestore(&mcore->rpc_schedule_lock, flags);
      break;
    }
    mcore->rpc_work_kick = false;
    spin_unlock_irqrestore(&mcore->rpc_schedule_lock, flags);

    ret = m5io_hub_rpc_transaction(mcore);
    if (ret)
      dev_dbg(mcore->hub.dev, "rpc transaction failed: %d\n", ret);
  }
}

static void m5io_hub_rpc_kick(struct m5io_hub_core *mcore)
{
  unsigned long flags;
  bool queue = false;

  spin_lock_irqsave(&mcore->rpc_schedule_lock, flags);
  mcore->rpc_work_kick = true;
  if (!mcore->rpc_work_running) {
    mcore->rpc_work_running = true;
    queue = true;
  }
  spin_unlock_irqrestore(&mcore->rpc_schedule_lock, flags);

  /* queue_work() refuses to queue an item which is already executing.  The
   * running flag above makes only the transition from idle to running queue
   * the item; kicks received while it executes are consumed by the worker's
   * next loop iteration. */
  if (queue)
    queue_work(system_highpri_wq, &mcore->rpc_work);
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
  if (READ_ONCE(mcore->ota_active)) {
    complete(&mcore->ota_ready);
    return IRQ_HANDLED;
  }

  /* Do not run a complete drain from the threaded IRQ itself.  A continuous
   * HOST_READY source (for example UART DATA) could otherwise hold spi_lock
   * across many exchanges and starve synchronous I2C/SPI callers.  The worker
   * owns every bus transaction and applies the same wall-clock bound to each
   * drain, while this IRQ only supplies the wakeup. */
  if (m5io_hub_bus_held_by_other(mcore))
    return IRQ_HANDLED;
  m5io_hub_rpc_kick(mcore);

  return IRQ_HANDLED;
}

/**
 * m5io_hub_SendChnData - 非阻塞发送一个 DATA 帧
 * @hub: hub 句柄
 * @chn: 异步 DATA 通道；I2C/SPI 通道须使用 m5io_hub_exec()
 * @data: 待发送的 Payload，@len 为 0 时可为 NULL
 * @len: Payload 长度，最大 255 字节
 *
 * DATA 发送池有 4 个全局共享的 PID 窗口。成功入队后立即返回；MCU 以
 * FUNC=0、ARG=4、LEN=4 的 WINDOW_ACK 回收窗口，载荷为 CH/PID/SEQ/status。
 * 只有本会话、本方向且 CH/PID/SEQ 匹配的 ACK 才能释放对应窗口。
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
  u8 pid, seq;
  int ret;

  if (!hub || !hub->spi)
    return -ENODEV;

  if (!chn || chn > M5IO_HUB_CHN_MAX || !len ||
      len > M5_RPC_DATA_MAX_PLEN || !data)
    return -EINVAL;

  mcore = container_of(hub, struct m5io_hub_core, hub);

  ret = m5_rpc_data_window_alloc(mcore, chn, &pid, &seq);
  if (ret) {
    m5io_dbg("m5io-hub-core: DATA alloc failed chn=%u len=%u ret=%d\n",
             chn, len, ret);
    return ret;
  }

  m5io_dbg("m5io-hub-core: DATA alloc chn=%u pid=%u seq=%u len=%u\n",
           chn, pid, seq, len);

  frame = (struct m5_rpc_data_frame *)tx_buf;
  m5_rpc_data_frame_fill(frame, pid, chn, seq, len, data);
  m5_rpc_data_frame_crc_fill(frame);

  tx_len = M5_RPC_DATA_HDR_LEN + len + M5_RPC_CRC_LEN;
  /* I2C/SPI callers wait synchronously for the matching completion.  Keep
   * those frames out of the best-effort UART FIFO so a continuous tty writer
   * cannot consume all queue space before the bus transaction is submitted. */
  ret = m5_rpc_frame_put(mcore, tx_buf, tx_len,
                         chn == M5_RPC_CHN_IIC1 ||
                         chn == M5_RPC_CHN_IIC2 ||
                         chn == M5_RPC_CHN_SPI);
  if (ret) {
    m5io_dbg("m5io-hub-core: DATA enqueue failed chn=%u pid=%u seq=%u ret=%d\n",
             chn, pid, seq, ret);
    m5_rpc_data_window_release(mcore, pid, chn, seq, ret);
  } else {
    m5io_dbg("m5io-hub-core: DATA enqueue chn=%u pid=%u seq=%u len=%u\n",
             chn, pid, seq, len);
  }
  return ret;
}
EXPORT_SYMBOL_GPL(m5io_hub_SendChnData);

int m5io_hub_send_data_async(struct m5io_hub *hub, unsigned int chn,
                             const u8 *data, unsigned int len)
{
  return m5io_hub_SendChnData(hub, chn, data, len);
}
EXPORT_SYMBOL_GPL(m5io_hub_send_data_async);

bool m5io_hub_data_pending(struct m5io_hub *hub, unsigned int chn)
{
  struct m5io_hub_core *mcore;
  if (!hub || !hub->spi || !chn || chn > M5IO_HUB_CHN_MAX)
    return false;
  mcore = container_of(hub, struct m5io_hub_core, hub);
  return !m5_rpc_data_channel_idle(mcore, chn);
}
EXPORT_SYMBOL_GPL(m5io_hub_data_pending);

static unsigned long m5io_deadline_left(unsigned long deadline)
{
  return time_before(jiffies, deadline) ? deadline - jiffies : 0;
}

/* HOST_READY is level-like on the board, but a short low pulse can still be
 * lost while Linux is between threaded IRQ runs.  Synchronous DATA users keep
 * a bounded fallback clock so a completed MCU ACK/result cannot sit forever
 * in the slave TX ring after that edge was missed. */
static void m5io_hub_sync_poll(struct m5io_hub *hub,
                               struct m5io_hub_core *mcore)
{
  if (!hub || !mcore || READ_ONCE(mcore->ota_active) ||
      m5io_hub_bus_held_by_other(mcore))
    return;
  m5io_hub_rpc_kick(mcore);
}

static int m5io_hub_spi_data_sync_limit(struct m5io_hub *hub,
                                          const u8 *tx, u8 *rx,
                                          unsigned int len,
                                          unsigned int chunk_limit)
{
  struct m5io_hub_core *mcore;
  struct m5_rpc_spi *rpc;
  struct m5io_hub_request begin = { .func = M5IO_HUB_FUNC_SPI, .arg = 2 };
  unsigned long flags, deadline;
  unsigned int off = 0;
  bool begun = false;
  int ret;
  u8 begin_payload[2];

  if (!hub || !hub->spi || !tx || !rx || !len || len > M5IO_HUB_SPI_MAX_LEN ||
      (chunk_limit != 255 && chunk_limit != 254))
    return -EINVAL;
  mcore = container_of(hub, struct m5io_hub_core, hub);
  rpc = &mcore->spi_rpc;
  mutex_lock(&rpc->lock);
  deadline = jiffies + msecs_to_jiffies(32000);
  spin_lock_irqsave(&mcore->rpc.token.state_lock, flags);
  rpc->data_waiting = true;
  rpc->expected_len = len;
  rpc->rx_len = 0;
  rpc->error = 0;
  spin_unlock_irqrestore(&mcore->rpc.token.state_lock, flags);

  begin_payload[0] = len & 0xff;
  begin_payload[1] = len >> 8;
  begin.payload = begin_payload;
  begin.payload_len = sizeof(begin_payload);
  ret = m5io_hub_exec(hub, &begin);
  if (ret || begin.status != 0) {
    ret = ret ? ret : -EIO;
    goto out;
  }
  begun = true;

  while (off < len) {
    unsigned int chunk = min_t(unsigned int, chunk_limit, len - off);
    do {
      if (!m5io_deadline_left(deadline)) {
        ret = -ETIMEDOUT;
        goto out;
      }
      ret = m5io_hub_SendChnData(hub, M5_RPC_CHN_SPI, tx + off, chunk);
      if (ret == -EAGAIN)
        usleep_range(1000, 2000);
    } while (ret == -EAGAIN);
    if (ret)
      goto out;
    while (!m5_rpc_data_channel_idle(mcore, M5_RPC_CHN_SPI)) {
      unsigned long left = m5io_deadline_left(deadline);

      if (!left) {
        ret = -ETIMEDOUT;
        goto out;
      }
      if (!wait_event_timeout(mcore->rpc.data.rx_wq,
                              m5_rpc_data_channel_idle(mcore,
                                                        M5_RPC_CHN_SPI),
                              min_t(unsigned long, left,
                                    msecs_to_jiffies(10))))
        m5io_hub_sync_poll(hub, mcore);
    }
    ret = READ_ONCE(mcore->rpc.data.channel_error[M5_RPC_CHN_SPI]);
    if (ret)
      goto out;
    off += chunk;
  }
  while (READ_ONCE(rpc->rx_len) < len && !READ_ONCE(rpc->error)) {
    unsigned long left = m5io_deadline_left(deadline);

    if (!left) {
      ret = -ETIMEDOUT;
      break;
    }
    if (!wait_event_timeout(rpc->wq,
                            READ_ONCE(rpc->rx_len) >= len ||
                            READ_ONCE(rpc->error),
                            min_t(unsigned long, left,
                                  msecs_to_jiffies(10))))
      m5io_hub_sync_poll(hub, mcore);
  }
  if (ret)
    ;
  else if (rpc->error)
    ret = rpc->error;
  else if (rpc->rx_len != len)
    ret = -EPROTO;
  else {
    while (!(m5_rpc_data_channel_idle(mcore, M5_RPC_CHN_SPI) &&
             m5_rpc_data_rx_channel_idle(mcore, M5_RPC_CHN_SPI))) {
      unsigned long left = m5io_deadline_left(deadline);

      if (!left) {
        ret = -ETIMEDOUT;
        break;
      }
      if (!wait_event_timeout(mcore->rpc.data.rx_wq,
                              m5_rpc_data_channel_idle(mcore,
                                                       M5_RPC_CHN_SPI) &&
                              m5_rpc_data_rx_channel_idle(mcore,
                                                          M5_RPC_CHN_SPI),
                              min_t(unsigned long, left,
                                    msecs_to_jiffies(10))))
        m5io_hub_sync_poll(hub, mcore);
    }
    if (!ret)
    memcpy(rx, rpc->rx, len);
  }
out:
  if (ret && begun) {
    int flush_ret = m5io_hub_spi_flush(hub);

    if (flush_ret)
      dev_warn_ratelimited(mcore->hub.dev,
                           "SPI DATA recovery flush failed: xfer=%d flush=%d\n",
                           ret, flush_ret);
  }
  spin_lock_irqsave(&mcore->rpc.token.state_lock, flags);
  rpc->data_waiting = false;
  spin_unlock_irqrestore(&mcore->rpc.token.state_lock, flags);
  mutex_unlock(&rpc->lock);
  return ret;
}

int m5io_hub_send_data_sync(struct m5io_hub *hub, unsigned int chn,
                            const u8 *tx, u8 *rx, unsigned int len)
{
  if (chn != M5_RPC_CHN_SPI)
    return -EINVAL;
  return m5io_hub_spi_data_sync_limit(hub, tx, rx, len, 255);
}
EXPORT_SYMBOL_GPL(m5io_hub_send_data_sync);

int m5io_hub_send_spi_sync(struct m5io_hub *hub, const u8 *tx, u8 *rx,
                           unsigned int len, unsigned int word_bits)
{
  return m5io_hub_spi_data_sync_limit(hub, tx, rx, len,
                                       word_bits == 16 ? 254 : 255);
}
EXPORT_SYMBOL_GPL(m5io_hub_send_spi_sync);

int m5io_hub_spi_flush(struct m5io_hub *hub)
{
  struct m5io_hub_request request = {
    .func = M5IO_HUB_FUNC_SPI,
    .arg = 8,
  };
  u8 payload = 0;
  int ret;

  if (!hub || !hub->spi)
    return -ENODEV;

  request.payload = &payload;
  request.payload_len = 1;
  ret = m5io_hub_exec(hub, &request);
  if (!ret && (request.status != 8 || request.reply_len != 4 ||
               request.reply[0] != 0))
    ret = -EPROTO;

  /* Drop host-side windows and de-duplication state after the MCU has had a
   * chance to discard its exchange.  This is channel-local and preserves
   * pending DATA history for I2C/UART/IR. */
  m5io_hub_data_abort_channel(hub, M5IO_HUB_CHN_SPI);
  return ret;
}
EXPORT_SYMBOL_GPL(m5io_hub_spi_flush);

int m5io_hub_i2c_reset(struct m5io_hub *hub, unsigned int bus)
{
  struct m5io_hub_request request = {
    .func = M5IO_HUB_FUNC_I2C,
    .arg = 7,
  };
  u8 payload;
  int ret;

  if (!hub || bus < 1 || bus > 2)
    return -EINVAL;
  payload = bus;
  request.payload = &payload;
  request.payload_len = 1;
  ret = m5io_hub_exec(hub, &request);
  if (ret)
    return ret;
  if (request.reply_len)
    return -EPROTO;
  switch (request.status) {
  case 0:
    return 0;
  case 3:
    return -EINVAL;
  case 4:
    return -EBUSY;
  default:
    return -EIO;
  }
}
EXPORT_SYMBOL_GPL(m5io_hub_i2c_reset);

int m5io_hub_i2c_data_sync(struct m5io_hub *hub, unsigned int chn,
                           const u8 *tx, unsigned int tx_len,
                           u8 *rx, unsigned int rx_cap,
                           unsigned int *rx_len)
{
  struct m5io_hub_core *mcore;
  struct m5_rpc_i2c_channel *i2c;
  unsigned long deadline;
  int ret;

  if (!hub || !hub->spi || !tx || !tx_len || tx_len > M5_RPC_DATA_MAX_PLEN ||
      chn < M5_RPC_CHN_IIC1 || chn > M5_RPC_CHN_IIC2 ||
      (rx_cap && (!rx || !rx_len)) || rx_cap > M5_RPC_DATA_MAX_PLEN)
    return -EINVAL;
  mcore = container_of(hub, struct m5io_hub_core, hub);
  i2c = &mcore->i2c.chan[chn - M5_RPC_CHN_IIC1];
  mutex_lock(&i2c->lock);
  m5io_dbg("m5io-hub-core: I2C sync begin chn=%u tx_len=%u rx_cap=%u\n",
           chn, tx_len, rx_cap);
  deadline = jiffies + msecs_to_jiffies(27500);
  spin_lock_irq(&mcore->rpc.token.state_lock);
  i2c->data_waiting = true;
  i2c->chn = chn;
  i2c->expected_len = rx_cap ? rx_cap : M5_RPC_DATA_MAX_PLEN;
  i2c->rx_len = 0;
  i2c->error = 0;
  spin_unlock_irq(&mcore->rpc.token.state_lock);

  do {
    if (!m5io_deadline_left(deadline)) {
      ret = -ETIMEDOUT;
      goto out;
    }
    ret = m5io_hub_SendChnData(hub, chn, tx, tx_len);
    if (ret)
      m5io_dbg("m5io-hub-core: I2C sync DATA submit chn=%u ret=%d\n",
               chn, ret);
    if (ret == -EAGAIN)
      usleep_range(1000, 2000);
  } while (ret == -EAGAIN);
  if (ret)
    goto out;
  m5io_dbg("m5io-hub-core: I2C sync DATA accepted chn=%u\n", chn);

  /* AB/AC is delivered before the original downlink window is released.
   * Wait for that ACK as well: it proves the MCU has finished the old I2C
   * operation before the caller can submit the next one on this channel.
   * Writes may produce AC errors too, and must not silently report success. */
  while (!(READ_ONCE(i2c->error) ||
           (m5_rpc_data_channel_idle(mcore, chn) &&
            m5_rpc_data_rx_channel_idle(mcore, chn) &&
            (READ_ONCE(mcore->rpc.data.channel_error[chn]) ||
             !rx_cap || READ_ONCE(i2c->rx_len))))) {
    unsigned long left = m5io_deadline_left(deadline);

    if (!left) {
      ret = -ETIMEDOUT;
      m5io_dbg("m5io-hub-core: I2C sync wait timeout chn=%u txwin=%#lx "
               "txidle=%d rxidle=%d rx_len=%u expected=%u error=%d cherr=%d\n",
               chn, READ_ONCE(mcore->rpc.data.tx_windows),
               m5_rpc_data_channel_idle(mcore, chn),
               m5_rpc_data_rx_channel_idle(mcore, chn),
               READ_ONCE(i2c->rx_len), READ_ONCE(i2c->expected_len),
               READ_ONCE(i2c->error),
               READ_ONCE(mcore->rpc.data.channel_error[chn]));
      goto out;
    }
    if (!wait_event_timeout(i2c->wq,
                            READ_ONCE(i2c->error) ||
                            (m5_rpc_data_channel_idle(mcore, chn) &&
                             m5_rpc_data_rx_channel_idle(mcore, chn) &&
                             (READ_ONCE(mcore->rpc.data.channel_error[chn]) ||
                              !rx_cap || READ_ONCE(i2c->rx_len))),
                            min_t(unsigned long, left,
                                  msecs_to_jiffies(10))))
      m5io_hub_sync_poll(hub, mcore);
  }
  ret = READ_ONCE(i2c->error);
  if (!ret)
    ret = READ_ONCE(mcore->rpc.data.channel_error[chn]);
  if (ret)
    goto out;
  if (i2c->rx_len >= 2 && i2c->rx[0] == 0xAC) {
    switch (i2c->rx[1]) {
    case 1: ret = -ENXIO; break;
    case 2: ret = -EREMOTEIO; break;
    case 3: ret = -EAGAIN; break;
    case 5: ret = -ETIMEDOUT; break;
    default: ret = -EIO; break;
    }
    goto out;
  }
  if (rx_cap) {
    *rx_len = i2c->rx_len;
    if (*rx_len > rx_cap)
      ret = -EPROTO;
    else
      memcpy(rx, i2c->rx, *rx_len);
  }
out:
  if (ret) {
    int reset_ret = 0;

    /* A DATA error leaves both the host window and the MCU's per-bus I2C
     * context uncertain.  Release the host state first, then use the
     * protocol-defined I2C_RESET to rebuild only this controller.  Preserve
     * the original transfer error for the caller.
     *
     * Address/data NACKs are a completed, well-formed I2C transaction.  They
     * are the expected result for an empty bus (and for a normal probe of an
     * absent address), so resetting the controller in that case only creates
     * an avoidable control TOKEN race with the next transfer.  Keep the
     * explicit reset for timeout, arbitration, protocol and transport errors
     * where the MCU context really can be left uncertain. */
    m5io_hub_data_abort_channel(hub, chn);
    if (ret != -ENXIO && ret != -EREMOTEIO)
      reset_ret = m5io_hub_i2c_reset(hub, chn);
    if (reset_ret)
      dev_warn_ratelimited(mcore->hub.dev,
                           "I2C%u recovery reset failed: xfer=%d reset=%d\n",
                           chn, ret, reset_ret);
  }
  spin_lock_irq(&mcore->rpc.token.state_lock);
  i2c->data_waiting = false;
  spin_unlock_irq(&mcore->rpc.token.state_lock);
  mutex_unlock(&i2c->lock);
  return ret;
}

EXPORT_SYMBOL_GPL(m5io_hub_i2c_data_sync);

/**
 * m5io_hub_register_chn_data_handler - 注册通道 DATA 接收处理函数
 * @hub: hub 句柄
 * @chn: DATA 通道
 * @handler: 裸 Payload 接收处理函数
 * @data: 传给 @handler 的私有数据
 *
 * 每个通道只允许一个处理函数。处理函数在可睡眠的工作队列上下文调用，
 * @buf 只在本次回调期间有效。处理函数在内部 handler 锁之外调用，可以
 * 睡眠；处理函数不得从 DATA 回调上下文调用
 * m5io_hub_unregister_chn_data_handler()。
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
 * 返回时已经没有该处理函数正在执行。不得从 DATA 回调上下文调用本函数，
 * 因为注销会同步等待已经开始的回调结束。
 */
void m5io_hub_unregister_chn_data_handler(
    struct m5io_hub *hub, unsigned int chn,
    m5io_hub_chn_data_handler_t handler, void *data) {
  struct m5io_hub_core *mcore;
  bool detached = false;

  if (!hub || !chn || chn > M5IO_HUB_CHN_MAX)
    return;

  mcore = container_of(hub, struct m5io_hub_core, hub);
  mutex_lock(&mcore->rpc.data.handler_lock);
  if (mcore->rpc.data.handler[chn] == handler &&
      mcore->rpc.data.handler_data[chn] == data) {
    mcore->rpc.data.handler[chn] = NULL;
    mcore->rpc.data.handler_data[chn] = NULL;
    detached = true;
  }
  mutex_unlock(&mcore->rpc.data.handler_lock);

  /*
   * The handler is detached before waiting, so no new callback can become
   * active for this registration.  Keep this wait outside handler_lock: the
   * callback decrements the in-flight count after it returns; it does not
   * need handler_lock to do so.
   */
  if (detached)
    wait_event(mcore->rpc.data.handler_wq[chn],
               !atomic_read(&mcore->rpc.data.handler_inflight[chn]));
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
 * m5io_hub_exec - execute a TOKEN and optional I2C/SPI/UART DATA transaction
 * @hub: hub 句柄
 * @request: request fields and terminal reply; buffers must live until return
 *
 * Context: sleepable; never call from a DATA/GPIO event callback.
 * Return: 0 on terminal success, a mapped firmware error, or a transport error.
 * An I2C timeout or protocol failure applies to the current request only;
 * subsequent requests are allowed to use the bus normally.
 */
int m5io_hub_exec(struct m5io_hub *hub, struct m5io_hub_request *request)
{
  struct m5io_hub_core *mcore;
  int tid, ret;

  if (!hub || !hub->spi)
    return -ENODEV;
  if (!request || request->func > M5IO_HUB_FUNC_MAX ||
      request->arg > M5_RPC_TOKEN_ARG_MASK ||
      request->payload_len > M5_RPC_TOKEN_MAX_PLEN ||
      (request->payload_len && !request->payload))
    return -EINVAL;
  if (request->data_len || request->data_tx || request->data_rx)
    return -EOPNOTSUPP;

  request->reply_len = 0;
  request->status = 0xff;
  mcore = container_of(hub, struct m5io_hub_core, hub);
  mutex_lock(&mcore->exec_lock);
  tid = m5_rpc_token_submit(mcore, request);
  if (tid < 0)
    ret = tid;
  else
    ret = m5_rpc_token_wait_result(mcore, tid, request);
  mutex_unlock(&mcore->exec_lock);
  return ret;
}
EXPORT_SYMBOL_GPL(m5io_hub_exec);

int m5io_hub_hello(struct m5io_hub *hub, u8 payload[15])
{
  struct m5io_hub_request req = {
    .func = M5_RPC_FUNC_PROTOCOL_CONTROL,
    .arg = 1,
  };
  int ret;

  if (!hub || !payload)
    return -EINVAL;
  ret = m5io_hub_exec(hub, &req);
  if (ret)
    return ret;
  if (req.reply_len != 15)
    return -EPROTO;
  memcpy(payload, req.reply, 15);
  return 0;
}
EXPORT_SYMBOL_GPL(m5io_hub_hello);

static int m5io_hub_mcu_reset_request(struct m5io_hub *hub,
                                       bool wait_for_app)
{
  struct m5io_hub_core *mcore;
  struct m5io_hub_request request = {
    .func = M5_RPC_FUNC_PROTOCOL_CONTROL,
    .arg = 3,
  };
  int ret;

  if (!hub || !hub->spi)
    return -ENODEV;
  mcore = container_of(hub, struct m5io_hub_core, hub);

  /* Explicit maintenance request: ordinary TOKEN allocation and deadline,
   * no automatic resend and no independent NRST fallback. */
  ret = m5io_hub_exec(hub, &request);
  if (ret)
    return ret;
  if (request.reply_len)
    return -EPROTO;
  ret = m5_rpc_response_status_to_errno(request.status);
  if (ret || !wait_for_app)
    return ret;

  /* The response is the last frame of the old MCU session.  The MCU resets
   * immediately after it leaves the response on SPI, so discard every host
   * side DATA/ACK/partial-frame record before the next caller can submit a
   * request.  Keeping the old SEQ, dedup and ACK state here lets the restarted
   * firmware see a new session while Linux still believes it is draining the
   * old one, which reproduces as a delayed cross-bus timeout after reset. */
  cancel_delayed_work_sync(&mcore->data_ack_retry_work);
  cancel_work_sync(&mcore->rpc_work);
  m5_rpc_rx_stream_reset(mcore);
  m5_rpc_token_session_reset(mcore);
  m5_rpc_data_state_reset(mcore);
  spin_lock_irq(&mcore->tx_lock);
  kfifo_reset(&mcore->tx_fifo);
  kfifo_reset(&mcore->sync_tx_fifo);
  spin_unlock_irq(&mcore->tx_lock);

  /* MCU_RESET is acknowledged before the firmware reaches its reset handler.
   * Keep the public recovery API from handing a still-booting MCU to the
   * next I2C/UART request.  OTA uses the raw variant below because its
   * bootloader listens only during the short window immediately after reset.
   * Poll HELLO instead of sleeping a fixed interval: the application startup
   * time varies with scheduler load, and callers must not begin a new DATA
   * session while the MCU is still in its reset/boot window. */
  {
    unsigned long deadline = jiffies + msecs_to_jiffies(5000);
    u8 hello[15];
    int hello_ret = -ETIMEDOUT;

    do {
      hello_ret = m5io_hub_hello(hub, hello);
      if (!hello_ret)
        return 0;
      if (hello_ret == -ENODEV || hello_ret == -EBUSY)
        return hello_ret;
      msleep(100);
    } while (time_before(jiffies, deadline));

    dev_warn(hub->dev, "MCU_RESET: application HELLO timeout: %d\n",
             hello_ret);
    return hello_ret ? hello_ret : -ETIMEDOUT;
  }
}

int m5io_hub_mcu_reset(struct m5io_hub *hub)
{
  unsigned int chn;

  /* Reset is a protocol session boundary.  Release every host-side DATA
   * window before asking the MCU to discard its matching state. */
  for (chn = 1; chn <= M5IO_HUB_CHN_MAX; chn++)
    m5io_hub_data_abort_channel(hub, chn);

  return m5io_hub_mcu_reset_request(hub, true);
}
EXPORT_SYMBOL_GPL(m5io_hub_mcu_reset);

static int m5io_hub_ota_spi_xfer(struct m5io_hub_core *mcore,
                                 const void *tx, void *rx, unsigned int len)
{
  struct spi_transfer transfer = {
    .tx_buf = tx,
    .rx_buf = rx,
    .len = len,
  };

  return spi_sync_transfer(mcore->hub.spi, &transfer, 1);
}

/*
 * Lock the m5hub data line for a whole upgrade.  From here until
 * m5io_hub_ota_bus_unlock() every other context is refused with -EBUSY, so
 * nothing can interleave with the IAP command stream.  Frames that were
 * already queued are dropped: their requests cannot be completed while the
 * firmware is being replaced, and letting them through would corrupt the IAP
 * sequence.  The calling thread stays the owner so that it is still allowed
 * to issue the APP reset request.
 */
static void m5io_hub_ota_bus_lock(struct m5io_hub_core *mcore)
{
  unsigned long flags;
  unsigned int i;
  bool busy;

  WARN_ON_ONCE(READ_ONCE(mcore->bus_owner));

  spin_lock_irqsave(&mcore->tx_lock, flags);
  mcore->bus_owner = current;
  m5_rpc_token_cancel_all(mcore, -ECANCELED);
  kfifo_reset(&mcore->tx_fifo);
  kfifo_reset(&mcore->sync_tx_fifo);
  spin_unlock_irqrestore(&mcore->tx_lock, flags);

  /* Stop work that may already be running with a frame in hand. */
  cancel_work_sync(&mcore->rpc_work);
  m5io_hub_data_ack_retry_cancel(mcore);
  for (i = 1; i <= M5IO_HUB_CHN_MAX; i++)
    m5io_hub_data_abort_channel(&mcore->hub, i);

  /*
   * A timed-out TOKEN is deliberately quarantined during normal operation:
   * its response may still be on the wire, so reusing that TID could complete
   * a different command.  OTA is the explicit maintenance boundary that
   * invalidates every old command.  First give cancelled callers a chance to
   * drop their references, then clock out any already queued responses while
   * the APP is still running.  Only after the physical worker is idle may the
   * cancelled slots be reclaimed for the MCU_RESET command.  This keeps the
   * normal four-TID lifetime rule intact while guaranteeing that a saturated
   * stress run cannot make the recovery command itself return -EBUSY.
   */
  for (i = 0; i < 100; ++i) {
    busy = false;
    spin_lock_irqsave(&mcore->rpc.token.state_lock, flags);
    for (unsigned int tid = 0; tid < M5IO_HUB_RPC_TID_NR; ++tid) {
      const struct m5_rpc_token_slot *slot =
          &mcore->rpc.token.slot[tid];
      if (slot->caller || slot->queued || slot->in_flight) {
        busy = true;
        break;
      }
    }
    spin_unlock_irqrestore(&mcore->rpc.token.state_lock, flags);
    if (!busy)
      break;
    usleep_range(1000, 2000);
  }

  for (i = 0; i < M5IO_HUB_RPC_TID_NR * 2; ++i) {
    int drain_ret = m5io_hub_rpc_transaction(mcore);
    bool pending = false;

    if (drain_ret)
      break;
    spin_lock_irqsave(&mcore->rpc.token.state_lock, flags);
    for (unsigned int tid = 0; tid < M5IO_HUB_RPC_TID_NR; ++tid) {
      const struct m5_rpc_token_slot *slot =
          &mcore->rpc.token.slot[tid];
      if (slot->state == M5_TOKEN_QUARANTINED && slot->sent &&
          !slot->final_received) {
        pending = true;
        break;
      }
    }
    spin_unlock_irqrestore(&mcore->rpc.token.state_lock, flags);
    if (!pending)
      break;
  }

  spin_lock_irqsave(&mcore->rpc.token.state_lock, flags);
  for (i = 0; i < M5IO_HUB_RPC_TID_NR; ++i) {
    struct m5_rpc_token_slot *slot = &mcore->rpc.token.slot[i];

    if (!slot->caller && !slot->queued && !slot->in_flight) {
      slot->state = M5_TOKEN_FREE;
      slot->sent = false;
      slot->final_received = false;
      slot->rx_len = 0;
    }
  }
  spin_unlock_irqrestore(&mcore->rpc.token.state_lock, flags);
}

/* Release the data line so normal m5hub traffic can resume. */
static void m5io_hub_ota_bus_unlock(struct m5io_hub_core *mcore)
{
  unsigned long flags;

  spin_lock_irqsave(&mcore->tx_lock, flags);
  mcore->bus_owner = NULL;
  spin_unlock_irqrestore(&mcore->tx_lock, flags);
}

/*
 * Take exclusive ownership of the SPI bus for the IAP command stream.
 * ota_active also tells the shared IRQ thread to stop running RPC
 * transactions and to treat HOST_READY as the IAP response signal only.
 */
static void m5io_hub_ota_begin(struct m5io_hub_core *mcore)
{
  WRITE_ONCE(mcore->ota_active, true);
  cancel_work_sync(&mcore->rpc_work);
  /* Wait for an IRQ transaction which was already in progress. */
  mutex_lock(&mcore->spi_lock);
  /*
   * Purge pending DATA ACKs only now that the bus is exclusively ours.  An IRQ
   * transaction that was still running when ota_active was raised could
   * otherwise queue one more retry behind an earlier purge, and that stale ACK
   * would then be delivered to the MCU we just reset.  The retry worker never
   * takes spi_lock, so cancelling it here cannot deadlock.
   */
  m5io_hub_data_ack_retry_cancel(mcore);
  /* An aborted RPC transaction may have left a partial RX frame behind. */
  m5_rpc_rx_stream_reset(mcore);
}

/*
 * Leave the raw IAP command stream and hand the SPI bus back.  The m5hub data
 * line itself stays locked by the caller until m5io_hub_ota_bus_unlock().
 */
static void m5io_hub_ota_end(struct m5io_hub_core *mcore)
{
  mutex_unlock(&mcore->spi_lock);
  WRITE_ONCE(mcore->ota_active, false);
  /* Drop frames queued by normal RPC users before the reset. */
  spin_lock_irq(&mcore->tx_lock);
  kfifo_reset(&mcore->tx_fifo);
  kfifo_reset(&mcore->sync_tx_fifo);
  spin_unlock_irq(&mcore->tx_lock);
}

/*
 * Clock out a reply left pending by an interrupted upgrade.  The IAP accepts
 * only an exact eight-byte OPC_READ while a reply is pending and silently
 * discards every other command, so this must run before OPC_IAP is sent.
 * A free-running read returns 0xff when nothing was pending and is otherwise
 * harmless: the IAP re-arms its DMA after each transaction.
 */
static int m5io_hub_ota_drain(struct m5io_hub_core *mcore,
                              struct m5io_hub_ota_request *result)
{
  u8 read_command[M5IO_HUB_IAP_RESPONSE_SIZE];
  u8 response[M5IO_HUB_IAP_RESPONSE_SIZE];
  int ret;

  memset(read_command, 0xff, sizeof(read_command));
  read_command[0] = M5IO_HUB_IAP_READ;
  memset(response, 0xff, sizeof(response));

  ret = m5io_hub_ota_spi_xfer(mcore, read_command, response,
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
  dev_info(mcore->hub.dev,
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
static int m5io_hub_ota_poke(struct m5io_hub_core *mcore)
{
  u8 dummy[M5IO_HUB_IAP_RESPONSE_SIZE];
  u8 discard[M5IO_HUB_IAP_RESPONSE_SIZE];
  int ret;

  memset(dummy, 0xff, sizeof(dummy));
  memset(discard, 0, sizeof(discard));

  ret = m5io_hub_ota_spi_xfer(mcore, dummy, discard, sizeof(dummy));
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
static int m5io_hub_ota_probe(struct m5io_hub_core *mcore,
                              struct m5io_hub_ota_request *result)
{
  u8 read_command[M5IO_HUB_IAP_RESPONSE_SIZE];
  u8 response[M5IO_HUB_IAP_RESPONSE_SIZE];
  int ret;

  memset(read_command, 0xff, sizeof(read_command));
  read_command[0] = M5IO_HUB_IAP_READ;
  memset(response, 0, sizeof(response));

  ret = m5io_hub_ota_spi_xfer(mcore, read_command, response,
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

static int m5io_hub_ota_command(struct m5io_hub_core *mcore,
                                const u8 *command, unsigned int command_len,
                                u8 expected_command, u32 expected_address,
                                u16 expected_length,
                                unsigned int timeout_ms,
                                struct m5io_hub_ota_request *result)
{
  u8 read_command[M5IO_HUB_IAP_RESPONSE_SIZE];
  u8 response[M5IO_HUB_IAP_RESPONSE_SIZE];
  unsigned long timeout;
  unsigned int poll;
  bool ready;
  int ret;

  reinit_completion(&mcore->ota_ready);
  ret = m5io_hub_ota_spi_xfer(mcore, command, NULL, command_len);
  if (ret)
    return ret;

  timeout = wait_for_completion_timeout(&mcore->ota_ready,
                                        msecs_to_jiffies(timeout_ms));
  ready = timeout != 0;

  /*
   * Some board revisions expose HOST_READY as a level IRQ that can be lost
   * while the normal RPC worker is being stopped for OTA.  If the IRQ did not
   * arrive, probe the IAP response directly.  During erase/program the IAP
   * DMA is stopped, so these short reads are ignored; once the response is
   * published they return the normal eight-byte response.
   */
  for (poll = 0; !ready && poll < timeout_ms; poll++) {
    memset(read_command, 0xff, sizeof(read_command));
    read_command[0] = M5IO_HUB_IAP_READ;
    memset(response, 0xff, sizeof(response));
    ret = m5io_hub_ota_spi_xfer(mcore, read_command, response,
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
    dev_warn(mcore->hub.dev,
             "OTA command 0x%02x timed out waiting for HOST_READY (irq=%d)\n",
             expected_command, mcore->hub.irq);
    return -ETIMEDOUT;
  }

  if (timeout) {
    memset(read_command, 0xff, sizeof(read_command));
    read_command[0] = M5IO_HUB_IAP_READ;
    memset(response, 0, sizeof(response));
    ret = m5io_hub_ota_spi_xfer(mcore, read_command, response,
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
    dev_warn(mcore->hub.dev,
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
 * Firmware upgrade entry.  Three failures were found and fixed here, all of
 * which made an upgrade impossible; the measured evidence is in
 * OTA-BRINGUP-REPORT.md next to this file.
 *
 *  1. The bus must be frozen the moment the reset is acknowledged, before any
 *     settle delay.  The bootloader only listens for 300 ms after the reset and
 *     never extends that window, and an aborted RPC transaction used to keep
 *     clocking the bus for up to a second (see m5io_hub_rpc_transaction), which
 *     spent the whole window.  Measured: OPC_IAP left the driver 1.77 s late.
 *
 *  2. Bytes sent after the reset request postpone the reset itself.  The APP
 *     runs NVIC_SystemReset() only once its reply has been clocked all the way
 *     out, and its parser takes an early return -- ahead of that code -- while
 *     an incomplete frame candidate and a newer transaction are both pending.
 *     So the reset request can be acknowledged and still never happen; the
 *     probe transaction below is what promotes it, and a burst of retries would
 *     hold it off forever.
 *
 *  3. A dropped command and an absent MCU look identical, because a command the
 *     bootloader never receives is answered by nobody.  The probe distinguishes
 *     them by status, not by command: the bootloader replies to an unknown
 *     opcode with BAD_COMMAND, whose response command byte is also 0xff, so
 *     testing the command byte would never match.  Only then is OPC_IAP sent.
 *
 * Measured on a healthy board: reset acknowledged at 4 ms, bootloader detected
 * at 24 ms, OPC_IAP on the wire at 44 ms -- inside the 300 ms window.  A board
 * whose MCU accepted the reset request but never executed it failed all of this
 * and needed replacing, so treat "reset acknowledged, probes all silent" as a
 * hardware suspect rather than a timing bug.
 */
static int m5io_hub_ota_update(struct m5io_hub_core *mcore,
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
  m5io_hub_ota_bus_lock(mcore);

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

  dev_info(mcore->hub.dev, "OTA start: image=%u bytes timeout=%u ms irq=%d\n",
           request->image_len, timeout_ms, mcore->hub.irq);

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
    dev_info(mcore->hub.dev, "OTA: waiting up to 60 seconds for external MCU reset\n");
    reset_ret = -EOPNOTSUPP;
  } else {
    /* OTA must enter the bootloader window immediately after the reset
     * response.  Do not use the public recovery wrapper's app-ready delay. */
    reset_ret = m5io_hub_mcu_reset_request(&mcore->hub, false);
    /* A definite refusal means the APP is still running. Never inject raw
     * IAP bytes after BUSY/UNSUPPORTED/INVALID or steal quarantined TIDs.
     * A timeout is uncertain and may mean the MCU already entered IAP. */
    if (reset_ret && reset_ret != -ETIMEDOUT) {
      ret = reset_ret;
      goto out_free_packet;
    }
  }
  if (reset_ret)
    dev_info(mcore->hub.dev,
             "OTA: APP reset not acknowledged after %ums (%d), probing for a "
             "bootloader already parked in IAP\n",
             jiffies_to_msecs(jiffies - entry_start), reset_ret);
  else
    dev_info(mcore->hub.dev, "OTA: APP reset acknowledged after %ums\n",
             jiffies_to_msecs(jiffies - entry_start));

  /*
   * Step 2: freeze the bus at once.  The bootloader only listens for OPC_IAP
   * for 300 ms after the reset, so every millisecond spent clocking the bus
   * between here and the command below is taken away from that window.
   * Freezing first also aborts the RPC transaction which is still draining
   * with the resetting MCU on MISO, which would otherwise keep the bus busy
   * for up to a second.  Step 1 cannot succeed once an upgrade has been
   * interrupted: the IAP keeps iap_active set until a real reset, so the MCU
   * stays in the bootloader and no longer answers APP-side RPCs.  The static
   * command stream below works in both states, which is what makes the
   * interrupted case recoverable.
   */
  m5io_hub_ota_begin(mcore);

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
    if (m5io_hub_ota_poke(mcore) || m5io_hub_ota_probe(mcore, request)) {
      dev_warn(mcore->hub.dev, "OTA probe transfer failed\n");
      ret = -EIO;
      goto out_unlock_ota;
    }
    dev_dbg(mcore->hub.dev,
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
      dev_info(mcore->hub.dev,
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
    dev_info(mcore->hub.dev,
             "OTA: OPC_IAP attempt %u/%u, %ums after OTA start "
             "(bootloader window is 300 ms)\n",
             attempt, M5IO_HUB_IAP_ENTRY_ATTEMPTS,
             jiffies_to_msecs(jiffies - entry_start));

    ret = m5io_hub_ota_command(mcore, packet, 1, M5IO_HUB_IAP_ENTRY,
                               M5IO_HUB_OTA_START_ADDRESS, 0,
                               M5IO_HUB_IAP_ENTRY_PROBE_TIMEOUT_MS, request);
    if (!ret)
      break;
    if (ret != -ETIMEDOUT) {
      dev_warn(mcore->hub.dev,
               "OTA: IAP entry rejected, reply cmd=0x%02x status=%u "
               "addr=0x%08x\n",
               request->response_command, request->response_status,
               request->response_address);
      break;
    }
    if (((attempt % M5IO_HUB_IAP_ENTRY_DRAIN_EVERY) == 0) &&
        m5io_hub_ota_drain(mcore, request))
      break;
  }

  if (ret) {
    if (ret == -ETIMEDOUT)
      dev_warn(mcore->hub.dev,
               "OTA: OPC_IAP got no answer in %u attempts over %ums; the MCU "
               "answers neither the APP reset nor the bootloader, power-cycle "
               "the device to recover\n",
               M5IO_HUB_IAP_ENTRY_ATTEMPTS,
               jiffies_to_msecs(jiffies - entry_start));
    goto out_unlock_ota;
  }

  dev_info(mcore->hub.dev, "OTA: IAP entered, writing %u pages\n",
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

    ret = m5io_hub_ota_command(mcore, packet,
                               M5IO_HUB_IAP_HEADER_SIZE +
                                   M5IO_HUB_OTA_PAGE_SIZE,
                               M5IO_HUB_IAP_WREN, address,
                               M5IO_HUB_OTA_PAGE_SIZE, timeout_ms, request);
    if (ret)
      goto out_unlock_ota;
    request->completed_pages = page + 1;
  }

  packet[0] = M5IO_HUB_IAP_START;
  ret = m5io_hub_ota_command(mcore, packet, 1, M5IO_HUB_IAP_START,
                             M5IO_HUB_OTA_START_ADDRESS, 0, timeout_ms,
                             request);

out_unlock_ota:
  m5io_hub_ota_end(mcore);
out_free_packet:
  kfree(packet);
out_free_image:
  kfree(image);
out_unlock_bus:
  m5io_hub_ota_bus_unlock(mcore);

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
      hello_ret = m5io_hub_hello(&mcore->hub, hello_payload);
      dev_info(mcore->hub.dev,
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
  struct m5io_hub_core *mcore = container_of(misc, struct m5io_hub_core,
                                              ota_misc);
  struct m5io_hub_ota_request request;
  long ret;

  if (command != M5IO_HUB_IOCTL_OTA)
    return -ENOTTY;
  if (copy_from_user(&request, (void __user *)argument, sizeof(request)))
    return -EFAULT;

  mutex_lock(&mcore->ota_lock);
  if (READ_ONCE(mcore->ota_active)) {
    ret = -EBUSY;
  } else {
    ret = m5io_hub_ota_update(mcore, &request);
  }
  mutex_unlock(&mcore->ota_lock);

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
  struct m5io_hub_core *mcore = data;

  misc_deregister(&mcore->ota_misc);
}

int m5io_hub_data_status(struct m5io_hub *hub, u8 payload[15])
{
  struct m5io_hub_request req = {
    .func = M5_RPC_FUNC_PROTOCOL_CONTROL,
    .arg = M5_RPC_ARG_DATA_STATUS,
  };
  int ret;

  if (!hub || !payload)
    return -EINVAL;
  ret = m5io_hub_exec(hub, &req);
  if (ret)
    return ret;
  if (req.reply_len != 15)
    return -EPROTO;
  memcpy(payload, req.reply, 15);
  return 0;
}
EXPORT_SYMBOL_GPL(m5io_hub_data_status);

static int m5_rpc_token_gpio_exec(struct m5io_hub *hub, u8 subcmd,
                                  const u8 *payload, u8 plen, u8 *rx,
                                  unsigned int *rx_len) {
  struct m5io_hub_request request = {
    .func = M5_RPC_FUNC_GPIO,
    .arg = subcmd,
    .payload = payload,
    .payload_len = plen,
  };
  int ret = m5io_hub_exec(hub, &request);

  if (ret)
    return ret;
  if (rx && rx_len) {
    *rx_len = request.reply_len;
    memcpy(rx, request.reply, request.reply_len);
  } else if (request.reply_len) {
    return -EPROTO;
  }
  return 0;
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

  if (rx_len != 1 || rx[0] > 1)
    return -EPROTO;

  return rx[0];
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
  u8 wire_type;
  int ret;

  if (!hub || !hub->spi)
    return -ENODEV;

  ret = m5io_hub_check_pin(pin);
  if (ret)
    return ret;

  switch (type) {
  case IRQ_TYPE_EDGE_FALLING:
    /* MCU protocol values are FALLING=0, RISING=1, BOTH=2; they do not
     * match Linux IRQ_TYPE_* bit values. */
    wire_type = M5_RPC_GPIO_EDGE_FALLING;
    break;
  case IRQ_TYPE_EDGE_RISING:
    wire_type = M5_RPC_GPIO_EDGE_RISING;
    break;
  case IRQ_TYPE_EDGE_BOTH:
    wire_type = M5_RPC_GPIO_EDGE_BOTH;
    break;
  default:
    return -EINVAL;
  }

  cmd[0] = pin;
  cmd[1] = wire_type;
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

  cancel_delayed_work_sync(&mcore->data_ack_retry_work);
  cancel_work_sync(&mcore->rpc_work);
  /* rpc_work may have queued an ACK while the first cancellation was active. */
  cancel_delayed_work_sync(&mcore->data_ack_retry_work);
  cancel_work_sync(&mcore->data_rx_work);
  cancel_work_sync(&mcore->gpio_irq_work);
  m5_rpc_rx_stream_reset(mcore);
  m5_rpc_data_state_reset(mcore);
}

/**
 * m5io_hub_probe - SPI 设备探测
 * @spi: 绑定到 m5io-hub 的 SPI 设备
 *
 * 分配并初始化核心上下文（RPC 槽位、RX/TX FIFO、等待队列），配置 SPI
 * 模式后申请线程化 IRQ，最后按设备树注册 GPIO、GPIO LEDs、UART、I2C
 * 和 SPI 子设备。资源均通过 devm 接口申请，unbind 时自动释放。
 *
 * Return: 0 表示成功；负值为错误码（内存分配、spi_setup()、IRQ 申请或
 *         设备树子设备注册失败）。
 */
/* ------------------------------------------------------------------------ *
 * MCU diagnostic log channel (CH = M5IO_HUB_CHN_LOG)
 *
 * The firmware writes plain text on this uplink-only channel.  The driver
 * accumulates it and exposes it as /proc/m5io-hub-log.  A read returns what
 * has accumulated and clears it, so `cat` shows everything logged since the
 * previous read.  The buffer is deliberately lossy: dropping log bytes is
 * always better than stalling the SPI transaction that carries them.
 * ------------------------------------------------------------------------ */

static void m5io_hub_log_rx(struct m5io_hub_core *mcore, const u8 *buf,
                            unsigned int len) {
  unsigned int drop;

  if (!len)
    return;

  mutex_lock(&mcore->log_lock);
  if (len > M5IO_HUB_LOG_BUF_SIZE) {
    drop = len - M5IO_HUB_LOG_BUF_SIZE;
    buf += drop;
    len = M5IO_HUB_LOG_BUF_SIZE;
    mcore->log_dropped += drop;
  }
  if (mcore->log_len + len > M5IO_HUB_LOG_BUF_SIZE) {
    drop = mcore->log_len + len - M5IO_HUB_LOG_BUF_SIZE;
    memmove(mcore->log_buf, mcore->log_buf + drop, mcore->log_len - drop);
    mcore->log_len -= drop;
    mcore->log_dropped += drop;
  }
  memcpy(mcore->log_buf + mcore->log_len, buf, len);
  mcore->log_len += len;
  mutex_unlock(&mcore->log_lock);
}

static ssize_t m5io_hub_log_proc_read(struct file *file, char __user *ubuf,
                                      size_t count, loff_t *ppos) {
  struct m5io_hub_core *mcore = pde_data(file_inode(file));
  unsigned int n;
  int ret;

  if (!count)
    return 0;

  mutex_lock(&mcore->log_lock);
  n = min_t(unsigned int, count, mcore->log_len);
  if (n) {
    if (copy_to_user(ubuf, mcore->log_buf, n)) {
      ret = -EFAULT;
    } else {
      memmove(mcore->log_buf, mcore->log_buf + n, mcore->log_len - n);
      mcore->log_len -= n;
      ret = n;
    }
  } else if (mcore->log_dropped) {
    /* Keep truncation visible instead of silently losing lines. */
    char note[80];
    int len = scnprintf(note, sizeof(note),
                        "[m5io-hub-log] dropped %u bytes\n",
                        mcore->log_dropped);

    mcore->log_dropped = 0;
    if (len > (int)count || copy_to_user(ubuf, note, len))
      ret = -EFAULT;
    else
      ret = len;
  } else {
    ret = 0;
  }
  mutex_unlock(&mcore->log_lock);
  return ret;
}

static const struct proc_ops m5io_hub_log_proc_ops = {
  .proc_read = m5io_hub_log_proc_read,
  .proc_lseek = noop_llseek,
};

static void m5io_hub_log_proc_remove(void *data) {
  struct m5io_hub_core *mcore = data;

  if (mcore->log_proc) {
    proc_remove(mcore->log_proc);
    mcore->log_proc = NULL;
  }
}

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
  {
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(mcore->i2c.chan); i++) {
      mutex_init(&mcore->i2c.chan[i].lock);
      init_waitqueue_head(&mcore->i2c.chan[i].wq);
      mcore->i2c.chan[i].tid = -1;
      mcore->i2c.chan[i].chn = M5_RPC_CHN_IIC1 + i;
    }
  }
  mutex_init(&mcore->spi_rpc.lock);
  init_waitqueue_head(&mcore->spi_rpc.wq);
  mcore->spi_rpc.data_waiting = false;
  mutex_init(&mcore->ota_lock);
  mutex_init(&mcore->log_lock);
  init_completion(&mcore->ota_ready);
  mcore->ota_active = false;

  mcore->spi_rpc.tx_buf = devm_kmalloc(&spi->dev, M5IO_HUB_SPI_TX_BUF_LEN,
                                       GFP_KERNEL);
  if (!mcore->spi_rpc.tx_buf)
    return -ENOMEM;
  mcore->rpc_xfer_tx_buf = devm_kmalloc(&spi->dev, M5IO_HUB_FIFO_SIZE,
                                        GFP_KERNEL);
  if (!mcore->rpc_xfer_tx_buf)
    return -ENOMEM;
  mcore->rpc_xfer_rx_buf = devm_kmalloc(&spi->dev, M5IO_HUB_FIFO_SIZE,
                                        GFP_KERNEL);
  if (!mcore->rpc_xfer_rx_buf)
    return -ENOMEM;

  /* RPC 工作项：调用方走 worker；IRQ 在线程化中断中激活排空事务 */
  {
    int i;

    INIT_WORK(&mcore->rpc_work, m5io_hub_rpc_worker);
    INIT_DELAYED_WORK(&mcore->data_ack_retry_work,
                      m5_rpc_data_ack_retry_work);
    INIT_WORK(&mcore->data_rx_work, m5_rpc_data_rx_worker);
    INIT_WORK(&mcore->gpio_irq_work, m5io_hub_gpio_irq_worker);
    mutex_init(&mcore->spi_lock);
    mutex_init(&mcore->exec_lock);
    mutex_init(&mcore->rpc.data.handler_lock);
    mutex_init(&mcore->gpio_irq_handler_lock);
    spin_lock_init(&mcore->gpio_irq_pending_lock);
    INIT_KFIFO(mcore->gpio_irq_fifo);
    spin_lock_init(&mcore->rpc.data.tx_window_lock);
    mcore->rpc.data.next_seq = 0;
    spin_lock_init(&mcore->rpc.data.ack_retry_lock);
    INIT_KFIFO(mcore->rpc.data.ack_retry_fifo);
    spin_lock_init(&mcore->rpc.data.ack_tx_lock);
    INIT_KFIFO(mcore->rpc.data.ack_tx_fifo);
    spin_lock_init(&mcore->rpc.token.state_lock);
    spin_lock_init(&mcore->rpc_schedule_lock);
    mcore->rpc_work_kick = false;
    mcore->rpc_work_running = false;
    for (i = 0; i < ARRAY_SIZE(mcore->rpc.data.handler_wq); i++) {
      init_waitqueue_head(&mcore->rpc.data.handler_wq[i]);
      atomic_set(&mcore->rpc.data.handler_inflight[i], 0);
    }
    for (i = 0; i < ARRAY_SIZE(mcore->rpc.token.slot); i++)
      init_completion(&mcore->rpc.token.slot[i].done);
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

    fifo_buf = devm_kmalloc(&spi->dev, M5IO_HUB_FIFO_SIZE, GFP_KERNEL);
    if (!fifo_buf)
      return -ENOMEM;

    ret = kfifo_init(&mcore->sync_tx_fifo, fifo_buf,
                     M5IO_HUB_FIFO_SIZE);
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

  snprintf(mcore->ota_misc_name, sizeof(mcore->ota_misc_name),
           "m5io-hub%d", spi_get_chipselect(spi, 0));
  mcore->ota_misc.minor = MISC_DYNAMIC_MINOR;
  mcore->ota_misc.name = mcore->ota_misc_name;
  mcore->ota_misc.fops = &m5io_hub_misc_fops;
  mcore->ota_misc.parent = &spi->dev;
  mcore->ota_misc.mode = 0660;
  ret = misc_register(&mcore->ota_misc);
  if (ret) {
    dev_err(&spi->dev, "failed to register OTA device: %d\n", ret);
    return ret;
  }
  ret = devm_add_action_or_reset(&spi->dev, m5io_hub_misc_deregister,
                                 mcore);
  if (ret)
    return ret;

  mcore->log_proc = proc_create_data(M5IO_HUB_LOG_PROC_NAME, 0444, NULL,
                                     &m5io_hub_log_proc_ops, mcore);
  if (!mcore->log_proc) {
    dev_warn(&spi->dev, "failed to create /proc/%s\n",
             M5IO_HUB_LOG_PROC_NAME);
  } else {
    ret = devm_add_action_or_reset(&spi->dev, m5io_hub_log_proc_remove,
                                   mcore);
    if (ret)
      return ret;
  }

  /* Runs after DT child devices and the devm IRQ have been removed. */
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

  /* All child devices are described by enabled nodes in the device tree. */
  ret = devm_of_platform_populate(&spi->dev);
  if (ret) {
    dev_err(&spi->dev, "failed to populate DT child devices: %d\n", ret);
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

MODULE_DESCRIPTION("M5Stack M5IO-HUB core driver");
MODULE_LICENSE("GPL");
