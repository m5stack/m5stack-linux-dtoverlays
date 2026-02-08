// /*
//  * Driver for TCA8418 I2C keyboard
//  *
//  * Copyright (C) 2011 Fuel7, Inc.  All rights reserved.
//  *
//  * Author: Kyle Manna <kyle.manna@fuel7.com>
//  *
//  * This program is free software; you can redistribute it and/or
//  * modify it under the terms of the GNU General Public
//  * License v2 as published by the Free Software Foundation.
//  *
//  * This program is distributed in the hope that it will be useful,
//  * but WITHOUT ANY WARRANTY; without even the implied warranty of
//  * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
//  * General Public License for more details.
//  *
//  * You should have received a copy of the GNU General Public
//  * License along with this program; if not, write to the
//  * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
//  * Boston, MA 021110-1307, USA.
//  *
//  * If you can't comply with GPLv2, alternative licensing terms may be
//  * arranged. Please contact Fuel7, Inc. (http://fuel7.com/) for proprietary
//  * alternative licensing inquiries.
//  */
// #include <linux/delay.h>
// #include <linux/i2c.h>
// #include <linux/init.h>
// #include <linux/input.h>
// #include <linux/input/matrix_keypad.h>
// #include <linux/interrupt.h>
// #include <linux/module.h>
// #include <linux/of.h>
// #include <linux/property.h>
// #include <linux/slab.h>
// #include <linux/types.h>
// #include <linux/gpio/consumer.h>
// #include <linux/workqueue.h>

// /* TCA8418 hardware limits */
// #define TCA8418_MAX_ROWS 8
// #define TCA8418_MAX_COLS 10

// /* TCA8418 register offsets */
// #define REG_CFG            0x01
// #define REG_INT_STAT       0x02
// #define REG_KEY_LCK_EC     0x03
// #define REG_KEY_EVENT_A    0x04
// #define REG_KEY_EVENT_B    0x05
// #define REG_KEY_EVENT_C    0x06
// #define REG_KEY_EVENT_D    0x07
// #define REG_KEY_EVENT_E    0x08
// #define REG_KEY_EVENT_F    0x09
// #define REG_KEY_EVENT_G    0x0A
// #define REG_KEY_EVENT_H    0x0B
// #define REG_KEY_EVENT_I    0x0C
// #define REG_KEY_EVENT_J    0x0D
// #define REG_KP_LCK_TIMER   0x0E
// #define REG_UNLOCK1        0x0F
// #define REG_UNLOCK2        0x10
// #define REG_GPIO_INT_STAT1 0x11
// #define REG_GPIO_INT_STAT2 0x12
// #define REG_GPIO_INT_STAT3 0x13
// #define REG_GPIO_DAT_STAT1 0x14
// #define REG_GPIO_DAT_STAT2 0x15
// #define REG_GPIO_DAT_STAT3 0x16
// #define REG_GPIO_DAT_OUT1  0x17
// #define REG_GPIO_DAT_OUT2  0x18
// #define REG_GPIO_DAT_OUT3  0x19
// #define REG_GPIO_INT_EN1   0x1A
// #define REG_GPIO_INT_EN2   0x1B
// #define REG_GPIO_INT_EN3   0x1C
// #define REG_KP_GPIO1       0x1D
// #define REG_KP_GPIO2       0x1E
// #define REG_KP_GPIO3       0x1F
// #define REG_GPI_EM1        0x20
// #define REG_GPI_EM2        0x21
// #define REG_GPI_EM3        0x22
// #define REG_GPIO_DIR1      0x23
// #define REG_GPIO_DIR2      0x24
// #define REG_GPIO_DIR3      0x25
// #define REG_GPIO_INT_LVL1  0x26
// #define REG_GPIO_INT_LVL2  0x27
// #define REG_GPIO_INT_LVL3  0x28
// #define REG_DEBOUNCE_DIS1  0x29
// #define REG_DEBOUNCE_DIS2  0x2A
// #define REG_DEBOUNCE_DIS3  0x2B
// #define REG_GPIO_PULL1     0x2C
// #define REG_GPIO_PULL2     0x2D
// #define REG_GPIO_PULL3     0x2E

// /* TCA8418 bit definitions */
// #define CFG_AI           BIT(7)
// #define CFG_GPI_E_CFG    BIT(6)
// #define CFG_OVR_FLOW_M   BIT(5)
// #define CFG_INT_CFG      BIT(4)
// #define CFG_OVR_FLOW_IEN BIT(3)
// #define CFG_K_LCK_IEN    BIT(2)
// #define CFG_GPI_IEN      BIT(1)
// #define CFG_KE_IEN       BIT(0)

