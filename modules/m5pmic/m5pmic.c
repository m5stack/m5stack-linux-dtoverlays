// SPDX-License-Identifier: GPL-2.0-only
/*
 * M5Stack M5PM1 power-management MCU driver.
 *
 * M5PM1 is an I2C-addressed MCU rather than a generic regulator PMIC.  This
 * driver deliberately exposes the parts which have stable Linux semantics:
 * the five GPIOs (including their pull and drive configuration), GPIO IRQs,
 * and the read-only voltage/temperature measurements through hwmon.  Power
 * switching, timers, watchdog, NeoPixel and system commands remain available
 * to a board-specific userspace policy until their safety policy is defined.
 *
 * The M5PM1 may enter an I2C idle-sleep state.  Its first transaction after
 * sleep is a wake-up transaction and is reported as an I2C error by the MCU.
 * The small retry loop in the regmap bus handles that documented behaviour.
 */

#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/gpio/driver.h>
#include <linux/hwmon.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/pinctrl/pinconf-generic.h>
#include <linux/regmap.h>

#define M5PMIC_DRV_NAME             "m5pmic"
#define M5PMIC_DEVICE_ID            0x50
#define M5PMIC_DEVICE_MODEL         0x20
#define M5PMIC_NGPIO                5

/* Register map. The MCU rejects a burst which crosses one of these ranges. */
#define M5PMIC_REG_DEVICE_ID        0x00
#define M5PMIC_REG_DEVICE_MODEL     0x01
#define M5PMIC_REG_GPIO_MODE        0x10
#define M5PMIC_REG_GPIO_OUT         0x11
#define M5PMIC_REG_GPIO_IN          0x12
#define M5PMIC_REG_GPIO_DRV         0x13
#define M5PMIC_REG_GPIO_PUPD0       0x14
#define M5PMIC_REG_GPIO_PUPD1       0x15
#define M5PMIC_REG_GPIO_FUNC0       0x16
#define M5PMIC_REG_GPIO_FUNC1       0x17
#define M5PMIC_REG_GPIO_WAKE_CFG    0x19
#define M5PMIC_REG_VBAT_L           0x22
#define M5PMIC_REG_VIN_L            0x24
#define M5PMIC_REG_5VOUT_L          0x26
#define M5PMIC_REG_ADC_RES_L        0x28
#define M5PMIC_REG_ADC_CTRL         0x2a
#define M5PMIC_REG_IRQ_STATUS1      0x40
#define M5PMIC_REG_IRQ_STATUS2      0x41
#define M5PMIC_REG_IRQ_STATUS3      0x42
#define M5PMIC_REG_IRQ_MASK1        0x43

#define M5PMIC_GPIO_MASK            GENMASK(M5PMIC_NGPIO - 1, 0)
#define M5PMIC_ADC_START            BIT(0)
#define M5PMIC_ADC_TEMP_CHANNEL     6
#define M5PMIC_I2C_RETRIES          4
#define M5PMIC_I2C_RETRY_DELAY_MS   50
#define M5PMIC_ADC_TIMEOUT_MS       500
#define M5PMIC_ADC_POLL_MS          10

struct m5pmic {
	struct device *dev;
	struct i2c_client *client;
	struct regmap *regmap;
	struct mutex lock;
	struct gpio_chip gpio;
	struct irq_chip irq_chip;
	u8 irq_mask;
	u8 irq_type[M5PMIC_NGPIO];
	int parent_irq;
};

static bool m5pmic_in_range(unsigned int reg, unsigned int first,
			    unsigned int last)
{
	return reg >= first && reg <= last;
}

static bool m5pmic_readable_reg(struct device *dev, unsigned int reg)
{
	return m5pmic_in_range(reg, 0x00, 0x0a) ||
	       m5pmic_in_range(reg, 0x10, 0x19) ||
	       m5pmic_in_range(reg, 0x20, 0x2a) ||
	       m5pmic_in_range(reg, 0x30, 0x35) ||
	       m5pmic_in_range(reg, 0x38, 0x3c) ||
	       m5pmic_in_range(reg, 0x40, 0x4a) ||
	       reg == 0x50 || reg == 0x53 ||
	       m5pmic_in_range(reg, 0x60, 0xbf);
}

