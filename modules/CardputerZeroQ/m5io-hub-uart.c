// SPDX-License-Identifier: GPL-2.0
/*
 * M5IO-HUB virtual UART driver.
 *
 * The UART pins live on the M5IO-HUB MCU.  The Linux tty side is therefore
 * backed by FUNC=4 TOKEN requests and DATA channel 4/5 frames rather than by
 * memory mapped registers or an IRQ line.  TX is serialized by a work item so
 * the sleepable RPC transaction is never run while the uart_port spinlock is
 * held.  RX DATA frames are delivered by the hub core's process-context
 * channel callback directly to the tty flip buffer.
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

#define M5IO_HUB_UART_NAME          "m5io-hub-uart"
#define M5IO_HUB_TTY_NAME           "ttyM5HUB"
#define M5IO_HUB_UART_NR            2
#define M5IO_HUB_UART_MAX_TX        1024
#define M5IO_HUB_UART_MIN_BAUD      1200
#define M5IO_HUB_UART_MAX_BAUD      1000000
#define M5IO_HUB_UART_CONFIG_LEN    8
#define M5IO_HUB_UART_ARG_CONFIG    1
#define M5IO_HUB_UART_ARG_TX        2
#define M5IO_HUB_UART_ARG_DISABLE   6

struct m5io_hub_uart;

struct m5io_hub_uart_port {
    struct uart_port port;
    struct m5io_hub_uart *mu;
    struct delayed_work tx_work;
    struct mutex config_lock;
    atomic_t tx_enabled;
    atomic_t tx_inflight;
    atomic_t tx_pending;
    atomic_t rx_enabled;
    unsigned int channel;
    u8 tx_buf[M5IO_HUB_UART_MAX_TX];
    bool registered;
    bool configured;
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

static unsigned int m5io_hub_uart_tx_empty(struct uart_port *port)
{
    struct m5io_hub_uart_port *up =
        container_of(port, struct m5io_hub_uart_port, port);

    return atomic_read(&up->tx_inflight) || atomic_read(&up->tx_pending) ?
           0 : TIOCSER_TEMT;
}

static unsigned int m5io_hub_uart_get_mctrl(struct uart_port *port)
{
    /* The MCU protocol has no modem-status operation.  The virtual link is
     * permanently present and has no hardware flow-control signals. */
    return TIOCM_CTS | TIOCM_DSR | TIOCM_CAR;
}

static void m5io_hub_uart_set_mctrl(struct uart_port *port, unsigned int mctrl)
{
    /* RTS/DTR are not exposed by FUNC=4. */
}

static void m5io_hub_uart_stop_tx(struct uart_port *port)
{
    struct m5io_hub_uart_port *up =
        container_of(port, struct m5io_hub_uart_port, port);

    atomic_set(&up->tx_enabled, 0);
    atomic_set(&up->tx_pending, 0);
}

static void m5io_hub_uart_rx_data(void *data, const u8 *buf, unsigned int len)
{
    struct m5io_hub_uart_port *up = data;
    struct tty_port *tty_port;
    int copied;

    if (!up || !buf || !len || !up->configured ||
        !atomic_read(&up->rx_enabled) ||
        !up->port.state)
        return;

    tty_port = &up->port.state->port;
    copied = tty_insert_flip_string(tty_port, buf, len);
    up->port.icount.rx += copied;
    if (copied != len)
        up->port.icount.buf_overrun++;
    if (copied)
        tty_flip_buffer_push(tty_port);
}

