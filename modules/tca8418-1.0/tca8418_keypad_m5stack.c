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
#include <linux/types.h>
#include <linux/gpio/consumer.h>
#include <linux/leds.h>
#include <linux/workqueue.h>

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

#define TCA8418_DBLCLICK_MS	300
#define TCA8418_LONGPRESS_MS	500
#define TCA8418_GPIO_BLINK_MS	250
#define TCA8418_GPIO_FAST_MS	80

#define TCA8418_LED_OFF		0
#define TCA8418_LED_BLINK	1
#define TCA8418_LED_FAST	2
#define TCA8418_LED_ON		3

#define M5IOE1_VERSION                  0xBA
#define M5IOE1_NEW_MODE_VERSION		0xF6

struct tca8418_keypad;

struct tca8418_mode_led {
	struct gpio_desc *gpio;
	struct i2c_client *mode_client;
	struct regmap *mode_regmap;
	struct delayed_work blink_work;
	const char *name;
	u8 mode_reg;
	u8 mode;
	bool has_mode;
	bool gpio_on;
};

struct tca8418_layer_key {
	struct tca8418_mode_led *led;
	struct delayed_work longpress_work;
	ktime_t last_release;
	int scan_code;
	bool pressed;
	bool oneshot;
	bool locked;
	bool longpress;
	bool unlock_release;
};


enum tca8418_active_mode {
	TCA8418_MODE_NONE,
	TCA8418_MODE_SYM,
	TCA8418_MODE_FN,
	TCA8418_MODE_ASMUX,
};

struct tca8418_keypad {
	struct i2c_client *client;
	struct input_dev *input;
	struct led_classdev capslock_led;

	struct tca8418_mode_led tables_sel_led;
	struct tca8418_mode_led capslock_led_ctl;
	struct tca8418_mode_led fn_led;
	int map_base_index;
	
	int sym_button_code;
	int fn_button_code;
	int asmux_button_code;
	bool new_keyboard_mode;

	struct tca8418_layer_key sym_key;
	struct tca8418_layer_key fn_key;
	int sym_button_flag;
	int fn_button_flag;
	int asmux_button_flag;
	ktime_t sym_timer;
	ktime_t asmux_timer;
	ktime_t asmux_last_release;
	bool asmux_pressed;
	bool asmux_second_click;
	bool asmux_oneshot;
	bool asmux_shift_active;
	unsigned int asmux_shift_code;
	bool asmux_locked;
	bool asmux_unlock_pending;
	bool asmux_longpress;
	struct delayed_work asmux_longpress_work;
	struct delayed_work asmux_blink_off_work;

	unsigned int row_shift;
	unsigned short *keycode1;
	unsigned short *keycode2;
	unsigned short *last_keycode;
	bool *key_suppressed;