static bool m5pmic_writeable_reg(struct device *dev, unsigned int reg)
{
	return m5pmic_in_range(reg, 0x05, 0x0a) ||
	       m5pmic_in_range(reg, 0x10, 0x19) || reg == 0x2a ||
	       m5pmic_in_range(reg, 0x30, 0x35) ||
	       m5pmic_in_range(reg, 0x38, 0x3d) ||
	       m5pmic_in_range(reg, 0x40, 0x45) ||
	       m5pmic_in_range(reg, 0x49, 0x4a) || reg == 0x50 ||
	       reg == 0x53 || m5pmic_in_range(reg, 0x60, 0xbf);
}

static bool m5pmic_volatile_reg(struct device *dev, unsigned int reg)
{
	return reg == 0x05 || m5pmic_in_range(reg, 0x12, 0x19) ||
	       m5pmic_in_range(reg, 0x20, 0x2a) ||
	       m5pmic_in_range(reg, 0x40, 0x42);
}

/* One byte per transaction also works with the m5io-hub SMBus emulation. */
static int m5pmic_reg_read(void *context, unsigned int reg, unsigned int *val)
{
	struct m5pmic *pmic = context;
	int ret = -EIO;
	int try;

	if (reg > 0xff)
		return -EINVAL;
	for (try = 0; try < M5PMIC_I2C_RETRIES; try++) {
		ret = i2c_smbus_read_byte_data(pmic->client, reg);
		if (ret >= 0) {
			*val = ret;
			return 0;
		}
		if (ret != -EIO && ret != -ENXIO && ret != -EREMOTEIO &&
		    ret != -ETIMEDOUT)
			break;
		if (try + 1 < M5PMIC_I2C_RETRIES)
			msleep(M5PMIC_I2C_RETRY_DELAY_MS);
	}
	return ret;
}

static int m5pmic_reg_write(void *context, unsigned int reg, unsigned int val)
{
	struct m5pmic *pmic = context;
	int ret = -EIO;
	int try;

	if (reg > 0xff || val > 0xff)
		return -EINVAL;
	for (try = 0; try < M5PMIC_I2C_RETRIES; try++) {
		ret = i2c_smbus_write_byte_data(pmic->client, reg, val);
		if (!ret)
			return 0;
		if (ret != -EIO && ret != -ENXIO && ret != -EREMOTEIO &&
		    ret != -ETIMEDOUT)
			break;
		if (try + 1 < M5PMIC_I2C_RETRIES)
			msleep(M5PMIC_I2C_RETRY_DELAY_MS);
	}
	return ret;
}

static const struct regmap_bus m5pmic_regmap_bus = {
	.reg_read = m5pmic_reg_read,
	.reg_write = m5pmic_reg_write,
};

static const struct regmap_config m5pmic_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = 0xbf,
	.readable_reg = m5pmic_readable_reg,
	.writeable_reg = m5pmic_writeable_reg,
	.volatile_reg = m5pmic_volatile_reg,
	.cache_type = REGCACHE_NONE,
};

static unsigned int m5pmic_func_reg(unsigned int offset)
{
	return offset < 4 ? M5PMIC_REG_GPIO_FUNC0 : M5PMIC_REG_GPIO_FUNC1;
}

static unsigned int m5pmic_func_shift(unsigned int offset)
{
	return (offset & 3) * 2;
}

static unsigned int m5pmic_func_mask(unsigned int offset)
{
	unsigned int shift = m5pmic_func_shift(offset);

	return GENMASK(shift + 1, shift);
}

static unsigned int m5pmic_pull_reg(unsigned int offset)
{
	return offset < 4 ? M5PMIC_REG_GPIO_PUPD0 : M5PMIC_REG_GPIO_PUPD1;
}

static unsigned int m5pmic_pull_shift(unsigned int offset)
{
	return (offset & 3) * 2;
}

static int m5pmic_set_func(struct m5pmic *pmic, unsigned int offset,
			   unsigned int function)
{
	unsigned int shift = m5pmic_func_shift(offset);

	return regmap_update_bits(pmic->regmap, m5pmic_func_reg(offset),
				 m5pmic_func_mask(offset), function << shift);
}

static int m5pmic_release_gpio_irq(struct m5pmic *pmic, unsigned int offset)
{
	unsigned int status;
	int ret;

	/* IRQ mode clamps the pin low until its status bit is explicitly cleared. */
	ret = regmap_read(pmic->regmap, M5PMIC_REG_IRQ_STATUS1, &status);
	if (ret)
		return ret;
	if (!(status & BIT(offset)))
		return 0;
	return regmap_write(pmic->regmap, M5PMIC_REG_IRQ_STATUS1,
				(~BIT(offset)) & 0xff);
}

