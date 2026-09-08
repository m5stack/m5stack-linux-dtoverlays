#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/gpio/driver.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/mfd/m5io-hub.h>
#include <linux/bitops.h>

struct m5io_hub_gpio {
    struct m5io_hub *hub;
    struct gpio_chip gc;
    struct mutex irq_lock;      /* 保护中断相关寄存器的读写 */
    u32 irq_mask;                /* 软件缓存的中断屏蔽状态 */
    u32 irq_type_rise;
    u32 irq_type_fall;
};

/* ---------------- GPIO 基本操作 ---------------- */

static int m5io_hub_gpio_get(struct gpio_chip *gc, unsigned offset)
{
    struct m5io_hub_gpio *mg = gpiochip_get_data(gc);
    unsigned int val;
    int ret;

    ret = regmap_read(mg->hub->regmap, M5IO_HUB_REG_GPIO_IN, &val);
    if (ret)
        return ret;

    return !!(val & BIT(offset));
}

static void m5io_hub_gpio_set(struct gpio_chip *gc, unsigned offset, int value)
{
    struct m5io_hub_gpio *mg = gpiochip_get_data(gc);

    regmap_update_bits(mg->hub->regmap, M5IO_HUB_REG_GPIO_OUT,
                         BIT(offset), value ? BIT(offset) : 0);
}

static int m5io_hub_gpio_direction_input(struct gpio_chip *gc, unsigned offset)
{
    struct m5io_hub_gpio *mg = gpiochip_get_data(gc);

    return regmap_update_bits(mg->hub->regmap, M5IO_HUB_REG_GPIO_DIR,
                                BIT(offset), 0);
}

static int m5io_hub_gpio_direction_output(struct gpio_chip *gc,
                                             unsigned offset, int value)
{
    struct m5io_hub_gpio *mg = gpiochip_get_data(gc);

    m5io_hub_gpio_set(gc, offset, value);
    return regmap_update_bits(mg->hub->regmap, M5IO_HUB_REG_GPIO_DIR,
                                BIT(offset), BIT(offset));
}

/* ---------------- IRQ chip 操作 ----------------
   这些函数由内核irq子系统在 mask/unmask/set_type 时调用，
   对应 request_irq() -> irq_startup() -> irq_unmask() 等流程 */

static void m5io_hub_irq_mask(struct irq_data *d)
{
    struct m5io_hub_gpio *mg = irq_data_get_irq_chip_data(d);

    mg->irq_mask &= ~BIT(d->hwirq);
    regmap_update_bits(mg->hub->regmap, M5IO_HUB_REG_IRQ_MASK,
                         BIT(d->hwirq), 0);
}

static void m5io_hub_irq_unmask(struct irq_data *d)
{
    struct m5io_hub_gpio *mg = irq_data_get_irq_chip_data(d);

    mg->irq_mask |= BIT(d->hwirq);
    regmap_update_bits(mg->hub->regmap, M5IO_HUB_REG_IRQ_MASK,
                         BIT(d->hwirq), BIT(d->hwirq));
}

static int m5io_hub_irq_set_type(struct irq_data *d, unsigned int type)
{
    struct m5io_hub_gpio *mg = irq_data_get_irq_chip_data(d);
    int hwirq = d->hwirq;

    switch (type) {
    case IRQ_TYPE_EDGE_RISING:
        mg->irq_type_rise |= BIT(hwirq);
        mg->irq_type_fall &= ~BIT(hwirq);
        break;
    case IRQ_TYPE_EDGE_FALLING:
        mg->irq_type_rise &= ~BIT(hwirq);
        mg->irq_type_fall |= BIT(hwirq);
        break;
    case IRQ_TYPE_EDGE_BOTH:
        mg->irq_type_rise |= BIT(hwirq);
        mg->irq_type_fall |= BIT(hwirq);
        break;
    default:
        return -EINVAL;
    }

    regmap_update_bits(mg->hub->regmap, M5IO_HUB_REG_IRQ_TYPE_R,
                         BIT(hwirq), mg->irq_type_rise & BIT(hwirq));
    regmap_update_bits(mg->hub->regmap, M5IO_HUB_REG_IRQ_TYPE_F,
                         BIT(hwirq), mg->irq_type_fall & BIT(hwirq));
    return 0;
}

static void m5io_hub_irq_bus_lock(struct irq_data *d)
{
    struct m5io_hub_gpio *mg = irq_data_get_irq_chip_data(d);
    mutex_lock(&mg->irq_lock);
}

static void m5io_hub_irq_bus_sync_unlock(struct irq_data *d)
{
    struct m5io_hub_gpio *mg = irq_data_get_irq_chip_data(d);
    mutex_unlock(&mg->irq_lock);
}