    struct work_struct capslock_work;
    bool capslock_state;
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

static bool tca8418_time_before_ms(ktime_t then, unsigned int ms)
{
	return ktime_to_ms(ktime_sub(ktime_get(), then)) <= ms;
}

static void tca8418_led_apply_gpio(struct tca8418_mode_led *led)
{
	if (led->gpio)
		gpiod_set_value_cansleep(led->gpio, led->gpio_on);
}

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

static void tca8418_led_blink_work(struct work_struct *work)
{
	struct tca8418_mode_led *led =
		container_of(to_delayed_work(work), struct tca8418_mode_led,
			     blink_work);
	unsigned int delay_ms;

	if (!led->gpio || led->mode == TCA8418_LED_OFF ||
	    led->mode == TCA8418_LED_ON)
		return;

	led->gpio_on = !led->gpio_on;
	tca8418_led_apply_gpio(led);

	delay_ms = (led->mode == TCA8418_LED_FAST) ?
		TCA8418_GPIO_FAST_MS : TCA8418_GPIO_BLINK_MS;
	schedule_delayed_work(&led->blink_work, msecs_to_jiffies(delay_ms));
}

static void tca8418_led_set(struct tca8418_mode_led *led, u8 mode)
{
	int error;
	u8 old_mode = led->mode;

	if (led->mode == mode) {
		TCA8418_LOG("%s led mode unchanged: %u(%s)\n",
			    led->name ? led->name : "unknown", mode,
			    tca8418_led_mode_name(mode));
		return;
	}

	led->mode = mode;

	if (led->has_mode) {
		TCA8418_LOG("%s led mode %u(%s) -> %u(%s), write reg=0x%02x\n",
			    led->name ? led->name : "unknown", old_mode,
			    tca8418_led_mode_name(old_mode), mode,
			    tca8418_led_mode_name(mode), led->mode_reg);

		error = regmap_write(led->mode_regmap, led->mode_reg, mode);
		TCA8418_LOG("%s led mode write reg=0x%02x value=%u(%s) ret=%d\n",
			    led->name ? led->name : "unknown", led->mode_reg,
			    mode, tca8418_led_mode_name(mode), error);
		if (error < 0)
			dev_warn(&led->mode_client->dev,
				 "failed to set led mode reg 0x%02x: %d\n",
				 led->mode_reg, error);
	} else {
		TCA8418_LOG("%s led gpio mode %u(%s) -> %u(%s)\n",
			    led->name ? led->name : "unknown", old_mode,
			    tca8418_led_mode_name(old_mode), mode,
			    tca8418_led_mode_name(mode));
	}

	cancel_delayed_work_sync(&led->blink_work);

	if (!led->has_mode && mode != TCA8418_LED_OFF)
		mode = TCA8418_LED_ON;

	switch (mode) {
	case TCA8418_LED_ON:
		led->gpio_on = true;
		tca8418_led_apply_gpio(led);
		break;
	case TCA8418_LED_BLINK:
	case TCA8418_LED_FAST:
		led->gpio_on = true;
		tca8418_led_apply_gpio(led);
		schedule_delayed_work(&led->blink_work, 0);
		break;
	default:
		led->gpio_on = false;
		tca8418_led_apply_gpio(led);
		break;
	}
}

static void tca8418_report_key(struct tca8418_keypad *keypad_data,
			       unsigned int scan_code, unsigned int keycode,
			       bool pressed)
{
	input_event(keypad_data->input, EV_MSC, MSC_SCAN, scan_code);
	input_report_key(keypad_data->input, keycode, pressed);
}

static bool tca8418_is_layer_active(struct tca8418_layer_key *layer)
{
	return layer->pressed || layer->oneshot || layer->locked;
}

static void tca8418_layer_update_led(struct tca8418_layer_key *layer)
{
	if (layer->longpress)
		tca8418_led_set(layer->led, TCA8418_LED_ON);
	else if (layer->locked)
		tca8418_led_set(layer->led, TCA8418_LED_FAST);
	else if (layer->oneshot)
		tca8418_led_set(layer->led, TCA8418_LED_BLINK);
	else
		tca8418_led_set(layer->led, TCA8418_LED_OFF);
}

static void tca8418_layer_longpress_work(struct work_struct *work)
{
	struct tca8418_layer_key *layer =
		container_of(to_delayed_work(work), struct tca8418_layer_key,
			     longpress_work);

	if (!layer->pressed || layer->locked)
		return;

	layer->longpress = true;
	layer->oneshot = false;
	tca8418_layer_update_led(layer);
}

static void tca8418_layer_deactivate(struct tca8418_layer_key *layer)
{
	cancel_delayed_work_sync(&layer->longpress_work);

	layer->pressed = false;
	layer->oneshot = false;
	layer->locked = false;
	layer->longpress = false;
	layer->unlock_release = false;
	tca8418_layer_update_led(layer);
}

static void tca8418_asmux_update_led(struct tca8418_keypad *keypad_data);

static void tca8418_asmux_deactivate(struct tca8418_keypad *keypad_data)
{
	cancel_delayed_work_sync(&keypad_data->asmux_longpress_work);
	cancel_delayed_work_sync(&keypad_data->asmux_blink_off_work);

	if (keypad_data->asmux_locked || keypad_data->asmux_shift_active ||
	    keypad_data->asmux_longpress)
		tca8418_report_key(keypad_data, keypad_data->asmux_button_code,
				    KEY_LEFTSHIFT, false);

	keypad_data->asmux_pressed = false;
	keypad_data->asmux_second_click = false;
	keypad_data->asmux_oneshot = false;
	keypad_data->asmux_shift_active = false;
	keypad_data->asmux_locked = false;
	keypad_data->asmux_unlock_pending = false;
	keypad_data->asmux_longpress = false;
	tca8418_asmux_update_led(keypad_data);
}

static void tca8418_deactivate_other_modes(struct tca8418_keypad *keypad_data,
					   enum tca8418_active_mode active)
{
	if (active != TCA8418_MODE_SYM)
		tca8418_layer_deactivate(&keypad_data->sym_key);

