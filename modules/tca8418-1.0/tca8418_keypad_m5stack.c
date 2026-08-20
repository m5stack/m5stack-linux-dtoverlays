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
#include <linux/regmap.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include "tca8418_fsm.c"

#define TCA8418_LOG_ENABLE	0

#if TCA8418_LOG_ENABLE
#define TCA8418_LOG(fmt, ...) \
	printk(KERN_INFO "tca8418_keypad: " fmt, ##__VA_ARGS__)
#else
#define TCA8418_LOG(fmt, ...) do { } while (0)
#endif

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

#define TCA8418_LED_OFF		0
#define TCA8418_LED_BLINK	1
#define TCA8418_LED_FAST	2
#define TCA8418_LED_ON		3

struct tca8418_keypad;

struct tca8418_mode_led {
	struct regmap *mode_regmap;
	u8 mode_register;
	u8 mode;
};

struct tca8418_key_record {
	bool pressed;
	unsigned short keycode;
};


struct tca8418_function_key
{
	struct tca8418_mode_led led;
	int scan_code;
	unsigned short *keymap;
	tca8418_t fsm;
	struct delayed_work key_timeout_work;
};



struct tca8418_keypad {
	struct i2c_client *client;
	struct input_dev *input;
	struct workqueue_struct *wq;
	struct work_struct irq_work;

	struct tca8418_function_key sym_key;
	struct tca8418_function_key shift_key;
	struct tca8418_function_key fn_key;
	struct tca8418_function_key *_key[3];

	unsigned int row_shift;
	unsigned short *keymap;
	struct tca8418_key_record *key_records;
	unsigned int key_count;
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

#if TCA8418_LOG_ENABLE
static const char *tca8418_led_mode_name(u8 mode)
{
	switch (mode) {
	case TCA8418_LED_OFF:
		return "off";
	case TCA8418_LED_BLINK:
		return "blink";
	case TCA8418_LED_FAST:
		return "fast";
	case TCA8418_LED_ON:
		return "on";
	default:
		return "unknown";
	}
}
#endif

static void tca8418_led_set(struct tca8418_mode_led *led, u8 mode)
{
	int error;
#if TCA8418_LOG_ENABLE
	u8 old_mode = led->mode;
#endif

	if (led->mode == mode) {
		TCA8418_LOG("led mode unchanged: %u(%s)\n", mode,
			    tca8418_led_mode_name(mode));
		return;
	}

	if (!led->mode_regmap)
		return;

	led->mode = mode;
	TCA8418_LOG("led mode %u(%s) -> %u(%s), write reg=0x%02x\n",
		    old_mode, tca8418_led_mode_name(old_mode), mode,
			    tca8418_led_mode_name(mode), led->mode_register);

	error = regmap_write(led->mode_regmap, led->mode_register, mode);
	TCA8418_LOG("led mode write reg=0x%02x value=%u(%s) ret=%d\n",
		    led->mode_register, mode, tca8418_led_mode_name(mode), error);
	if (error < 0)
		dev_warn(regmap_get_device(led->mode_regmap),
			 "failed to set led mode reg 0x%02x: %d\n",
			 led->mode_register, error);
}

static void tca8418_report_key(struct tca8418_keypad *keypad_data,
			       unsigned int scan_code, unsigned int keycode,
			       bool pressed)
{
	input_event(keypad_data->input, EV_MSC, MSC_SCAN, scan_code);
	input_report_key(keypad_data->input, keycode, pressed);
}

static struct tca8418_keypad *tca8418_fsm_keypad(tca8418_t *device)
{
	return device->ctx;
}

static struct tca8418_mode_led *tca8418_fsm_led(tca8418_t *device)
{
	struct tca8418_keypad *keypad_data = tca8418_fsm_keypad(device);
	unsigned int index = clamp_t(unsigned int, device->id, 0,
				     ARRAY_SIZE(keypad_data->_key) - 1);

	return &keypad_data->_key[index]->led;
}

void emit_key_pres_status(tca8418_t *device)
{
	struct tca8418_keypad *keypad_data = tca8418_fsm_keypad(device);
	unsigned int scan_code = keypad_data->_key[device->id]->scan_code;
	unsigned int keycode;

	if (scan_code >= keypad_data->key_count)
		return;
	keycode = keypad_data->keymap[scan_code];

	tca8418_report_key(keypad_data, scan_code, keycode, true);
}

void emit_key_release_status(tca8418_t *device)
{
	struct tca8418_keypad *keypad_data = tca8418_fsm_keypad(device);
	unsigned int scan_code = keypad_data->_key[device->id]->scan_code;
	unsigned int keycode;

	if (scan_code >= keypad_data->key_count)
		return;
	keycode = keypad_data->keymap[scan_code];

	tca8418_report_key(keypad_data, scan_code, keycode, false);
}

void led_ON(tca8418_t *device)
{
	tca8418_led_set(tca8418_fsm_led(device), TCA8418_LED_ON);
}

void led_OFF(tca8418_t *device)
{
	struct tca8418_mode_led *led = tca8418_fsm_led(device);

	tca8418_led_set(led, TCA8418_LED_OFF);
}

void led_BLINK(tca8418_t *device)
{
	tca8418_led_set(tca8418_fsm_led(device), TCA8418_LED_BLINK);
}

void led_FAST(tca8418_t *device)
{
	tca8418_led_set(tca8418_fsm_led(device), TCA8418_LED_FAST);
}

static unsigned int tca8418_fsm_timer_index(tca8418_t *device)
{
	return clamp_t(unsigned int, device->id, 0, 2);
}

static void tca8418_fsm_cancel_timer(struct tca8418_keypad *keypad_data,
					     tca8418_t *device)
{
	struct tca8418_function_key *key_obj =
		keypad_data->_key[tca8418_fsm_timer_index(device)];

	cancel_delayed_work_sync(&key_obj->key_timeout_work);
}

static void tca8418_fsm_schedule_timer(struct tca8418_keypad *keypad_data,
				       tca8418_t *device)
{
	struct tca8418_function_key *key_obj =
		keypad_data->_key[tca8418_fsm_timer_index(device)];

	mod_delayed_work(keypad_data->wq, &key_obj->key_timeout_work,
			 msecs_to_jiffies(300));
}

static void tca8418_fsm_timer_work(struct work_struct *work)
{
	struct tca8418_function_key *key_obj =
		container_of(to_delayed_work(work),
			     struct tca8418_function_key, key_timeout_work);
	tca8418_t *device = &key_obj->fsm;
	struct tca8418_keypad *keypad_data = tca8418_fsm_keypad(device);

	if (device->state == ST_ACTIVE ||
	    device->state == ST_WAIT_ACTIVE_LEAVE)
		tca8418_dispatch(device, EV_TIMEOUT_300);
	input_sync(keypad_data->input);
}

static void tca8418_fsm_update_timer(struct tca8418_keypad *keypad_data,
				      tca8418_t *device)
{
	if (device->state == ST_ACTIVE ||
	    device->state == ST_WAIT_ACTIVE_LEAVE)
		tca8418_fsm_schedule_timer(keypad_data, device);
	else
		tca8418_fsm_cancel_timer(keypad_data, device);
}

static bool tca8418_fsm_can_activate(struct tca8418_keypad *keypad_data,
					     tca8418_t *device)
{
	if (device->layer_active)
		return true;

	if (device->id == 0)
		return !keypad_data->shift_key.fsm.layer_active &&
			!keypad_data->fn_key.fsm.layer_active;

	return !keypad_data->sym_key.fsm.layer_active;
}

static void tca8418_fsm_process_function(struct tca8418_keypad *keypad_data,
					 tca8418_t *device, bool pressed)
{
	Event event;

	if (!pressed && device->state == ST_ONESHOT) {
		tca8418_dispatch(device, EV_OTHER_KEY_RELE);
		tca8418_fsm_update_timer(keypad_data, device);
		return;
	}

	if (pressed && !tca8418_fsm_can_activate(keypad_data, device))
		return;

	event = pressed ? EV_KEY_PRES : EV_KEY_RELE;
	tca8418_dispatch(device, event);
	tca8418_fsm_update_timer(keypad_data, device);
}

static void tca8418_fsm_cleanup_device(struct tca8418_keypad *keypad_data,
					       tca8418_t *device)
{
	tca8418_fsm_cancel_timer(keypad_data, device);
	if (device->layer_active) {
		disable_layer(device);
		emit_key_release_status(device);
		led_OFF(device);
	}
	device->state = ST_IDLE;
}

static void tca8418_fsm_cleanup(struct tca8418_keypad *keypad_data)
{
	unsigned int scan_code;

	for (scan_code = 0; scan_code < keypad_data->key_count; scan_code++) {
		struct tca8418_key_record *record =
			&keypad_data->key_records[scan_code];

		if (!record->pressed)
			continue;
		tca8418_report_key(keypad_data, scan_code, record->keycode, false);
		memset(record, 0, sizeof(*record));
	}
	tca8418_fsm_cleanup_device(keypad_data, &keypad_data->sym_key.fsm);
	tca8418_fsm_cleanup_device(keypad_data, &keypad_data->shift_key.fsm);
	tca8418_fsm_cleanup_device(keypad_data, &keypad_data->fn_key.fsm);
}


static void tca8418_fsm_process(struct tca8418_keypad *keypad_data,
				struct tca8418_function_key *key_obj, bool pressed)
{
	tca8418_fsm_process_function(keypad_data, &key_obj->fsm, pressed);
}

static bool tca8418_all_ordinary_keys_released(
		struct tca8418_keypad *keypad_data)
{
	unsigned int scan_code;

	for (scan_code = 0; scan_code < keypad_data->key_count; scan_code++) {
		if (keypad_data->key_records[scan_code].pressed)
			return false;
	}

	return true;
}

static void tca8418_dispatch_other_key_release(
		struct tca8418_keypad *keypad_data)
{
	int i;

	for (i = 0; i < 3; i++) {
		if (keypad_data->_key[i]->fsm.state != ST_ONESHOT)
			continue;
		tca8418_dispatch(&keypad_data->_key[i]->fsm,
					 EV_OTHER_KEY_RELE);
	}
}

static void tca8418_process_ordinary_key(struct tca8418_keypad *keypad_data,
						u8 code, bool pressed)
{
	struct tca8418_key_record *record;
	unsigned int keycode;

	if (code >= keypad_data->key_count)
		return;

	record = &keypad_data->key_records[code];
	if (pressed) {
		if (record->pressed)
			return;

		if (keypad_data->sym_key.fsm.layer_active)
			keycode = keypad_data->sym_key.keymap[code];
		else if (keypad_data->fn_key.fsm.layer_active)
			keycode = keypad_data->fn_key.keymap[code];
		else
			keycode = keypad_data->keymap[code];

		record->keycode = keycode;
		record->pressed = true;
	} else {
		if (!record->pressed)
			return;

		keycode = record->keycode;
		record->pressed = false;
	}

	tca8418_report_key(keypad_data, code, keycode, pressed);

	if (!pressed && tca8418_all_ordinary_keys_released(keypad_data))
		tca8418_dispatch_other_key_release(keypad_data);
}


static void tca8418_process_key_event(struct tca8418_keypad *keypad_data,
					 u8 code, bool pressed)
{
	for (int i = 0; i < 3; i++)
		if (keypad_data->_key[i]->scan_code == code)
		{
			tca8418_fsm_process(keypad_data, keypad_data->_key[i], pressed);
			return;
		}
	tca8418_process_ordinary_key(keypad_data, code, pressed);
}


static void tca8418_read_keypad(struct tca8418_keypad *keypad_data)
{
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

		tca8418_process_key_event(keypad_data, code, state);

	} while (1);

	input_sync(keypad_data->input);
}

static void tca8418_irq_work(struct work_struct *work)
{
	struct tca8418_keypad *keypad_data =
		container_of(work, struct tca8418_keypad, irq_work);
	u8 reg;
	int error;

	for (;;) {
		error = tca8418_read_byte(keypad_data, REG_INT_STAT, &reg);
		if (error < 0) {
			dev_err(&keypad_data->client->dev,
				"unable to read REG_INT_STAT\n");
			return;
		}

		if (!reg)
			return;

		if (reg & INT_STAT_OVR_FLOW_INT)
			dev_warn(&keypad_data->client->dev, "overflow occurred\n");

		if (reg & INT_STAT_K_INT)
			tca8418_read_keypad(keypad_data);

		/* Clear all interrupts, even IRQs we didn't check (GPI, CAD, LCK). */
		error = tca8418_write_byte(keypad_data, REG_INT_STAT, 0xff);
		if (error) {
			dev_err(&keypad_data->client->dev,
				"unable to clear REG_INT_STAT\n");
			return;
		}
	}
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

	queue_work(keypad_data->wq, &keypad_data->irq_work);

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

static void tca8418_put_i2c_client(void *data)
{
	struct i2c_client *client = data;

	put_device(&client->dev);
}

static void tca8418_cleanup(void *data)
{
	struct tca8418_keypad *keypad_data = data;
	unsigned int timer;

	cancel_work_sync(&keypad_data->irq_work);
	for (timer = 0; timer < ARRAY_SIZE(keypad_data->_key); timer++)
		cancel_delayed_work_sync(&keypad_data->_key[timer]->key_timeout_work);

	tca8418_fsm_cleanup(keypad_data);
	if (keypad_data->input)
		input_sync(keypad_data->input);
	destroy_workqueue(keypad_data->wq);
}

static int tca8418_parse_led_mode(struct device *dev,
				  struct tca8418_mode_led *led,
				  const char *property)
{
	struct of_phandle_args args;
	struct i2c_client *client;
	int error;

	if (!dev->of_node) {
		dev_err(dev, "missing device tree node for %s\n", property);
		return -EINVAL;
	}

	error = of_parse_phandle_with_fixed_args(dev->of_node, property, 1, 0,
						 &args);
	if (error == -ENOENT) {
		dev_err(dev, "missing required property %s\n", property);
		return -EINVAL;
	}
	if (error)
		return error;

	client = of_find_i2c_device_by_node(args.np);
	of_node_put(args.np);
	if (!client)
		return -EPROBE_DEFER;

	led->mode_regmap = dev_get_regmap(&client->dev, NULL);
	if (!led->mode_regmap) {
		put_device(&client->dev);
		return -EPROBE_DEFER;
	}

	error = devm_add_action_or_reset(dev, tca8418_put_i2c_client, client);
	if (error)
		return error;

	led->mode_register = args.args[0] & 0xff;

	TCA8418_LOG("%s parsed led mode target=%s reg=0x%02x\n",
			property, dev_name(&client->dev), led->mode_register);

	return 0;
}

static int tca8418_keypad_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct tca8418_keypad *keypad_data;
	struct input_dev *input;
	u32 rows = 0, cols = 0;
	u32 sym_button_code;
	u32 fn_button_code;
	u32 shift_button_code;
	unsigned int keymap_size;
	int error, row_shift;
	u8 reg;

	/* Check i2c driver capabilities */
	if (!i2c_check_functionality(client->adapter, I2C_FUNC_SMBUS_BYTE_DATA)) {
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

	keypad_data->_key[0] = &keypad_data->sym_key;
	keypad_data->_key[1] = &keypad_data->shift_key;
	keypad_data->_key[2] = &keypad_data->fn_key;

	keypad_data->client = client;
	keypad_data->row_shift = row_shift;
	keypad_data->wq = alloc_ordered_workqueue("tca8418", WQ_MEM_RECLAIM);
	if (!keypad_data->wq)
		return -ENOMEM;
	INIT_WORK(&keypad_data->irq_work, tca8418_irq_work);

	keymap_size = rows << row_shift;
	keypad_data->key_count = keymap_size;
	keypad_data->key_records = devm_kcalloc(dev, keymap_size,
						 sizeof(*keypad_data->key_records),
						 GFP_KERNEL);
	if (!keypad_data->key_records)
		return -ENOMEM;
	for (int i = 0; i < 3; i++) {
		tca8418_ctor(&keypad_data->_key[i]->fsm, i);
		keypad_data->_key[i]->fsm.ctx = keypad_data;
		INIT_DELAYED_WORK(&keypad_data->_key[i]->key_timeout_work,
				  tca8418_fsm_timer_work);
	}

	error = devm_add_action_or_reset(dev, tca8418_cleanup, keypad_data);
	if (error)
		return error;


	error = device_property_read_u32(dev, "sym-button-code", &sym_button_code);
	if (error)
		return dev_err_probe(dev, error,
				     "failed to read sym-button-code\n");

	error = device_property_read_u32(dev, "fn-button-code", &fn_button_code);
	if (error)
		return dev_err_probe(dev, error,
				     "failed to read fn-button-code\n");

	error = device_property_read_u32(dev, "shift-button-code",
					 &shift_button_code);
	if (error)
		return dev_err_probe(dev, error,
				     "failed to read shift-button-code\n");

	if (sym_button_code >= keymap_size || fn_button_code >= keymap_size ||
	    shift_button_code >= keymap_size) {
		dev_err(dev, "function key scan code out of range\n");
		return -EINVAL;
	}

	keypad_data->sym_key.scan_code = sym_button_code;
	keypad_data->shift_key.scan_code = shift_button_code;
	keypad_data->fn_key.scan_code = fn_button_code;

	error = tca8418_parse_led_mode(dev, &keypad_data->sym_key.led,
				       "tables-sel-led-mode");
	if (error)
		return dev_err_probe(dev, error,
				     "failed to parse tables-sel-led-mode\n");

	error = tca8418_parse_led_mode(dev, &keypad_data->shift_key.led,
				       "capslock-led-mode");
	if (error)
		return dev_err_probe(dev, error,
				     "failed to parse capslock-led-mode\n");

	error = tca8418_parse_led_mode(dev, &keypad_data->fn_key.led,
				       "fn-led-mode");
	if (error)
		return dev_err_probe(dev, error,
				     "failed to parse fn-led-mode\n");

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

	keypad_data->keymap = input->keycode;
	error = matrix_keypad_build_keymap(NULL, "linux,keymap1", rows, cols, NULL, input);
	if (error) {
		dev_err(dev, "Failed to build keymap1\n");
		return error;
	}
	{
		unsigned short *tmp_keymap = input->keycode;

		input->keycode = keypad_data->keymap;
		keypad_data->sym_key.keymap = tmp_keymap;
	}

	error = matrix_keypad_build_keymap(NULL, "linux,keymap2", rows, cols, NULL, input);
	if (error) {
		dev_err(dev, "Failed to build keymap2\n");
		return error;
	}
	{
		unsigned short *tmp_keymap = input->keycode;

		input->keycode = keypad_data->keymap;
		keypad_data->fn_key.keymap = tmp_keymap;
	}

	input_set_capability(input, EV_KEY, KEY_CD);
	input_set_capability(input, EV_KEY, KEY_DVD);

	if (device_property_read_bool(dev, "keypad,autorepeat"))
		__set_bit(EV_REP, input->evbit);

	input_set_drvdata(input, keypad_data);

	input_set_capability(input, EV_MSC, MSC_SCAN);
	// __set_bit(EV_MSC, input->evbit);
	// __set_bit(MSC_SCAN, input->mscbit);

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
