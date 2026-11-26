// SPDX-License-Identifier: GPL-2.0
/*
 * Power fail GPIO driver
 *
 * The CardputerZero power slider does not cut the power itself. The boost
 * that feeds the CM0 is enabled by the slider OR by PWR_EN from the M5IOE1
 * (PY32) I/O expander. When the slider goes off, the M5IOE1 keeps PWR_EN high
 * for OFF_TIME milliseconds (registers 0xB2/0xB3, 2000 ms after every M5IOE1
 * power-up) and then drops it. The irq GPIO falls with the slider.
 *
 * 2 s is too short for a clean shutdown, so with a power controller (the
 * m5ioe1 node) this driver:
 *
 *   1. programs OFF_TIME to power-hold-ms (default 20000) at probe and again
 *      when the slider goes off (the M5IOE1 re-reads it during the countdown),
 *   2. on a confirmed slider-off, runs orderly_poweroff(): userspace stops
 *      every service and app and unmounts the filesystems,
 *   3. as a backstop, if the system is still up power-hold-ms - 4 s after the
 *      flip, runs SysRq S (sync) and U (remount read-only) before the cut,
 *   4. on power off (only), lowers OFF_TIME to 200 ms in .shutdown, so power
 *      goes as soon as Linux is down instead of at the end of the hold.
 *
 * Without a power controller, or if OFF_TIME cannot be programmed, it keeps
 * the previous behaviour: SysRq U within the 2 s window.
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/i2c.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/reboot.h>
#include <linux/sysrq.h>
#include <linux/workqueue.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/atomic.h>
#include <linux/backlight.h>

/* M5IOE1 registers */
#define M5IOE1_OFF_TIME             0xB2    /* u16 little endian, ms */
#define M5IOE1_POWER_STATE          0xB6
#define M5IOE1_POWER_ON             1       /* slider on */
#define M5IOE1_POWER_DEBOUNCE       2       /* slider off, 100 ms debounce */
#define M5IOE1_POWER_COUNTDOWN      3       /* counting OFF_TIME down */
#define M5IOE1_POWER_CUTTING        4

#define M5IOE1_OFF_TIME_STOCK       2000
#define M5IOE1_OFF_TIME_MIN         200

#define POWERFAIL_HOLD_DEFAULT_MS   20000
#define POWERFAIL_HOLD_MIN_MS       8000    /* a clean poweroff takes ~5 s */
#define POWERFAIL_HOLD_MAX_MS       60000
#define POWERFAIL_BACKSTOP_MS       4000    /* before the cut */

struct powerfail_suo_data {
    struct device *dev;
    struct gpio_desc *gpiod;
    struct gpio_desc *disabled_gpiod;
    struct backlight_device *backlight;
    struct i2c_client *ioe;     /* M5IOE1, NULL: legacy behaviour */
    u32 hold_ms;
    int irq;
    u32 debounce_ms;
    atomic_t triggered;
    atomic_t slider_off;        /* orderly poweroff requested by the slider */
    struct work_struct work;
    struct delayed_work backstop;
};

static int powerfail_set_off_time(struct powerfail_suo_data *data, u16 ms)
{
    /* Both bytes in one transfer: the M5IOE1 applies OFF_TIME per write. */
    u8 buf[2] = { ms & 0xff, ms >> 8 };
    int ret;

    ret = i2c_smbus_write_i2c_block_data(data->ioe, M5IOE1_OFF_TIME,
                                         sizeof(buf), buf);
    if (ret)
        return ret;

    ret = i2c_smbus_read_word_data(data->ioe, M5IOE1_OFF_TIME);
    if (ret < 0)
        return ret;

    return ret == ms ? 0 : -EIO;
}

static void powerfail_legacy(struct powerfail_suo_data *data)
{
    /*
     * U: remount readonly
     *
     * Corresponding Magic SysRq:
     *   echo u > /proc/sysrq-trigger
     */
    dev_emerg(data->dev, "SysRq U: emergency remount filesystems readonly\n");
    handle_sysrq('u');

    if (data->backlight) {
        dev_emerg(data->dev, "set backlight brightness to 0\n");
        backlight_device_set_brightness(data->backlight, 0);
    }

    if (data->disabled_gpiod) {
        dev_emerg(data->dev, "assert disabled gpio low\n");
        gpiod_set_value_cansleep(data->disabled_gpiod, 1);
    }
}

static void powerfail_backstop_work(struct work_struct *work)
{
    struct powerfail_suo_data *data =
        container_of(to_delayed_work(work), struct powerfail_suo_data,
                     backstop);

    dev_emerg(data->dev,
              "poweroff still running near the power cut: SysRq S + U\n");
    handle_sysrq('s');
    msleep(1000);
    handle_sysrq('u');
}

/*
 * True if the slider-off is real: the M5IOE1 counts down. A read error is
 * taken as real, since the irq GPIO has been low for the whole debounce.
 */
