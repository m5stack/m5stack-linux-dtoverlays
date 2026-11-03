// SPDX-License-Identifier: GPL-2.0
/*
 * M5Stack M5IO-HUB virtual UART driver
 *
 * UART 引脚在 M5IO-HUB MCU 上，Linux 侧没有寄存器也没有中断线：
 *   - 配置/关闭走 FUNC=4 的 TOKEN（m5io_hub_uart_config / _reset）；
 *   - 收发字节走 DATA 通道（m5io_hub_uart_send_data / register_rx），
 *     协议细节、DATA 窗口与 ACK 都由 core 处理；
 *   - 发送窗口是否为空用 m5io_hub_uart_tx_empty() 查询（MCU 侧 FIFO 可能
 *     还在排空，只靠驱动自己的计数判断不了）；
 *   - 停止/恢复发送用 m5io_hub_uart_stop_tx()/_start_tx()。uart_ops 的
 *     stop_tx/start_tx 持有 port->lock 且不可睡眠，所以驱动只记录状态并合并
 *     请求，由 tx_work 在进程上下文转发；只有真正停过才补 START，正常写路径
 *     不额外多发 TOKEN；
 *   - 停止接收用 m5io_hub_uart_stop_rx()，同样由 tx_work 在进程上下文转发
 *     （uart_ops.stop_rx 也是持 port->lock 的原子上下文）；
 *   - tx_empty 的回调契约是“不可睡眠”，不能直接查 core（core 全程可能睡眠），
 *     由 tx_empty_work 异步查询 MCU 并缓存结果，回调只读缓存。
 * TX 放在 work item 里做，因为配置与发送都可能睡眠，不能占用 uart_port
 * 的 spinlock；RX 回调在 core 的工作队列上下文里直接送 tty flip buffer。
 * API 里的 bus 就是 USART1/2（1/2）。RX 的“已配置/已使能”门控在 core
 * （register_rx 即打开 RX 门），驱动只在 CREAD=0 或 tty 未打开时本地丢弃。
 * API 不回读实际生效的波特率，tty 侧按请求值计算 timeout。
 */

#include <linux/atomic.h>
#include <linux/kfifo.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/serial_core.h>
#include <linux/slab.h>
#include <linux/tty.h>
#include <linux/tty_flip.h>
#include <linux/workqueue.h>

#include "m5io-hub.h"

#define M5IO_HUB_UART_NAME  "m5io-hub-uart"
#define M5IO_HUB_TTY_NAME   "ttyM5HUB"
#define M5IO_HUB_UART_NR    2
#define M5IO_HUB_UART_MAX_TX 1024
/* 协议 DATA 单片上限 255 字节；超过要拆帧，由驱动按片提交。 */
#define M5IO_HUB_UART_CHUNK 255U
#define M5IO_HUB_UART_MIN_BAUD 1200U
#define M5IO_HUB_UART_MAX_BAUD 1000000U
/* MCU 在 RX 还有排队数据时会用 BUSY 拒绝 CONFIG，稍后重试即可。 */
#define M5IO_HUB_UART_CFG_BUSY_TRIES 6U
#define M5IO_HUB_UART_CFG_BUSY_DELAY_US 20000U
/* tx_empty 缓存：有人查询后的刷新窗口，以及窗口内的轮询间隔。 */
#define M5IO_HUB_UART_TX_EMPTY_WINDOW_MS 2000U
#define M5IO_HUB_UART_TX_EMPTY_POLL_MS 10U

struct m5io_hub_uart;

enum m5io_hub_uart_tx_cmd {
  M5IO_HUB_UART_TX_CMD_NONE = 0,
  M5IO_HUB_UART_TX_CMD_STOP,
  M5IO_HUB_UART_TX_CMD_START,
};

