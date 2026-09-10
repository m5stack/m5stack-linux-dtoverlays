/*
 * M5Stack M5IO-HUB GPIO child driver
 *
 * GPIO operations and interrupt configuration are transported by the RPC
 * helpers provided by the M5IO-HUB core driver.
 */

#include <linux/bitops.h>
#include <linux/gpio/driver.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/mfd/m5io-hub.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>

#define M5IO_HUB_PIN_MODE_INPUT 0
#define M5IO_HUB_PIN_MODE_OUTPUT 1

struct m5io_hub_gpio {
  struct m5io_hub *hub;
  struct gpio_chip gc;
  struct mutex irq_lock;
  unsigned long irq_enabled;
  unsigned long irq_hw_enabled;
  unsigned int irq_type[M5IO_HUB_NGPIO];
  unsigned int irq_hw_type[M5IO_HUB_NGPIO];
};

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

static int m5io_hub_gpio_irq_set_type(struct irq_data *data,
                                      unsigned int type) {
  struct gpio_chip *gc = irq_data_get_irq_chip_data(data);
  struct m5io_hub_gpio *mg = gpiochip_get_data(gc);
  unsigned int pin = irqd_to_hwirq(data);

  type &= IRQ_TYPE_SENSE_MASK;
  switch (type) {
  case IRQ_TYPE_EDGE_RISING:
  case IRQ_TYPE_EDGE_FALLING:
  case IRQ_TYPE_EDGE_BOTH:
    mg->irq_type[pin] = type;
    irq_set_handler_locked(data, handle_edge_irq);
    return 0;
  default:
    return -EINVAL;
  }
}

static void m5io_hub_gpio_irq_bus_sync_unlock(struct irq_data *data) {
  struct gpio_chip *gc = irq_data_get_irq_chip_data(data);
  struct m5io_hub_gpio *mg = gpiochip_get_data(gc);
  unsigned int pin;
  int ret;

  for (pin = 0; pin < gc->ngpio; pin++) {
    if (test_bit(pin, &mg->irq_enabled)) {
      if (test_bit(pin, &mg->irq_hw_enabled) &&
          mg->irq_hw_type[pin] == mg->irq_type[pin])
        continue;

      ret = m5io_hub_attachInterrupt(mg->hub, pin, mg->irq_type[pin]);
      if (ret) {
        dev_err_ratelimited(gc->parent,
                            "failed to enable irq for GPIO %u: %d\n", pin,
                            ret);
        continue;
      }

      set_bit(pin, &mg->irq_hw_enabled);
      mg->irq_hw_type[pin] = mg->irq_type[pin];
    } else if (test_bit(pin, &mg->irq_hw_enabled)) {
      ret = m5io_hub_detachInterrupt(mg->hub, pin);
      if (ret) {
        dev_err_ratelimited(gc->parent,
                            "failed to disable irq for GPIO %u: %d\n", pin,
                            ret);
        continue;
      }

      clear_bit(pin, &mg->irq_hw_enabled);
      mg->irq_hw_type[pin] = IRQ_TYPE_NONE;
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

static void m5io_hub_gpio_irq_report(void *data, unsigned int pin) {
  struct m5io_hub_gpio *mg = data;
  unsigned int irq;

  if (pin >= mg->gc.ngpio || !test_bit(pin, &mg->irq_enabled))
    return;

  irq = irq_find_mapping(mg->gc.irq.domain, pin);
  if (irq)
    handle_nested_irq(irq);
  else
    dev_dbg(mg->gc.parent, "no Linux IRQ mapped for GPIO %u\n", pin);
}

static int m5io_hub_gpio_get(struct gpio_chip *gc, unsigned int offset) {
  struct m5io_hub_gpio *mg = gpiochip_get_data(gc);

  return m5io_hub_digitalRead(mg->hub, offset);
}

static int m5io_hub_gpio_set(struct gpio_chip *gc, unsigned int offset,
                             int value) {
  struct m5io_hub_gpio *mg = gpiochip_get_data(gc);

  return m5io_hub_digitalWrite(mg->hub, offset, value);
}

static int m5io_hub_gpio_direction_input(struct gpio_chip *gc,
                                         unsigned int offset) {
  struct m5io_hub_gpio *mg = gpiochip_get_data(gc);

  return m5io_hub_pinMode(mg->hub, offset, M5IO_HUB_PIN_MODE_INPUT);
}

static int m5io_hub_gpio_direction_output(struct gpio_chip *gc,
                                          unsigned int offset, int value) {
  struct m5io_hub_gpio *mg = gpiochip_get_data(gc);
  int ret;

  /* Program the output latch before enabling the output to avoid a glitch. */
  ret = m5io_hub_digitalWrite(mg->hub, offset, value);
  if (ret)
    return ret;

  return m5io_hub_pinMode(mg->hub, offset, M5IO_HUB_PIN_MODE_OUTPUT);
}

static int m5io_hub_gpio_probe(struct platform_device *pdev) {
  struct m5io_hub *hub = dev_get_drvdata(pdev->dev.parent);
  struct m5io_hub_gpio *mg;
  struct gpio_irq_chip *girq;
  int ret;

  if (!hub || !hub->spi)
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
  mg->gc.get = m5io_hub_gpio_get;
  mg->gc.set = m5io_hub_gpio_set;
  mg->gc.direction_input = m5io_hub_gpio_direction_input;
  mg->gc.direction_output = m5io_hub_gpio_direction_output;
  mg->gc.can_sleep = true;

  girq = &mg->gc.irq;
  gpio_irq_chip_set_chip(girq, &m5io_hub_gpio_irqchip);
  girq->handler = handle_edge_irq;
  girq->default_type = IRQ_TYPE_NONE;
  girq->threaded = true;

  ret = devm_gpiochip_add_data(&pdev->dev, &mg->gc, mg);
  if (ret) {
    dev_err(&pdev->dev, "failed to register gpiochip: %d\n", ret);
    return ret;
  }

  ret = m5io_hub_register_gpio_irq_handler(
      hub, m5io_hub_gpio_irq_report, mg);
  if (ret) {
    dev_err(&pdev->dev, "failed to register GPIO irq handler: %d\n", ret);
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

  m5io_hub_unregister_gpio_irq_handler(
      mg->hub, m5io_hub_gpio_irq_report, mg);

  mutex_lock(&mg->irq_lock);
  for_each_set_bit(pin, &mg->irq_hw_enabled, mg->gc.ngpio) {
    if (m5io_hub_detachInterrupt(mg->hub, pin))
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
