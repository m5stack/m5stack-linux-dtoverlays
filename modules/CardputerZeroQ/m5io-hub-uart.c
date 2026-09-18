#include <linux/module.h>
#include <linux/platform_device.h>
#include "m5io-hub.h"
#include <linux/serial_core.h>
#include <linux/tty.h>
#include <linux/tty_flip.h>

#define M5IO_HUB_UART_NAME      "m5io-hub-uart"
#define M5IO_HUB_TTY_NAME       "ttyM5HUB"
#define M5IO_HUB_UART_NR        1

struct m5io_hub_uart;

struct m5io_hub_uart_port {
    struct uart_port port;
    struct m5io_hub_uart *mu;
};

struct m5io_hub_uart {
    struct m5io_hub *hub;
    struct m5io_hub_uart_port up;
};

static struct uart_driver m5io_hub_uart_core_driver = {
    .owner = THIS_MODULE,
    .driver_name = M5IO_HUB_UART_NAME,
    .dev_name = M5IO_HUB_TTY_NAME,
    .major = 0,
    .minor = 0,
    .nr = M5IO_HUB_UART_NR,
};

static unsigned int m5io_hub_uart_tx_empty(struct uart_port *port)
{
    /* TODO: 根据硬件TX FIFO状态返回 TIOCSER_TEMT 或 0 */
    return TIOCSER_TEMT;
}

static unsigned int m5io_hub_uart_get_mctrl(struct uart_port *port)
{
    /* TODO: 读取调制解调器状态位 */
    return TIOCM_CTS | TIOCM_DSR | TIOCM_CAR;
}

static void m5io_hub_uart_set_mctrl(struct uart_port *port, unsigned int mctrl)
{
    /* TODO: 根据mctrl控制RTS/DTR等信号 */
}

static void m5io_hub_uart_stop_tx(struct uart_port *port)
{
    /* TODO: 关闭TX中断或停止发送状态机 */
}

static bool m5io_hub_uart_tx_fifo_has_room(struct m5io_hub_uart *mu)
{
    /* TODO: 查询硬件发送FIFO是否有空间 */
    return true;
}

static int m5io_hub_uart_tx_fifo_write_byte(struct m5io_hub_uart *mu, u8 ch)
{
    /* TODO: 通过regmap/RPC把一个字节写入硬件发送FIFO */
    return 0;
}

static void m5io_hub_uart_start_tx(struct uart_port *port)
{
    struct m5io_hub_uart_port *up = container_of(port, struct m5io_hub_uart_port,
                                                 port);
    struct m5io_hub_uart *mu = up->mu;
    struct circ_buf *xmit;
    unsigned long flags;
    unsigned int budget;

    if (!port->state)
        return;

    xmit = &port->state->xmit;
    budget = port->fifosize ? port->fifosize : 16;

    spin_lock_irqsave(&port->lock, flags);

    if (uart_tx_stopped(port) || uart_circ_empty(xmit)) {
        spin_unlock_irqrestore(&port->lock, flags);
        return;
    }

    while (!uart_circ_empty(xmit) && budget--) {
        u8 ch = xmit->buf[xmit->tail];
        int ret;

        if (!m5io_hub_uart_tx_fifo_has_room(mu))
            break;

        ret = m5io_hub_uart_tx_fifo_write_byte(mu, ch);
        if (ret)
            break;

        xmit->tail = (xmit->tail + 1) & (UART_XMIT_SIZE - 1);
        port->icount.tx++;
    }

    if (uart_circ_chars_pending(xmit) < WAKEUP_CHARS)
        uart_write_wakeup(port);

    if (uart_circ_empty(xmit))
        m5io_hub_uart_stop_tx(port);

    spin_unlock_irqrestore(&port->lock, flags);
}

static void m5io_hub_uart_stop_rx(struct uart_port *port)
{
    /* TODO: 关闭RX中断 */
}

static void m5io_hub_uart_enable_ms(struct uart_port *port)
{
    /* TODO: 打开调制解调器状态变化中断（如需） */
}

static void m5io_hub_uart_break_ctl(struct uart_port *port, int break_state)
{
    /* TODO: 控制发送BREAK信号 */
}

static int m5io_hub_uart_startup(struct uart_port *port)
{
    /* TODO:
     * 1. 初始化硬件UART通道
     * 2. 使能中断
     * 3. 可选：清空FIFO
     */
    return 0;
}