	/* Fn is a layer selector and may be combined with Shift/ASMUX. */
	if (active != TCA8418_MODE_FN && active != TCA8418_MODE_ASMUX)
		tca8418_layer_deactivate(&keypad_data->fn_key);
	if (active != TCA8418_MODE_ASMUX && active != TCA8418_MODE_FN)
		tca8418_asmux_deactivate(keypad_data);
}

static void tca8418_handle_layer_key(struct tca8418_keypad *keypad_data,
				     struct tca8418_layer_key *layer,
				     enum tca8418_active_mode active,
				     bool pressed)
{
	if (pressed) {
		tca8418_deactivate_other_modes(keypad_data, active);

		layer->pressed = true;
		layer->longpress = false;

		if (layer->locked) {
			layer->locked = false;
			layer->oneshot = false;
			layer->unlock_release = true;
		} else if (layer->oneshot) {
			layer->oneshot = false;
			if (tca8418_time_before_ms(layer->last_release,
						   TCA8418_DBLCLICK_MS))
				layer->locked = true;
			else
				layer->unlock_release = true;
		} else {
			layer->oneshot = false;
			schedule_delayed_work(&layer->longpress_work,
				msecs_to_jiffies(TCA8418_LONGPRESS_MS));
		}

		tca8418_layer_update_led(layer);
		return;
	}

	cancel_delayed_work_sync(&layer->longpress_work);

	if (!layer->pressed)
		return;

	layer->pressed = false;

	if (layer->unlock_release) {
		layer->unlock_release = false;
	} else if (layer->longpress) {
		layer->longpress = false;
	} else if (!layer->locked) {
		layer->oneshot = true;
		layer->last_release = ktime_get();
	}

	tca8418_layer_update_led(layer);
}

static void tca8418_consume_layer_oneshot(struct tca8418_layer_key *layer)
{
	if (!layer->oneshot)
		return;

	layer->oneshot = false;
	tca8418_layer_update_led(layer);
}

static void tca8418_asmux_update_led(struct tca8418_keypad *keypad_data)
{
	if (keypad_data->asmux_longpress)
		tca8418_led_set(&keypad_data->capslock_led_ctl,
				TCA8418_LED_ON);
	else if (keypad_data->asmux_locked)
		tca8418_led_set(&keypad_data->capslock_led_ctl,
				TCA8418_LED_FAST);
	else if (keypad_data->asmux_oneshot)
		tca8418_led_set(&keypad_data->capslock_led_ctl,
				TCA8418_LED_BLINK);
	else
		tca8418_led_set(&keypad_data->capslock_led_ctl,
				keypad_data->capslock_state ? TCA8418_LED_ON :
				TCA8418_LED_OFF);
}

static void tca8418_asmux_blink_off_work(struct work_struct *work)
{
	struct tca8418_keypad *keypad_data =
		container_of(to_delayed_work(work), struct tca8418_keypad,
			     asmux_blink_off_work);

	if (!keypad_data->asmux_pressed && !keypad_data->asmux_oneshot &&
	    !keypad_data->asmux_locked && !keypad_data->asmux_longpress)
		tca8418_asmux_update_led(keypad_data);
}

static void tca8418_asmux_longpress_work(struct work_struct *work)
{
	struct tca8418_keypad *keypad_data =
		container_of(to_delayed_work(work), struct tca8418_keypad,
			     asmux_longpress_work);

	if (!keypad_data->asmux_pressed || keypad_data->asmux_locked)
		return;

	keypad_data->asmux_longpress = true;
	tca8418_report_key(keypad_data, keypad_data->asmux_button_code,
			    KEY_LEFTSHIFT, true);
	tca8418_asmux_update_led(keypad_data);
}

static void tca8418_handle_asmux_key(struct tca8418_keypad *keypad_data,
				     unsigned int scan_code, bool pressed)
{
	if (pressed) {
		tca8418_deactivate_other_modes(keypad_data, TCA8418_MODE_ASMUX);

		cancel_delayed_work_sync(&keypad_data->asmux_blink_off_work);
		keypad_data->asmux_pressed = true;
		keypad_data->asmux_longpress = false;

		if (keypad_data->asmux_locked) {
			keypad_data->asmux_unlock_pending = true;
			keypad_data->asmux_second_click = false;
			return;
		}

		if (keypad_data->asmux_oneshot) {
			keypad_data->asmux_oneshot = false;
			if (tca8418_time_before_ms(keypad_data->asmux_last_release,
						   TCA8418_DBLCLICK_MS)) {
				keypad_data->asmux_locked = true;
				tca8418_report_key(keypad_data, scan_code,
						    KEY_LEFTSHIFT, true);
			} else {
				keypad_data->asmux_second_click = true;
			}
			tca8418_asmux_update_led(keypad_data);
			return;
		}

		keypad_data->asmux_oneshot = false;

		schedule_delayed_work(&keypad_data->asmux_longpress_work,
			msecs_to_jiffies(TCA8418_LONGPRESS_MS));
		return;
	}

	cancel_delayed_work_sync(&keypad_data->asmux_longpress_work);

	if (!keypad_data->asmux_pressed)
		return;

	keypad_data->asmux_pressed = false;

	if (keypad_data->asmux_unlock_pending) {
		keypad_data->asmux_unlock_pending = false;
		keypad_data->asmux_locked = false;
		tca8418_report_key(keypad_data, scan_code, KEY_LEFTSHIFT, false);
		tca8418_asmux_update_led(keypad_data);
		return;
	}

	if (keypad_data->asmux_second_click) {
		keypad_data->asmux_second_click = false;
		tca8418_asmux_update_led(keypad_data);
		return;
	}

	keypad_data->asmux_second_click = false;

	if (keypad_data->asmux_longpress) {
		tca8418_report_key(keypad_data, scan_code, KEY_LEFTSHIFT, false);
		keypad_data->asmux_longpress = false;
		tca8418_asmux_update_led(keypad_data);
	} else if (!keypad_data->asmux_locked) {
		keypad_data->asmux_oneshot = true;
		keypad_data->asmux_last_release = ktime_get();
		tca8418_asmux_update_led(keypad_data);
	}
}

static void tca8418_consume_asmux_oneshot(struct tca8418_keypad *keypad_data,
					  unsigned int scan_code)
{
	if (!keypad_data->asmux_oneshot)
		return;

	keypad_data->asmux_oneshot = false;
	keypad_data->asmux_shift_active = true;
	keypad_data->asmux_shift_code = scan_code;
	cancel_delayed_work_sync(&keypad_data->asmux_blink_off_work);
	tca8418_report_key(keypad_data, scan_code, KEY_LEFTSHIFT, true);
	tca8418_asmux_update_led(keypad_data);
}

static void tca8418_activate_asmux_hold(struct tca8418_keypad *keypad_data)
{
	if (!keypad_data->asmux_pressed || keypad_data->asmux_locked ||
	    keypad_data->asmux_longpress)
		return;

	cancel_delayed_work_sync(&keypad_data->asmux_longpress_work);
	if (keypad_data->asmux_longpress)
		return;

	keypad_data->asmux_longpress = true;
	tca8418_report_key(keypad_data, keypad_data->asmux_button_code,
			    KEY_LEFTSHIFT, true);
	tca8418_asmux_update_led(keypad_data);
}

static void tca8418_release_asmux_oneshot(struct tca8418_keypad *keypad_data,
					  unsigned int scan_code)
{
	if (!keypad_data->asmux_shift_active ||
	    keypad_data->asmux_shift_code != scan_code)
		return;

	keypad_data->asmux_shift_active = false;
	tca8418_report_key(keypad_data, scan_code, KEY_LEFTSHIFT, false);
	tca8418_asmux_update_led(keypad_data);
}

static void tca8418_handle_key_new(struct tca8418_keypad *keypad_data,
				   unsigned int code, bool state)
{
	struct input_dev *input = keypad_data->input;
	unsigned short *keymap = input->keycode;

	if (code == keypad_data->fn_key.scan_code) {
		tca8418_handle_layer_key(keypad_data, &keypad_data->fn_key,
					 TCA8418_MODE_FN, state);
		return;
	}

	if (code == keypad_data->sym_key.scan_code) {
		tca8418_handle_layer_key(keypad_data, &keypad_data->sym_key,
					 TCA8418_MODE_SYM, state);
		return;
	}

	if (code == keypad_data->asmux_button_code) {
		tca8418_handle_asmux_key(keypad_data, code, state);
		return;
	}

	if (state) {
		unsigned int report_code = keymap[code];

		keypad_data->key_suppressed[code] = false;

		if (tca8418_is_layer_active(&keypad_data->fn_key)) {
			report_code = keypad_data->keycode2[code];
			tca8418_consume_layer_oneshot(&keypad_data->fn_key);
		} else if (tca8418_is_layer_active(&keypad_data->sym_key)) {
			report_code = keypad_data->keycode1[code];
			tca8418_consume_layer_oneshot(&keypad_data->sym_key);
		}

		if (report_code == KEY_RESERVED) {
			keypad_data->key_suppressed[code] = true;
			keypad_data->last_keycode[code] = KEY_RESERVED;
			return;
		}

		tca8418_activate_asmux_hold(keypad_data);
		tca8418_consume_asmux_oneshot(keypad_data, code);
		keypad_data->last_keycode[code] = report_code;
		tca8418_report_key(keypad_data, code, report_code, true);
	} else {
		unsigned int report_code = keypad_data->last_keycode[code];

		if (keypad_data->key_suppressed[code]) {
			keypad_data->key_suppressed[code] = false;
			keypad_data->last_keycode[code] = KEY_RESERVED;
			tca8418_release_asmux_oneshot(keypad_data, code);
			return;
		}

		if (!report_code)
			report_code = keymap[code];

		keypad_data->last_keycode[code] = 0;
		tca8418_report_key(keypad_data, code, report_code, false);
		tca8418_release_asmux_oneshot(keypad_data, code);
	}
}

static void tca8418_handle_key_legacy(struct tca8418_keypad *keypad_data,
				      unsigned int code, bool state)
{
	struct input_dev *input = keypad_data->input;
	unsigned short *keymap = input->keycode;
	unsigned int report_code = keymap[code];

	if (code == keypad_data->fn_button_code) {
		keypad_data->fn_button_flag = state ? 1 : 0;
		tca8418_led_set(&keypad_data->fn_led,
				state ? TCA8418_LED_ON : TCA8418_LED_OFF);
	}

	if (code == keypad_data->sym_button_code) {
		if (!state) {
			if (keypad_data->sym_button_flag)
				keypad_data->sym_button_flag--;
			keypad_data->sym_timer = ktime_get();
		} else {
			keypad_data->sym_button_flag = 1;
			if (tca8418_time_before_ms(keypad_data->sym_timer,
						   TCA8418_DBLCLICK_MS))
				keypad_data->sym_button_flag = 2;
		}
	}

	if (keypad_data->sym_button_flag) {
		report_code = keypad_data->keycode1[code];
		tca8418_led_set(&keypad_data->tables_sel_led, TCA8418_LED_ON);
	} else {
		tca8418_led_set(&keypad_data->tables_sel_led, TCA8418_LED_OFF);
	}

	if (keypad_data->fn_button_flag)
		report_code = keypad_data->keycode2[code];

	if (code == keypad_data->asmux_button_code) {
		report_code = KEY_LEFTSHIFT;
		if (keypad_data->asmux_button_flag) {
			report_code = KEY_CAPSLOCK;
			keypad_data->asmux_button_flag = 0;
		}
		if (!state) {
			keypad_data->asmux_timer = ktime_get();
		} else if (tca8418_time_before_ms(keypad_data->asmux_timer,
						  TCA8418_DBLCLICK_MS)) {
			report_code = KEY_CAPSLOCK;
			keypad_data->asmux_button_flag = 1;
		}
	}

	tca8418_report_key(keypad_data, code, report_code, state);
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

		if (keypad_data->new_keyboard_mode)
			tca8418_handle_key_new(keypad_data, code, state);
		else
			tca8418_handle_key_legacy(keypad_data, code, state);

	} while (1);