static int m5pmic_gpio_get(struct gpio_chip *gc, unsigned int offset)
{
	struct m5pmic *pmic = gpiochip_get_data(gc);
	unsigned int val;
	int ret;

	if (offset >= M5PMIC_NGPIO)
		return -EINVAL;
	mutex_lock(&pmic->lock);
	ret = regmap_read(pmic->regmap, M5PMIC_REG_GPIO_IN, &val);
	mutex_unlock(&pmic->lock);
	if (ret)
		return ret;
	return !!(val & BIT(offset));
}

static int m5pmic_gpio_set(struct gpio_chip *gc, unsigned int offset, int value)
{
	struct m5pmic *pmic = gpiochip_get_data(gc);
	int ret;

	if (offset >= M5PMIC_NGPIO)
		return -EINVAL;
	mutex_lock(&pmic->lock);
	ret = regmap_update_bits(pmic->regmap, M5PMIC_REG_GPIO_OUT, BIT(offset),
					 value ? BIT(offset) : 0);
	mutex_unlock(&pmic->lock);
	return ret;
}

static int m5pmic_gpio_get_direction(struct gpio_chip *gc,
				     unsigned int offset)
{
	struct m5pmic *pmic = gpiochip_get_data(gc);
	unsigned int val;
	int ret;

	if (offset >= M5PMIC_NGPIO)
		return -EINVAL;
	mutex_lock(&pmic->lock);
	ret = regmap_read(pmic->regmap, M5PMIC_REG_GPIO_MODE, &val);
	mutex_unlock(&pmic->lock);
	if (ret)
		return ret;
	return (val & BIT(offset)) ? GPIO_LINE_DIRECTION_OUT :
		GPIO_LINE_DIRECTION_IN;
}

static int m5pmic_gpio_direction_input(struct gpio_chip *gc,
				       unsigned int offset)
{
	struct m5pmic *pmic = gpiochip_get_data(gc);
	int ret;

	if (offset >= M5PMIC_NGPIO)
		return -EINVAL;
	mutex_lock(&pmic->lock);
	ret = m5pmic_release_gpio_irq(pmic, offset);
	if (!ret)
		ret = m5pmic_set_func(pmic, offset, 0);
	if (!ret)
		ret = regmap_update_bits(pmic->regmap, M5PMIC_REG_GPIO_MODE,
					 BIT(offset), 0);
	mutex_unlock(&pmic->lock);
	return ret;
}

static int m5pmic_gpio_direction_output(struct gpio_chip *gc,
					unsigned int offset, int value)
{
	struct m5pmic *pmic = gpiochip_get_data(gc);
	int ret;

	if (offset >= M5PMIC_NGPIO)
		return -EINVAL;
	mutex_lock(&pmic->lock);
	ret = m5pmic_release_gpio_irq(pmic, offset);
	if (!ret)
		ret = m5pmic_set_func(pmic, offset, 0);
	if (!ret)
		ret = regmap_update_bits(pmic->regmap, M5PMIC_REG_GPIO_OUT,
					 BIT(offset), value ? BIT(offset) : 0);
	if (!ret)
		ret = regmap_update_bits(pmic->regmap, M5PMIC_REG_GPIO_MODE,
					 BIT(offset), BIT(offset));
	mutex_unlock(&pmic->lock);
	return ret;
}

