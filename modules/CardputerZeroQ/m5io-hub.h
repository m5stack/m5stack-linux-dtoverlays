#ifndef __LINUX_MFD_M5IO_HUB_H
#define __LINUX_MFD_M5IO_HUB_H

#include <linux/regmap.h>
#include <linux/mutex.h>
#include <linux/kfifo.h>
#include <linux/serial_core.h>
#include <linux/printk.h>

/*
 * M5IO-HUB detailed debug logging switch.
 *
 * 0 (default): all per-transfer trace logging is compiled out, so the
 *              drivers stay quiet in dmesg during normal operation.
 * 1:           verbose trace logging, identical to the previous behaviour.
 *
 * Override at build time with: make M5IO_HUB_DEBUG=1
 */
#ifndef M5IO_HUB_DEBUG
#define M5IO_HUB_DEBUG 0
#endif

#if M5IO_HUB_DEBUG
#define m5io_dbg(fmt, ...) pr_info(fmt, ##__VA_ARGS__)
#else
#define m5io_dbg(fmt, ...) no_printk(fmt, ##__VA_ARGS__)
#endif

#define M5IO_HUB_NGPIO          21

#define M5IO_HUB_CHN_IIC1       1
#define M5IO_HUB_CHN_IIC2       2
#define M5IO_HUB_CHN_SPI        3
#define M5IO_HUB_CHN_UART1      4
#define M5IO_HUB_CHN_UART2      5
#define M5IO_HUB_CHN_IR         6
/*
 * MCU -> SoC diagnostic log stream.  Uplink only: the host never sends DATA
 * on this channel.  The firmware writes plain text here from any main-loop
 * context; the driver exposes it as /proc/m5io-hub-log.
 */
#define M5IO_HUB_CHN_LOG        7
#define M5IO_HUB_CHN_MAX        M5IO_HUB_CHN_LOG

#define M5IO_HUB_FUNC_I2C       2
#define M5IO_HUB_FUNC_SPI       3
#define M5IO_HUB_SPI_MAX_LEN    512
/* Even DATA chunks also work with the MCU's 16-bit SPI mode. */
#define M5IO_HUB_SPI_CHUNK_LEN  254
#define M5IO_HUB_FUNC_UART      4
#define M5IO_HUB_FUNC_BACKLIGHT 5
#define M5IO_HUB_FUNC_IR        6
#define M5IO_HUB_FUNC_MAX       M5IO_HUB_FUNC_IR
#define M5IO_HUB_TOKEN_PAYLOAD_MAX 15
#define M5IO_HUB_DATA_PAYLOAD_MAX 255

/* Synchronous v2 TOKEN request. Four independent immutable slots retain
 * requests through physical completion; a full/quarantined pool returns BUSY.
 * status is the terminal response ARG, and reply is its payload, including
 * firmware errors. Legacy DATA fields are retained for ABI layout only and
 * must be empty: business DATA uses the channel APIs. */
struct m5io_hub_request {
    u8 func;
    u8 arg;
    const u8 *payload;
    unsigned int payload_len;
    u8 status;
    u8 reply[M5IO_HUB_TOKEN_PAYLOAD_MAX];
    unsigned int reply_len;
    u8 data_chn;
    const u8 *data_tx;
    u8 *data_rx;
    unsigned int data_len;
};

/* ---- 寄存器映射（示例，需要根据实际协议手册确定）---- */
#define M5IO_HUB_REG_GPIO_DIR      0x00  /* 方向寄存器 */
#define M5IO_HUB_REG_GPIO_OUT      0x01  /* 输出电平寄存器 */
#define M5IO_HUB_REG_GPIO_IN       0x02  /* 输入电平寄存器 */
#define M5IO_HUB_REG_IRQ_STATUS    0x03  /* 中断状态寄存器（哪个pin触发了）*/
#define M5IO_HUB_REG_IRQ_MASK      0x04  /* 中断屏蔽寄存器 */
#define M5IO_HUB_REG_IRQ_TYPE_R    0x05  /* 上升沿触发使能 */
#define M5IO_HUB_REG_IRQ_TYPE_F    0x06  /* 下降沿触发使能 */


struct m5io_hub {
    struct device *dev;
    struct spi_device *spi;
    struct regmap *regmap;
    struct mutex lock;          /* 保护寄存器读写 */

    /* gpio子模块会用到这个irq相关字段 */
    int irq;                    /* 芯片上行中断号（对应spi->irq）*/
};

