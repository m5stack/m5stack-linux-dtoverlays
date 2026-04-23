/*
 * Driver for TCA8418 I2C keyboard
 *
 * Copyright (C) 2011 Fuel7, Inc.  All rights reserved.
 *
 * Author: Kyle Manna <kyle.manna@fuel7.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License v2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 021110-1307, USA.
 *
 * If you can't comply with GPLv2, alternative licensing terms may be
 * arranged. Please contact Fuel7, Inc. (http://fuel7.com/) for proprietary
 * alternative licensing inquiries.
 */

#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/input.h>
#include <linux/input/matrix_keypad.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/property.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/gpio/consumer.h>

/* TCA8418 hardware limits */
#define TCA8418_MAX_ROWS	8
#define TCA8418_MAX_COLS	10

/* TCA8418 register offsets */
#define REG_CFG			0x01
#define REG_INT_STAT		0x02
#define REG_KEY_LCK_EC		0x03
#define REG_KEY_EVENT_A		0x04
#define REG_KEY_EVENT_B		0x05
#define REG_KEY_EVENT_C		0x06
#define REG_KEY_EVENT_D		0x07
#define REG_KEY_EVENT_E		0x08
#define REG_KEY_EVENT_F		0x09
#define REG_KEY_EVENT_G		0x0A
#define REG_KEY_EVENT_H		0x0B
#define REG_KEY_EVENT_I		0x0C
#define REG_KEY_EVENT_J		0x0D
#define REG_KP_LCK_TIMER	0x0E
#define REG_UNLOCK1		0x0F
#define REG_UNLOCK2		0x10
#define REG_GPIO_INT_STAT1	0x11
#define REG_GPIO_INT_STAT2	0x12
#define REG_GPIO_INT_STAT3	0x13
#define REG_GPIO_DAT_STAT1	0x14
#define REG_GPIO_DAT_STAT2	0x15
#define REG_GPIO_DAT_STAT3	0x16
#define REG_GPIO_DAT_OUT1	0x17
#define REG_GPIO_DAT_OUT2	0x18
#define REG_GPIO_DAT_OUT3	0x19
#define REG_GPIO_INT_EN1	0x1A
#define REG_GPIO_INT_EN2	0x1B
#define REG_GPIO_INT_EN3	0x1C
#define REG_KP_GPIO1		0x1D
#define REG_KP_GPIO2		0x1E
#define REG_KP_GPIO3		0x1F
#define REG_GPI_EM1		0x20
#define REG_GPI_EM2		0x21
#define REG_GPI_EM3		0x22
#define REG_GPIO_DIR1		0x23
#define REG_GPIO_DIR2		0x24
#define REG_GPIO_DIR3		0x25
#define REG_GPIO_INT_LVL1	0x26
#define REG_GPIO_INT_LVL2	0x27
#define REG_GPIO_INT_LVL3	0x28
#define REG_DEBOUNCE_DIS1	0x29
#define REG_DEBOUNCE_DIS2	0x2A
#define REG_DEBOUNCE_DIS3	0x2B
#define REG_GPIO_PULL1		0x2C
#define REG_GPIO_PULL2		0x2D
#define REG_GPIO_PULL3		0x2E

/* TCA8418 bit definitions */
#define CFG_AI			BIT(7)
#define CFG_GPI_E_CFG		BIT(6)
#define CFG_OVR_FLOW_M		BIT(5)
#define CFG_INT_CFG		BIT(4)
#define CFG_OVR_FLOW_IEN	BIT(3)
#define CFG_K_LCK_IEN		BIT(2)
#define CFG_GPI_IEN		BIT(1)
#define CFG_KE_IEN		BIT(0)

#define INT_STAT_CAD_INT	BIT(4)
#define INT_STAT_OVR_FLOW_INT	BIT(3)
#define INT_STAT_K_LCK_INT	BIT(2)
#define INT_STAT_GPI_INT	BIT(1)
#define INT_STAT_K_INT		BIT(0)

/* TCA8418 register masks */
#define KEY_LCK_EC_KEC		0x7
#define KEY_EVENT_CODE		0x7f
#define KEY_EVENT_VALUE		0x80

struct tca8418_keypad {
	struct i2c_client *client;
	struct input_dev *input;
	struct gpio_desc *tables_sel_gpio;
	struct gpio_desc *capslock_gpio;
	int map_base_index;
	