static bool powerfail_confirmed(struct powerfail_suo_data *data)
{
    int state = i2c_smbus_read_byte_data(data->ioe, M5IOE1_POWER_STATE);

    if (state == M5IOE1_POWER_DEBOUNCE) {
        msleep(150);
        state = i2c_smbus_read_byte_data(data->ioe, M5IOE1_POWER_STATE);
    }

    return state < 0 || state == M5IOE1_POWER_COUNTDOWN ||
           state == M5IOE1_POWER_CUTTING;
}

static void powerfail_suo_work(struct work_struct *work)
{
    struct powerfail_suo_data *data =
        container_of(work, struct powerfail_suo_data, work);
    unsigned long flipped = jiffies;
    int val, ret;

    /*
     * Software debounce.
     */
    if (data->debounce_ms)
        msleep(data->debounce_ms);

    /*
     * Use the raw level from irq-gpios:
     *     Actual low level -> power-fail signal is active
     *     Actual high level -> power-fail signal is inactive
     */
    val = gpiod_get_raw_value_cansleep(data->gpiod);
    if (val < 0) {
        dev_err(data->dev, "failed to read powerfail gpio: %d\n", val);

        atomic_set(&data->triggered, 0);
        enable_irq(data->irq);
        return;
    }

    if (val > 0) {
        dev_warn(data->dev,
                 "powerfail irq occurred, but gpio is inactive, val=%d\n",
                 val);

        atomic_set(&data->triggered, 0);
        enable_irq(data->irq);
        return;
    }

    if (data->ioe && !powerfail_confirmed(data)) {
        dev_warn(data->dev,
                 "powerfail gpio low, but the power switch is still on\n");

        atomic_set(&data->triggered, 0);
        enable_irq(data->irq);
        return;
    }

    if (!data->ioe) {
        powerfail_legacy(data);
        return;
    }

    /*
     * Program the hold again: an M5IOE1 reset since probe would have put
     * it back to 2 s, and the countdown re-reads it.
     */
    ret = powerfail_set_off_time(data, data->hold_ms);
    if (ret) {
        dev_err(data->dev, "cannot extend the power hold: %d\n", ret);
        powerfail_legacy(data);
        return;
    }

    dev_emerg(data->dev,
              "power switch off: orderly poweroff, power held for %u ms\n",
              data->hold_ms);

    if (data->backlight)
        backlight_device_set_brightness(data->backlight, 0);

    atomic_set(&data->slider_off, 1);
    schedule_delayed_work(&data->backstop,
                          flipped + msecs_to_jiffies(data->hold_ms -
                                                     POWERFAIL_BACKSTOP_MS) -
                          jiffies);

    /*
     * force: if userspace cannot be asked to power off, the kernel syncs
     * and powers off on its own.
     */
    orderly_poweroff(true);
}

static irqreturn_t powerfail_suo_irq_handler(int irq, void *dev_id)
{
    struct powerfail_suo_data *data = dev_id;

    /*
     * Prevent repeated triggers.
     */
    if (atomic_xchg(&data->triggered, 1))
        return IRQ_HANDLED;

    /*
     * Disable subsequent interrupts.
     * Power-fail handling is allowed to run only once.
     */
    disable_irq_nosync(data->irq);

    /*
     * Do not run sync/remount/poweroff from the hard IRQ context.
     * Run it from a workqueue instead.
     */
    schedule_work(&data->work);

    return IRQ_HANDLED;
}

/*
 * The M5IOE1 controlling system power: the m5stack,power-controller phandle,
 * or else the controller of disabled-gpios when that is an M5IOE1 (overlays
 * that predate the property).
 */
static int powerfail_find_ioe(struct powerfail_suo_data *data)
{
    struct device_node *np = data->dev->of_node;
    struct device_node *ioe_np;

    ioe_np = of_parse_phandle(np, "m5stack,power-controller", 0);
    if (!ioe_np) {
        ioe_np = of_parse_phandle(np, "disabled-gpios", 0);
        if (ioe_np && !of_device_is_compatible(ioe_np, "m5stack,m5ioe1")) {
            of_node_put(ioe_np);
            ioe_np = NULL;
        }
    }
    if (!ioe_np)
        return 0;

    data->ioe = of_find_i2c_device_by_node(ioe_np);
    of_node_put(ioe_np);

    return data->ioe ? 0 : -EPROBE_DEFER;
}

static void powerfail_put_ioe(void *ioe)
{
    put_device(&((struct i2c_client *)ioe)->dev);
}