static struct irq_chip m5io_hub_irqchip = {
    .name = "m5io-hub-irq",
    .irq_mask = m5io_hub_irq_mask,
    .irq_unmask = m5io_hub_irq_unmask,
    .irq_set_type = m5io_hub_irq_set_type,
    .irq_bus_lock = m5io_hub_irq_bus_lock,
    .irq_bus_sync_unlock = m5io_hub_irq_bus_sync_unlock,
};

/* ---------------- 顶层中断线程：查询状态寄存器并分发 ---------------- */

static irqreturn_t m5io_hub_irq_thread(int irq, void *data)
{
    struct m5io_hub_gpio *mg = data;
    unsigned int status;
    int ret, pin;

    ret = regmap_read(mg->hub->regmap, M5IO_HUB_REG_IRQ_STATUS, &status);
    if (ret)
        return IRQ_NONE;

    if (!status)
        return IRQ_NONE;

    for_each_set_bit(pin, (unsigned long *)&status, M5IO_HUB_NGPIO) {
        int virq = irq_find_mapping(mg->gc.irq.domain, pin);

        if (virq)
            handle_nested_irq(virq);
    }

    /* 清除中断状态位（假设写1清零，具体看芯片协议）*/
    regmap_write(mg->hub->regmap, M5IO_HUB_REG_IRQ_STATUS, status);

    return IRQ_HANDLED;
}

/* ---------------- probe ---------------- */

static int m5io_hub_gpio_probe(struct platform_device *pdev)
{
    struct m5io_hub *hub = dev_get_drvdata(pdev->dev.parent);
    struct m5io_hub_gpio *mg;
    struct gpio_irq_chip *girq;
    int ret;

    mg = devm_kzalloc(&pdev->dev, sizeof(*mg), GFP_KERNEL);
    if (!mg)
        return -ENOMEM;

    mg->hub = hub;
    mutex_init(&mg->irq_lock);

    mg->gc.label = "m5io-hub-gpio";
    mg->gc.parent = &pdev->dev;
    mg->gc.owner = THIS_MODULE;
    mg->gc.base = -1;
    mg->gc.ngpio = M5IO_HUB_NGPIO;
    mg->gc.get = m5io_hub_gpio_get;
    mg->gc.set = m5io_hub_gpio_set;
    mg->gc.direction_input = m5io_hub_gpio_direction_input;
    mg->gc.direction_output = m5io_hub_gpio_direction_output;
    mg->gc.can_sleep = true;   /* 因为底层要走SPI，会睡眠 */

    /* ---- 关键：配置gpiochip自带的irqchip能力 ---- */
    girq = &mg->gc.irq;
    girq->chip = &m5io_hub_irqchip;
    girq->handler = handle_edge_irq;   /* 每个pin默认按边沿处理 */
    girq->default_type = IRQ_TYPE_NONE;
    girq->threaded = true;   /* 因为handle_nested_irq在线程上下文调用 */
    /* 注意：这里不设置parent_handler,因为我们是手动在
       m5io_hub_irq_thread里调用handle_nested_irq，
       而不是标准的chained irq模式 */

    ret = devm_gpiochip_add_data(&pdev->dev, &mg->gc, mg);
    if (ret) {
        dev_err(&pdev->dev, "failed to add gpiochip: %d\n", ret);
        return ret;
    }

    /* ---- 注册"上行"物理中断：spi->irq ---- */
    ret = devm_request_threaded_irq(&pdev->dev, hub->irq, NULL,
                                       m5io_hub_irq_thread,
                                       IRQF_ONESHOT | IRQF_TRIGGER_LOW,
                                       "m5io-hub-irq", mg);
    if (ret) {
        dev_err(&pdev->dev, "failed to request irq: %d\n", ret);
        return ret;
    }

    platform_set_drvdata(pdev, mg);
    dev_info(&pdev->dev, "m5io-hub-gpio probed, %d pins\n", mg->gc.ngpio);
    return 0;
}

static const struct of_device_id m5io_hub_gpio_of_match[] = {
    { .compatible = "m5stack,m5io-hub-gpio" },
    { }
};
MODULE_DEVICE_TABLE(of, m5io_hub_gpio_of_match);

static struct platform_driver m5io_hub_gpio_driver = {
    .driver = {
        .name = "m5io-hub-gpio",
        .of_match_table = m5io_hub_gpio_of_match,
    },
    .probe = m5io_hub_gpio_probe,
};
module_platform_driver(m5io_hub_gpio_driver);

MODULE_DESCRIPTION("M5Stack M5IO-HUB GPIO/IRQ driver");
MODULE_LICENSE("GPL");