struct m5io_hub_uart_port {
  struct uart_port port;
  struct m5io_hub_uart *mu;
  struct delayed_work tx_work;
  struct mutex config_lock;
  atomic_t tx_enabled;
  atomic_t tx_inflight;
  atomic_t tx_pending;
  /* uart_ops.stop_tx/start_tx 在原子上下文触发，下行命令由 tx_work 转发。 */
  atomic_t tx_cmd_pending;  /* enum m5io_hub_uart_tx_cmd，后到的命令覆盖先前的 */
  atomic_t tx_hold;         /* 1 = tty 要求停发（用于判断恢复时是否需要 START） */
  atomic_t mcu_tx_stopped;  /* 1 = MCU 已收到 STOP、尚未收到 START */
  /* uart_ops.stop_rx 同样在原子上下文触发；收到的 RX 停止命令由 tx_work 转发。 */
  atomic_t rx_stop_pending;
  atomic_t mcu_rx_stopped;  /* 1 = MCU 已收到 RX 停止命令 */
  atomic_t rx_enabled;
  /* tx_empty 的异步缓存：回调是原子上下文，不能睡眠查 MCU。 */
  struct delayed_work tx_empty_work;
  atomic_t tx_empty_valid; /* 1 = 缓存可信 */
  atomic_t tx_empty;       /* 1 = MCU 发送窗口为空 */
  unsigned long tx_empty_deadline; /* 仍有人查询的时间窗 */
  /* uart_flush_buffer()（TCFLSH）的代号：tx_work 用它判断 peek 的数据是否作废。 */
  unsigned int xmit_gen;
  u8 bus; /* API 端口号：1=USART1，2=USART2 */
  u8 tx_buf[M5IO_HUB_UART_CHUNK];
  bool registered;    /* uart_add_one_port() 成功 */
  bool rx_registered; /* m5io_hub_uart_register_rx() 生效中 */
  bool configured;
  bool io_failed;
  tcflag_t confirmed_cflag;
  unsigned int confirmed_baud;
};

struct m5io_hub_uart {
  struct m5io_hub *hub;
  struct m5io_hub_uart_port ports[M5IO_HUB_UART_NR];
};

static struct uart_driver m5io_hub_uart_driver = {
    .owner = THIS_MODULE,
    .driver_name = M5IO_HUB_UART_NAME,
    .dev_name = M5IO_HUB_TTY_NAME,
    .major = 0,
    .minor = 0,
    .nr = M5IO_HUB_UART_NR,
};

static struct m5io_hub_uart_port *m5io_hub_uart_from_port(struct uart_port *port)
{
  return container_of(port, struct m5io_hub_uart_port, port);
}

/*
 * uart_ops.tx_empty 的契约是“不可睡眠”（调用方可能持有 port->lock），而
 * core 的所有 API 都会睡眠（mutex/kzalloc/wait_for_completion），所以这里
 * 只能在 work 里查 MCU，回调本身只读缓存。
 */
static void m5io_hub_uart_tx_empty_work(struct work_struct *work)
{
  struct m5io_hub_uart_port *up = container_of(
      to_delayed_work(work), struct m5io_hub_uart_port, tx_empty_work);
  int ret;

  if (!READ_ONCE(up->configured) || READ_ONCE(up->io_failed))
    return;

  ret = m5io_hub_uart_tx_empty(up->mu->hub, up->bus);
  if (ret < 0) {
    atomic_set(&up->tx_empty_valid, 0);
    dev_dbg_ratelimited(up->port.dev, "UART%u tx_empty query failed: %d\n",
                        up->bus, ret);
  } else {
    atomic_set(&up->tx_empty, ret ? 1 : 0);
    atomic_set(&up->tx_empty_valid, 1);
  }

  /* 只有还有人盯着 tx_empty 时才继续刷新，避免关掉端口后后台轮询。 */
  if ((ret < 0 || !atomic_read(&up->tx_empty)) &&
      time_before(jiffies, READ_ONCE(up->tx_empty_deadline)))
    mod_delayed_work(system_wq, &up->tx_empty_work,
                     msecs_to_jiffies(M5IO_HUB_UART_TX_EMPTY_POLL_MS));
}

static unsigned int m5io_hub_uart_op_tx_empty(struct uart_port *port)
{
  struct m5io_hub_uart_port *up = m5io_hub_uart_from_port(port);

  /* 驱动里还有没提交/正在提交的块：肯定不空，也不必问 MCU。 */
  if (atomic_read(&up->tx_inflight) || atomic_read(&up->tx_pending))
    return 0;
  /* 配置不确定时不能声称发送完成；未配置则本来就没有待发数据。 */
  if (READ_ONCE(up->io_failed))
    return 0;
  if (!READ_ONCE(up->configured))
    return TIOCSER_TEMT;

  if (atomic_read(&up->tx_empty_valid) && atomic_read(&up->tx_empty))
    return TIOCSER_TEMT;

  /*
   * 原子上下文不能睡眠：延长刷新窗口并踢一次异步查询，本次先保守返回 0
   * （发送未完成）。uart_wait_until_sent 等调用点会反复查询，缓存随即更新。
   */
  WRITE_ONCE(up->tx_empty_deadline,
             jiffies + msecs_to_jiffies(M5IO_HUB_UART_TX_EMPTY_WINDOW_MS));
  mod_delayed_work(system_wq, &up->tx_empty_work, 0);
  return 0;
}