// #define INT_STAT_CAD_INT      BIT(4)
// #define INT_STAT_OVR_FLOW_INT BIT(3)
// #define INT_STAT_K_LCK_INT    BIT(2)
// #define INT_STAT_GPI_INT      BIT(1)
// #define INT_STAT_K_INT        BIT(0)

// /* TCA8418 register masks */
// #define KEY_LCK_EC_KEC  0x7
// #define KEY_EVENT_CODE  0x7f
// #define KEY_EVENT_VALUE 0x80

// struct tca8418_keypad {
//     struct i2c_client *client;
//     struct input_dev *input;
//     struct gpio_desc *tables_sel_gpio;
//     struct gpio_desc *capslock_gpio;
//     int capslock_status;
//     int map_base_index;
//     int switch_button_code;
//     unsigned int row_shift;
//     unsigned short *keycode;
//     int last_keycode;
//     struct timer_list report_timer;
//     spinlock_t report_lock;
    
//     /* 工作队列相关 */
//     struct workqueue_struct *workqueue;
//     struct work_struct irq_work;
//     struct delayed_work repeat_work;
// };

// /*
//  * Write a byte to the TCA8418
//  */
// static int tca8418_write_byte(struct tca8418_keypad *keypad_data, int reg, u8 val)
// {
//     int error;

//     error = i2c_smbus_write_byte_data(keypad_data->client, reg, val);
//     if (error < 0) {
//         dev_err(&keypad_data->client->dev, "%s failed, reg: %d, val: %d, error: %d\n", __func__, reg, val, error);
//         return error;
//     }

//     return 0;
// }

// /*
//  * Read a byte from the TCA8418
//  */
// static int tca8418_read_byte(struct tca8418_keypad *keypad_data, int reg, u8 *val)
// {
//     int error;

//     error = i2c_smbus_read_byte_data(keypad_data->client, reg);
//     if (error < 0) {
//         dev_err(&keypad_data->client->dev, "%s failed, reg: %d, error: %d\n", __func__, reg, error);
//         return error;
//     }

//     *val = (u8)error;
//     return 0;
// }

// /*
//  * 自动重复工作函数 - 报告状态2
//  */
// static void repeat_work_handler(struct work_struct *work)
// {
//     struct tca8418_keypad *keypad_data = container_of(work, struct tca8418_keypad, repeat_work.work);
//     unsigned short *keymap = keypad_data->input->keycode;
//     unsigned long flags;
//     int keycode;

//     // spin_lock_irqsave(&keypad_data->report_lock, flags);
//     keycode = keypad_data->last_keycode;
//     // spin_unlock_irqrestore(&keypad_data->report_lock, flags);

//     if (keycode >= 0) {
//         input_event(keypad_data->input, EV_MSC, MSC_SCAN, keycode);
//         input_report_key(keypad_data->input,
//                          keypad_data->map_base_index == 0 ? keymap[keycode] : keypad_data->keycode[keycode],
//                          2);  /* 状态2表示自动重复 */
//         input_sync(keypad_data->input);

//         /* 继续调度下一次重复 */
//         queue_delayed_work(keypad_data->workqueue, &keypad_data->repeat_work, msecs_to_jiffies(100));
//     }
// }

// static void replace_report_timer(struct timer_list *t)
// {
//     struct tca8418_keypad *keypad_data = from_timer(keypad_data, t, report_timer);
    
//     /* 启动延迟工作队列进行重复报告 */
//     queue_delayed_work(keypad_data->workqueue, &keypad_data->repeat_work, msecs_to_jiffies(100));
// }

// /*
//  * 键盘读取工作函数
//  */
// static void tca8418_read_keypad(struct tca8418_keypad *keypad_data)
// {
//     struct input_dev *input = keypad_data->input;
//     unsigned short *keymap  = input->keycode;
//     int error, col, row;
//     u8 reg, state, code;
//     unsigned long flags;

//     do {
//         error = tca8418_read_byte(keypad_data, REG_KEY_EVENT_A, &reg);
//         if (error < 0) {
//             dev_err(&keypad_data->client->dev, "unable to read REG_KEY_EVENT_A\n");
//             break;
//         }

//         /* Assume that key code 0 signifies empty FIFO */
//         if (reg <= 0) break;

//         state = reg & KEY_EVENT_VALUE;
//         code  = reg & KEY_EVENT_CODE;