static int powerfail_suo_probe(struct platform_device *pdev)
{
    struct device *dev = &pdev->dev;
    struct powerfail_suo_data *data;
    int ret;

    data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
    if (!data)
        return -ENOMEM;

    data->dev = dev;
    atomic_set(&data->triggered, 0);
    atomic_set(&data->slider_off, 0);
    INIT_WORK(&data->work, powerfail_suo_work);
    INIT_DELAYED_WORK(&data->backstop, powerfail_backstop_work);

    of_property_read_u32(dev->of_node, "debounce-ms", &data->debounce_ms);

    data->hold_ms = POWERFAIL_HOLD_DEFAULT_MS;
    of_property_read_u32(dev->of_node, "power-hold-ms", &data->hold_ms);
    data->hold_ms = clamp_t(u32, data->hold_ms, POWERFAIL_HOLD_MIN_MS,
                            POWERFAIL_HOLD_MAX_MS);

    /*
     * Corresponding Device Tree property:
     *
     *   irq-gpios = <&gpio 23 0x00>;
     */
    data->gpiod = devm_gpiod_get(dev, "irq", GPIOD_IN);
    if (IS_ERR(data->gpiod)) {
        ret = PTR_ERR(data->gpiod);
        dev_err(dev, "failed to get irq gpio: %d\n", ret);
        return ret;
    }

    data->disabled_gpiod = devm_gpiod_get_optional(dev, "disabled",
                                                   GPIOD_OUT_LOW);
    if (IS_ERR(data->disabled_gpiod))
        return dev_err_probe(dev,
                             PTR_ERR(data->disabled_gpiod),
                             "failed to get disabled gpio\n");

    data->backlight = devm_of_find_backlight(dev);
    if (IS_ERR(data->backlight))
        return dev_err_probe(dev,
                             PTR_ERR(data->backlight),
                             "failed to get backlight\n");

    ret = powerfail_find_ioe(data);
    if (ret)
        return dev_err_probe(dev, ret, "power controller not ready\n");
    if (data->ioe) {
        ret = devm_add_action_or_reset(dev, powerfail_put_ioe, data->ioe);
        if (ret)
            return ret;

        ret = powerfail_set_off_time(data, data->hold_ms);
        if (ret) {
            dev_warn(dev,
                     "cannot set the power hold (%d), using SysRq U only\n",
                     ret);
            data->ioe = NULL;   /* still released by the devm action */
        }
    }

    data->irq = platform_get_irq(pdev, 0);
    if (data->irq < 0) {
        dev_err(dev, "failed to get irq from interrupts property: %d\n",
                data->irq);
        return data->irq;
    }

    /*
     * The interrupt trigger type is determined by the Device Tree interrupts property:
     *
     *   interrupt-parent = <&gpio>;
     *   interrupts = <23 2>;  // falling edge
     */
    ret = devm_request_irq(dev,
                           data->irq,
                           powerfail_suo_irq_handler,
                           0,
                           dev_name(dev),
                           data);
    if (ret) {
        dev_err(dev, "failed to request irq %d: %d\n",
                data->irq, ret);
        return ret;
    }

    platform_set_drvdata(pdev, data);

    if (data->ioe)
        dev_info(dev,
                 "powerfail-suo driver probed, irq=%d, debounce=%u ms, orderly poweroff, power hold %u ms\n",
                 data->irq, data->debounce_ms, data->hold_ms);
    else
        dev_info(dev,
                 "powerfail-suo driver probed, irq=%d, debounce=%u ms, SysRq U only\n",
                 data->irq, data->debounce_ms);

    return 0;
}

static void powerfail_suo_shutdown(struct platform_device *pdev)
{
    struct powerfail_suo_data *data = platform_get_drvdata(pdev);

    if (!data || !data->ioe)
        return;

    cancel_delayed_work_sync(&data->backstop);

    /*
     * Powering off (never on reboot): Linux is down and the filesystems are
     * unmounted. After a slider-off, cut the power now. After a poweroff
     * from the menu, the slider is still on and the board stays powered as
     * before, but it now turns off 200 ms after the slider goes off.
     */
    if (system_state != SYSTEM_POWER_OFF)
        return;

    if (atomic_read(&data->slider_off) && data->disabled_gpiod)
        gpiod_set_value_cansleep(data->disabled_gpiod, 1);

    powerfail_set_off_time(data, M5IOE1_OFF_TIME_MIN);
}

static void powerfail_suo_remove(struct platform_device *pdev)
{
    struct powerfail_suo_data *data = platform_get_drvdata(pdev);

    if (!data)
        return;

    cancel_work_sync(&data->work);
    cancel_delayed_work_sync(&data->backstop);

    /* Unloaded: the 2 s window the SysRq-only behaviour expects. */
    if (data->ioe && !atomic_read(&data->slider_off))
        powerfail_set_off_time(data, M5IOE1_OFF_TIME_STOCK);
}

static const struct of_device_id powerfail_suo_of_match[] = {
    { .compatible = "cardputerzero,powerfail-suo" },
    { }
};
MODULE_DEVICE_TABLE(of, powerfail_suo_of_match);

static struct platform_driver powerfail_suo_driver = {
    .probe    = powerfail_suo_probe,
    .remove   = powerfail_suo_remove,
    .shutdown = powerfail_suo_shutdown,
    .driver = {
        .name = "powerfail-suo",
        .of_match_table = powerfail_suo_of_match,
    },
};

module_platform_driver(powerfail_suo_driver);

MODULE_AUTHOR("dianjixz <dianjixz@m5stack.com>");
MODULE_DESCRIPTION("Power fail GPIO driver: orderly poweroff from the power slider");
MODULE_LICENSE("GPL");