static void m5io_hub_uart_tx_work(struct work_struct *work)
{
    struct m5io_hub_uart_port *up = container_of(to_delayed_work(work),
                                                  struct m5io_hub_uart_port,
                                                  tx_work);
    struct uart_port *port = &up->port;
    for (;;) {
        struct tty_port *tty_port;
        unsigned long flags;
        unsigned int count;
        unsigned int pending;
        int ret;

        if (!atomic_read(&up->tx_enabled) || !port->state)
            break;

        spin_lock_irqsave(&port->lock, flags);
        tty_port = &port->state->port;
        if (uart_tx_stopped(port) || !kfifo_len(&tty_port->xmit_fifo)) {
            atomic_set(&up->tx_pending, 0);
            spin_unlock_irqrestore(&port->lock, flags);
            break;
        }

        count = min_t(unsigned int, kfifo_len(&tty_port->xmit_fifo),
                      M5IO_HUB_DATA_PAYLOAD_MAX);
        if (kfifo_out_peek(&tty_port->xmit_fifo, up->tx_buf, count) != count) {
            spin_unlock_irqrestore(&port->lock, flags);
            mod_delayed_work(system_wq, &up->tx_work, msecs_to_jiffies(20));
            break;
        }
        atomic_set(&up->tx_inflight, 1);
        spin_unlock_irqrestore(&port->lock, flags);

        /* UART bytes are DATA CH=4/5.  The old FUNC=4 ARG=2 TX TOKEN and
         * its port/length prefix are intentionally not emitted. */
        ret = m5io_hub_send_data_async(up->mu->hub, up->channel,
                                       up->tx_buf, count);

        atomic_set(&up->tx_inflight, 0);
        if (ret) {
            dev_warn_ratelimited(port->dev,
                                 "UART%u TX transaction failed: %d\n",
                                 up->channel - M5IO_HUB_CHN_UART1 + 1, ret);
            /* Leave the xmit tail untouched.  A transient RPC/FIFO failure
             * can be retried without silently dropping tty data. */
            mod_delayed_work(system_wq, &up->tx_work,
                             msecs_to_jiffies(20));
            break;
        }

        spin_lock_irqsave(&port->lock, flags);
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
}

static void m5io_hub_uart_start_tx(struct uart_port *port)
{
    struct m5io_hub_uart_port *up =
        container_of(port, struct m5io_hub_uart_port, port);

    atomic_set(&up->tx_enabled, 1);
    atomic_set(&up->tx_pending, 1);
    mod_delayed_work(system_wq, &up->tx_work, 0);
}

static void m5io_hub_uart_stop_rx(struct uart_port *port)
{
    struct m5io_hub_uart_port *up =
        container_of(port, struct m5io_hub_uart_port, port);

    atomic_set(&up->rx_enabled, 0);
}

static void m5io_hub_uart_enable_ms(struct uart_port *port)
{
}

static void m5io_hub_uart_break_ctl(struct uart_port *port, int break_state)
{
}

static int m5io_hub_uart_startup(struct uart_port *port)
{
    struct m5io_hub_uart_port *up =
        container_of(port, struct m5io_hub_uart_port, port);

    atomic_set(&up->rx_enabled, up->configured);
    atomic_set(&up->tx_enabled, up->configured);
    return 0;
}

static void m5io_hub_uart_shutdown(struct uart_port *port)
{
    struct m5io_hub_uart_port *up =
        container_of(port, struct m5io_hub_uart_port, port);

    atomic_set(&up->rx_enabled, 0);
    atomic_set(&up->tx_enabled, 0);
    atomic_set(&up->tx_pending, 0);
    cancel_delayed_work_sync(&up->tx_work);
    if (up->configured) {
        struct m5io_hub_request request = {
            .func = M5IO_HUB_FUNC_UART,
            .arg = M5IO_HUB_UART_ARG_DISABLE,
        };
        u8 payload = up->channel == M5IO_HUB_CHN_UART2 ? 2 : 1;

        request.payload = &payload;
        request.payload_len = 1;
        if (!m5io_hub_exec(up->mu->hub, &request) && request.status == 0)
            up->configured = false;
    }
}

static int m5io_hub_uart_configure(struct m5io_hub_uart_port *up,
                                    struct ktermios *termios,
                                    unsigned int baud)
{
    struct m5io_hub_request request = {0};
    u8 payload[M5IO_HUB_UART_CONFIG_LEN];
    u8 port_number = up->channel == M5IO_HUB_CHN_UART2 ? 2 : 1;
    int ret;

    payload[0] = port_number;
    payload[1] = baud;
    payload[2] = baud >> 8;
    payload[3] = baud >> 16;
    payload[4] = baud >> 24;
    payload[5] = (termios->c_cflag & CSIZE) == CS7 ? 7 : 8;
    payload[6] = (termios->c_cflag & PARENB) ?
                 ((termios->c_cflag & PARODD) ? 2 : 1) : 0;
    payload[7] = (termios->c_cflag & CSTOPB) ? 2 : 1;

    request.func = M5IO_HUB_FUNC_UART;
    request.arg = M5IO_HUB_UART_ARG_CONFIG;
    request.payload = payload;
    request.payload_len = sizeof(payload);
    ret = m5io_hub_exec(up->mu->hub, &request);
    if (ret)
        return ret;
    if (request.status != 0)
        return request.status == 4 ? -EBUSY :
               (request.status == 3 ? -EINVAL : -EIO);
    if (request.reply_len != sizeof(payload) ||
        request.reply[0] != port_number)
        return -EPROTO;
    up->configured = true;
    atomic_set(&up->rx_enabled, 1);
    atomic_set(&up->tx_enabled, 1);
    return 0;
}

static void m5io_hub_uart_set_termios(struct uart_port *port,
                                       struct ktermios *new,
                                       const struct ktermios *old)
{
    struct m5io_hub_uart_port *up =
        container_of(port, struct m5io_hub_uart_port, port);
    unsigned int baud;
    int ret;

    baud = uart_get_baud_rate(port, new, old, M5IO_HUB_UART_MIN_BAUD,
                              M5IO_HUB_UART_MAX_BAUD);
    uart_update_timeout(port, new->c_cflag, baud);

    if ((new->c_cflag & CSIZE) != CS7 && (new->c_cflag & CSIZE) != CS8) {
        dev_warn(port->dev, "UART%u supports only CS7 and CS8\n",
                 up->channel - M5IO_HUB_CHN_UART1 + 1);
        return;
    }
    if (new->c_cflag & CMSPAR) {
        dev_warn(port->dev, "UART%u does not support mark/space parity\n",
                 up->channel - M5IO_HUB_CHN_UART1 + 1);
        return;
    }

    mutex_lock(&up->config_lock);
    ret = m5io_hub_uart_configure(up, new, baud);
    mutex_unlock(&up->config_lock);
    if (ret)
        dev_warn(port->dev, "UART%u configuration failed: %d\n",
                 up->channel - M5IO_HUB_CHN_UART1 + 1, ret);
}

static const char *m5io_hub_uart_type(struct uart_port *port)
{
    return M5IO_HUB_UART_NAME;
}

static void m5io_hub_uart_release_port(struct uart_port *port)
{
}

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
    .tx_empty = m5io_hub_uart_tx_empty,
    .set_mctrl = m5io_hub_uart_set_mctrl,
    .get_mctrl = m5io_hub_uart_get_mctrl,
    .stop_tx = m5io_hub_uart_stop_tx,
    .start_tx = m5io_hub_uart_start_tx,
    .stop_rx = m5io_hub_uart_stop_rx,
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
    atomic_set(&up->tx_enabled, 0);
    atomic_set(&up->tx_inflight, 0);
    atomic_set(&up->tx_pending, 0);
    atomic_set(&up->rx_enabled, 0);
    up->mu = mu;
    up->channel = M5IO_HUB_CHN_UART1 + index;

    ret = m5io_hub_register_chn_data_handler(mu->hub, up->channel,
                                              m5io_hub_uart_rx_data, up);
    if (ret)
        return ret;
    ret = uart_add_one_port(&m5io_hub_uart_driver, port);
    if (ret) {
        m5io_hub_unregister_chn_data_handler(mu->hub, up->channel,
                                             m5io_hub_uart_rx_data, up);
        return ret;
    }
    up->registered = true;
    return 0;
}