static int m5pmic_gpio_set_config(struct gpio_chip *gc, unsigned int offset,
					  unsigned long config)
{
	struct m5pmic *pmic = gpiochip_get_data(gc);
	unsigned long param = pinconf_to_config_param(config);
	unsigned int reg, shift;
	int ret;

	if (offset >= M5PMIC_NGPIO)
		return -EINVAL;
	mutex_lock(&pmic->lock);
	switch (param) {
	case PIN_CONFIG_BIAS_DISABLE:
		reg = m5pmic_pull_reg(offset);
		shift = m5pmic_pull_shift(offset);
		ret = regmap_update_bits(pmic->regmap, reg, 0x3 << shift, 0);
		break;
	case PIN_CONFIG_BIAS_PULL_UP:
		reg = m5pmic_pull_reg(offset);
		shift = m5pmic_pull_shift(offset);
		ret = regmap_update_bits(pmic->regmap, reg, 0x3 << shift,
					  0x1 << shift);
		break;
	case PIN_CONFIG_BIAS_PULL_DOWN:
		reg = m5pmic_pull_reg(offset);
		shift = m5pmic_pull_shift(offset);
		ret = regmap_update_bits(pmic->regmap, reg, 0x3 << shift,
					  0x2 << shift);
		break;
	case PIN_CONFIG_DRIVE_OPEN_DRAIN:
		ret = regmap_update_bits(pmic->regmap, M5PMIC_REG_GPIO_DRV,
					 BIT(offset), BIT(offset));
		break;
	case PIN_CONFIG_DRIVE_PUSH_PULL:
		ret = regmap_update_bits(pmic->regmap, M5PMIC_REG_GPIO_DRV,
					 BIT(offset), 0);
		break;
	default:
		ret = -ENOTSUPP;
		break;
	}
	mutex_unlock(&pmic->lock);
	return ret;
}

static struct m5pmic *m5pmic_from_irq_data(struct irq_data *d)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(d);

	return gpiochip_get_data(gc);
}

static void m5pmic_irq_mask(struct irq_data *d)
{
	struct m5pmic *pmic = m5pmic_from_irq_data(d);
	unsigned int hwirq = irqd_to_hwirq(d);

	pmic->irq_mask |= BIT(hwirq);
	gpiochip_disable_irq(&pmic->gpio, hwirq);
}

static void m5pmic_irq_unmask(struct irq_data *d)
{
	struct m5pmic *pmic = m5pmic_from_irq_data(d);
	unsigned int hwirq = irqd_to_hwirq(d);

	pmic->irq_mask &= ~BIT(hwirq);
	gpiochip_enable_irq(&pmic->gpio, hwirq);
}

static void m5pmic_irq_bus_lock(struct irq_data *d)
{
	struct m5pmic *pmic = m5pmic_from_irq_data(d);

	mutex_lock(&pmic->lock);
}

static void m5pmic_irq_bus_sync_unlock(struct irq_data *d)
{
	struct m5pmic *pmic = m5pmic_from_irq_data(d);
	unsigned int hwirq = irqd_to_hwirq(d);

	if (!(pmic->irq_mask & BIT(hwirq))) {
		if (!m5pmic_release_gpio_irq(pmic, hwirq) &&
		    !m5pmic_set_func(pmic, hwirq, 1))
			regmap_update_bits(pmic->regmap, M5PMIC_REG_GPIO_MODE,
					   BIT(hwirq), 0);
	}
	regmap_update_bits(pmic->regmap, M5PMIC_REG_IRQ_MASK1,
				  BIT(hwirq), pmic->irq_mask & BIT(hwirq));
	mutex_unlock(&pmic->lock);
}

static int m5pmic_irq_set_type(struct irq_data *d, unsigned int type)
{
	struct m5pmic *pmic = m5pmic_from_irq_data(d);
	unsigned int hwirq = irqd_to_hwirq(d);

	if (hwirq >= M5PMIC_NGPIO || !(type & IRQ_TYPE_EDGE_BOTH) ||
	    type & IRQ_TYPE_LEVEL_MASK)
		return -EINVAL;
	pmic->irq_type[hwirq] = type & IRQ_TYPE_EDGE_BOTH;
	irq_set_handler_locked(d, handle_edge_irq);
	return 0;
}

static int m5pmic_irq_set_wake(struct irq_data *d, unsigned int on)
{
	struct m5pmic *pmic = m5pmic_from_irq_data(d);

	if (pmic->parent_irq <= 0)
		return -EINVAL;
	return irq_set_irq_wake(pmic->parent_irq, on);
}

static const struct irq_chip m5pmic_irq_chip_template = {
	.name = M5PMIC_DRV_NAME,
	.irq_mask = m5pmic_irq_mask,
	.irq_unmask = m5pmic_irq_unmask,
	.irq_bus_lock = m5pmic_irq_bus_lock,
	.irq_bus_sync_unlock = m5pmic_irq_bus_sync_unlock,
	.irq_set_type = m5pmic_irq_set_type,
	.irq_set_wake = m5pmic_irq_set_wake,
};