//         row = code / TCA8418_MAX_COLS;
//         col = code % TCA8418_MAX_COLS;
//         row = (col) ? row : row - 1;
//         col = (col) ? col - 1 : TCA8418_MAX_COLS - 1;

//         code = MATRIX_SCAN_CODE(row, col, keypad_data->row_shift);

//         if ((keypad_data->switch_button_code != -1) && (code == keypad_data->switch_button_code) && (state == 0)) {
//             if (keypad_data->map_base_index) {
//                 keypad_data->map_base_index = 0;
//                 gpiod_set_value(keypad_data->tables_sel_gpio, 0);
//             } else {
//                 keypad_data->map_base_index = 1;
//                 gpiod_set_value(keypad_data->tables_sel_gpio, 1);
//             }
//             continue;
//         }

//         if ((code == 32) && (state == 0)) {
//             keypad_data->capslock_status = !keypad_data->capslock_status;
//             if (keypad_data->capslock_gpio) gpiod_set_value(keypad_data->capslock_gpio, keypad_data->capslock_status);
//         }

//         if ((state == 1) && (code != keypad_data->last_keycode)) {
//             // spin_lock_irqsave(&keypad_data->report_lock, flags);
//             keypad_data->last_keycode = code;
//             // spin_unlock_irqrestore(&keypad_data->report_lock, flags);
            
//             mod_timer(&keypad_data->report_timer, jiffies + msecs_to_jiffies(600));
//         }

//         if ((state == 0) && (code == keypad_data->last_keycode)) {
//             /* 取消重复报告 */
//             cancel_delayed_work_sync(&keypad_data->repeat_work);
//             del_timer(&keypad_data->report_timer);
            
//             // spin_lock_irqsave(&keypad_data->report_lock, flags);
//             keypad_data->last_keycode = -1;
//             // spin_unlock_irqrestore(&keypad_data->report_lock, flags);
//         }

//         // printk("code:%d status :%d switch_code:%d\n", code, state, keypad_data->switch_button_code);
//         input_event(input, EV_MSC, MSC_SCAN, code);
//         input_report_key(input, keypad_data->map_base_index == 0 ? keymap[code] : keypad_data->keycode[code], state);
//     } while (1);

//     input_sync(input);
// }

// /*
//  * IRQ工作队列处理函数
//  */
// static void tca8418_irq_work_handler(struct work_struct *work)
// {
//     struct tca8418_keypad *keypad_data = container_of(work, struct tca8418_keypad, irq_work);
//     u8 reg;
//     int error;

//     error = tca8418_read_byte(keypad_data, REG_INT_STAT, &reg);
//     if (error) {
//         dev_err(&keypad_data->client->dev, "unable to read REG_INT_STAT\n");
//         return;
//     }

//     if (!reg) return;

//     if (reg & INT_STAT_OVR_FLOW_INT) 
//         dev_warn(&keypad_data->client->dev, "overflow occurred\n");

//     if (reg & INT_STAT_K_INT) 
//         tca8418_read_keypad(keypad_data);

//     /* Clear all interrupts, even IRQs we didn't check (GPI, CAD, LCK) */
//     reg   = 0xff;
//     error = tca8418_write_byte(keypad_data, REG_INT_STAT, reg);
//     if (error) 
//         dev_err(&keypad_data->client->dev, "unable to clear REG_INT_STAT\n");
// }

// /*
//  * Threaded IRQ handler - 调度工作队列
//  */
// static irqreturn_t tca8418_irq_handler(int irq, void *dev_id)
// {
//     struct tca8418_keypad *keypad_data = dev_id;

//     /* 将实际工作放到工作队列中处理 */
//     queue_work(keypad_data->workqueue, &keypad_data->irq_work);

//     return IRQ_HANDLED;
// }

// /*
//  * Configure the TCA8418 for keypad operation
//  */
// static int tca8418_configure(struct tca8418_keypad *keypad_data, u32 rows, u32 cols)
// {
//     int reg, error = 0;

//     /* Assemble a mask for row and column registers */
//     reg = ~(~0 << rows);
//     reg += (~(~0 << cols)) << 8;

//     /* Set registers to keypad mode */
//     error |= tca8418_write_byte(keypad_data, REG_KP_GPIO1, reg);
//     error |= tca8418_write_byte(keypad_data, REG_KP_GPIO2, reg >> 8);
//     error |= tca8418_write_byte(keypad_data, REG_KP_GPIO3, reg >> 16);