static int m5io_hub_uart_probe(struct platform_device *pdev)
{
    struct device_node *child;
    struct m5io_hub *hub = dev_get_drvdata(pdev->dev.parent);
    struct m5io_hub_uart *mu;
    bool seen[M5IO_HUB_UART_NR] = {false, false};
    unsigned int index;
    unsigned int registered = 0;
    u32 reg;
    int ret;

    if (!hub)
        return -EPROBE_DEFER;

    mu = devm_kzalloc(&pdev->dev, sizeof(*mu), GFP_KERNEL);
    if (!mu)
        return -ENOMEM;
    mu->hub = hub;

    for_each_available_child_of_node(pdev->dev.of_node, child) {
        if (of_property_read_u32(child, "reg", &reg) || reg >= M5IO_HUB_UART_NR) {
            dev_warn(&pdev->dev, "ignoring UART child without valid reg\n");
            continue;
        }
        seen[reg] = true;
    }

    /* A flat serial node is also accepted for older overlays. */
    if (!seen[0] && !seen[1]) {
        if (!of_property_read_u32(pdev->dev.of_node, "reg", &reg) &&
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
    dev_info(&pdev->dev, "registered %u M5IO-HUB UART port(s)\n", registered);
    return 0;

err_unregister:
    while (index--) {
        struct m5io_hub_uart_port *up = &mu->ports[index];

        if (!up->registered)
            continue;
        cancel_delayed_work_sync(&up->tx_work);
        m5io_hub_unregister_chn_data_handler(mu->hub, up->channel,
                                             m5io_hub_uart_rx_data, up);
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
        m5io_hub_unregister_chn_data_handler(mu->hub, up->channel,
                                             m5io_hub_uart_rx_data, up);
        uart_remove_one_port(&m5io_hub_uart_driver, &up->port);
        up->registered = false;
    }
}

static const struct of_device_id m5io_hub_uart_of_match[] = {
    { .compatible = "m5stack,m5io-hub-uart" },
    { }
};
MODULE_DEVICE_TABLE(of, m5io_hub_uart_of_match);

static struct platform_driver m5io_hub_uart_platform_driver = {
    .probe = m5io_hub_uart_probe,
    .remove = m5io_hub_uart_remove,
    .driver = {
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