static unsigned int m5io_hub_uart_get_mctrl(struct uart_port *port)
{
  /* 协议没有 modem 状态操作，虚拟链路固定在线且没有硬件流控信号。 */
  return TIOCM_CTS | TIOCM_DSR | TIOCM_CAR;
}

static void m5io_hub_uart_set_mctrl(struct uart_port *port, unsigned int mctrl)
{
  /* RTS/DTR 不在 FUNC=4 的范围内。 */
}

static void m5io_hub_uart_op_stop_tx(struct uart_port *port)
{
  struct m5io_hub_uart_port *up = m5io_hub_uart_from_port(port);

  atomic_set(&up->tx_enabled, 0);
  atomic_set(&up->tx_pending, 0);

  /*
   * 这里持有 port->lock 且不可睡眠，只记录状态并把请求合并到 tx_work；
   * 已经处于 hold 状态就不重复下发 STOP。
   */
  if (!READ_ONCE(up->configured) || atomic_xchg(&up->tx_hold, 1))
    return;
  atomic_xchg(&up->tx_cmd_pending, M5IO_HUB_UART_TX_CMD_STOP);
  mod_delayed_work(system_wq, &up->tx_work, 0);
}

static void m5io_hub_uart_rx_data(void *data, const u8 *buf, unsigned int len)
{
  struct m5io_hub_uart_port *up = data;
  struct tty_port *tty_port;
  unsigned int copied;

  if (!up || !buf || !len)
    return;

  /* core 已做 configured/rx_enabled 门控，这里只需挡住没有 tty 或 CREAD=0。 */
  if (!up->port.state || !atomic_read(&up->rx_enabled)) {
    dev_dbg_ratelimited(up->port.dev,
                        "UART%u: dropping %u RX bytes (tty=%d rx_enabled=%d)\n",
                        up->bus, len, up->port.state ? 1 : 0,
                        atomic_read(&up->rx_enabled));
    return;
  }

  tty_port = &up->port.state->port;
  copied = tty_insert_flip_string(tty_port, buf, len);
  up->port.icount.rx += copied;
  if (copied != len)
    up->port.icount.buf_overrun++;
  if (copied)
    tty_flip_buffer_push(tty_port);
}

/* 配置结果不确定时必须关掉 DATA 门：超时无法证明 MCU 当前是哪种帧格式。 */
static void m5io_hub_uart_mark_unknown(struct m5io_hub_uart_port *up)
{
  WRITE_ONCE(up->configured, false);
  WRITE_ONCE(up->io_failed, true);
  atomic_set(&up->tx_enabled, 0);
  atomic_set(&up->rx_enabled, 0);
  atomic_set(&up->tx_cmd_pending, M5IO_HUB_UART_TX_CMD_NONE);
  atomic_set(&up->tx_hold, 0);
  atomic_set(&up->mcu_tx_stopped, 0);
  atomic_set(&up->rx_stop_pending, 0);
  atomic_set(&up->mcu_rx_stopped, 0);
  atomic_set(&up->tx_empty_valid, 0);
}