//     /* Enable column debouncing */
//     error |= tca8418_write_byte(keypad_data, REG_DEBOUNCE_DIS1, reg);
//     error |= tca8418_write_byte(keypad_data, REG_DEBOUNCE_DIS2, reg >> 8);
//     error |= tca8418_write_byte(keypad_data, REG_DEBOUNCE_DIS3, reg >> 16);

//     if (error) return error;

//     tca8418_write_byte(keypad_data, REG_KP_LCK_TIMER, 0xA4);

//     error = tca8418_write_byte(keypad_data, REG_CFG, CFG_INT_CFG | CFG_OVR_FLOW_IEN | CFG_KE_IEN);

//     return error;
// }

// static int tca8418_keypad_probe(struct i2c_client *client)
// {
//     struct device *dev = &client->dev;
//     struct tca8418_keypad *keypad_data;
//     struct input_dev *input;
//     u32 rows = 0, cols = 0;
//     int error, row_shift;
//     u8 reg;

//     /* Check i2c driver capabilities */
//     if (!i2c_check_functionality(client->adapter, I2C_FUNC_SMBUS_BYTE)) {
//         dev_err(dev, "%s adapter not supported\n", dev_driver_string(&client->adapter->dev));
//         return -ENODEV;
//     }

//     error = matrix_keypad_parse_properties(dev, &rows, &cols);
//     if (error) return error;

//     if (!rows || rows > TCA8418_MAX_ROWS) {
//         dev_err(dev, "invalid rows\n");
//         return -EINVAL;
//     }

//     if (!cols || cols > TCA8418_MAX_COLS) {
//         dev_err(dev, "invalid columns\n");
//         return -EINVAL;
//     }

//     row_shift = get_count_order(cols);

//     /* Allocate memory for keypad_data and input device */
//     keypad_data = devm_kzalloc(dev, sizeof(*keypad_data), GFP_KERNEL);
//     if (!keypad_data) return -ENOMEM;

//     keypad_data->client    = client;
//     keypad_data->row_shift = row_shift;
//     keypad_data->last_keycode = -1;
    
//     /* 初始化自旋锁 */
//     // spin_lock_init(&keypad_data->report_lock);
    
//     /* 创建工作队列 */
//     keypad_data->workqueue = create_singlethread_workqueue("tca8418_wq");
//     if (!keypad_data->workqueue) {
//         dev_err(dev, "Failed to create workqueue\n");
//         return -ENOMEM;
//     }
    
//     /* 初始化工作项 */
//     INIT_WORK(&keypad_data->irq_work, tca8418_irq_work_handler);
//     INIT_DELAYED_WORK(&keypad_data->repeat_work, repeat_work_handler);
    
//     timer_setup(&keypad_data->report_timer, replace_report_timer, 0);

//     {
//         keypad_data->capslock_status = 0;
//         keypad_data->capslock_gpio   = devm_gpiod_get_optional(dev, "capslock", GPIOD_OUT_LOW);
//         if (keypad_data->capslock_gpio) 
//             gpiod_set_value(keypad_data->capslock_gpio, keypad_data->capslock_status);
//     }

//     {
//         keypad_data->map_base_index     = 0;
//         keypad_data->switch_button_code = -1;
//         device_property_read_u32(dev, "switch-button-code", &keypad_data->switch_button_code);
//         if (keypad_data->switch_button_code != -1) {
//             keypad_data->tables_sel_gpio = devm_gpiod_get_optional(dev, "tables-sel", GPIOD_OUT_LOW);
//             if (IS_ERR(keypad_data->tables_sel_gpio)) {
//                 error = PTR_ERR(keypad_data->tables_sel_gpio);
//                 goto err_destroy_wq;
//             }
//             gpiod_set_value(keypad_data->tables_sel_gpio, 0);
//         }
//     }

//     /* Read key lock register, if this fails assume device not present */
//     error = tca8418_read_byte(keypad_data, REG_KEY_LCK_EC, &reg);
//     if (error) {
//         error = -ENODEV;
//         goto err_destroy_wq;
//     }

//     /* Configure input device */
//     input = devm_input_allocate_device(dev);
//     if (!input) {
//         error = -ENOMEM;
//         goto err_destroy_wq;
//     }

//     keypad_data->input = input;

//     input->name       = client->name;
//     input->id.bustype = BUS_I2C;
//     input->id.vendor  = 0x0001;
//     input->id.product = 0x001;
//     input->id.version = 0x0001;