	int switch_button_code;
	int sym_button_code;
	int fn_button_code;
	int asmux_button_code;


	int sym_button_flage;
	int fn_button_flage;
	int asmux_button_flage;

	ktime_t asmux_timer;

	int capslock_state;
	unsigned int row_shift;
	unsigned short *keycode1;
	unsigned short *keycode2;
};

/*
 * Write a byte to the TCA8418
 */
static int tca8418_write_byte(struct tca8418_keypad *keypad_data,
			      int reg, u8 val)
{
	int error;

	error = i2c_smbus_write_byte_data(keypad_data->client, reg, val);
	if (error < 0) {
		dev_err(&keypad_data->client->dev,
			"%s failed, reg: %d, val: %d, error: %d\n",
			__func__, reg, val, error);
		return error;
	}

	return 0;
}

/*
 * Read a byte from the TCA8418
 */
static int tca8418_read_byte(struct tca8418_keypad *keypad_data,
			     int reg, u8 *val)
{
	int error;

	error = i2c_smbus_read_byte_data(keypad_data->client, reg);
	if (error < 0) {
		dev_err(&keypad_data->client->dev,
				"%s failed, reg: %d, error: %d\n",
				__func__, reg, error);
		return error;
	}

	*val = (u8)error;

	return 0;
}

static void tca8418_read_keypad(struct tca8418_keypad *keypad_data)
{
	struct input_dev *input = keypad_data->input;
	unsigned short *keymap = input->keycode;
	int error, col, row;
	u8 reg, state, code;

	do {
		error = tca8418_read_byte(keypad_data, REG_KEY_EVENT_A, &reg);
		if (error < 0) {
			dev_err(&keypad_data->client->dev,
				"unable to read REG_KEY_EVENT_A\n");
			break;
		}

		/* Assume that key code 0 signifies empty FIFO */
		if (reg <= 0)
			break;

		state = reg & KEY_EVENT_VALUE;
		code  = reg & KEY_EVENT_CODE;

		row = code / TCA8418_MAX_COLS;
		col = code % TCA8418_MAX_COLS;

		row = (col) ? row : row - 1;
		col = (col) ? col - 1 : TCA8418_MAX_COLS - 1;
		
		code = MATRIX_SCAN_CODE(row, col, keypad_data->row_shift);


		unsigned int report_code = keymap[code];
		{
			if(state==0)
			{//按键松开
				if(code == keypad_data->sym_button_code)
				{
					//上锁
					keypad_data->sym_button_flage = !keypad_data->sym_button_flage;
					gpiod_set_value_cansleep(keypad_data->tables_sel_gpio, keypad_data->sym_button_flage);
				}
				if(code == keypad_data->fn_button_code)
				{
					keypad_data->fn_button_flage = 0;
				}
			}
			else
			{//按键按下
				// if(code == keypad_data->sym_button_code)
				// {
				// 	keypad_data->sym_button_flage = 1;
				// }
				if(code == keypad_data->fn_button_code)
				{
					keypad_data->fn_button_flage = 1;
				}
			}
		
			if(keypad_data->sym_button_flage)
			{
				report_code = keypad_data->keycode1[code];
			}
			if(keypad_data->fn_button_flage)
			{
				report_code = keypad_data->keycode2[code];
			}

			if(code == keypad_data->asmux_button_code)
			{
				printk("asmux_button_code  0\n");
				report_code = KEY_LEFTSHIFT;
				if(keypad_data->asmux_button_flage)
				{
					printk("asmux_button_code  1\n");
					report_code = KEY_CAPSLOCK;
					keypad_data->asmux_button_flage = 0;
				}
				if(state==0)
				{//按键松开
					printk("asmux_button_code  2\n");
					keypad_data->asmux_timer = ktime_get();
				}
				else
				{//按键按下
					printk("asmux_button_code  3\n");
					s64 elapsed_us = ktime_to_us(ktime_sub(ktime_get(), keypad_data->asmux_timer));
					if(elapsed_us < 300000)
					{
						printk("asmux_button_code  4\n");
						report_code = KEY_CAPSLOCK;
						keypad_data->asmux_button_flage = 1;
					}
				}
			}
			if(report_code == KEY_CAPSLOCK)
			{
				if(state==0)
				{
					keypad_data->capslock_state = keypad_data->capslock_state ? 0 : 1;
					gpiod_set_value_cansleep(keypad_data->capslock_gpio, keypad_data->capslock_state);
					printk("report_code KEY_CAPSLOCK 0\n");
				}
				else
				{
					// gpiod_set_value_cansleep(keypad_data->capslock_gpio, keypad_data->capslock_state);
					printk("report_code KEY_CAPSLOCK 1\n");
				}
			}
			if(report_code == KEY_LEFTSHIFT)
			{
				if(state==0)
				{
					printk("report_code KEY_LEFTSHIFT 0\n");
				}
				else
				{
					printk("report_code KEY_LEFTSHIFT 1\n");
				}
			}
		}

		printk("code:%d\n", code);
		input_event(input, EV_MSC, MSC_SCAN, code);
		input_report_key(input, report_code, state);

	} while (1);

	input_sync(input);
}