	input_sync(keypad_data->input);
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

static void tca8418_put_i2c_client(void *data)
{
	struct i2c_client *client = data;

	put_device(&client->dev);
}

static void tca8418_cleanup(void *data)
{
	struct tca8418_keypad *keypad_data = data;

	cancel_delayed_work_sync(&keypad_data->sym_key.longpress_work);
	cancel_delayed_work_sync(&keypad_data->fn_key.longpress_work);
	cancel_delayed_work_sync(&keypad_data->asmux_longpress_work);
	cancel_delayed_work_sync(&keypad_data->asmux_blink_off_work);
	cancel_delayed_work_sync(&keypad_data->tables_sel_led.blink_work);
	cancel_delayed_work_sync(&keypad_data->fn_led.blink_work);
	cancel_delayed_work_sync(&keypad_data->capslock_led_ctl.blink_work);
	cancel_work_sync(&keypad_data->capslock_work);
}

static int tca8418_parse_led_mode(struct device *dev,
				  struct tca8418_mode_led *led,
				  const char *property)
{
	struct of_phandle_args args;
	struct i2c_client *client;
	int error;

	if (!dev->of_node)
		return 0;

	error = of_parse_phandle_with_fixed_args(dev->of_node, property, 1, 0,
						 &args);
	if (error == -ENOENT)
		return 0;
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

	led->mode_client = client;
	led->mode_reg = args.args[0] & 0xff;
	led->has_mode = true;
	led->name = property;

	TCA8418_LOG("%s parsed led mode target=%s reg=0x%02x\n",
		    property, dev_name(&client->dev), led->mode_reg);

	return 0;
}

static int tca8418_read_m5ioe1_version(struct device *dev,
				       unsigned int *version)
{
	static const char * const properties[] = {
		"fn-led-mode",
		"capslock-led-mode",
		"tables-sel-led-mode",
		"fn-led-gpio",
		"capslock-led-gpio",
		"tables-sel-led-gpio",
	};
	struct device_node *np = NULL;
	struct i2c_client *client;
	struct regmap *regmap;
	int error;
	int i;

	if (!dev->of_node)
		return -ENODEV;

	for (i = 0; i < ARRAY_SIZE(properties); i++) {
		np = of_parse_phandle(dev->of_node, properties[i], 0);
		if (np)
			break;
	}

	if (!np)
		return -ENODEV;

	client = of_find_i2c_device_by_node(np);
	of_node_put(np);
	if (!client)
		return -EPROBE_DEFER;

	regmap = dev_get_regmap(&client->dev, NULL);
	if (!regmap) {
		put_device(&client->dev);
		return -EPROBE_DEFER;
	}

	error = regmap_read(regmap, M5IOE1_VERSION, version);
	put_device(&client->dev);
	if (error)
		return error;

	return 0;
}

static void tca8418_capslock_work(struct work_struct *work)
{
    struct tca8418_keypad *keypad =
        container_of(work, struct tca8418_keypad, capslock_work);

    if (!keypad->asmux_locked && !keypad->asmux_longpress)
	tca8418_led_set(&keypad->capslock_led_ctl,
			keypad->capslock_state ? TCA8418_LED_ON :
			TCA8418_LED_OFF);
}

static int tca8418_input_event(struct input_dev *dev,
                   unsigned int type,
                   unsigned int code,
                   int value)
{
    struct tca8418_keypad *keypad = input_get_drvdata(dev);

    if (type == EV_LED && code == LED_CAPSL) {
        keypad->capslock_state = !!value;
        schedule_work(&keypad->capslock_work);
        return 0;
    }

    return -EINVAL;
}


static int tca8418_keypad_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct tca8418_keypad *keypad_data;
	struct input_dev *input;
	u32 rows = 0, cols = 0;
	u32 sym_button_code = -1;
	u32 fn_button_code = -1;
	u32 asmux_button_code = -1;
	unsigned int keymap_size;
	unsigned int m5ioe1_version;
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

