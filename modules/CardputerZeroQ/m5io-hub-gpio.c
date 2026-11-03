// SPDX-License-Identifier: GPL-2.0
/*
 * M5Stack M5IO-HUB GPIO child driver
 *
 * 全部硬件访问都走 m5io-hub-api.h 暴露的 m5io_hub_gpio_* 接口：这些接口是
 * FUNC=M5IO_HUB_GPIO_FUN 的 TOKEN 子命令封装（无 DATA 通道），本驱动不再关心
 * 帧格式、TID、CRC 与 SPI 传输。
 *
 * 与旧接口的差异（迁移要点）：
 *   m5io_hub_pinMode/digitalWrite/digitalRead -> m5io_hub_gpio_* 同名后缀
 *   m5io_hub_attachInterrupt(hub, pin, linux_type)
 *     -> m5io_hub_gpio_attachInterrupt(hub, pin, wire_type, handler, priv)
 *        ① 触发类型要传线上值 0=下降/1=上升/2=双边，不是 IRQ_TYPE_* 的位值；
 *        ② 每位引脚自带回调，核心侧不再有全局 handler，必须逐脚注册/注销。
 *   m5io_hub_register/unregister_gpio_irq_handler -> 删除，回调随 attach/detach 生效。
 */

#include <linux/bitops.h>
#include <linux/gpio/driver.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/version.h>

#include "m5io-hub.h"

/* m5io_hub_gpio_pinMode() 的 @mode 取值：0=输出、1=输入（不是 Arduino 的编号）。 */
#define M5IO_HUB_PIN_MODE_OUTPUT 0
#define M5IO_HUB_PIN_MODE_INPUT  1

struct m5io_hub_gpio {
  struct m5io_hub *hub;
  struct gpio_chip gc;
  struct mutex irq_lock;
  /* Linux 侧已 unmask 的引脚位图。 */
  unsigned long irq_enabled;
  /* 已在 MCU 上 attach 的引脚位图（与 irq_enabled 之差就是待同步的量）。 */
  unsigned long irq_hw_enabled;
  /* 用户请求的 Linux 触发类型（IRQ_TYPE_*）。 */
  unsigned int irq_type[M5IO_HUB_NGPIO];
  /* MCU 上当前生效的线上触发类型（0/1/2），仅在 irq_hw_enabled 置位时有效。 */
  u8 irq_wire_type[M5IO_HUB_NGPIO];
  /*
   * Cached line direction.  The protocol has no command that reads a pin's
   * direction back, so the driver records what it was asked to set.  Zero means
   * input, which is also the MCU power-on default.  Without the matching
   * get_direction callback gpiolib hits WARN_ON(!gc->get_direction) on any
   * direction read -- and reading /sys/kernel/debug/gpio reads every line.
   */
  u8 direction[M5IO_HUB_NGPIO];
};

/* 定义在 irq_chip 之后；sync_unlock 里作为 attachInterrupt 的逐脚回调传给核心。 */
static void m5io_hub_gpio_irq_report(void *data, unsigned int pin);

static void m5io_hub_gpio_irq_bus_lock(struct irq_data *data) {
  struct gpio_chip *gc = irq_data_get_irq_chip_data(data);
  struct m5io_hub_gpio *mg = gpiochip_get_data(gc);

  mutex_lock(&mg->irq_lock);
}

static void m5io_hub_gpio_irq_mask(struct irq_data *data) {
  struct gpio_chip *gc = irq_data_get_irq_chip_data(data);
  struct m5io_hub_gpio *mg = gpiochip_get_data(gc);

  clear_bit(irqd_to_hwirq(data), &mg->irq_enabled);
  gpiochip_disable_irq(gc, irqd_to_hwirq(data));
}

static void m5io_hub_gpio_irq_unmask(struct irq_data *data) {
  struct gpio_chip *gc = irq_data_get_irq_chip_data(data);
  struct m5io_hub_gpio *mg = gpiochip_get_data(gc);

  set_bit(irqd_to_hwirq(data), &mg->irq_enabled);
  gpiochip_enable_irq(gc, irqd_to_hwirq(data));
}

/*
 * 新的 attach 接口只接受线上编码，这里把 Linux 类型翻译成 0/1/2；
 * 返回负值表示该类型不受 MCU 支持（电平触发、IRQ_TYPE_NONE 等）。
 */
static int m5io_hub_gpio_wire_type(unsigned int type) {
  switch (type) {
  case IRQ_TYPE_EDGE_FALLING:
    return M5IO_HUB_GPIO_EDGE_FALLING;
  case IRQ_TYPE_EDGE_RISING:
    return M5IO_HUB_GPIO_EDGE_RISING;
  case IRQ_TYPE_EDGE_BOTH:
    return M5IO_HUB_GPIO_EDGE_BOTH;
  default:
    return -EINVAL;
  }
}