//     error = matrix_keypad_build_keymap(NULL, NULL, rows, cols, NULL, input);
//     if (error) {
//         dev_err(dev, "Failed to build keymap\n");
//         goto err_destroy_wq;
//     }

//     if (keypad_data->switch_button_code != -1) {
//         keypad_data->keycode        = input->keycode;
//         error                       = matrix_keypad_build_keymap(NULL, "linux,keymap1", rows, cols, NULL, input);
//         unsigned short *tmp_keycode = input->keycode;
//         input->keycode              = keypad_data->keycode;
//         keypad_data->keycode        = tmp_keycode;
//         if (error) {
//             dev_err(dev, "Failed to build keymap\n");
//             goto err_destroy_wq;
//         }
//     }

//     if (device_property_read_bool(dev, "keypad,autorepeat")) 
//         __set_bit(EV_REP, input->evbit);
    
//     input_set_capability(input, EV_MSC, MSC_SCAN);

//     error = devm_request_threaded_irq(dev, client->irq, NULL, tca8418_irq_handler, 
//                                       IRQF_SHARED | IRQF_ONESHOT, client->name, keypad_data);
//     if (error) {
//         dev_err(dev, "Unable to claim irq %d; error %d\n", client->irq, error);
//         goto err_destroy_wq;
//     }

//     /* Initialize the chip */
//     error = tca8418_configure(keypad_data, rows, cols);
//     if (error < 0) goto err_destroy_wq;

//     error = input_register_device(input);
//     if (error) {
//         dev_err(dev, "Unable to register input device, error: %d\n", error);
//         goto err_destroy_wq;
//     }

//     i2c_set_clientdata(client, keypad_data);

//     return 0;

// err_destroy_wq:
//     cancel_work_sync(&keypad_data->irq_work);
//     cancel_delayed_work_sync(&keypad_data->repeat_work);
//     destroy_workqueue(keypad_data->workqueue);
//     return error;
// }

// static void tca8418_keypad_remove(struct i2c_client *client)
// {
//     struct tca8418_keypad *keypad_data = i2c_get_clientdata(client);

//     if (keypad_data) {
//         del_timer_sync(&keypad_data->report_timer);
//         cancel_work_sync(&keypad_data->irq_work);
//         cancel_delayed_work_sync(&keypad_data->repeat_work);
//         destroy_workqueue(keypad_data->workqueue);
//     }
// }

// static const struct i2c_device_id tca8418_id[] = {{
//                                                       "m5stack_tca8418",
//                                                       8418,
//                                                   },
//                                                   {}};
// MODULE_DEVICE_TABLE(i2c, tca8418_id);

// static const struct of_device_id tca8418_dt_ids[] = {{
//                                                          .compatible = "m5stack,tca8418",
//                                                      },
//                                                      {
//                                                          .compatible = "m5stack,tca8418_m5stack",
//                                                      },
//                                                      {}};
// MODULE_DEVICE_TABLE(of, tca8418_dt_ids);

// static struct i2c_driver tca8418_keypad_driver = {
//     .driver =
//         {
//             .name           = "m5stack_tca8418_keypad",
//             .of_match_table = tca8418_dt_ids,
//         },
//     .probe    = tca8418_keypad_probe,
//     .remove   = tca8418_keypad_remove,
//     .id_table = tca8418_id,
// };

// module_i2c_driver(tca8418_keypad_driver);

// MODULE_AUTHOR("Kyle Manna <kyle.manna@fuel7.com>");
// MODULE_DESCRIPTION("Keypad driver for TCA8418");
// MODULE_LICENSE("GPL");



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
#define TCA8418_MAX_ROWS 8
#define TCA8418_MAX_COLS 10