static void m5io_hub_uart_tx_work(struct work_struct *work)
{
  struct m5io_hub_uart_port *up =
      container_of(to_delayed_work(work), struct m5io_hub_uart_port, tx_work);
  struct uart_port *port = &up->port;

  /* 串行化配置与提交，同时保证“MCU 当前帧格式仍已知”这个前提。 */
  mutex_lock(&up->config_lock);
  for (;;) {
    struct tty_port *tty_port;
    unsigned long flags;
    unsigned int count, pending, gen;
    int ret;

    /* 进程上下文里补发 uart_ops.stop_tx/start_tx 在原子上下文做不了的下行命令。 */
    switch (atomic_xchg(&up->tx_cmd_pending, M5IO_HUB_UART_TX_CMD_NONE)) {
    case M5IO_HUB_UART_TX_CMD_STOP:
      /* 先竖“MCU 已停”的标志：并发的 start_tx 看到它才会补 START。 */
      atomic_set(&up->mcu_tx_stopped, 1);
      ret = m5io_hub_uart_stop_tx(up->mu->hub, up->bus);
      if (ret)
        dev_dbg_ratelimited(port->dev, "UART%u stop_tx failed: %d\n",
                            up->bus, ret);
      break;
    case M5IO_HUB_UART_TX_CMD_START:
      /* STOP 可能在到达 MCU 之前就被 START 覆盖，这种情况下无需下发。 */
      if (!atomic_read(&up->mcu_tx_stopped))
        break;
      ret = m5io_hub_uart_start_tx(up->mu->hub, up->bus);
      if (ret) {
        /*
         * START 没下成：把 hold 重新竖起，下一次 uart_ops.start_tx 才会
         * 再补一次；否则本地以为已恢复、MCU 却仍是停发状态，数据会卡死。
         */
        atomic_set(&up->tx_hold, 1);
        dev_dbg_ratelimited(port->dev, "UART%u start_tx failed: %d\n",
                            up->bus, ret);
      } else {
        atomic_set(&up->mcu_tx_stopped, 0);
      }
      break;
    default:
      break;
    }

    /* 进程上下文里补发 uart_ops.stop_rx 在原子上下文做不了的下行命令。 */
    if (atomic_xchg(&up->rx_stop_pending, 0) &&
        atomic_read(&up->mcu_rx_stopped)) {
      ret = m5io_hub_uart_stop_rx(up->mu->hub, up->bus);
      if (ret) {
        /* 失败就撤销标志，下一次 stop_rx 会重试。 */
        atomic_set(&up->mcu_rx_stopped, 0);
        dev_dbg_ratelimited(port->dev, "UART%u stop_rx failed: %d\n",
                            up->bus, ret);
      }
    }

    if (!READ_ONCE(up->configured) || !atomic_read(&up->tx_enabled) ||
        !port->state || atomic_read(&up->mcu_tx_stopped))
      break;

    spin_lock_irqsave(&port->lock, flags);
    tty_port = &port->state->port;
    if (uart_tx_stopped(port) || !kfifo_len(&tty_port->xmit_fifo)) {
      atomic_set(&up->tx_pending, 0);
      spin_unlock_irqrestore(&port->lock, flags);
      break;
    }

    gen = up->xmit_gen;
    count = min_t(unsigned int, kfifo_len(&tty_port->xmit_fifo),
                  M5IO_HUB_UART_CHUNK);
    if (kfifo_out_peek(&tty_port->xmit_fifo, up->tx_buf, count) != count) {
      spin_unlock_irqrestore(&port->lock, flags);
      mod_delayed_work(system_wq, &up->tx_work, msecs_to_jiffies(20));
      break;
    }
    atomic_set(&up->tx_inflight, 1);
    spin_unlock_irqrestore(&port->lock, flags);

    /* 新字节进 MCU FIFO，之前“已排空”的缓存不再可信。 */
    atomic_set(&up->tx_empty_valid, 0);
    ret = m5io_hub_uart_send_data(up->mu->hub, up->bus, up->tx_buf, count);

    atomic_set(&up->tx_inflight, 0);
    if (ret) {
      /*
       * -EAGAIN/-ENOSPC 表示 DATA 还没进传输层（窗口满），重试即可；
       * 其它错误说明结果不明，必须关门等下一次成功 CONFIG。
       */
      if (ret == -EAGAIN || ret == -ENOSPC) {
        mod_delayed_work(system_wq, &up->tx_work, msecs_to_jiffies(20));
      } else {
        m5io_hub_uart_mark_unknown(up);
        dev_warn_ratelimited(port->dev,
                             "UART%u TX stopped: %d; reconfigure required\n",
                             up->bus, ret);
      }
      break;
    }

    spin_lock_irqsave(&port->lock, flags);
    if (unlikely(up->xmit_gen != gen)) {
      /*
       * 发送期间发生了 TCFLSH：peek 到的字节已经被清掉，tail 不能再前进。
       * 已经发出去的这部分无法追回（与其它 uart 驱动在 flush 边界上的
       * 行为一致），后续数据从新 FIFO 头继续发送。
       */
      atomic_set(&up->tx_pending, 0);
      spin_unlock_irqrestore(&port->lock, flags);
      break;
    }
    tty_port = &port->state->port;
    uart_xmit_advance(port, count);
    pending = kfifo_len(&tty_port->xmit_fifo);
    if (pending < WAKEUP_CHARS)
      uart_write_wakeup(port);
    if (!pending || uart_tx_stopped(port))
      atomic_set(&up->tx_pending, 0);
    spin_unlock_irqrestore(&port->lock, flags);

    if (!pending)
      break;
  }
  mutex_unlock(&up->config_lock);
}