static int m5io_hub_gpio_irq_set_type(struct irq_data *data,
                                      unsigned int type) {
  struct gpio_chip *gc = irq_data_get_irq_chip_data(data);
  struct m5io_hub_gpio *mg = gpiochip_get_data(gc);
  unsigned int pin = irqd_to_hwirq(data);

  type &= IRQ_TYPE_SENSE_MASK;
  if (m5io_hub_gpio_wire_type(type) < 0)
    return -EINVAL;

  mg->irq_type[pin] = type;
  irq_set_handler_locked(data, handle_edge_irq);
  return 0;
}

static void m5io_hub_gpio_irq_bus_sync_unlock(struct irq_data *data) {
  struct gpio_chip *gc = irq_data_get_irq_chip_data(data);
  struct m5io_hub_gpio *mg = gpiochip_get_data(gc);
  unsigned int pin;

  for (pin = 0; pin < gc->ngpio; pin++) {
    int ret;

    if (test_bit(pin, &mg->irq_enabled)) {
      int wire = m5io_hub_gpio_wire_type(mg->irq_type[pin]);

      /* irq_set_type() 已过滤非法类型，这里只是兜底。 */
      if (wire < 0)
        continue;

      if (test_bit(pin, &mg->irq_hw_enabled) &&
          mg->irq_wire_type[pin] == (u8)wire)
        continue;

      /*
       * attach 同时“注册回调 + 配边沿”：重复 attach 既用于首次使能，也用于
       * 仅改触发类型（回调与私有指针不变，核心侧按 pin 覆盖即可）。
       */
      ret = m5io_hub_gpio_attachInterrupt(
          mg->hub, pin, (u8)wire, m5io_hub_gpio_irq_report, mg);
      if (ret) {
        dev_err_ratelimited(gc->parent,
                            "failed to enable irq for GPIO %u: %d\n", pin,
                            ret);
        continue;
      }

      set_bit(pin, &mg->irq_hw_enabled);
      mg->irq_wire_type[pin] = (u8)wire;
    } else if (test_bit(pin, &mg->irq_hw_enabled)) {
      /* detach 会一并注销该脚的回调。 */
      ret = m5io_hub_gpio_detachInterrupt(mg->hub, pin);
      if (ret) {
        dev_err_ratelimited(gc->parent,
                            "failed to disable irq for GPIO %u: %d\n", pin,
                            ret);
        continue;
      }

      clear_bit(pin, &mg->irq_hw_enabled);
      mg->irq_wire_type[pin] = 0;
    }
  }

  mutex_unlock(&mg->irq_lock);
}

static const struct irq_chip m5io_hub_gpio_irqchip = {
    .name = "m5io-hub-gpio",
    .irq_mask = m5io_hub_gpio_irq_mask,
    .irq_unmask = m5io_hub_gpio_irq_unmask,
    .irq_set_type = m5io_hub_gpio_irq_set_type,
    .irq_bus_lock = m5io_hub_gpio_irq_bus_lock,
    .irq_bus_sync_unlock = m5io_hub_gpio_irq_bus_sync_unlock,
    .flags = IRQCHIP_IMMUTABLE,
    GPIOCHIP_IRQ_RESOURCE_HELPERS,
};

/*
 * 每脚一份回调（attachInterrupt 注册的就是它），所以 @data 就是本 gpiochip，
 * MCU 上行事件只带 pin，私有指针由核心按 pin 找回。
 */
static void m5io_hub_gpio_irq_report(void *data, unsigned int pin) {
  struct m5io_hub_gpio *mg = data;
  unsigned int irq;

  if (!mg || pin >= mg->gc.ngpio || !test_bit(pin, &mg->irq_enabled))
    return;

  irq = irq_find_mapping(mg->gc.irq.domain, pin);
  if (irq)
    handle_nested_irq(irq);
  else
    dev_dbg(mg->gc.parent, "no Linux IRQ mapped for GPIO %u\n", pin);
}