	keypad_data->client = client;
	keypad_data->row_shift = row_shift;
	keymap_size = rows << row_shift;
	keypad_data->last_keycode = devm_kcalloc(dev, keymap_size,
						  sizeof(*keypad_data->last_keycode),
						  GFP_KERNEL);
	if (!keypad_data->last_keycode)
		return -ENOMEM;
	keypad_data->key_suppressed = devm_kcalloc(dev, keymap_size,
						   sizeof(*keypad_data->key_suppressed),
						   GFP_KERNEL);
	if (!keypad_data->key_suppressed)
		return -ENOMEM;

	device_property_read_u32(dev, "sym-button-code", &sym_button_code);
	device_property_read_u32(dev, "fn-button-code", &fn_button_code);
	device_property_read_u32(dev, "asmux-button-code", &asmux_button_code);
	keypad_data->sym_button_code = sym_button_code;
	keypad_data->fn_button_code = fn_button_code;
	error = tca8418_read_m5ioe1_version(dev, &m5ioe1_version);
	if (error)
		return dev_err_probe(dev, error,
				     "failed to read M5IOE1 version\n");

	keypad_data->new_keyboard_mode =
		m5ioe1_version >= M5IOE1_NEW_MODE_VERSION;
	dev_info(dev, "M5IOE1 version 0x%02x, using %s keyboard mode\n",
		 m5ioe1_version,
		 keypad_data->new_keyboard_mode ? "new" : "old");
	keypad_data->sym_key.scan_code = sym_button_code;
	keypad_data->fn_key.scan_code = fn_button_code;
	keypad_data->asmux_button_code = asmux_button_code;