static void m5io_hub_uart_shutdown(struct uart_port *port)
{
    /* TODO: 关闭中断并释放硬件资源 */
}

static void m5io_hub_uart_set_termios(struct uart_port *port,
                                       struct ktermios *new,
                                       const struct ktermios *old)
{
    unsigned int baud;

    baud = uart_get_baud_rate(port, new, old, 1200, 4000000);
    uart_update_timeout(port, new->c_cflag, baud);

    /* TODO:
     * 1. 根据new->c_cflag配置数据位/校验/停止位
     * 2. 配置波特率
     * 3. 根据new->c_iflag处理IGNPAR/INPCK等策略
     */
}

static const char *m5io_hub_uart_type(struct uart_port *port)
{
    return M5IO_HUB_UART_NAME;
}

static void m5io_hub_uart_release_port(struct uart_port *port)
{
    /* 无需IORESOURCE，占位函数 */
}

static int m5io_hub_uart_request_port(struct uart_port *port)
{
    /* TODO: 若后续需要申请IO资源，可在此实现 */
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

static int m5io_hub_uart_probe(struct platform_device *pdev)
{
    struct m5io_hub *hub = dev_get_drvdata(pdev->dev.parent);
    struct m5io_hub_uart *mu;
    struct uart_port *port;
    int ret;

    if (!hub)
        return -EPROBE_DEFER;

    mu = devm_kzalloc(&pdev->dev, sizeof(*mu), GFP_KERNEL);
    if (!mu)
        return -ENOMEM;

    mu->hub = hub;

    port = &mu->up.port;
    port->dev = &pdev->dev;
    port->line = 0;
    port->type = PORT_16550A;
    port->iotype = UPIO_MEM;
    port->fifosize = 64;
    port->flags = UPF_BOOT_AUTOCONF;
    port->ops = &m5io_hub_uart_ops;
    port->uartclk = 48000000; /* TODO: 换成设备真实输入时钟 */
    spin_lock_init(&port->lock);

    mu->up.mu = mu;

    /* TODO:
     * 1. 实现中断处理：RX数据进入tty_flip_buffer_push()
     * 2. 实现发送路径：从port->state->xmit取数据写到硬件FIFO
     * 3. 用hub->regmap或RPC接口访问M5IO-HUB寄存器
     * 底层读写走 hub->regmap 访问SPI寄存器模拟UART FIFO
     */

    ret = uart_add_one_port(&m5io_hub_uart_core_driver, port);
    if (ret)
        return ret;

    platform_set_drvdata(pdev, mu);
    dev_info(&pdev->dev, "m5io-hub-uart probed, line=%u\n", port->line);
    return 0;
}

static int m5io_hub_uart_remove(struct platform_device *pdev)
{
    struct m5io_hub_uart *mu = platform_get_drvdata(pdev);

    uart_remove_one_port(&m5io_hub_uart_core_driver, &mu->up.port);
    return 0;
}

static const struct of_device_id m5io_hub_uart_of_match[] = {
    { .compatible = "m5stack,m5io-hub-uart" },
    { }
};
MODULE_DEVICE_TABLE(of, m5io_hub_uart_of_match);

static struct platform_driver m5io_hub_uart_plat_driver = {
    .driver = {
        .name = M5IO_HUB_UART_NAME,
        .of_match_table = m5io_hub_uart_of_match,
    },
    .probe = m5io_hub_uart_probe,
    .remove = m5io_hub_uart_remove,
};

static int __init m5io_hub_uart_init(void)
{
    int ret;

    ret = uart_register_driver(&m5io_hub_uart_core_driver);
    if (ret)
        return ret;

    ret = platform_driver_register(&m5io_hub_uart_plat_driver);
    if (ret) {
        uart_unregister_driver(&m5io_hub_uart_core_driver);
        return ret;
    }

    return 0;
}

static void __exit m5io_hub_uart_exit(void)
{
    platform_driver_unregister(&m5io_hub_uart_plat_driver);
    uart_unregister_driver(&m5io_hub_uart_core_driver);
}

module_init(m5io_hub_uart_init);
module_exit(m5io_hub_uart_exit);

MODULE_DESCRIPTION("M5Stack M5IO-HUB UART bridge driver (stub)");
MODULE_LICENSE("GPL");