static void m5io_hub_uart_op_start_tx(struct uart_port *port)
{
  struct m5io_hub_uart_port *up = m5io_hub_uart_from_port(port);

  atomic_set(&up->tx_pending, 1);
  if (!READ_ONCE(up->configured))
    return;
  atomic_set(&up->tx_enabled, 1);

  /*
   * 只有真正从“已停”恢复时才需要补 START（例如 XOFF/XON 之后）；正常写
   * 路径 hold 本来就是 0，不再额外发一条 TOKEN。后到的 START 会覆盖还没
   * 被 tx_work 取走的 STOP。
   */
  if (atomic_xchg(&up->tx_hold, 0))
    atomic_xchg(&up->tx_cmd_pending, M5IO_HUB_UART_TX_CMD_START);

  mod_delayed_work(system_wq, &up->tx_work, 0);
}

static void m5io_hub_uart_op_stop_rx(struct uart_port *port)
{
  struct m5io_hub_uart_port *up = m5io_hub_uart_from_port(port);

  atomic_set(&up->rx_enabled, 0);

  /*
   * 原子上下文只记录并合并请求；已经停过就不重复下发。配置未知时不发
   * 命令，等重新配置成功后由 configure 清掉 mcu_rx_stopped。
   */
  if (!READ_ONCE(up->configured) || atomic_xchg(&up->mcu_rx_stopped, 1))
    return;
  atomic_set(&up->rx_stop_pending, 1);
  mod_delayed_work(system_wq, &up->tx_work, 0);
}

static void m5io_hub_uart_flush_buffer(struct uart_port *port)
{
  struct m5io_hub_uart_port *up = m5io_hub_uart_from_port(port);

  /*
   * uart 核心已经持有 port->lock，并且先 kfifo_reset() 了 xmit_fifo 才调用
   * 这里。递增代号让正在 RPC 中的 tx_work 知道：它 peek 到的那批字节已被
   * TCFLSH 丢掉，不能再 uart_xmit_advance()，否则 tail 会越过 head、kfifo
   * 长度下溢成天文数字并发出垃圾数据。
   */
  up->xmit_gen++;
  atomic_set(&up->tx_pending, 0);
  /* MCU 侧可能还在发 TCFLSH 前收下的字节，让下次 tx_empty 重新问一次。 */
  atomic_set(&up->tx_empty_valid, 0);
}

static void m5io_hub_uart_enable_ms(struct uart_port *port) {}

static void m5io_hub_uart_break_ctl(struct uart_port *port, int break_state) {}

static int m5io_hub_uart_startup(struct uart_port *port)
{
  struct m5io_hub_uart_port *up = m5io_hub_uart_from_port(port);
  int ret;

  /*
   * register_rx 就是 core 侧的 RX 门：tty 打开才注册，关闭就注销，未配置
   * 期间由 core 的 configured 门控继续挡住数据。
   */
  if (!up->rx_registered) {
    ret = m5io_hub_uart_register_rx(up->mu->hub, up->bus,
                                    m5io_hub_uart_rx_data, up);
    if (ret)
      return ret;
    up->rx_registered = true;
  }

  atomic_set(&up->tx_enabled, READ_ONCE(up->configured) ? 1 : 0);
  atomic_set(&up->rx_enabled,
             READ_ONCE(up->configured) &&
                     (READ_ONCE(up->confirmed_cflag) & CREAD) ?
                 1 :
                 0);
  return 0;
}

static void m5io_hub_uart_shutdown(struct uart_port *port)
{
  struct m5io_hub_uart_port *up = m5io_hub_uart_from_port(port);
  int ret;

  atomic_set(&up->rx_enabled, 0);
  atomic_set(&up->tx_enabled, 0);
  atomic_set(&up->tx_pending, 0);
  atomic_set(&up->tx_cmd_pending, M5IO_HUB_UART_TX_CMD_NONE);
  atomic_set(&up->tx_hold, 0);
  atomic_set(&up->mcu_tx_stopped, 0);
  atomic_set(&up->tx_empty_valid, 0);
  cancel_delayed_work_sync(&up->tx_work);
  cancel_delayed_work_sync(&up->tx_empty_work);

  mutex_lock(&up->config_lock);
  /*
   * uart_ops.stop_rx 是原子上下文，请求可能在 tx_work 被取消前来不及转发。
   * shutdown 在进程上下文，这里同步补发一次：pending 已被 work 消费过就
   * 不再重复，保证每个 stop_rx 请求恰好下发一次。
   */
  if (atomic_xchg(&up->rx_stop_pending, 0) &&
      atomic_read(&up->mcu_rx_stopped) && READ_ONCE(up->configured)) {
    ret = m5io_hub_uart_stop_rx(up->mu->hub, up->bus);
    if (ret)
      dev_dbg_ratelimited(port->dev, "UART%u stop_rx failed: %d\n",
                          up->bus, ret);
  }
  atomic_set(&up->mcu_rx_stopped, 0);

  if (up->rx_registered) {
    m5io_hub_uart_unregister_rx(up->mu->hub, up->bus);
    up->rx_registered = false;
  }
  if (up->configured) {
    /* 关闭该端口，释放 MCU 侧配置。 */
    ret = m5io_hub_uart_reset(up->mu->hub, up->bus);
    if (!ret)
      WRITE_ONCE(up->configured, false);
    else if (ret != -EBUSY && ret != -EINVAL)
      m5io_hub_uart_mark_unknown(up);
  }
  mutex_unlock(&up->config_lock);
}