/* TCA8418 register offsets */
#define REG_CFG            0x01
#define REG_INT_STAT       0x02
#define REG_KEY_LCK_EC     0x03
#define REG_KEY_EVENT_A    0x04
#define REG_KEY_EVENT_B    0x05
#define REG_KEY_EVENT_C    0x06
#define REG_KEY_EVENT_D    0x07
#define REG_KEY_EVENT_E    0x08
#define REG_KEY_EVENT_F    0x09
#define REG_KEY_EVENT_G    0x0A
#define REG_KEY_EVENT_H    0x0B
#define REG_KEY_EVENT_I    0x0C
#define REG_KEY_EVENT_J    0x0D
#define REG_KP_LCK_TIMER   0x0E
#define REG_UNLOCK1        0x0F
#define REG_UNLOCK2        0x10
#define REG_GPIO_INT_STAT1 0x11
#define REG_GPIO_INT_STAT2 0x12
#define REG_GPIO_INT_STAT3 0x13
#define REG_GPIO_DAT_STAT1 0x14
#define REG_GPIO_DAT_STAT2 0x15
#define REG_GPIO_DAT_STAT3 0x16
#define REG_GPIO_DAT_OUT1  0x17
#define REG_GPIO_DAT_OUT2  0x18
#define REG_GPIO_DAT_OUT3  0x19
#define REG_GPIO_INT_EN1   0x1A
#define REG_GPIO_INT_EN2   0x1B
#define REG_GPIO_INT_EN3   0x1C
#define REG_KP_GPIO1       0x1D
#define REG_KP_GPIO2       0x1E
#define REG_KP_GPIO3       0x1F
#define REG_GPI_EM1        0x20
#define REG_GPI_EM2        0x21
#define REG_GPI_EM3        0x22
#define REG_GPIO_DIR1      0x23
#define REG_GPIO_DIR2      0x24
#define REG_GPIO_DIR3      0x25
#define REG_GPIO_INT_LVL1  0x26
#define REG_GPIO_INT_LVL2  0x27
#define REG_GPIO_INT_LVL3  0x28
#define REG_DEBOUNCE_DIS1  0x29
#define REG_DEBOUNCE_DIS2  0x2A
#define REG_DEBOUNCE_DIS3  0x2B
#define REG_GPIO_PULL1     0x2C
#define REG_GPIO_PULL2     0x2D
#define REG_GPIO_PULL3     0x2E

/* TCA8418 bit definitions */
#define CFG_AI           BIT(7)
#define CFG_GPI_E_CFG    BIT(6)
#define CFG_OVR_FLOW_M   BIT(5)
#define CFG_INT_CFG      BIT(4)
#define CFG_OVR_FLOW_IEN BIT(3)
#define CFG_K_LCK_IEN    BIT(2)
#define CFG_GPI_IEN      BIT(1)
#define CFG_KE_IEN       BIT(0)

#define INT_STAT_CAD_INT      BIT(4)
#define INT_STAT_OVR_FLOW_INT BIT(3)
#define INT_STAT_K_LCK_INT    BIT(2)
#define INT_STAT_GPI_INT      BIT(1)
#define INT_STAT_K_INT        BIT(0)

/* TCA8418 register masks */
#define KEY_LCK_EC_KEC  0x7
#define KEY_EVENT_CODE  0x7f
#define KEY_EVENT_VALUE 0x80

struct tca8418_keypad {
    struct i2c_client *client;
    struct input_dev *input;
    struct gpio_desc *tables_sel_gpio;
    struct gpio_desc *capslock_gpio;
    int capslock_status;
    int map_base_index;
    int switch_button_code;
    unsigned int row_shift;
    unsigned short *keycode;

    int last_keycode;
    struct timer_list report_timer;
    spinlock_t report_lock;
};

/*
 * Write a byte to the TCA8418
 */
static int tca8418_write_byte(struct tca8418_keypad *keypad_data, int reg, u8 val)
{
    int error;

    error = i2c_smbus_write_byte_data(keypad_data->client, reg, val);
    if (error < 0) {
        dev_err(&keypad_data->client->dev, "%s failed, reg: %d, val: %d, error: %d\n", __func__, reg, val, error);
        return error;
    }

    return 0;
}

/*
 * Read a byte from the TCA8418
 */
static int tca8418_read_byte(struct tca8418_keypad *keypad_data, int reg, u8 *val)
{
    int error;

    error = i2c_smbus_read_byte_data(keypad_data->client, reg);
    if (error < 0) {
        dev_err(&keypad_data->client->dev, "%s failed, reg: %d, error: %d\n", __func__, reg, error);
        return error;
    }

    *val = (u8)error;

    return 0;
}

// static void replace_report_timer(struct timer_list *t)
// {
//     struct tca8418_keypad *keypad_data = from_timer(tca8418_keypad, t, report_timer);
//     unsigned short *keymap             = keypad_data->input->keycode;
//     input_event(keypad_data->input, EV_MSC, MSC_SCAN, keypad_data->last_keycode);
//     input_report_key(keypad_data->input,
//                      keypad_data->map_base_index == 0 ? keymap[keypad_data->last_keycode]
//                                                       : keypad_data->keycode[keypad_data->last_keycode],
//                      2);
//     input_sync(keypad_data->input);
//     mod_timer(&keypad_data->report_timer, jiffies + msecs_to_jiffies(100));
// }