/*
 * Threaded IRQ handler and this can (and will) sleep.
 */
static irqreturn_t tca8418_irq_handler(int irq, void *dev_id)
{
	struct tca8418_keypad *keypad_data = dev_id;
	u8 reg;
	int error;

	error = tca8418_read_byte(keypad_data, REG_INT_STAT, &reg);
	if (error) {
		dev_err(&keypad_data->client->dev,
			"unable to read REG_INT_STAT\n");
		return IRQ_NONE;
	}

	if (!reg)
		return IRQ_NONE;

	if (reg & INT_STAT_OVR_FLOW_INT)
		dev_warn(&keypad_data->client->dev, "overflow occurred\n");

	if (reg & INT_STAT_K_INT)
		tca8418_read_keypad(keypad_data);

	/* Clear all interrupts, even IRQs we didn't check (GPI, CAD, LCK) */
	reg = 0xff;
	error = tca8418_write_byte(keypad_data, REG_INT_STAT, reg);
	if (error)
		dev_err(&keypad_data->client->dev,
			"unable to clear REG_INT_STAT\n");

	return IRQ_HANDLED;
}

/*
 * Configure the TCA8418 for keypad operation
 */
static int tca8418_configure(struct tca8418_keypad *keypad_data,
			     u32 rows, u32 cols)
{
	int reg, error = 0;

	/* Assemble a mask for row and column registers */
	reg  =  ~(~0 << rows);
	reg += (~(~0 << cols)) << 8;

	/* Set registers to keypad mode */
	error |= tca8418_write_byte(keypad_data, REG_KP_GPIO1, reg);
	error |= tca8418_write_byte(keypad_data, REG_KP_GPIO2, reg >> 8);
	error |= tca8418_write_byte(keypad_data, REG_KP_GPIO3, reg >> 16);

	/* Enable column debouncing */
	error |= tca8418_write_byte(keypad_data, REG_DEBOUNCE_DIS1, reg);
	error |= tca8418_write_byte(keypad_data, REG_DEBOUNCE_DIS2, reg >> 8);
	error |= tca8418_write_byte(keypad_data, REG_DEBOUNCE_DIS3, reg >> 16);

	if (error)
		return error;

	error = tca8418_write_byte(keypad_data, REG_CFG,
				CFG_INT_CFG | CFG_OVR_FLOW_IEN | CFG_KE_IEN);

	return error;
}