	if (keypad_data->new_keyboard_mode) {
		error = tca8418_parse_led_mode(dev, &keypad_data->tables_sel_led,
					       "tables-sel-led-mode");
		if (error)
			return dev_err_probe(dev, error,
					     "failed to parse tables-sel-led-mode\n");

		error = tca8418_parse_led_mode(dev, &keypad_data->capslock_led_ctl,
					       "capslock-led-mode");
		if (error)
			return dev_err_probe(dev, error,
					     "failed to parse capslock-led-mode\n");

		error = tca8418_parse_led_mode(dev, &keypad_data->fn_led,
					       "fn-led-mode");
		if (error)
			return dev_err_probe(dev, error,
					     "failed to parse fn-led-mode\n");
	} else {
		keypad_data->capslock_led_ctl.gpio =
			devm_gpiod_get_optional(dev, "capslock-led", GPIOD_OUT_LOW);
		keypad_data->tables_sel_led.gpio =
			devm_gpiod_get_optional(dev, "tables-sel-led", GPIOD_OUT_LOW);
		keypad_data->fn_led.gpio =
			devm_gpiod_get_optional(dev, "fn-led", GPIOD_OUT_LOW);

		if (IS_ERR(keypad_data->capslock_led_ctl.gpio))
			return dev_err_probe(dev, PTR_ERR(keypad_data->capslock_led_ctl.gpio),
					     "failed to get capslock led gpio\n");
		if (IS_ERR(keypad_data->tables_sel_led.gpio))
			return dev_err_probe(dev, PTR_ERR(keypad_data->tables_sel_led.gpio),
					     "failed to get table select led gpio\n");
		if (IS_ERR(keypad_data->fn_led.gpio))
			return dev_err_probe(dev, PTR_ERR(keypad_data->fn_led.gpio),
					     "failed to get fn led gpio\n");
	}