typedef void (*m5io_hub_gpio_irq_handler_t)(void *data, unsigned int pin);
/*
 * DATA handlers run in process context and may sleep.  They must not call
 * m5io_hub_unregister_chn_data_handler() from a DATA callback: that API
 * waits synchronously for in-flight callbacks to finish.
 */
typedef void (*m5io_hub_chn_data_handler_t)(void *data, const u8 *buf,
                                            unsigned int len);

int m5io_hub_pinMode(struct m5io_hub *hub, unsigned int pin, int mode);
int m5io_hub_exec(struct m5io_hub *hub, struct m5io_hub_request *request);
int m5io_hub_hello(struct m5io_hub *hub, u8 payload[15]);
int m5io_hub_mcu_reset(struct m5io_hub *hub);
int m5io_hub_data_status(struct m5io_hub *hub, u8 payload[15]);
/* Atomic-context safe: includes DATA still awaiting the MCU's window ACK. */
bool m5io_hub_data_pending(struct m5io_hub *hub, unsigned int chn);
/* Abort a broken DATA exchange on one channel and release host-side window
 * and de-duplication state after a protocol-specific flush. */
void m5io_hub_data_abort_channel(struct m5io_hub *hub, unsigned int chn);
/* FUNC=2/ARG=7; resets one I2C controller and restores last config. */
int m5io_hub_i2c_reset(struct m5io_hub *hub, unsigned int bus);
int m5io_hub_rpc_transfer(struct m5io_hub *hub);
int m5io_hub_SendChnData(struct m5io_hub *hub, unsigned int chn,
                         const u8 *data, unsigned int len);
/* Protocol DATA transport helpers.  DATA is independent of FUNC TOKENs and
 * uses the direction-wide PID window.  WINDOW_ACK carries CH/PID/SEQ/status;
 * for bidirectional transfers the synchronous form also copies the matching
 * DATA payload into @rx. */
int m5io_hub_send_data_async(struct m5io_hub *hub, unsigned int chn,
                             const u8 *data, unsigned int len);
int m5io_hub_send_data_sync(struct m5io_hub *hub, unsigned int chn,
                            const u8 *tx, u8 *rx, unsigned int len);
int m5io_hub_send_spi_sync(struct m5io_hub *hub, const u8 *tx, u8 *rx,
                           unsigned int len, unsigned int word_bits);
/* FUNC=3/ARG=8; discard the MCU's pending SPI exchange and release the
 * corresponding host-side DATA window state. */
int m5io_hub_spi_flush(struct m5io_hub *hub);
int m5io_hub_i2c_data_sync(struct m5io_hub *hub, unsigned int chn,
                           const u8 *tx, unsigned int tx_len,
                           u8 *rx, unsigned int rx_cap,
                           unsigned int *rx_len);
int m5io_hub_ir_config(struct m5io_hub *hub, const u8 payload[15],
                       u8 actual[15]);
int m5io_hub_ir_receive(struct m5io_hub *hub, bool enable);
int m5io_hub_ir_reset(struct m5io_hub *hub);
int m5io_hub_ir_disable(struct m5io_hub *hub);
int m5io_hub_ir_send(struct m5io_hub *hub, const u8 *data, unsigned int len);
int m5io_hub_ir_register_rx(struct m5io_hub *hub,
                             m5io_hub_chn_data_handler_t handler, void *data);
void m5io_hub_ir_unregister_rx(struct m5io_hub *hub,
                               m5io_hub_chn_data_handler_t handler, void *data);
int m5io_hub_register_chn_data_handler(
    struct m5io_hub *hub, unsigned int chn,
    m5io_hub_chn_data_handler_t handler, void *data);
void m5io_hub_unregister_chn_data_handler(
    struct m5io_hub *hub, unsigned int chn,
    m5io_hub_chn_data_handler_t handler, void *data);
int m5io_hub_digitalWrite(struct m5io_hub *hub, unsigned int pin, int value);
int m5io_hub_digitalRead(struct m5io_hub *hub, unsigned int pin);
int m5io_hub_attachInterrupt(struct m5io_hub *hub, unsigned int pin,
                             unsigned int type);
int m5io_hub_detachInterrupt(struct m5io_hub *hub, unsigned int pin);
int m5io_hub_register_gpio_irq_handler(
    struct m5io_hub *hub, m5io_hub_gpio_irq_handler_t handler, void *data);
void m5io_hub_unregister_gpio_irq_handler(
    struct m5io_hub *hub, m5io_hub_gpio_irq_handler_t handler, void *data);


#endif
