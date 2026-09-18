#ifndef __LINUX_MFD_M5IO_HUB_H
#define __LINUX_MFD_M5IO_HUB_H

#include <linux/regmap.h>
#include <linux/mutex.h>
#include <linux/kfifo.h>
#include <linux/serial_core.h>

#define M5IO_HUB_NGPIO          17

#define M5IO_HUB_CHN_IIC1       1
#define M5IO_HUB_CHN_IIC2       2
#define M5IO_HUB_CHN_SPI        3
#define M5IO_HUB_CHN_UART1      4
#define M5IO_HUB_CHN_UART2      5
#define M5IO_HUB_CHN_MAX        M5IO_HUB_CHN_UART2

#define M5IO_HUB_FUNC_I2C       2
#define M5IO_HUB_TOKEN_PAYLOAD_MAX 15
#define M5IO_HUB_DATA_PAYLOAD_MAX 255

/* Synchronous TOKEN request. Optional I2C DATA uses PID=0, CH=bus.
 * reply/status are the terminal TOKEN payload/ARG, even on firmware errors.
 * data_rx is published only after both DATA and a successful TOKEN arrive.
 */
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


struct m5io_hub_uart_port {
    struct uart_port port;
    DECLARE_KFIFO(rx_fifo, u8, 256);   /* 256字节环形缓冲区 */
};


struct m5io_hub {
    struct device *dev;
    struct spi_device *spi;
    struct regmap *regmap;
    struct mutex lock;          /* 保护寄存器读写 */

    /* gpio子模块会用到这个irq相关字段 */
    int irq;                    /* 芯片上行中断号（对应spi->irq）*/
};

typedef void (*m5io_hub_gpio_irq_handler_t)(void *data, unsigned int pin);
typedef void (*m5io_hub_chn_data_handler_t)(void *data, const u8 *buf,
                                            unsigned int len);

int m5io_hub_pinMode(struct m5io_hub *hub, unsigned int pin, int mode);
int m5io_hub_exec(struct m5io_hub *hub, struct m5io_hub_request *request);
int m5io_hub_rpc_transfer(struct m5io_hub *hub);
int m5io_hub_SendChnData(struct m5io_hub *hub, unsigned int chn,
                         const u8 *data, unsigned int len);
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