static int m5io_hub_uart_configure(struct m5io_hub_uart_port *up,
                                   struct ktermios *termios, unsigned int baud)
{
  unsigned int attempt;
  int ret;

  for (attempt = 0; attempt < M5IO_HUB_UART_CFG_BUSY_TRIES; attempt++) {
    ret = m5io_hub_uart_config(up->mu->hub, up->bus, baud,
                               (termios->c_cflag & CSIZE) == CS7 ? 7 : 8,
                               (termios->c_cflag & PARENB) ?
                                   ((termios->c_cflag & PARODD) ? 2 : 1) :
                                   0,
                               (termios->c_cflag & CSTOPB) ? 2 : 1, 0);
    if (ret != -EBUSY)
      break;
    usleep_range(M5IO_HUB_UART_CFG_BUSY_DELAY_US,
                 M5IO_HUB_UART_CFG_BUSY_DELAY_US + 2000);
  }

  if (ret) {
    /*
     * -EINVAL 是 MCU 明确拒绝（帧格式非法），此时原配置仍然有效；
     * 其它错误（含 BUSY 打到上限、超时）都不能证明 MCU 还在用什么配置。
     */
    if (ret != -EINVAL)
      m5io_hub_uart_mark_unknown(up);
    return ret;
  }

  /*
   * API 只返回成功/失败，不回读实际分频；按请求值更新 tty timeout。
   */
  uart_update_timeout(&up->port, termios->c_cflag, baud);
  up->confirmed_cflag = termios->c_cflag;
  up->confirmed_baud = baud;
  WRITE_ONCE(up->io_failed, false);
  WRITE_ONCE(up->configured, true);
  /* CONFIG 会重新打开该端口，之前 stop_rx 留下的 MCU 状态已失效。 */
  atomic_set(&up->mcu_rx_stopped, 0);
  atomic_set(&up->rx_stop_pending, 0);
  atomic_set(&up->tx_empty_valid, 0);
  atomic_set(&up->rx_enabled,
             up->rx_registered && (termios->c_cflag & CREAD) != 0);
  atomic_set(&up->tx_enabled, 1);
  return 0;
}

static void m5io_hub_uart_set_termios(struct uart_port *port,
                                      struct ktermios *new_termios,
                                      const struct ktermios *old)
{
  struct m5io_hub_uart_port *up = m5io_hub_uart_from_port(port);
  unsigned int baud;
  int ret;

  baud = uart_get_baud_rate(port, new_termios, old, M5IO_HUB_UART_MIN_BAUD,
                            M5IO_HUB_UART_MAX_BAUD);
  mutex_lock(&up->config_lock);
  if ((new_termios->c_cflag & CSIZE) != CS7 &&
      (new_termios->c_cflag & CSIZE) != CS8) {
    dev_warn(port->dev, "UART%u supports only CS7 and CS8\n", up->bus);
    ret = -EINVAL;
    goto restore;
  }
  if (new_termios->c_cflag & CMSPAR) {
    dev_warn(port->dev, "UART%u does not support mark/space parity\n",
             up->bus);
    ret = -EINVAL;
    goto restore;
  }

  ret = m5io_hub_uart_configure(up, new_termios, baud);
restore:
  if (ret) {
    /*
     * set_termios 不能返回 errno：不要把被拒绝的帧格式发布成实际硬件状态，
     * 恢复成最后一次确认过的配置（未知状态继续保持关门）。
     */
    const tcflag_t mask = CSIZE | PARENB | PARODD | CSTOPB | CMSPAR | CREAD;
    tcflag_t confirmed =
        up->confirmed_baud ? up->confirmed_cflag :
                             (old ? old->c_cflag : (CS8 | CREAD));
    unsigned int previous_baud = up->confirmed_baud ?
                                     up->confirmed_baud :
                                     (old ? tty_termios_baud_rate(old) :
                                            9600);

    new_termios->c_cflag = (new_termios->c_cflag & ~mask) |
                           (confirmed & mask);
    tty_termios_encode_baud_rate(new_termios, previous_baud, previous_baud);
    uart_update_timeout(port, new_termios->c_cflag, previous_baud);
  }
  if (READ_ONCE(up->configured) && atomic_read(&up->tx_pending))
    mod_delayed_work(system_wq, &up->tx_work, 0);
  mutex_unlock(&up->config_lock);
  if (ret)
    dev_warn(port->dev, "UART%u configuration failed: %d\n", up->bus, ret);
}