static int m5pmic_clear_status(struct m5pmic *pmic, unsigned int reg,
				       unsigned int status)
{
	/*
	 * Status bits are write-zero-to-clear and write-one-to-keep.  Invert the
	 * snapshot so that only the bits observed by the caller are cleared.  A
	 * new event arriving between the read and this write remains pending.
	 */
	return regmap_write(pmic->regmap, reg, (~status) & 0xff);
}

static irqreturn_t m5pmic_irq_thread(int irq, void *data)
{
	struct m5pmic *pmic = data;
	unsigned int status1, status2, status3, input;
	unsigned long pending, bit;
	int ret;

	mutex_lock(&pmic->lock);
	ret = regmap_read(pmic->regmap, M5PMIC_REG_IRQ_STATUS1, &status1);
	if (ret)
		goto out_unlock;
	ret = regmap_read(pmic->regmap, M5PMIC_REG_IRQ_STATUS2, &status2);
	if (ret)
		goto out_unlock;
	ret = regmap_read(pmic->regmap, M5PMIC_REG_IRQ_STATUS3, &status3);
	if (ret)
		goto out_unlock;
	ret = regmap_read(pmic->regmap, M5PMIC_REG_GPIO_IN, &input);
	if (ret)
		goto out_unlock;

	/* Consume unexposed status2/status3 sources so a level parent can clear. */
	if (status1 && m5pmic_clear_status(pmic, M5PMIC_REG_IRQ_STATUS1,
						status1))
		dev_warn_ratelimited(pmic->dev, "failed to clear GPIO IRQ status\n");
	if (status2 && m5pmic_clear_status(pmic, M5PMIC_REG_IRQ_STATUS2,
						status2))
		dev_warn_ratelimited(pmic->dev, "failed to clear system IRQ status\n");
	if (status3 && m5pmic_clear_status(pmic, M5PMIC_REG_IRQ_STATUS3,
						status3))
		dev_warn_ratelimited(pmic->dev,
				     "failed to clear button/wakeup IRQ status\n");
	pending = status1 & M5PMIC_GPIO_MASK & ~pmic->irq_mask;
	mutex_unlock(&pmic->lock);

	for_each_set_bit(bit, &pending, M5PMIC_NGPIO) {
		unsigned int child_irq;
		bool high = !!(input & BIT(bit));
		unsigned int type = pmic->irq_type[bit];

		if ((type == IRQ_TYPE_EDGE_RISING && !high) ||
		    (type == IRQ_TYPE_EDGE_FALLING && high))
			continue;
		child_irq = irq_find_mapping(pmic->gpio.irq.domain, bit);
		if (child_irq)
			handle_nested_irq(child_irq);
	}
	return (status1 || status2 || status3) ? IRQ_HANDLED : IRQ_NONE;

out_unlock:
	mutex_unlock(&pmic->lock);
	dev_warn_ratelimited(pmic->dev, "failed to read IRQ status: %d\n", ret);
	return IRQ_NONE;
}

static int m5pmic_read_adc_temp(struct m5pmic *pmic, long *value)
{
	unsigned int ctrl, raw;
	unsigned long deadline;
	int ret;

	ret = regmap_write(pmic->regmap, M5PMIC_REG_ADC_CTRL,
				(M5PMIC_ADC_TEMP_CHANNEL << 1) | M5PMIC_ADC_START);
	if (ret)
		return ret;
	deadline = jiffies + msecs_to_jiffies(M5PMIC_ADC_TIMEOUT_MS);
	do {
		msleep(M5PMIC_ADC_POLL_MS);
		ret = regmap_read(pmic->regmap, M5PMIC_REG_ADC_CTRL, &ctrl);
		if (ret)
			return ret;
		if (!(ctrl & M5PMIC_ADC_START))
			break;
	} while (time_before(jiffies, deadline));
	if (ctrl & M5PMIC_ADC_START)
		return -ETIMEDOUT;
	ret = regmap_read(pmic->regmap, M5PMIC_REG_ADC_RES_L, &raw);
	if (ret)
		return ret;
	*value = raw;
	ret = regmap_read(pmic->regmap, M5PMIC_REG_ADC_RES_L + 1, &raw);
	if (ret)
		return ret;
	*value |= (raw & 0x0f) << 8;
	*value *= 1000;
	return 0;
}