static void tca8418_read_keypad(struct tca8418_keypad *keypad_data)
{
    struct input_dev *input = keypad_data->input;
    unsigned short *keymap  = input->keycode;
    int error, col, row;
    u8 reg, state, code;
    do {
        error = tca8418_read_byte(keypad_data, REG_KEY_EVENT_A, &reg);
        if (error < 0) {
            dev_err(&keypad_data->client->dev, "unable to read REG_KEY_EVENT_A\n");
            break;
        }

        /* Assume that key code 0 signifies empty FIFO */
        if (reg <= 0) break;

        state = reg & KEY_EVENT_VALUE;
        code  = reg & KEY_EVENT_CODE;

        row = code / TCA8418_MAX_COLS;
        col = code % TCA8418_MAX_COLS;

        row = (col) ? row : row - 1;
        col = (col) ? col - 1 : TCA8418_MAX_COLS - 1;

        code = MATRIX_SCAN_CODE(row, col, keypad_data->row_shift);
        if ((keypad_data->switch_button_code != -1) && (code == keypad_data->switch_button_code) && (state == 0)) {
            if (keypad_data->map_base_index) {
                keypad_data->map_base_index = 0;
                gpiod_set_value(keypad_data->tables_sel_gpio, 0);
            } else {
                keypad_data->map_base_index = 1;
                gpiod_set_value(keypad_data->tables_sel_gpio, 1);
            }
            continue;
        }
		int key_val = keypad_data->map_base_index == 0 ? keymap[code] : keypad_data->keycode[code];
        if ((key_val == 66) && (state == 0)) {
            keypad_data->capslock_status = !keypad_data->capslock_status;
            if (keypad_data->capslock_gpio) gpiod_set_value(keypad_data->capslock_gpio, keypad_data->capslock_status);
        }

        // if ((state == 1) && (code != keypad_data->last_keycode)) {
        //     keypad_data->last_keycode = code;
        //     mod_timer(&keypad_data->report_timer, jiffies + msecs_to_jiffies(600));
        // }
        // if ((state == 0) && (code == keypad_data->last_keycode)) {
        //     del_timer(&keypad_data->report_timer);
        //     keypad_data->last_keycode = -1;
        // }

        // printk("code:%d status :%d switch_code:%d\n", code, state, keypad_data->switch_button_code);
        input_event(input, EV_MSC, MSC_SCAN, code);
        input_report_key(input, key_val, state);

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
        dev_err(&keypad_data->client->dev, "unable to read REG_INT_STAT\n");
        return IRQ_NONE;
    }

    if (!reg) return IRQ_NONE;

    if (reg & INT_STAT_OVR_FLOW_INT) dev_warn(&keypad_data->client->dev, "overflow occurred\n");

    if (reg & INT_STAT_K_INT) tca8418_read_keypad(keypad_data);

    /* Clear all interrupts, even IRQs we didn't check (GPI, CAD, LCK) */
    reg   = 0xff;
    error = tca8418_write_byte(keypad_data, REG_INT_STAT, reg);
    if (error) dev_err(&keypad_data->client->dev, "unable to clear REG_INT_STAT\n");

    return IRQ_HANDLED;
}

/*
 * Configure the TCA8418 for keypad operation
 */
