/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef CARDPUTERZERO_KEYBOARD_STATE_H
#define CARDPUTERZERO_KEYBOARD_STATE_H

/* Text snapshot ABI exposed by the modifier_state sysfs attribute. */
#define CPZ_KBD_STATE_ABI_VERSION 1

enum cpz_kbd_modifier_state {
	CPZ_KBD_MOD_OFF = 0,
	CPZ_KBD_MOD_PRESSED = 1,
	CPZ_KBD_MOD_ONESHOT = 2,
	CPZ_KBD_MOD_LOCKED = 3,
	CPZ_KBD_MOD_HELD = 4,
};

enum cpz_kbd_effective_layer {
	CPZ_KBD_LAYER_NORMAL = 0,
	CPZ_KBD_LAYER_SYM = 1,
	CPZ_KBD_LAYER_FN = 2,
};

enum cpz_kbd_state_reason {
	CPZ_KBD_REASON_INIT = 0,
	CPZ_KBD_REASON_PRESS = 1,
	CPZ_KBD_REASON_RELEASE = 2,
	CPZ_KBD_REASON_LONGPRESS = 3,
	CPZ_KBD_REASON_LOCK = 4,
	CPZ_KBD_REASON_UNLOCK = 5,
	CPZ_KBD_REASON_CONSUMED = 6,
	CPZ_KBD_REASON_SUSPEND = 7,
	CPZ_KBD_REASON_RESUME = 8,
	CPZ_KBD_REASON_REMOVE = 9,
};

#define CPZ_KBD_CHANGED_SYM   (1U << 0)
#define CPZ_KBD_CHANGED_SHIFT (1U << 1)
#define CPZ_KBD_CHANGED_FN    (1U << 2)
#define CPZ_KBD_CHANGED_CTRL  (1U << 3)
#define CPZ_KBD_CHANGED_ALT   (1U << 4)

/*
 * EV_MSC/MSC_RAW metadata emitted immediately before MSC_SCAN and EV_KEY.
 * Bits 31..16: "CZ", bits 15..8: ABI version, bits 7..0: effective layer.
 */
#define CPZ_KBD_MSC_RAW_MAGIC        0x435a0000U
#define CPZ_KBD_MSC_RAW_VERSION_MASK 0x0000ff00U
#define CPZ_KBD_MSC_RAW_LAYER_MASK   0x000000ffU
#define CPZ_KBD_MSC_RAW_VALUE(layer) \
	(CPZ_KBD_MSC_RAW_MAGIC | (CPZ_KBD_STATE_ABI_VERSION << 8) | \
	 ((layer) & CPZ_KBD_MSC_RAW_LAYER_MASK))
#define CPZ_KBD_MSC_RAW_IS_STATE(value) \
	(((value) & 0xffff0000U) == CPZ_KBD_MSC_RAW_MAGIC)
#define CPZ_KBD_MSC_RAW_VERSION(value) \
	(((value) & CPZ_KBD_MSC_RAW_VERSION_MASK) >> 8)
#define CPZ_KBD_MSC_RAW_LAYER(value) \
	((value) & CPZ_KBD_MSC_RAW_LAYER_MASK)

#endif