static int tca8418_keypad_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct tca8418_keypad *keypad_data;
	struct input_dev *input;
	u32 rows = 0, cols = 0;
	int error, row_shift;
	u8 reg;

	/* Check i2c driver capabilities */
	if (!i2c_check_functionality(client->adapter, I2C_FUNC_SMBUS_BYTE)) {
		dev_err(dev, "%s adapter not supported\n",
			dev_driver_string(&client->adapter->dev));
		return -ENODEV;
	}

	error = matrix_keypad_parse_properties(dev, &rows, &cols);
	if (error)
		return error;

	if (!rows || rows > TCA8418_MAX_ROWS) {
		dev_err(dev, "invalid rows\n");
		return -EINVAL;
	}

	if (!cols || cols > TCA8418_MAX_COLS) {
		dev_err(dev, "invalid columns\n");
		return -EINVAL;
	}

	row_shift = get_count_order(cols);

	/* Allocate memory for keypad_data and input device */
	keypad_data = devm_kzalloc(dev, sizeof(*keypad_data), GFP_KERNEL);
	if (!keypad_data)
		return -ENOMEM;

	keypad_data->client = client;
	keypad_data->row_shift = row_shift;

	{
		keypad_data->capslock_state = 0;
		keypad_data->map_base_index = 0;
		// keypad_data->switch_button_code = -1;
		keypad_data->sym_button_code = -1;
		keypad_data->fn_button_code = -1;
		keypad_data->asmux_button_code = -1;

		device_property_read_u32(dev, "sym-button-code", &keypad_data->sym_button_code);
		device_property_read_u32(dev, "fn-button-code", &keypad_data->fn_button_code);
		device_property_read_u32(dev, "asmux-button-code", &keypad_data->asmux_button_code);

		keypad_data->capslock_gpio = devm_gpiod_get_optional(dev, "capslock", GPIOD_OUT_HIGH);
		keypad_data->tables_sel_gpio = devm_gpiod_get_optional(dev, "tables-sel", GPIOD_OUT_HIGH);

	}

	/* Read key lock register, if this fails assume device not present */
	error = tca8418_read_byte(keypad_data, REG_KEY_LCK_EC, &reg);
	if (error)
		return -ENODEV;

	/* Configure input device */
	input = devm_input_allocate_device(dev);
	if (!input)
		return -ENOMEM;

	keypad_data->input = input;

	input->name = client->name;
	input->id.bustype = BUS_I2C;
	input->id.vendor  = 0x0001;
	input->id.product = 0x001;
	input->id.version = 0x0001;

	error = matrix_keypad_build_keymap(NULL, NULL, rows, cols, NULL, input);
	if (error) {
		dev_err(dev, "Failed to build keymap\n");
		return error;
	}

	keypad_data->keycode1 = input->keycode;
	error = matrix_keypad_build_keymap(NULL, "linux,keymap1", rows, cols, NULL, input);
	unsigned short *tmp_keycode1 = input->keycode;
	input->keycode = keypad_data->keycode1;
	keypad_data->keycode1 = tmp_keycode1;
	if (error) {
		dev_err(dev, "Failed to build keymap1\n");
		return error;
	}
	keypad_data->keycode2 = input->keycode;
	error = matrix_keypad_build_keymap(NULL, "linux,keymap2", rows, cols, NULL, input);
	unsigned short *tmp_keycode2 = input->keycode;
	input->keycode = keypad_data->keycode2;
	keypad_data->keycode2 = tmp_keycode2;
	if (error) {
		dev_err(dev, "Failed to build keymap2\n");
		return error;
	}

	if (device_property_read_bool(dev, "keypad,autorepeat"))
		__set_bit(EV_REP, input->evbit);

	input_set_capability(input, EV_MSC, MSC_SCAN);

	error = devm_request_threaded_irq(dev, client->irq,
					  NULL, tca8418_irq_handler,
					  IRQF_SHARED | IRQF_ONESHOT,
					  client->name, keypad_data);
	if (error) {
		dev_err(dev, "Unable to claim irq %d; error %d\n",
			client->irq, error);
		return error;
	}

	/* Initialize the chip */
	error = tca8418_configure(keypad_data, rows, cols);
	if (error < 0)
		return error;

	error = input_register_device(input);
	if (error) {
		dev_err(dev, "Unable to register input device, error: %d\n",
			error);
		return error;
	}

	return 0;
}

static const struct i2c_device_id tca8418_id[] = {
	{ "tca8418", 8418, },
	{ }
};
MODULE_DEVICE_TABLE(i2c, tca8418_id);

static const struct of_device_id tca8418_dt_ids[] = {
	{ .compatible = "m5stack,tca8418", },
	{ .compatible = "m5stack,tca8418c", },
	{ }
};
MODULE_DEVICE_TABLE(of, tca8418_dt_ids);

static struct i2c_driver tca8418_keypad_driver = {
	.driver = {
		.name	= "tca8418_keypad",
		.of_match_table = tca8418_dt_ids,
	},
	.probe		= tca8418_keypad_probe,
	.id_table	= tca8418_id,
};
module_i2c_driver(tca8418_keypad_driver);

MODULE_AUTHOR("Kyle Manna <kyle.manna@fuel7.com>");
MODULE_DESCRIPTION("Keypad driver for TCA8418");
MODULE_LICENSE("GPL");