static int m5pmic_hwmon_read(struct device *dev, enum hwmon_sensor_types type,
				     u32 attr, int channel, long *val)
{
	struct m5pmic *pmic = dev_get_drvdata(dev);
	unsigned int lo, hi;
	unsigned int reg;
	int ret;

	if (attr != hwmon_in_input && attr != hwmon_temp_input)
		return -EOPNOTSUPP;
	mutex_lock(&pmic->lock);
	if (type == hwmon_temp) {
		ret = m5pmic_read_adc_temp(pmic, val);
		goto out_unlock;
	}
	if (type != hwmon_in || channel < 0 || channel > 2) {
		ret = -EINVAL;
		goto out_unlock;
	}
	reg = M5PMIC_REG_VBAT_L + channel * 2;
	ret = regmap_read(pmic->regmap, reg, &lo);
	if (ret)
		goto out_unlock;
	ret = regmap_read(pmic->regmap, reg + 1, &hi);
	if (ret)
		goto out_unlock;
	/* M5PM1 voltage registers are already expressed in millivolts. */
	*val = (hi << 8) | lo;

out_unlock:
	mutex_unlock(&pmic->lock);
	return ret;
}

static umode_t m5pmic_hwmon_is_visible(const void *data,
					       enum hwmon_sensor_types type,
					       u32 attr, int channel)
{
	if (type == hwmon_in && channel >= 0 && channel < 3 &&
	    (attr == hwmon_in_input || attr == hwmon_in_label))
		return 0444;
	if (type == hwmon_temp && channel == 0 &&
	    (attr == hwmon_temp_input || attr == hwmon_temp_label))
		return 0444;
	return 0;
}

static int m5pmic_hwmon_read_string(struct device *dev,
					    enum hwmon_sensor_types type,
					    u32 attr, int channel,
					    const char **str)
{
	static const char * const voltage_labels[] = {
		"VBAT", "VIN", "5VOUT",
	};

	if (type == hwmon_in && attr == hwmon_in_label &&
	    channel >= 0 && channel < ARRAY_SIZE(voltage_labels)) {
		*str = voltage_labels[channel];
		return 0;
	}
	if (type == hwmon_temp && attr == hwmon_temp_label && channel == 0) {
		*str = "Die";
		return 0;
	}
	return -EOPNOTSUPP;
}

static const struct hwmon_ops m5pmic_hwmon_ops = {
	.is_visible = m5pmic_hwmon_is_visible,
	.read = m5pmic_hwmon_read,
	.read_string = m5pmic_hwmon_read_string,
};

static const struct hwmon_channel_info m5pmic_hwmon_in = {
	.type = hwmon_in,
	.config = (const u32[]){
		HWMON_I_INPUT | HWMON_I_LABEL,
		HWMON_I_INPUT | HWMON_I_LABEL,
		HWMON_I_INPUT | HWMON_I_LABEL,
		0,
	},
};

static const struct hwmon_channel_info m5pmic_hwmon_temp = {
	.type = hwmon_temp,
	.config = (const u32[]){ HWMON_T_INPUT | HWMON_T_LABEL, 0 },
};

static const struct hwmon_channel_info *m5pmic_hwmon_info[] = {
	&m5pmic_hwmon_in,
	&m5pmic_hwmon_temp,
	NULL,
};

static const struct hwmon_chip_info m5pmic_hwmon_chip_info = {
	.ops = &m5pmic_hwmon_ops,
	.info = m5pmic_hwmon_info,
};

static int m5pmic_read_id(struct m5pmic *pmic, unsigned int reg,
			   unsigned int expected, const char *name)
{
	unsigned int value;
	int ret;

	ret = regmap_read(pmic->regmap, reg, &value);
	if (ret)
		return ret;
	if (value != expected) {
		dev_err(pmic->dev, "unexpected %s 0x%02x (expected 0x%02x)\n",
			name, value, expected);
		return -ENODEV;
	}
	return 0;
}