static int tca8418_configure(struct tca8418_keypad *keypad_data, u32 rows, u32 cols)
{
    int reg, error = 0;

    /* Assemble a mask for row and column registers */
    reg = ~(~0 << rows);
    reg += (~(~0 << cols)) << 8;

    /* Set registers to keypad mode */
    error |= tca8418_write_byte(keypad_data, REG_KP_GPIO1, reg);
    error |= tca8418_write_byte(keypad_data, REG_KP_GPIO2, reg >> 8);
    error |= tca8418_write_byte(keypad_data, REG_KP_GPIO3, reg >> 16);

    /* Enable column debouncing */
    error |= tca8418_write_byte(keypad_data, REG_DEBOUNCE_DIS1, reg);
    error |= tca8418_write_byte(keypad_data, REG_DEBOUNCE_DIS2, reg >> 8);
    error |= tca8418_write_byte(keypad_data, REG_DEBOUNCE_DIS3, reg >> 16);

    if (error) return error;

    tca8418_write_byte(keypad_data, REG_KP_LCK_TIMER, 0xA4);

    error = tca8418_write_byte(keypad_data, REG_CFG, CFG_INT_CFG | CFG_OVR_FLOW_IEN | CFG_KE_IEN);

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
        dev_err(dev, "%s adapter not supported\n", dev_driver_string(&client->adapter->dev));
        return -ENODEV;
    }

    error = matrix_keypad_parse_properties(dev, &rows, &cols);
    if (error) return error;

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
    if (!keypad_data) return -ENOMEM;

    keypad_data->client    = client;
    keypad_data->row_shift = row_shift;

    timer_setup(&keypad_data->report_timer, replace_report_timer, 0);

    {
        keypad_data->capslock_status = 0;
        keypad_data->capslock_gpio   = devm_gpiod_get_optional(dev, "capslock", GPIOD_OUT_LOW);
        if (keypad_data->capslock_gpio) gpiod_set_value(keypad_data->capslock_gpio, keypad_data->capslock_status);
    }

    {
        keypad_data->map_base_index     = 0;
        keypad_data->switch_button_code = -1;
        device_property_read_u32(dev, "switch-button-code", &keypad_data->switch_button_code);
        if (keypad_data->switch_button_code != -1) {
            keypad_data->tables_sel_gpio = devm_gpiod_get_optional(dev, "tables-sel", GPIOD_OUT_LOW);
            if (IS_ERR(keypad_data->tables_sel_gpio)) return PTR_ERR(keypad_data->tables_sel_gpio);
            gpiod_set_value(keypad_data->tables_sel_gpio, 0);
        }
    }

    /* Read key lock register, if this fails assume device not present */
    error = tca8418_read_byte(keypad_data, REG_KEY_LCK_EC, &reg);
    if (error) return -ENODEV;

    /* Configure input device */
    input = devm_input_allocate_device(dev);
    if (!input) return -ENOMEM;

    keypad_data->input = input;

    input->name       = client->name;
    input->id.bustype = BUS_I2C;
    input->id.vendor  = 0x0001;
    input->id.product = 0x001;
    input->id.version = 0x0001;

    error = matrix_keypad_build_keymap(NULL, NULL, rows, cols, NULL, input);
    if (error) {
        dev_err(dev, "Failed to build keymap\n");
        return error;
    }
    if (keypad_data->switch_button_code != -1) {
        keypad_data->keycode        = input->keycode;
        error                       = matrix_keypad_build_keymap(NULL, "linux,keymap1", rows, cols, NULL, input);
        unsigned short *tmp_keycode = input->keycode;
        input->keycode              = keypad_data->keycode;
        keypad_data->keycode        = tmp_keycode;
        if (error) {
            dev_err(dev, "Failed to build keymap\n");
            return error;
        }
    }

    if (device_property_read_bool(dev, "keypad,autorepeat")) __set_bit(EV_REP, input->evbit);

    input_set_capability(input, EV_MSC, MSC_SCAN);

    error = devm_request_threaded_irq(dev, client->irq, NULL, tca8418_irq_handler, IRQF_SHARED | IRQF_ONESHOT,
                                      client->name, keypad_data);
    if (error) {
        dev_err(dev, "Unable to claim irq %d; error %d\n", client->irq, error);
        return error;
    }

    /* Initialize the chip */
    error = tca8418_configure(keypad_data, rows, cols);
    if (error < 0) return error;

    error = input_register_device(input);
    if (error) {
        dev_err(dev, "Unable to register input device, error: %d\n", error);
        return error;
    }

    return 0;
}

static const struct i2c_device_id tca8418_id[] = {{
                                                      "m5stack_tca8418",
                                                      8418,
                                                  },
                                                  {}};
MODULE_DEVICE_TABLE(i2c, tca8418_id);

static const struct of_device_id tca8418_dt_ids[] = {{
                                                         .compatible = "m5stack,tca8418",
                                                     },
                                                     {
                                                         .compatible = "m5stack,tca8418_m5stack",
                                                     },
                                                     {}};
MODULE_DEVICE_TABLE(of, tca8418_dt_ids);

static struct i2c_driver tca8418_keypad_driver = {
    .driver =
        {
            .name           = "m5stack_tca8418_keypad",
            .of_match_table = tca8418_dt_ids,
        },
    .probe    = tca8418_keypad_probe,
    .id_table = tca8418_id,
};
module_i2c_driver(tca8418_keypad_driver);

MODULE_AUTHOR("Kyle Manna <kyle.manna@fuel7.com>");
MODULE_DESCRIPTION("Keypad driver for TCA8418");
MODULE_LICENSE("GPL");