	INIT_DELAYED_WORK(&keypad_data->tables_sel_led.blink_work,
			  tca8418_led_blink_work);
	INIT_DELAYED_WORK(&keypad_data->fn_led.blink_work,
			  tca8418_led_blink_work);
	INIT_DELAYED_WORK(&keypad_data->capslock_led_ctl.blink_work,
			  tca8418_led_blink_work);
	INIT_DELAYED_WORK(&keypad_data->sym_key.longpress_work,
			  tca8418_layer_longpress_work);
	INIT_DELAYED_WORK(&keypad_data->fn_key.longpress_work,
			  tca8418_layer_longpress_work);
	INIT_DELAYED_WORK(&keypad_data->asmux_longpress_work,
			  tca8418_asmux_longpress_work);
	INIT_DELAYED_WORK(&keypad_data->asmux_blink_off_work,
			  tca8418_asmux_blink_off_work);
	INIT_WORK(&keypad_data->capslock_work, tca8418_capslock_work);

	keypad_data->sym_key.led = &keypad_data->tables_sel_led;
	keypad_data->fn_key.led = &keypad_data->fn_led;

	error = devm_add_action_or_reset(dev, tca8418_cleanup, keypad_data);
	if (error)
		return error;

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
	if (error) {
		dev_err(dev, "Failed to build keymap1\n");
		return error;
	}
	{
		unsigned short *tmp_keycode1 = input->keycode;

		input->keycode = keypad_data->keycode1;
		keypad_data->keycode1 = tmp_keycode1;
	}

	keypad_data->keycode2 = input->keycode;
	error = matrix_keypad_build_keymap(NULL, "linux,keymap2", rows, cols, NULL, input);
	if (error) {
		dev_err(dev, "Failed to build keymap2\n");
		return error;
	}
	{
		unsigned short *tmp_keycode2 = input->keycode;

		input->keycode = keypad_data->keycode2;
		keypad_data->keycode2 = tmp_keycode2;
	}

	if (device_property_read_bool(dev, "keypad,autorepeat"))
		__set_bit(EV_REP, input->evbit);

	input_set_drvdata(input, keypad_data);

	if (keypad_data->capslock_led_ctl.gpio ||
	    keypad_data->capslock_led_ctl.has_mode)
	{
		__set_bit(EV_LED, input->evbit);
		__set_bit(LED_CAPSL, input->ledbit);
		input->event = tca8418_input_event;
	}

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