static int m5pmic_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct m5pmic *pmic;
	unsigned int i;
	int ret;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_SMBUS_BYTE_DATA))
		return dev_err_probe(dev, -EOPNOTSUPP,
				     "SMBus byte-data transfers are required\n");
	pmic = devm_kzalloc(dev, sizeof(*pmic), GFP_KERNEL);
	if (!pmic)
		return -ENOMEM;
	pmic->dev = dev;
	pmic->client = client;
	pmic->parent_irq = client->irq;
	mutex_init(&pmic->lock);
	for (i = 0; i < M5PMIC_NGPIO; i++)
		pmic->irq_type[i] = IRQ_TYPE_EDGE_BOTH;
	pmic->irq_mask = M5PMIC_GPIO_MASK;

	pmic->regmap = devm_regmap_init(dev, &m5pmic_regmap_bus, pmic,
				       &m5pmic_regmap_config);
	if (IS_ERR(pmic->regmap))
		return dev_err_probe(dev, PTR_ERR(pmic->regmap),
				     "failed to create regmap\n");
	ret = m5pmic_read_id(pmic, M5PMIC_REG_DEVICE_ID, M5PMIC_DEVICE_ID,
				     "device ID");
	if (ret)
		return ret;
	ret = m5pmic_read_id(pmic, M5PMIC_REG_DEVICE_MODEL,
				     M5PMIC_DEVICE_MODEL, "device model");
	if (ret)
		return ret;

	pmic->gpio.label = M5PMIC_DRV_NAME;
	pmic->gpio.parent = dev;
	pmic->gpio.owner = THIS_MODULE;
	pmic->gpio.ngpio = M5PMIC_NGPIO;
	pmic->gpio.can_sleep = true;
	pmic->gpio.get = m5pmic_gpio_get;
	pmic->gpio.set = m5pmic_gpio_set;
	pmic->gpio.get_direction = m5pmic_gpio_get_direction;
	pmic->gpio.direction_input = m5pmic_gpio_direction_input;
	pmic->gpio.direction_output = m5pmic_gpio_direction_output;
	pmic->gpio.set_config = m5pmic_gpio_set_config;
	pmic->gpio.base = -1;

	if (client->irq > 0) {
		pmic->irq_chip = m5pmic_irq_chip_template;
		gpio_irq_chip_set_chip(&pmic->gpio.irq, &pmic->irq_chip);
		pmic->gpio.irq.handler = handle_edge_irq;
		pmic->gpio.irq.default_type = IRQ_TYPE_NONE;
		pmic->gpio.irq.threaded = true;
	}
	ret = devm_gpiochip_add_data(dev, &pmic->gpio, pmic);
	if (ret)
		return dev_err_probe(dev, ret, "failed to register GPIO chip\n");

	if (client->irq > 0) {
		ret = devm_request_threaded_irq(dev, client->irq, NULL,
						m5pmic_irq_thread, IRQF_ONESHOT,
						M5PMIC_DRV_NAME, pmic);
		if (ret)
			return dev_err_probe(dev, ret,
					     "failed to request parent IRQ %d\n",
					     client->irq);
		ret = regmap_write(pmic->regmap, M5PMIC_REG_IRQ_MASK1,
					   M5PMIC_GPIO_MASK);
		if (ret)
			return dev_err_probe(dev, ret, "failed to mask GPIO IRQs\n");
	}

	ret = PTR_ERR_OR_ZERO(devm_hwmon_device_register_with_info(dev, "m5pmic",
							 pmic,
							 &m5pmic_hwmon_chip_info,
							 NULL));
	if (ret)
		return dev_err_probe(dev, ret, "failed to register hwmon\n");
	i2c_set_clientdata(client, pmic);
	dev_info(dev, "M5PM1 detected at 0x%02x%s\n", client->addr,
		 client->irq > 0 ? " with GPIO IRQ parent" : "");
	return 0;
}

static const struct of_device_id m5pmic_of_match[] = {
	{ .compatible = "m5stack,m5pm1" },
	{ }
};
MODULE_DEVICE_TABLE(of, m5pmic_of_match);

static const struct i2c_device_id m5pmic_i2c_ids[] = {
	{ "m5pm1", 0 },
	{ "m5pmic", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, m5pmic_i2c_ids);

static struct i2c_driver m5pmic_driver = {
	.driver = {
		.name = M5PMIC_DRV_NAME,
		.of_match_table = m5pmic_of_match,
	},
	.probe = m5pmic_probe,
	.id_table = m5pmic_i2c_ids,
};
module_i2c_driver(m5pmic_driver);

MODULE_AUTHOR("M5Stack / CardputerZeroQ integration");
MODULE_DESCRIPTION("M5Stack M5PM1 GPIO, IRQ and hwmon driver");
MODULE_LICENSE("GPL");
