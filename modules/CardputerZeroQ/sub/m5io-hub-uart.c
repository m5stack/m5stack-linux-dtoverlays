#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/mfd/m5io-hub.h>

struct m5io_hub_uart {
    struct m5io_hub *hub;
    /* TODO: 添加uart_port结构体，走标准tty子系统 */
};

static int m5io_hub_uart_probe(struct platform_device *pdev)
{
    struct m5io_hub *hub = dev_get_drvdata(pdev->dev.parent);
    struct m5io_hub_uart *mu;

    mu = devm_kzalloc(&pdev->dev, sizeof(*mu), GFP_KERNEL);
    if (!mu)
        return -ENOMEM;

    mu->hub = hub;

    /* TODO:
     * 1. 分配 struct uart_port
     * 2. 实现 uart_ops (startup/shutdown/set_termios/start_tx等)
     * 3. uart_add_one_port() 注册到tty子系统
     * 底层读写走 hub->regmap 访问SPI寄存器模拟UART FIFO
     */

    platform_set_drvdata(pdev, mu);
    dev_info(&pdev->dev, "m5io-hub-uart probed (stub)\n");
    return 0;
}

static const struct of_device_id m5io_hub_uart_of_match[] = {
    { .compatible = "m5stack,m5io-hub-uart" },
    { }
};
MODULE_DEVICE_TABLE(of, m5io_hub_uart_of_match);

static struct platform_driver m5io_hub_uart_driver = {
    .driver = {
        .name = "m5io-hub-uart",
        .of_match_table = m5io_hub_uart_of_match,
    },
    .probe = m5io_hub_uart_probe,
};
module_platform_driver(m5io_hub_uart_driver);

MODULE_DESCRIPTION("M5Stack M5IO-HUB UART bridge driver (stub)");
MODULE_LICENSE("GPL");