static const char *m5io_hub_uart_type(struct uart_port *port)
{
  return M5IO_HUB_UART_NAME;
}

static void m5io_hub_uart_release_port(struct uart_port *port) {}

static int m5io_hub_uart_request_port(struct uart_port *port)
{
  return 0;
}

static void m5io_hub_uart_config_port(struct uart_port *port, int flags)
{
  if (flags & UART_CONFIG_TYPE)
    port->type = PORT_16550A;
}

static int m5io_hub_uart_verify_port(struct uart_port *port,
                                     struct serial_struct *ser)
{
  if (ser->type != PORT_UNKNOWN && ser->type != PORT_16550A)
    return -EINVAL;
  if (ser->baud_base && ser->baud_base < M5IO_HUB_UART_MIN_BAUD)
    return -EINVAL;
  return 0;
}

static const struct uart_ops m5io_hub_uart_ops = {
    .tx_empty = m5io_hub_uart_op_tx_empty,
    .set_mctrl = m5io_hub_uart_set_mctrl,
    .get_mctrl = m5io_hub_uart_get_mctrl,
    .stop_tx = m5io_hub_uart_op_stop_tx,
    .start_tx = m5io_hub_uart_op_start_tx,
    .stop_rx = m5io_hub_uart_op_stop_rx,
    .flush_buffer = m5io_hub_uart_flush_buffer,
    .enable_ms = m5io_hub_uart_enable_ms,
    .break_ctl = m5io_hub_uart_break_ctl,
    .startup = m5io_hub_uart_startup,
    .shutdown = m5io_hub_uart_shutdown,
    .set_termios = m5io_hub_uart_set_termios,
    .type = m5io_hub_uart_type,
    .release_port = m5io_hub_uart_release_port,
    .request_port = m5io_hub_uart_request_port,
    .config_port = m5io_hub_uart_config_port,
    .verify_port = m5io_hub_uart_verify_port,
};

static int m5io_hub_uart_register_port(struct platform_device *pdev,
                                       struct m5io_hub_uart *mu,
                                       unsigned int index)
{
  struct m5io_hub_uart_port *up = &mu->ports[index];
  struct uart_port *port = &up->port;
  int ret;

  port->dev = &pdev->dev;
  port->line = index;
  port->type = PORT_16550A;
  port->iotype = UPIO_MEM;
  port->fifosize = M5IO_HUB_UART_MAX_TX;
  port->flags = UPF_BOOT_AUTOCONF;
  port->ops = &m5io_hub_uart_ops;
  port->uartclk = M5IO_HUB_UART_MAX_BAUD * 16;
  spin_lock_init(&port->lock);
  mutex_init(&up->config_lock);
  INIT_DELAYED_WORK(&up->tx_work, m5io_hub_uart_tx_work);
  INIT_DELAYED_WORK(&up->tx_empty_work, m5io_hub_uart_tx_empty_work);
  atomic_set(&up->tx_empty_valid, 0);
  atomic_set(&up->tx_empty, 0);
  WRITE_ONCE(up->tx_empty_deadline, 0);
  atomic_set(&up->tx_enabled, 0);
  atomic_set(&up->tx_inflight, 0);
  atomic_set(&up->tx_pending, 0);
  atomic_set(&up->tx_cmd_pending, M5IO_HUB_UART_TX_CMD_NONE);
  atomic_set(&up->tx_hold, 0);
  atomic_set(&up->mcu_tx_stopped, 0);
  atomic_set(&up->rx_stop_pending, 0);
  atomic_set(&up->mcu_rx_stopped, 0);
  atomic_set(&up->rx_enabled, 0);
  up->mu = mu;
  /* DT 的 reg=0/1 对应 API 端口号 1/2（USART1/USART2）。 */
  up->bus = (u8)(index + 1);

  ret = uart_add_one_port(&m5io_hub_uart_driver, port);
  if (ret)
    return ret;
  up->registered = true;
  return 0;
}

