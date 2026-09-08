#ifndef __LINUX_MFD_M5IO_HUB_H
#define __LINUX_MFD_M5IO_HUB_H

#include <linux/regmap.h>
#include <linux/mutex.h>
#include <linux/kfifo.h>

#define M5IO_HUB_NGPIO          16

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
    struct mutex lock;          /* 保护寄存器读写 */

    /* gpio子模块会用到这个irq相关字段 */
    int irq;                    /* 芯片上行中断号（对应spi->irq）*/
};

#endif