static int m5io_hub_gpio_get(struct gpio_chip *gc, unsigned int offset) {
  struct m5io_hub_gpio *mg = gpiochip_get_data(gc);

  /* 成功返回 0/1；失败返回负错误码，gpiolib 会原样上抛。 */
  return m5io_hub_gpio_digitalRead(mg->hub, (u8)offset);
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(7, 0, 0)
static int m5io_hub_gpio_set(struct gpio_chip *gc, unsigned int offset,
                             int value) {
#else
static void m5io_hub_gpio_set(struct gpio_chip *gc, unsigned int offset,
                              int value) {
#endif
  struct m5io_hub_gpio *mg = gpiochip_get_data(gc);
  int ret;

  /* 接口语义：非 0=高、0=低；固件拒绝对输入脚写值（返回 -EINVAL 一类）。 */
  ret = m5io_hub_gpio_digitalWrite(mg->hub, (u8)offset, value ? 1 : 0);
  if (ret)
    dev_err_ratelimited(gc->parent, "failed to set GPIO %u: %d\n", offset,
                        ret);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(7, 0, 0)
  return ret;
#endif
}

static int m5io_hub_gpio_get_direction(struct gpio_chip *gc,
                                       unsigned int offset) {
  struct m5io_hub_gpio *mg = gpiochip_get_data(gc);

  return mg->direction[offset] ? GPIO_LINE_DIRECTION_OUT
                               : GPIO_LINE_DIRECTION_IN;
}

static int m5io_hub_gpio_direction_input(struct gpio_chip *gc,
                                         unsigned int offset) {
  struct m5io_hub_gpio *mg = gpiochip_get_data(gc);
  int ret;

  ret = m5io_hub_gpio_pinMode(mg->hub, (u8)offset, M5IO_HUB_PIN_MODE_INPUT);
  if (!ret)
    mg->direction[offset] = 0;
  return ret;
}

static int m5io_hub_gpio_direction_output(struct gpio_chip *gc,
                                          unsigned int offset, int value) {
  struct m5io_hub_gpio *mg = gpiochip_get_data(gc);
  int ret;

  /* MCU rejects digitalWrite on an input; the protocol has no atomic mode/value command. */
  ret = m5io_hub_gpio_pinMode(mg->hub, (u8)offset, M5IO_HUB_PIN_MODE_OUTPUT);
  if (ret)
    return ret;
  mg->direction[offset] = 1;

  return m5io_hub_gpio_digitalWrite(mg->hub, (u8)offset, value ? 1 : 0);
}

static int m5io_hub_gpio_probe(struct platform_device *pdev) {
  struct m5io_hub *hub;
  struct m5io_hub_gpio *mg;
  struct gpio_irq_chip *girq;
  int ret;

  /*
   * struct m5io_hub 现在是新 API 里的不透明句柄，驱动不再检查 hub->spi，
   * 只等父设备把句柄挂上 drvdata；底层未就绪时各接口自身返回 -ENODEV。
   */
  if (!pdev->dev.parent)
    return -EPROBE_DEFER;
  hub = dev_get_drvdata(pdev->dev.parent);
  if (!hub)
    return -EPROBE_DEFER;

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
  mg->gc.names = m5io_hub_gpio_names;
  mg->gc.get = m5io_hub_gpio_get;
  mg->gc.set = m5io_hub_gpio_set;
  mg->gc.get_direction = m5io_hub_gpio_get_direction;
  mg->gc.direction_input = m5io_hub_gpio_direction_input;
  mg->gc.direction_output = m5io_hub_gpio_direction_output;
  mg->gc.can_sleep = true;

  girq = &mg->gc.irq;
  gpio_irq_chip_set_chip(girq, &m5io_hub_gpio_irqchip);
  girq->handler = handle_edge_irq;
  girq->default_type = IRQ_TYPE_NONE;
  girq->threaded = true;

  /*
   * 新接口按脚注册回调，probe 阶段没有引脚需要 attach，因此这里不再像旧版
   * 那样调用 m5io_hub_register_gpio_irq_handler() 注册全局 handler。
   */
  ret = devm_gpiochip_add_data(&pdev->dev, &mg->gc, mg);
  if (ret) {
    dev_err(&pdev->dev, "failed to register gpiochip: %d\n", ret);
    return ret;
  }

  platform_set_drvdata(pdev, mg);
  dev_info(&pdev->dev, "registered %u GPIOs with interrupt support\n",
           mg->gc.ngpio);
  return 0;
}

static void m5io_hub_gpio_remove(struct platform_device *pdev) {
  struct m5io_hub_gpio *mg = platform_get_drvdata(pdev);
  unsigned int pin;

  if (!mg)
    return;

  mutex_lock(&mg->irq_lock);
  for_each_set_bit(pin, &mg->irq_hw_enabled, mg->gc.ngpio) {
    if (m5io_hub_gpio_detachInterrupt(mg->hub, (u8)pin))
      dev_warn(&pdev->dev, "failed to disable irq for GPIO %u\n", pin);
  }
  mg->irq_hw_enabled = 0;
  mutex_unlock(&mg->irq_lock);
}

static const struct of_device_id m5io_hub_gpio_of_match[] = {
    {.compatible = "m5stack,m5io-hub-gpio"},
    {},
};
MODULE_DEVICE_TABLE(of, m5io_hub_gpio_of_match);

static struct platform_driver m5io_hub_gpio_driver = {
    .driver =
        {
            .name = "m5io-hub-gpio",
            .of_match_table = m5io_hub_gpio_of_match,
        },
    .probe = m5io_hub_gpio_probe,
    .remove = m5io_hub_gpio_remove,
};
module_platform_driver(m5io_hub_gpio_driver);

MODULE_DESCRIPTION("M5Stack M5IO-HUB GPIO driver");
MODULE_LICENSE("GPL");