static int m5io_hub_uart_probe(struct platform_device *pdev)
{
  struct device *dev = &pdev->dev;
  struct device_node *child;
  struct m5io_hub_uart *mu;
  struct m5io_hub *hub;
  bool seen[M5IO_HUB_UART_NR] = {false, false};
  unsigned int index;
  unsigned int registered = 0;
  u32 reg;
  int ret;

  hub = dev_get_drvdata(dev->parent);
  if (!hub)
    return -EPROBE_DEFER;

  mu = devm_kzalloc(dev, sizeof(*mu), GFP_KERNEL);
  if (!mu)
    return -ENOMEM;
  mu->hub = hub;

  if (of_get_child_count(dev->of_node)) {
    /*
     * 有子节点：只注册 status="okay" 的 serial@N；如果 DT 把子节点全部
     * disable，就不应再凭父节点 reg 造出一个端口。
     */
    for_each_available_child_of_node(dev->of_node, child) {
      if (of_property_read_u32(child, "reg", &reg) ||
          reg >= M5IO_HUB_UART_NR) {
        dev_warn(dev, "ignoring UART child without valid reg\n");
        continue;
      }
      seen[reg] = true;
    }
  } else {
    /* 扁平平铺节点（没有总线子节点）时，按节点 reg（缺省 0）注册一路。 */
    if (!of_property_read_u32(dev->of_node, "reg", &reg) &&
        reg < M5IO_HUB_UART_NR)
      seen[reg] = true;
    else
      seen[0] = true;
  }

  for (index = 0; index < M5IO_HUB_UART_NR; index++) {
    if (!seen[index])
      continue;
    ret = m5io_hub_uart_register_port(pdev, mu, index);
    if (ret)
      goto err_unregister;
    registered++;
  }
  if (!registered)
    return -ENODEV;

  platform_set_drvdata(pdev, mu);
  dev_info(dev, "registered %u M5IO-HUB UART port(s)\n", registered);
  return 0;

err_unregister:
  while (index--) {
    struct m5io_hub_uart_port *up = &mu->ports[index];

    if (!up->registered)
      continue;
    cancel_delayed_work_sync(&up->tx_work);
    cancel_delayed_work_sync(&up->tx_empty_work);
    uart_remove_one_port(&m5io_hub_uart_driver, &up->port);
  }
  return ret;
}

static void m5io_hub_uart_remove(struct platform_device *pdev)
{
  struct m5io_hub_uart *mu = platform_get_drvdata(pdev);
  unsigned int index;

  if (!mu)
    return;
  for (index = 0; index < M5IO_HUB_UART_NR; index++) {
    struct m5io_hub_uart_port *up = &mu->ports[index];

    if (!up->registered)
      continue;
    cancel_delayed_work_sync(&up->tx_work);
    cancel_delayed_work_sync(&up->tx_empty_work);
    uart_remove_one_port(&m5io_hub_uart_driver, &up->port);
    if (up->rx_registered) {
      m5io_hub_uart_unregister_rx(mu->hub, up->bus);
      up->rx_registered = false;
    }
    up->registered = false;
  }
}

static const struct of_device_id m5io_hub_uart_of_match[] = {
    {.compatible = "m5stack,m5io-hub-uart"},
    {},
};
MODULE_DEVICE_TABLE(of, m5io_hub_uart_of_match);

static struct platform_driver m5io_hub_uart_platform_driver = {
    .probe = m5io_hub_uart_probe,
    .remove = m5io_hub_uart_remove,
    .driver =
        {
            .name = M5IO_HUB_UART_NAME,
            .of_match_table = m5io_hub_uart_of_match,
        },
};

static int __init m5io_hub_uart_init(void)
{
  int ret;

  ret = uart_register_driver(&m5io_hub_uart_driver);
  if (ret)
    return ret;
  ret = platform_driver_register(&m5io_hub_uart_platform_driver);
  if (ret)
    uart_unregister_driver(&m5io_hub_uart_driver);
  return ret;
}
module_init(m5io_hub_uart_init);

static void __exit m5io_hub_uart_exit(void)
{
  platform_driver_unregister(&m5io_hub_uart_platform_driver);
  uart_unregister_driver(&m5io_hub_uart_driver);
}
module_exit(m5io_hub_uart_exit);

MODULE_DESCRIPTION("M5Stack M5IO-HUB virtual UART driver");
MODULE_LICENSE("GPL");
