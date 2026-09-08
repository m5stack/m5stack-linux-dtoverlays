#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/i2c.h>
#include <linux/mfd/m5io-hub.h>

struct m5io_hub_i2c {
    struct m5io_hub *hub;
    struct i2c_adapter adap;
};

static int m5io_hub_i2c_xfer(struct i2c_adapter *adap,
                                struct i2c_msg *msgs, int num)
{
    struct m5io_hub_i2c *mi = i2c_get_adapdata(adap);

    /* TODO: 把i2c_msg转换成对m5io-hub内部寄存器的SPI读写序列
       比如：写目标i2c地址+数据到某个"I2C桥"寄存器，
       芯片内部硬件模块负责真正的I2C时序 */

    return num;  /* 占位：假装成功 */
}

static const struct i2c_algorithm m5io_hub_i2c_algo = {
    .master_xfer = m5io_hub_i2c_xfer,
};

static int m5io_hub_i2c_probe(struct platform_device *pdev)
{
    struct m5io_hub *hub = dev_get_drvdata(pdev->dev.parent);
    struct m5io_hub_i2c *mi;
    int ret;

    mi = devm_kzalloc(&pdev->dev, sizeof(*mi), GFP_KERNEL);
    if (!mi)
        return -ENOMEM;

    mi->hub = hub;
    mi->adap.owner = THIS_MODULE;
    mi->adap.algo = &m5io_hub_i2c_algo;
    mi->adap.dev.parent = &pdev->dev;
    mi->adap.dev.of_node = pdev->dev.of_node;
    snprintf(mi->adap.name, sizeof(mi->adap.name), "m5io-hub-i2c");
    i2c_set_adapdata(&mi->adap, mi);

    ret = devm_i2c_add_adapter(&pdev->dev, &mi->adap);
    if (ret)
        return ret;

    platform_set_drvdata(pdev, mi);
    dev_info(&pdev->dev, "m5io-hub-i2c probed (stub)\n");
    return 0;
}

static const struct of_device_id m5io_hub_i2c_of_match[] = {
    { .compatible = "m5stack,m5io-hub-i2c" },
    { }
};
MODULE_DEVICE_TABLE(of, m5io_hub_i2c_of_match);

static struct platform_driver m5io_hub_i2c_driver = {
    .driver = {
        .name = "m5io-hub-i2c",
        .of_match_table = m5io_hub_i2c_of_match,
    },
    .probe = m5io_hub_i2c_probe,
};
module_platform_driver(m5io_hub_i2c_driver);

MODULE_DESCRIPTION("M5Stack M5IO-HUB I2C bridge driver (stub)");
MODULE_LICENSE("GPL");