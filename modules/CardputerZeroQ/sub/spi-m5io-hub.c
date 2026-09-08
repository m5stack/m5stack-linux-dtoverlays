#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/spi/spi.h>
#include <linux/mfd/m5io-hub.h>

struct m5io_hub_spi {
    struct m5io_hub *hub;
    struct spi_controller *ctlr;
};

static int m5io_hub_spi_transfer_one(struct spi_controller *ctlr,
                                        struct spi_device *spi,
                                        struct spi_transfer *t)
{
    struct m5io_hub_spi *ms = spi_controller_get_devdata(ctlr);

    /* TODO: 把spi_transfer转换成对m5io-hub内部
       "SPI桥"寄存器的读写序列 */

    return 0;
}

static int m5io_hub_spi_probe(struct platform_device *pdev)
{
    struct m5io_hub *hub = dev_get_drvdata(pdev->dev.parent);
    struct spi_controller *ctlr;
    struct m5io_hub_spi *ms;
    int ret;

    ctlr = devm_spi_alloc_host(&pdev->dev, sizeof(*ms));
    if (!ctlr)
        return -ENOMEM;

    ms = spi_controller_get_devdata(ctlr);
    ms->hub = hub;
    ms->ctlr = ctlr;

    ctlr->dev.of_node = pdev->dev.of_node;
    ctlr->transfer_one = m5io_hub_spi_transfer_one;
    ctlr->num_chipselect = 1;
    ctlr->mode_bits = SPI_MODE_0 | SPI_MODE_3;

    ret = devm_spi_register_controller(&pdev->dev, ctlr);
    if (ret)
        return ret;

    platform_set_drvdata(pdev, ms);
    dev_info(&pdev->dev, "m5io-hub-spi probed (stub)\n");
    return 0;
}

static const struct of_device_id m5io_hub_spi_of_match[] = {
    { .compatible = "m5stack,m5io-hub-spi" },
    { }
};
MODULE_DEVICE_TABLE(of, m5io_hub_spi_of_match);

static struct platform_driver m5io_hub_spi_driver = {
    .driver = {
        .name = "m5io-hub-spi",
        .of_match_table = m5io_hub_spi_of_match,
    },
    .probe = m5io_hub_spi_probe,
};
module_platform_driver(m5io_hub_spi_driver);

MODULE_DESCRIPTION("M5Stack M5IO-HUB SPI bridge driver (stub)");
MODULE_LICENSE("GPL");