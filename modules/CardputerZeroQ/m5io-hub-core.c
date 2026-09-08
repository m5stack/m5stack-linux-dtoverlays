#include <linux/module.h>
#include <linux/spi/spi.h>
#include <linux/regmap.h>
#include <linux/mfd/core.h>
#include <linux/of.h>
#include <linux/mfd/m5io-hub.h>

// /* ---- regmap 配置：走SPI访问寄存器 ---- */
// static const struct regmap_config m5io_hub_regmap_config = {
//     .reg_bits = 8,
//     .val_bits = 8,
//     .max_register = 0x20,
//     /* 如果协议是"写reg地址,再读一个字节"的简单SPI协议，
//        用默认的spi regmap bus即可；如果协议复杂，需要自定义
//        regmap_bus */
// };

/* ---- MFD cell定义：对应DT里的4个子节点 ---- */
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

static int m5io_hub_probe(struct spi_device *spi)
{
    struct m5io_hub *hub;
    int ret;

    hub = devm_kzalloc(&spi->dev, sizeof(*hub), GFP_KERNEL);
    if (!hub)
        return -ENOMEM;

    hub->dev = &spi->dev;
    hub->spi = spi;
    hub->irq = spi->irq;
    mutex_init(&hub->lock);

    spi->mode = SPI_MODE_0;   /* 根据实际硬件调整 */
    spi->bits_per_word = 8;
    ret = spi_setup(spi);
    if (ret)
        return ret;

    // hub->regmap = devm_regmap_init_spi(spi, &m5io_hub_regmap_config);
    // if (IS_ERR(hub->regmap))
    //     return PTR_ERR(hub->regmap);

    spi_set_drvdata(spi, hub);

    /* ---- 关键：通过MFD框架，把hub指针传给每个子设备 ----
       子驱动probe时可以通过 dev_get_drvdata(pdev->dev.parent)
       拿到这个hub指针 */
    ret = devm_mfd_add_devices(&spi->dev, PLATFORM_DEVID_AUTO,
                                 m5io_hub_devs, ARRAY_SIZE(m5io_hub_devs),
                                 NULL, 0, NULL);
    if (ret) {
        dev_err(&spi->dev, "failed to add mfd devices: %d\n", ret);
        return ret;
    }

    dev_info(&spi->dev, "m5io-hub probed, irq=%d\n", hub->irq);
    return 0;
}

static void m5io_hub_remove(struct spi_device *spi)
{
    /* devm_* 自动释放资源，一般不需要手动清理 */
}

static const struct of_device_id m5io_hub_of_match[] = {
    { .compatible = "m5stack,m5io-hub" },
    { }
};
MODULE_DEVICE_TABLE(of, m5io_hub_of_match);

static const struct spi_device_id m5io_hub_spi_id[] = {
    { "m5io-hub", 0 },
    { }
};
MODULE_DEVICE_TABLE(spi, m5io_hub_spi_id);

static struct spi_driver m5io_hub_driver = {
    .driver = {
        .name = "m5io-hub",
        .of_match_table = m5io_hub_of_match,
    },
    .probe  = m5io_hub_probe,
    .remove = m5io_hub_remove,
    .id_table = m5io_hub_spi_id,
};
module_spi_driver(m5io_hub_driver);

MODULE_DESCRIPTION("M5Stack M5IO-HUB MFD core driver");
MODULE_LICENSE("GPL");