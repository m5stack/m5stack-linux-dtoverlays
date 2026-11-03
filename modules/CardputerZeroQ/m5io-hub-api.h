#ifndef __LINUX_M5IO_HUB_API_H
#define __LINUX_M5IO_HUB_API_H

#include <linux/regmap.h>
#include <linux/mutex.h>
#include <linux/kfifo.h>
#include <linux/serial_core.h>
#include <linux/printk.h>
#include <linux/i2c.h>
#include <linux/spi/spi.h>

/* 不透明句柄与本段原型用到的外部类型的前置声明。
 * 缺了它们，"struct m5io_hub *" 会变成只属于参数表的隐藏 tag，调用方传进来的
 * 同名结构体会被判成不兼容类型；struct i2c_msg / struct spi_message 同理。 */
struct m5io_hub;
struct i2c_msg;
struct spi_message;

/* =====================================================================
 * 协议简述（m5io-hub v2）
 * ---------------------------------------------------------------------
 * 链路：SoC(SPI master) <-> MCU(STM32G070)。主机拉 CS 收发，MCU 用 HOST_READY
 * 中断提示有待读数据。帧分两类：TOKEN（短控制）与 DATA（长数据）。
 * 所有多字节字段小端；每帧末尾统一 1 B CRC-8/ATM（poly 0x07，init 0x00）。
 *
 * 1) TOKEN 帧：按 FUNC + ARG 寻址，载荷 ≤ 15 B
 *
 *      字节0: TYPE(bit0)=0 | P(bit1) | TID(bits3:2) | FUNC(bits7:4)
 *      字节1: LEN(低4位)    | ARG(高4位)
 *      字节2..: payload[LEN]
 *      末1字节: CRC-8/ATM（poly 0x07, init 0x00）
 *
 *    请求：FUNC=外设分区，ARG=子命令，payload=子命令参数
 *    应答：TYPE/P 不变，TID 与 FUNC 原样回填，ARG=status，payload=应答数据
 *    status：0=OK 1=INPROGRESS 2=EOPNOTSUPP 3=EINVAL 4=BUSY 7=EIO，其余=EPROTO
 *    TID：0..3 四个并发槽；WINDOW_ACK 例外，固定 TID=0 且不占槽。
 *
 * 2) DATA 帧：按 CHN 通道寻址，载荷 1..255 B
 *
 *      字节0: TYPE(bit0)=1 | PID(bits2:1) | CHN(bits7:3)
 *      字节1: STA(bits7:6)|SEQ(bits5:0)
 *      字节2: LEN
 *      字节3..: payload[LEN]
 *      末1字节: CRC-8/ATM（poly 0x07, init 0x00）
 *
 *    每个方向、每个通道同时只有一个在飞窗口；PID 0..3 由发送方分配。
 *
 *    字节1 拆为两段：
 *      SEQ(bits5:0)  通道内独立序号，每个通道各自 0..63 循环递增，
 *                    通道之间互不相关；接收端按 SEQ 顺序拼接分片。
 *      STA(bits7:6) 本帧在同一条消息中的位置：
 *                    0 = 传输中（传输包）
 *                    1 = 引导帧（表示从机发送区还有多少个数据包，供soc的spi收发状态机使用）
 *                    2 = 传输完成（最后一个包，单包消息也用它）
 *                    3 = 保留（原“开始且结束/单包”标志已删除）
 *
 *    帧边界上的 0xff 是上半空位填充，会跳过；payload/CRC 内的 0xff 按普通数据处理。
 *
 *    每收到一帧 MCU->SoC 的 DATA，主机必须回 WINDOW_ACK，否则 MCU 的 PID
 *    永久泄漏（4 个漏完即停发）：
 *      TOKEN: FUNC=M5IO_HUB_PROTOCOL_FUN | ARG=M5IO_HUB_PROTOCOL_dataAck | LEN=6
 *      payload[6]={u8 chn, u8 pid, u16 op_id(LE), u8 kind, u8 status}
 *      kind: 0=WINDOW 1=COMPLETE；status: 0=READY 1=BUSY 2=UNSUPPORTED 3=INVALID 4=FULL 7=FAILED
 *
 * 3) 外设分区（FUNC 与 DATA 通道）
 *
 *      FUNC=M5IO_HUB_PROTOCOL_FUN  无 DATA 通道（握手/复位/窗口确认/状态查询）
 *      FUNC=M5IO_HUB_GPIO_FUN      无 DATA 通道
 *      FUNC=M5IO_HUB_IIC_FUN       DATA CH=M5IO_HUB_I2C_CH1/CH2
 *      FUNC=M5IO_HUB_SPI_FUN       DATA CH=M5IO_HUB_SPI_CH1..CH4
 *      FUNC=M5IO_HUB_UART_FUN      DATA CH=M5IO_HUB_UART_CH1/CH2
 *      FUNC=M5IO_HUB_PWM_FUN       无 DATA 通道
 *      FUNC=M5IO_HUB_IR_FUN        DATA CH=M5IO_HUB_IR_CH
 *      CH=M5IO_HUB_CHN_LOG         MCU 日志，仅上行
 * ===================================================================== */

/* DATA 帧字节 1 的位域：STA(bits7:6) | SEQ(bits5:0)。
 * 普通 DATA 帧的 SEQ 是通道内独立序号，每通道 0..63 循环递增，通道之间互不相关；
 * 从机引导帧（STA=1）例外：它使用独立的引导帧序号，与通道号、通道 DATA 的 SEQ
 * 都无关，详见下方从机引导帧定义。
 * STA 标记本帧在同一条消息中的位置：0=传输包，1=从机引导帧，2=传输完成（末包）。
 * 不再有“首包”标记：多帧消息的首个数据帧就是 0；也不再区分单包，单包消息
 * 直接就是 STA=2。3 保留（原“开始且结束/单包”标志已删除）。
 * 组帧：byte1 = (sta << M5IO_HUB_DATA_STA_SHIFT) | (seq & M5IO_HUB_DATA_SEQ_MASK)
 */
#define M5IO_HUB_DATA_SEQ_MASK   0x3fU
#define M5IO_HUB_DATA_STA_MASK   0xc0U
#define M5IO_HUB_DATA_STA_SHIFT  6U

enum {
    M5IO_HUB_DATA_STA_MID       = 0, /* 传输中（传输包） */
    M5IO_HUB_DATA_STA_GUIDE     = 1, /* 从机引导帧，见下方定义 */
    M5IO_HUB_DATA_STA_END       = 2  /* 传输完成（最后一个包，单包消息也是 2） */
    /* 3 保留：原“开始且结束（单包）”标志已删除 */
};

/* 从机引导帧（slave guide frame）：MCU -> SoC 方向的一种 DATA 帧，STA=1。
 *
 * 用途：MCU 把待发数据缓存到发送队列；SoC 拉 CS 后，MCU 在规定时间内于发送
 * 缓冲区头部创建一个引导帧，然后把发送缓冲区切换到 SPI 的 DMA 发送。引导帧
 * 告诉 SoC 当前发送区里有多少个待发的数据包，SoC 的 SPI 收发状态机用“已经
 * 收到的数据包数”和这个计数比较，判断后面还需要接收多少个数据包。
 *
 * 帧格式（普通 DATA 帧，LEN 占位 2 字节，没有用户 payload）：
 *   - STA = 1；
 *   - CHN：与后续数据帧相同的 DATA 通道号；
 *   - SEQ：使用独立的引导帧序号（0..63 循环递增），与通道号、该通道 DATA
 *     帧的 SEQ 都无关，也不占用通道 DATA 的序号；SoC 用它是否重复来判断
 *     是否绕回了 MCU 循环缓冲区的头部；
 *   - LEN = 2（M5IO_HUB_DATA_GUIDE_LEN）；LEN 后紧跟 2 字节
 *     u16 count(LE)：当前发送区中待发 DATA 包的数量，不含引导帧自身；
 *     这 2 字节是状态信息，不是用户数据 payload；
 *   - 和其它 MCU->SoC DATA 帧一样必须回 WINDOW_ACK，否则会占满 MCU 的
 *     PID 窗口。
 *
 * 引导帧之后的数据包按普通 DATA 规则发送：除最后一包外 STA=0，最后一包
 * STA=2（单包消息也用 STA=2，不再有单独的“单包”标志）。
 *
 * 环形发送缓冲：MCU 的发送区是循环队列，引导帧写在当前队头。因为引导帧
 * SEQ 是独立计数的，SoC 如果收到一个 SEQ 与之前收到过的引导帧重复的引导帧，
 * 就说明已经绕回 MCU 循环缓冲区的头部，忽略该引导帧即可（不要把它当成新的
 * 计数）。
 *
 * SoC -> MCU 方向没有引导帧：请求长度由 TOKEN 的 len 字段决定；该方向多帧
 * 消息没有首包标记，末帧用 STA=2，单包消息也用 STA=2。
 */
#define M5IO_HUB_DATA_GUIDE_LEN 2U

typedef void (*m5io_hub_gpio_irq_handler_t)(void *p, unsigned int pin);
typedef void (*m5io_hub_chn_data_handler_t)(void *p, const u8 *buf, unsigned int len);
/* TOKEN 头 FUNC 字段（第 1 字节高 4 位）。一个 FUNC 就是一个外设分区：
 * TOKEN 用它确定子命令空间，DATA 用通道号 CH 选择数据流。 */
enum {
    M5IO_HUB_PROTOCOL_FUN = 0,
    M5IO_HUB_GPIO_FUN = 1,
    M5IO_HUB_IIC_FUN,
    M5IO_HUB_SPI_FUN,
    M5IO_HUB_UART_FUN,
    M5IO_HUB_PWM_FUN,
    M5IO_HUB_IR_FUN
};

/* FUNC=0 协议控制：不属于任何外设，负责握手、MCU 复位、DATA 窗口确认与状态查询。
 * ARG 取 1..6；其中 2 和 6 目前没有发送方，只在应答校验表里出现。 */
enum {
    M5IO_HUB_PROTOCOL_hello      = 1,
    M5IO_HUB_PROTOCOL_reserved2  = 2,
    M5IO_HUB_PROTOCOL_mcuReset   = 3,
    M5IO_HUB_PROTOCOL_dataAck    = 4,
    M5IO_HUB_PROTOCOL_dataStatus = 5,
    M5IO_HUB_PROTOCOL_reserved6  = 6
};

/* GPIO 引脚数：逻辑索引 0..M5IO_HUB_NGPIO-1，offset 直接等于 MCU 侧索引，
 * 必须与 MCU 固件的 USER_GPIO_PIN_COUNT 保持一致（当前 21）。 */
#define M5IO_HUB_NGPIO 21

/*
 * 名称数组按 gpiochip 偏移索引（offset == MCU 逻辑索引），长度必须等于
 * M5IO_HUB_NGPIO，否则 gpiolib 读到 21 路时会越界访问本数组。未命名的脚
 * 用 NULL 占位，gpiolib 会退化成 "gpio-<n>"。
 *   0..2  GPIO1..3     3..5  LED1..3      6  SD_DET
 *   7     MIC_EN       8     CAMERA_EN    9  GROVE1_EN   10  SD_EN
 *   11    AMP_EN       12    QWIIC_EN     13 HAT_EN      14  GROVE2_EN
 *   15    MIC_SW       16    LCD_KEY_RST  17 UART1TX-PA9  18 UART1RX-PA10  19 I2C2SCL-PB10  20 I2C2SDA-PB11
 */
static const char *const m5io_hub_gpio_names[M5IO_HUB_NGPIO] = {
    "GPIO1", "GPIO2", "GPIO3", "LED1", "LED2", "LED3", "SD_DET",
    "MIC_EN", "CAMERA_EN", "GROVE1_EN", "SD_EN", "AMP_EN", "QWIIC_EN",
    "HAT_EN", "GROVE2_EN", "MIC_SW", "LCD_KEY_RST", "UART1TX-PA9", "UART1RX-PA10",
    "I2C2SCL-PB10", "I2C2SDA-PB11",
};


/*
 * m5io_hub_gpio_attachInterrupt() 的 @type 取值：线上编码 0=下降、1=上升、2=双边。
 * 与 Linux IRQ_TYPE_EDGE_* 的位定义完全不同（FALLING=0x2、RISING=0x1、
 * BOTH=0x3），因此必须显式翻译，不能直接透传。
 */
#define M5IO_HUB_GPIO_EDGE_FALLING 0
#define M5IO_HUB_GPIO_EDGE_RISING  1
#define M5IO_HUB_GPIO_EDGE_BOTH    2
/* FUNC=1 GPIO：1..5 为下行子命令，6 是预留的复用切换，7 只用于 MCU 上行中断事件。
 * 该分区没有 DATA 通道，全部通过 TOKEN 完成。 */
enum {
    M5IO_HUB_GPIO_pinMode = 1,
    M5IO_HUB_GPIO_digitalWrite,
    M5IO_HUB_GPIO_digitalRead,
    M5IO_HUB_GPIO_attachInterrupt,
    M5IO_HUB_GPIO_detachInterrupt,
    M5IO_HUB_GPIO_funMode,
    M5IO_HUB_GPIO_interruptEvent
};

/* FUNC=2 I2C：TOKEN 只做配置与复位，总线报文本走 DATA，通道号等于总线号。
 * CH1/CH2 是 DATA 通道号而非 ARG，数值与子命令 1/2 重合但语义不同。 */
enum {
    M5IO_HUB_I2C_config = 1,
    M5IO_HUB_I2C_reset  = 7,
    M5IO_HUB_I2C_CH1    = 1,
    M5IO_HUB_I2C_CH2    = 2
};

/* FUNC=3 SPI：TOKEN 负责配置、交换开窗/关窗、释放与异常清理；交换数据走 DATA 通道。
 * 支持 4 路总线（bus 0..3），依次占用 CH1..CH4；SPI_CHx 是 DATA 通道号而非 ARG。 */
enum {
    M5IO_HUB_SPI_config       = 1,
    M5IO_HUB_SPI_begin        = 2,
    M5IO_HUB_SPI_end          = 3,
    M5IO_HUB_SPI_begin_txonly = 4,
    M5IO_HUB_SPI_reset        = 6,
    M5IO_HUB_SPI_flush        = 8,
    M5IO_HUB_SPI_CH1          = 3,
    M5IO_HUB_SPI_CH2          = 4,
    M5IO_HUB_SPI_CH3          = 5,
    M5IO_HUB_SPI_CH4          = 6
};

/* FUNC=4 UART：TOKEN 只做配置与关闭，串口字节走 DATA；USART1/2 依次占用 CH1/CH2。
 * 注意载荷里的 port（1/2）与通道号不是同一套编号。 */
enum {
    M5IO_HUB_UART_config = 1,
    M5IO_HUB_UART_is_tx_empty  = 2,
    M5IO_HUB_UART_stop_tx = 3,
    M5IO_HUB_UART_start_tx = 4,
    M5IO_HUB_UART_stop_rx = 5,
    M5IO_HUB_UART_reset  = 6,
    M5IO_HUB_UART_CH1    = 7,
    M5IO_HUB_UART_CH2    = 8
};

/* FUNC=5 背光 PWM（固定 PA8/TIM1_CH1）：只有配置和设亮度两个子命令，无 DATA 通道，
 * 也没有读回命令，亮度以 Linux 侧缓存为准。 */
enum {
    M5IO_HUB_PWM_config = 1,
    M5IO_HUB_PWM_set    = 2,
    M5IO_HUB_PWM_reset  = 3
};

/* FUNC=6 IR：TOKEN 管时序配置、接收开关、复位与关闭；红外波形走 DATA CH（CH1），
 * 一个逻辑消息占一帧。 */
enum {
    M5IO_HUB_IR_config  = 1,
    M5IO_HUB_IR_receive = 3,
    M5IO_HUB_IR_reset   = 5,
    M5IO_HUB_IR_CH      = 9
};

enum {
    M5IO_HUB_LOG_CH      = 10
};


/* =====================================================================
 * 以下接口按 M5IO-HUB 协议组织：每条注释给出分区（FUNC / DATA 通道）、
 * 请求模板、应答模板，以及该外设 DATA 载荷的含义。
 * 注意：本段用到 struct i2c_msg / struct spi_message。
 * ===================================================================== */

/**
 * m5io_hub_gpio_pinMode - 设置引脚方向
 * @hub: hub 句柄
 * @pin: GPIO 逻辑索引 0..M5IO_HUB_NGPIO-1
 * @mode: 0=输出、1=输入
 *
 * 分区：FUNC=M5IO_HUB_GPIO_FUN (GPIO)，无 DATA 通道
 * 请求：FUNC=M5IO_HUB_GPIO_FUN ARG=M5IO_HUB_GPIO_pinMode LEN=2 payload[2]={u8 pin, u8 mode}
 * 应答：FUNC=M5IO_HUB_GPIO_FUN ARG=status LEN=0
 * DATA：无（GPIO 全部走 TOKEN）
 */
int m5io_hub_gpio_pinMode(struct m5io_hub *hub, u8 pin, u8 mode);

/**
 * m5io_hub_gpio_funMode - 引脚复用切换（GPIO <-> 原生外设）
 * @hub: hub 句柄
 * @pin: GPIO 逻辑索引 0..M5IO_HUB_NGPIO-1
 * @mode: 0=复用为该脚的原生外设、1=当普通 GPIO
 *
 * 分区：FUNC=M5IO_HUB_GPIO_FUN (GPIO)，无 DATA 通道
 * 请求：FUNC=M5IO_HUB_GPIO_FUN ARG=M5IO_HUB_GPIO_funMode LEN=3 payload[3]={u8 pin, u8 mode, u8 子参数}；核心应答校验表在
 *       payload[1]==0xfe 时期望 5 B 应答，否则 3 B（core:1044-1047）。
 * 应答：固定长度命令——ARG 原样回填子命令值，payload[0]={u8 status}，后跟实际模式
 * DATA：无
 */
int m5io_hub_gpio_funMode(struct m5io_hub *hub, u8 pin, u8 mode);

/**
 * m5io_hub_gpio_digitalWrite - 输出电平
 * @hub: hub 句柄
 * @pin: 0..M5IO_HUB_NGPIO-1
 * @value: 非 0=高、0=低
 *
 * 分区：FUNC=M5IO_HUB_GPIO_FUN (GPIO)，无 DATA 通道
 * 请求：FUNC=M5IO_HUB_GPIO_FUN ARG=M5IO_HUB_GPIO_digitalWrite LEN=2 payload[2]={u8 pin, u8 value}
 * 应答：FUNC=M5IO_HUB_GPIO_FUN ARG=status LEN=0
 * DATA：无；固件拒绝对输入脚写值（status=3）。
 */
int m5io_hub_gpio_digitalWrite(struct m5io_hub *hub, u8 pin, u8 value);

/**
 * m5io_hub_gpio_digitalRead - 读电平
 * @hub: hub 句柄
 * @pin: 0..M5IO_HUB_NGPIO-1
 *
 * 分区：FUNC=M5IO_HUB_GPIO_FUN (GPIO)，无 DATA 通道
 * 请求：FUNC=M5IO_HUB_GPIO_FUN ARG=M5IO_HUB_GPIO_digitalRead LEN=1 payload[1]={u8 pin}
 * 应答：FUNC=M5IO_HUB_GPIO_FUN ARG=status LEN=1 payload[1]={u8 level(0/1)}
 * DATA：无
 */
int m5io_hub_gpio_digitalRead(struct m5io_hub *hub, u8 pin);

/**
 * m5io_hub_gpio_attachInterrupt - 使能边沿中断并注册回调
 * @hub: hub 句柄
 * @pin: 0..M5IO_HUB_NGPIO-1
 * @type: 线上值 0=下降、1=上升、2=双边（与 Linux IRQ_TYPE_* 不同）
 * @handler: 事件回调
 * @p: 私有指针
 *
 * 分区：FUNC=M5IO_HUB_GPIO_FUN (GPIO)，无 DATA 通道
 * 请求：FUNC=M5IO_HUB_GPIO_FUN ARG=M5IO_HUB_GPIO_attachInterrupt LEN=2 payload[2]={u8 pin, u8 type}
 * 应答：FUNC=M5IO_HUB_GPIO_FUN ARG=status LEN=0
 * DATA：无。中断事件由 MCU 以 FUNC=M5IO_HUB_GPIO_FUN ARG=M5IO_HUB_GPIO_interruptEvent LEN=1 上行（payload[1]={u8 pin}），
 *       不是 DATA 帧。
 */
int m5io_hub_gpio_attachInterrupt(struct m5io_hub *hub, u8 pin, u8 type, m5io_hub_gpio_irq_handler_t handler, void *p);

/**
 * m5io_hub_gpio_detachInterrupt - 关闭边沿中断
 * @hub: hub 句柄
 * @pin: 0..M5IO_HUB_NGPIO-1
 *
 * 分区：FUNC=M5IO_HUB_GPIO_FUN (GPIO)，无 DATA 通道
 * 请求：FUNC=M5IO_HUB_GPIO_FUN ARG=M5IO_HUB_GPIO_detachInterrupt LEN=1 payload[1]={u8 pin}
 * 应答：FUNC=M5IO_HUB_GPIO_FUN ARG=status LEN=0
 * DATA：无
 */
int m5io_hub_gpio_detachInterrupt(struct m5io_hub *hub, u8 pin);

/**
 * m5io_hub_i2c_config - 配置一路 I2C 控制器
 * @hub: hub 句柄
 * @bus: 总线号 1 或 2（对应 DATA CH1/CH2）
 * @clk: 速率档位 0=100 kHz、1=400 kHz（不是 Hz 数值）
 *
 * 分区：FUNC=M5IO_HUB_IIC_FUN (I2C)，数据通道 DATA CH=M5IO_HUB_I2C_CH1/CH2
 * 请求：FUNC=M5IO_HUB_IIC_FUN ARG=M5IO_HUB_I2C_config LEN=2 payload[2]={u8 bus, u8 clk}
 * 应答：FUNC=M5IO_HUB_IIC_FUN ARG=status LEN=0
 * DATA：本命令不携带总线数据；总线字节走 CH=M5IO_HUB_I2C_CH1/CH2，首字节 0xAA 写 / 0xAB 读，
 *       I2C 事务报文格式见文件顶部 "4) 各通道 DATA 载荷格式"。
 */
int m5io_hub_i2c_config(struct m5io_hub *hub, u8 bus, u8 clk);

/**
 * m5io_hub_i2c_reset - 复位一路 I2C 控制器
 * @hub: hub 句柄
 * @bus: 总线号 1 或 2
 *
 * 分区：FUNC=M5IO_HUB_IIC_FUN (I2C)，数据通道 DATA CH=M5IO_HUB_I2C_CH1/CH2
 * 请求：FUNC=M5IO_HUB_IIC_FUN ARG=M5IO_HUB_I2C_reset LEN=1 payload[1]={u8 bus}
 * 应答：FUNC=M5IO_HUB_IIC_FUN ARG=status LEN=0
 * DATA：无载荷数据；复位后仍用 CH=M5IO_HUB_I2C_CH1/CH2 传 I2C 报文。
 */
int m5io_hub_i2c_reset(struct m5io_hub *hub, u8 bus);

/**
 * m5io_hub_i2c_transfer - 执行一次 I2C 消息传输
 * @hub: hub 句柄
 * @bus: 总线号 1 或 2
 * @msgs: Linux I2C 消息数组（1 或 2 条；2 条须是同地址的写+读）
 * @num: 消息条数 1..2
 *
 * 分区：FUNC=M5IO_HUB_IIC_FUN (I2C)，数据通道 DATA CH=M5IO_HUB_I2C_CH1/CH2
 * 请求：DATA 帧，payload[tx_len] 首字节为操作码
 *       单条写：   {u8 0xAA, u8 addr, u8 data[n]}          长度 2+n
 *       单条读：   {u8 0xAB, u8 addr, u8 rdlen}          长度 3；应答 {u8 0xAB, u8 addr, u8 data[rdlen]}
 *       合并写读： {u8 0xAB, u8 addr, u8 wr[n], u8 rdlen} 长度 3+n；应答同上
 * 应答：反向 DATA（同 CH/PID）+ 每帧一次 WINDOW_ACK；写操作无应答载荷。
 * DATA：addr 为 7 bit 从地址（0x08..0x77）；单笔数据上限 253 B
 *       （写+读合并时首条写 ≤252、第二条读 ≤253）。
 */
int m5io_hub_i2c_transfer(struct m5io_hub *hub, u8 bus, struct i2c_msg *msgs, int num);

/* I2C DATA 通道操作码（payload 首字节）：0xAA 写、0xAB 读/合并写读、
 * 0xAC Quick Command（只有地址阶段，无数据阶段）。 */
#define M5IO_HUB_I2C_OP_WRITE  0xAAU
#define M5IO_HUB_I2C_OP_READ   0xABU
#define M5IO_HUB_I2C_OP_QUICK  0xACU

/**
 * m5io_hub_i2c_quick - 发起一次 SMBus Quick Command（纯地址事务）
 * @hub: hub 句柄
 * @bus: 总线号 1 或 2（对应 DATA CH=M5IO_HUB_I2C_CH1/CH2）
 * @addr: 7 bit 从地址 0x08..0x77
 * @read: false=Quick Write（R/W# 位为写），true=Quick Read（R/W# 位为读）
 *
 * 分区：FUNC=M5IO_HUB_IIC_FUN (I2C)，数据通道 DATA CH=M5IO_HUB_I2C_CH1/CH2
 * 请求：DATA 帧 CH=M5IO_HUB_I2C_CH1/CH2
 *       payload[3]={u8 0xAC, u8 addr, u8 rw}   rw: 0=写 1=读
 * 应答：反向 DATA 帧（同 CH/PID）+ 一次 WINDOW_ACK
 *       payload[3]={u8 0xAC, u8 addr, u8 acked}  acked: 0=NAK 1=ACK
 * DATA：无数据阶段——总线上只有 START + 地址 + R/W# + STOP，不传输数据字节。
 *
 * Return: 0 从机 ACK；-ENXIO 从机 NAK/无应答；其余为传输错误。
 */
int m5io_hub_i2c_quick(struct m5io_hub *hub, u8 bus, u8 addr, bool read);

/**
 * m5io_hub_i2c_quick_write - SMBus Quick Write（i2cdetect 默认探测方式）
 * @hub: hub 句柄
 * @bus: 总线号 1 或 2
 * @addr: 7 bit 从地址 0x08..0x77
 *
 * 分区：FUNC=M5IO_HUB_IIC_FUN (I2C)，数据通道 DATA CH=M5IO_HUB_I2C_CH1/CH2
 * 请求：DATA 帧 payload[3]={u8 0xAC, u8 addr, u8 0}
 * 应答：payload[3]={u8 0xAC, u8 addr, u8 acked}
 * DATA：无数据阶段；等价于 m5io_hub_i2c_quick(hub, bus, addr, false)。
 * Return: 0 从机 ACK；-ENXIO 从机 NAK/无应答。
 */
int m5io_hub_i2c_quick_write(struct m5io_hub *hub, u8 bus, u8 addr);

/**
 * m5io_hub_i2c_quick_read - SMBus Quick Read（R/W# 位为读的纯地址事务）
 * @hub: hub 句柄
 * @bus: 总线号 1 或 2
 * @addr: 7 bit 从地址 0x08..0x77
 *
 * 分区：FUNC=M5IO_HUB_IIC_FUN (I2C)，数据通道 DATA CH=M5IO_HUB_I2C_CH1/CH2
 * 请求：DATA 帧 payload[3]={u8 0xAC, u8 addr, u8 1}
 * 应答：payload[3]={u8 0xAC, u8 addr, u8 acked}
 * DATA：无数据阶段（不等同于 SMBus Receive Byte，后者会读回 1 B）；
 *       等价于 m5io_hub_i2c_quick(hub, bus, addr, true)。
 * Return: 0 从机 ACK；-ENXIO 从机 NAK/无应答。
 */
int m5io_hub_i2c_quick_read(struct m5io_hub *hub, u8 bus, u8 addr);

// int m5io_hub_i2c_exec(struct m5io_hub *hub)

/**
 * m5io_hub_uart_config - 配置一路 UART
 * @hub: hub 句柄
 * @bus: 端口号 1=USART1(CH4)、2=USART2(CH5)；注意与通道号差 3
 * @baudrate: 波特率 Hz（固件限 1200..1000000）
 * @dbit: 数据位 7 或 8
 * @parity: 0=无、1=偶、2=奇
 * @sbit: 停止位 1 或 2
 * @mode: 流控/模式
 *
 * 分区：FUNC=M5IO_HUB_UART_FUN (UART)，数据通道 DATA CH=M5IO_HUB_UART_CH1/CH2
 * 请求：FUNC=M5IO_HUB_UART_FUN ARG=M5IO_HUB_UART_config LEN=10
 *       payload[10]={u8 port, u32 baud(LE), u8 dbit, u8 parity, u8 sbit, u16 watermark(LE)=0x0100}
 * 应答：FUNC=M5IO_HUB_UART_FUN ARG=status LEN=10，payload[10] 回显实际生效配置（结构同上）
 * DATA：本命令不下发串口数据；串口字节走 CH=M5IO_HUB_UART_CH1/CH2，纯字节流、无前缀，
 *       单片 ≤255 B。
 */
int m5io_hub_uart_config(struct m5io_hub *hub, u8 bus, unsigned int baudrate, u8 dbit, u8 parity, u8 sbit, u8 mode);

/**
 * m5io_hub_uart_reset - 关闭一路 UART（协议里的 DISABLE）
 * @hub: hub 句柄
 * @bus: 端口号 1 或 2
 *
 * 分区：FUNC=M5IO_HUB_UART_FUN (UART)，数据通道 DATA CH=M5IO_HUB_UART_CH1/CH2
 * 请求：FUNC=M5IO_HUB_UART_FUN ARG=M5IO_HUB_UART_reset LEN=1 payload[1]={u8 port}
 * 应答：FUNC=M5IO_HUB_UART_FUN ARG=status LEN=0
 * DATA：无载荷数据；关闭后该 CH 的收发窗口由主机侧用
 *       m5io_hub_data_abort_channel() 清理。
 */
int m5io_hub_uart_reset(struct m5io_hub *hub, u8 bus);

/**
 * m5io_hub_uart_send_data - 发送串口字节
 * @hub: hub 句柄
 * @bus: 端口号 1 或 2
 * @data: 原始字节
 * @len: 字节数（单片 ≤255）
 *
 * 分区：FUNC=M5IO_HUB_UART_FUN (UART)，数据通道 DATA CH=M5IO_HUB_UART_CH1/CH2
 * 请求：DATA 帧 CH=M5IO_HUB_UART_CH1（bus=1）/M5IO_HUB_UART_CH2（bus=2），payload[len]={u8 data[len]}，无前缀
 * 应答：WINDOW_ACK（FUNC=M5IO_HUB_PROTOCOL_FUN/ARG=M5IO_HUB_PROTOCOL_dataAck/LEN=6，payload[6]={u8 chn, u8 pid, u16 op_id(LE), u8 kind, u8 status}）
 * DATA：纯字节流；超过 255 B 必须拆成多帧，SEQ 按通道递增（6 bit 回绕），
 *       STA 标记 0=传输包、2=末包（单包消息也用 2）；接收端按 SEQ 顺序拼回。
 */
int m5io_hub_uart_send_data(struct m5io_hub *hub, u8 bus, const u8 *data, unsigned int len);

/**
 * m5io_hub_uart_register_rx - 注册串口接收回调
 * @hub: hub 句柄
 * @bus: 端口号 1 或 2
 * @handler: 收到一帧 CH=M5IO_HUB_UART_CH1/CH2 DATA 时调用
 * @p: 私有指针
 *
 * 无线上帧。
 * 分区：DATA CH=M5IO_HUB_UART_CH1/CH2（UART）
 * DATA：回调 buf/len 是一帧原始串口字节（≤255）；驱动会先做 configured /
 *       rx_enabled 门控，被门控时字节直接丢弃（但 WINDOW_ACK 已在 core 层
 *       发出，不会造成 MCU 窗口泄漏）。
 */
int m5io_hub_uart_register_rx(struct m5io_hub *hub, u8 bus, m5io_hub_chn_data_handler_t handler, void *p);

/**
 * m5io_hub_uart_unregister_rx - 注销串口接收回调
 * @hub: hub 句柄
 * @bus: 端口号 1 或 2
 *
 * 无线上帧；同步等待在飞回调结束，不可在回调内调用。
 */
void m5io_hub_uart_unregister_rx(struct m5io_hub *hub, u8 bus);

/**
 * m5io_hub_uart_tx_empty - 查询 UART 发送窗口是否为空
 * @hub: hub 句柄
 * @bus: 端口号 1 或 2
 *
 * 分区：FUNC=M5IO_HUB_UART_FUN (UART)，数据通道 DATA CH=M5IO_HUB_UART_CH1/CH2
 * 请求：FUNC=M5IO_HUB_UART_FUN ARG=M5IO_HUB_UART_txEmpty LEN=1 payload[1]={u8 port}
 * 应答：FUNC=M5IO_HUB_UART_FUN ARG=status LEN=1 payload[1]={u8 empty}，empty=1 表示发送窗口为空
 */
int m5io_hub_uart_tx_empty(struct m5io_hub *hub, u8 bus);

/**
 * m5io_hub_uart_stop_tx - 停止 UART 发送，丢弃 MCU 侧尚未发出的队列数据
 * @hub: hub 句柄
 * @bus: 端口号 1 或 2
 *
 * 分区：FUNC=M5IO_HUB_UART_FUN (UART)，数据通道 DATA CH=M5IO_HUB_UART_CH1/CH2
 * 请求：FUNC=M5IO_HUB_UART_FUN ARG=M5IO_HUB_UART_stop_tx LEN=1 payload[1]={u8 port}
 * 应答：FUNC=M5IO_HUB_UART_FUN ARG=status LEN=0
 * DATA：无载荷数据；只丢弃尚未发出的字节，配置保留，之后仍可继续
 *       m5io_hub_uart_send_data()。
 */
int m5io_hub_uart_stop_tx(struct m5io_hub *hub, u8 bus);

/**
 * m5io_hub_uart_start_tx - 恢复 UART 发送
 * @hub: hub 句柄
 * @bus: 端口号 1 或 2
 *
 * 分区：FUNC=M5IO_HUB_UART_FUN (UART)，数据通道 DATA CH=M5IO_HUB_UART_CH1/CH2
 * 请求：FUNC=M5IO_HUB_UART_FUN ARG=M5IO_HUB_UART_start_tx LEN=1 payload[1]={u8 port}
 * 应答：FUNC=M5IO_HUB_UART_FUN ARG=status LEN=0
 * DATA：无载荷数据；与 m5io_hub_uart_stop_tx() 配对，恢复后继续接受
 *       m5io_hub_uart_send_data() 的 DATA。配置保持不变。
 */
int m5io_hub_uart_start_tx(struct m5io_hub *hub, u8 bus);

/**
 * m5io_hub_uart_stop_rx - 停止 UART 接收
 * @hub: hub 句柄
 * @bus: 端口号 1 或 2
 *
 * 分区：FUNC=M5IO_HUB_UART_FUN (UART)，数据通道 DATA CH=M5IO_HUB_UART_CH1/CH2
 * 请求：FUNC=M5IO_HUB_UART_FUN ARG=M5IO_HUB_UART_stop_rx LEN=1 payload[1]={u8 port}
 * 应答：FUNC=M5IO_HUB_UART_FUN ARG=status LEN=0
 * DATA：无载荷数据；MCU 停止向上报告该端口的 RX DATA，配置保持不变。
 *       恢复接收由重新 config 或后续恢复命令负责。
 */
int m5io_hub_uart_stop_rx(struct m5io_hub *hub, u8 bus);


/**
 * m5io_hub_spi_config - 配置指定 SPI 总线
 * @hub: hub 句柄
 * @bus: 总线号 0..3（对应 CH=M5IO_HUB_SPI_CH1..CH4）
 * @clk: SCLK Hz（250 kHz..20 MHz）
 * @direction: 传输方向（全双工/半双工/只发/只收）
 * @datasize: 8 或 16（bits_per_word）
 * @mode: SPI_CPOL|SPI_CPHA 组合
 * @nss: 片选编号
 * @firstbit: 位序（MSB first / LSB first）
 * @wmode: 工作模式
 *
 * 分区：FUNC=M5IO_HUB_SPI_FUN (SPI)，数据通道 DATA CH=M5IO_HUB_SPI_CH1..CH4
 * 请求：FUNC=M5IO_HUB_SPI_FUN ARG=M5IO_HUB_SPI_config LEN=7
 *       payload[7]={u8 bus, u8 mode, u8 datasize, u16 clk_khz(LE), u16 watermark(LE)=0x0100}
 * 应答：FUNC=M5IO_HUB_SPI_FUN ARG=status LEN=0；BUSY 时带 5 B 详情
 * DATA：本命令只配置；交换数据走 CH=M5IO_HUB_SPI_CH1..CH4（原始 MOSI/MISO），格式见顶部说明。
 */
int m5io_hub_spi_config(struct m5io_hub *hub, u8 bus, unsigned int clk, u8 direction, u8 datasize, u8 mode, u8 nss, u8 firstbit, u8 wmode);

/**
 * m5io_hub_spi_reset - 释放/关闭指定 SPI 总线（协议里的 DISABLE）
 * @hub: hub 句柄
 * @bus: 总线号 0..3（对应 CH=M5IO_HUB_SPI_CH1..CH4）
 *
 * 分区：FUNC=M5IO_HUB_SPI_FUN (SPI)，数据通道 DATA CH=M5IO_HUB_SPI_CH1..CH4
 * 请求：FUNC=M5IO_HUB_SPI_FUN ARG=M5IO_HUB_SPI_reset LEN=1 payload[1]={u8 bus}
 * 应答：FUNC=M5IO_HUB_SPI_FUN ARG=status LEN=0；BUSY 时带 5 B 详情
 * DATA：无载荷数据。与 flush 的区别：disable 会丢掉配置，flush 只丢
 *       未完成的交换；两接口都需要随后清理主机侧 CH=M5IO_HUB_SPI_CH1..CH4 状态。
 */
int m5io_hub_spi_reset(struct m5io_hub *hub, u8 bus);

/**
 * m5io_hub_spi_begin - 打开一次 SPI 交换窗口
 * @hub: hub 句柄
 * @bus: 总线号 0..3（对应 CH=M5IO_HUB_SPI_CH1..CH4）
 * @len: 本次交换总字节数（≤512；16 bit 模式下须为偶数）
 *
 * 分区：FUNC=M5IO_HUB_SPI_FUN (SPI)，数据通道 DATA CH=M5IO_HUB_SPI_CH1..CH4
 * 请求：FUNC=M5IO_HUB_SPI_FUN ARG=M5IO_HUB_SPI_begin LEN=3
 *       payload[3]={u8 bus, u16 len(LE)}
 * 应答：FUNC=M5IO_HUB_SPI_FUN ARG=status LEN=0（BUSY 时附 5 B 详情）
 * DATA：本命令只开窗，不传数据；随后用 DATA 帧 CH=M5IO_HUB_SPI_CH1..CH4 分片传输，
 *       单片 ≤255 B（16 bit 模式 ≤254 B 且长度偶数）。
 *
 * Return: 0 成功；-EINVAL len 非法；-EBUSY 上一笔未收尾。
 */
int m5io_hub_spi_begin(struct m5io_hub *hub, u8 bus, unsigned int len);

/**
 * m5io_hub_spi_begin_txonly - 打开一次只发不收的 SPI 交换窗口
 * @hub: hub 句柄
 * @bus: 总线号 0..3（对应 CH=M5IO_HUB_SPI_CH1..CH4）
 * @len: 只发方向的字节数（≤512；16 bit 模式下须为偶数）
 *
 * 分区：FUNC=M5IO_HUB_SPI_FUN (SPI)，数据通道 DATA CH=M5IO_HUB_SPI_CH1..CH4
 * 请求：FUNC=M5IO_HUB_SPI_FUN ARG=M5IO_HUB_SPI_begin_txonly LEN=3
 *       payload[3]={u8 bus, u16 len(LE)}
 * 应答：FUNC=M5IO_HUB_SPI_FUN ARG=status LEN=0（BUSY 时附 5 B 详情）
 * DATA：主机照常上传 MOSI 分片 payload[len]={u8 data[len]}（单片 ≤255 B，
 *       16 bit 模式 ≤254 B 且偶数）；MCU 仍做真实交换，但不回等长 MISO，
 *       而是在收满 @len 字节后上行一帧 DATA CH=M5IO_HUB_SPI_CH1..CH4：
 *       payload[1]={u8 status}（0 成功），主机需回 WINDOW_ACK。
 *       即上行字节数由 len 降为 1。
 *
 * 约束：与 m5io_hub_spi_begin() 互斥，同一时刻只允许一个窗口；
 *       收尾可用 m5io_hub_spi_end()，异常用 m5io_hub_spi_flush()。
 *
 * Return: 0 成功（窗口已打开）；-EINVAL len 非法；-EBUSY 已有窗口在飞。
 */
int m5io_hub_spi_begin_txonly(struct m5io_hub *hub, u8 bus, unsigned int len);

/**
 * m5io_hub_spi_end - 关闭 SPI 交换窗口
 * @hub: hub 句柄
 * @bus: 总线号 0..3（对应 CH=M5IO_HUB_SPI_CH1..CH4）
 * @len: 实际已传输的字节数（应与 begin 一致，供 MCU 侧核对）
 *
 * 分区：FUNC=M5IO_HUB_SPI_FUN (SPI)，数据通道 DATA CH=M5IO_HUB_SPI_CH1..CH4
 * 请求：FUNC=M5IO_HUB_SPI_FUN ARG=M5IO_HUB_SPI_end LEN=3
 *       payload[3]={u8 bus, u16 len(LE)}
 * 应答：FUNC=M5IO_HUB_SPI_FUN ARG=status LEN=0
 * DATA：无数据阶段；正常收尾时关闭窗口并释放 PID。若窗口因异常无法关闭，
 *       改用 m5io_hub_spi_flush()。
 *
 * Return: 0 成功；-EINVAL len 与已传输字节数不符；其余为传输错误。
 */
int m5io_hub_spi_end(struct m5io_hub *hub, u8 bus, unsigned int len);

/**
 * m5io_hub_spi_transfer - 执行一条 SPI message（按 CS 分组，每组一次 RPC）
 * @hub: hub 句柄
 * @bus: 总线号 0..3（对应 CH=M5IO_HUB_SPI_CH1..CH4）
 * @msg: 待执行的消息；`msg->spi` 为目标设备（CPOL/CPHA 取自 spi->mode），
 *       `msg->transfers` 为若干 struct spi_transfer
 *
 * 分区：FUNC=M5IO_HUB_SPI_FUN (SPI)，数据通道 DATA CH=M5IO_HUB_SPI_CH1..CH4
 *
 * 分组：一条 message 可含多个 CS 组。顺序遍历 transfers，遇到 cs_change 为真
 *       的段或最后一段即本组结束；每组重新下发一次 CONFIG 再做一次连续交换，
 *       因此 MCU 侧复位后不会沿用过期配置。
 *
 * 请求：每组两段
 *       ① FUNC=M5IO_HUB_SPI_FUN ARG=M5IO_HUB_SPI_begin LEN=3
 *          payload[3]={u8 bus, u16 len(LE)}，len 为本组拼接后的总字节数
 *       ② 若干 DATA 帧 CH=M5IO_HUB_SPI_CH1..CH4
 *          payload[len]={u8 data[len]}；8 bit 模式单片 ≤255 B，
 *          16 bit 模式单片 ≤254 B 且长度必须为偶数
 * 应答：① begin 回 FUNC=M5IO_HUB_SPI_FUN ARG=status LEN=0（BUSY 时附 5 B 详情）
 *       ② 每片回一帧等长 DATA（MISO），并各回一次 WINDOW_ACK：
 *          payload[6]={u8 chn, u8 pid, u16 op_id(LE), u8 kind, u8 status}
 *
 * DATA：MOSI 为组内各段 tx_buf 顺序拼接，tx_buf 为 NULL 的段补 0；
 *       MISO 按各段 len 切回各自的 rx_buf；16 bit 模式主机侧按大端拆/拼字。
 *
 * 校验（不满足即失败，且不产生任何总线动作）：组内每段 len 非 0，
 *       speed_hz 与 bits_per_word 必须一致；speed_hz 限 250 kHz..20 MHz
 *       （0 不回落设备默认值，会被拒）；bits_per_word 只能 8 或 16；
 *       禁止 cs_off、delay、word_delay、tx_nbits/rx_nbits > SPI_NBITS_SINGLE；
 *       末段的 cs_change 无意义。设备级 cs_setup/cs_hold/cs_inactive/
 *       word_delay 非 0 同样拒绝。
 *
 * 输出：msg->actual_length 为已完成字节数，msg->status 为最终状态。
 * Return: 0 成功；负值为错误码（同时写入 msg->status）。
 */
int m5io_hub_spi_transfer(struct m5io_hub *hub, u8 bus, struct spi_message *msg);

/**
 * m5io_hub_spi_flush - 丢弃 MCU 侧未完成的 SPI 交换，解除 BUSY
 * @hub: hub 句柄
 * @bus: 总线号 0..3（对应 CH=M5IO_HUB_SPI_CH1..CH4）
 *
 * 分区：FUNC=M5IO_HUB_SPI_FUN (SPI)，数据通道 DATA CH=M5IO_HUB_SPI_CH1..CH4
 * 请求：FUNC=M5IO_HUB_SPI_FUN ARG=M5IO_HUB_SPI_flush LEN=1 payload[1]={u8 bus}
 * 应答：固定长度命令——ARG 原样回填 flush、LEN=4、payload[0]={u8 status}（0 成功），
 *       payload[1..3] 为 MCU 侧残留计数
 * DATA：无数据阶段；成功后主机侧同时丢弃 CH=M5IO_HUB_SPI_CH1..CH4 的在飞窗口与
 *       去重状态，其它通道不受影响。
 *
 * Return: 0 成功；-EPROTO 应答不是 ARG=flush/LEN=4/payload[0]=0。
 */
int m5io_hub_spi_flush(struct m5io_hub *hub, u8 bus);

/**
 * m5io_hub_ir_config - 配置红外时序（旧签名）
 * @hub: hub 句柄
 * @clk: 载波频率 Hz（协议实际要 15 B 配置块，不是单个频率）
 *
 * 分区：FUNC=M5IO_HUB_IR_FUN (IR)，数据通道 DATA CH=M5IO_HUB_IR_CH
 * 请求：FUNC=M5IO_HUB_IR_FUN ARG=M5IO_HUB_IR_config LEN=15 payload[15]={u8 cfg[15]}（参数只有 clk，无法覆盖 15 B 配置，需内部补齐）
 * 应答：FUNC=M5IO_HUB_IR_FUN ARG=status LEN=15（实际生效配置）
 * DATA：本命令不下发波形；红外字节走 CH=M5IO_HUB_IR_CH。
 * 注意：与文件上半部分的 m5io_hub_ir_config(hub, const u8 payload[15],
 *       u8 actual[15]) 同名但签名不同，同一编译单元同时可见会直接报错。
 */
int m5io_hub_ir_config(struct m5io_hub *hub, unsigned int clk);

/**
 * m5io_hub_ir_reset - 复位红外外设
 * @hub: hub 句柄
 *
 * 分区：FUNC=M5IO_HUB_IR_FUN (IR)，数据通道 DATA CH=M5IO_HUB_IR_CH
 * 请求：FUNC=M5IO_HUB_IR_FUN ARG=M5IO_HUB_IR_reset LEN=0
 * 应答：FUNC=M5IO_HUB_IR_FUN ARG=status LEN=0
 * DATA：无载荷数据；复位后清主机侧 CH=M5IO_HUB_IR_CH 窗口。
 */
int m5io_hub_ir_reset(struct m5io_hub *hub);

/**
 * m5io_hub_ir_start_reace - 开始红外接收
 * @hub: hub 句柄
 *
 * 分区：FUNC=M5IO_HUB_IR_FUN (IR)，数据通道 DATA CH=M5IO_HUB_IR_CH
 * 请求：FUNC=M5IO_HUB_IR_FUN ARG=M5IO_HUB_IR_receive LEN=1 payload[1]={u8 enable=1}
 * 应答：FUNC=M5IO_HUB_IR_FUN ARG=status LEN=0
 * DATA：收到的波形按 DATA CH=M5IO_HUB_IR_CH 上行，一帧=一个逻辑消息（APP 限 1..13 B），
 *       主机需回 WINDOW_ACK。
 */
int m5io_hub_ir_start_reace(struct m5io_hub *hub);

/**
 * m5io_hub_ir_stop_reace - 停止红外接收
 * @hub: hub 句柄
 *
 * 分区：FUNC=M5IO_HUB_IR_FUN (IR)，数据通道 DATA CH=M5IO_HUB_IR_CH
 * 请求：FUNC=M5IO_HUB_IR_FUN ARG=M5IO_HUB_IR_receive LEN=1 payload[1]={u8 enable=0}
 * 应答：FUNC=M5IO_HUB_IR_FUN ARG=status LEN=0
 * DATA：无；停止后 CH=M5IO_HUB_IR_CH 仍可上行 MCU 已排队的帧，主机仍要回 ACK。
 */
int m5io_hub_ir_stop_reace(struct m5io_hub *hub);

/**
 * m5io_hub_ir_send_data - 发送一个红外逻辑消息
 * @hub: hub 句柄
 * @data: 波形字节
 * @len: 1..255（驱动上限；APP 只收 1..13）
 *
 * 分区：FUNC=M5IO_HUB_IR_FUN (IR)，数据通道 DATA CH=M5IO_HUB_IR_CH
 * 请求：DATA 帧 CH=M5IO_HUB_IR_CH，payload[len]={u8 data[len]}（无操作码、无长度前缀）
 * 应答：WINDOW_ACK（FUNC=M5IO_HUB_PROTOCOL_FUN/ARG=M5IO_HUB_PROTOCOL_dataAck/LEN=6，payload[6]={u8 chn, u8 pid, u16 op_id(LE), u8 kind, u8 status}）
 * DATA：一个逻辑消息只能占一帧 DATA，不能拆帧。
 */
int m5io_hub_ir_send_data(struct m5io_hub *hub, const u8 *data, unsigned int len);

/**
 * m5io_hub_ir_register_rx - 注册红外接收回调
 * @hub: hub 句柄
 * @handler: 回调
 * @p: 私有指针
 *
 * 无线上帧；分区为 DATA CH=M5IO_HUB_IR_CH。
 * DATA：回调收到一帧红外波形原始字节（1..13）。
 */
int m5io_hub_ir_register_rx(struct m5io_hub *hub, m5io_hub_chn_data_handler_t handler, void *p);

/**
 * m5io_hub_ir_unregister_rx - 注销红外接收回调
 * @hub: hub 句柄
 *
 * 无线上帧；同步等待在飞回调结束。
 */
void m5io_hub_ir_unregister_rx(struct m5io_hub *hub);

/**
 * m5io_hub_pwm_config - 配置背光并设置亮度（两步发送）
 * @hub: hub 句柄
 * @frequency: PWM 频率 Hz（1..65535，固件回读实际值）
 * @brightness: 亮度 0..255
 *
 * 分区：FUNC=M5IO_HUB_PWM_FUN (PWM/背光)，无 DATA 通道
 * 请求：① FUNC=M5IO_HUB_PWM_FUN ARG=M5IO_HUB_PWM_config LEN=2 payload[2]={u16 freq_hz(LE)}，应答 ARG=status LEN=2 回读实际频率
 *       ② FUNC=M5IO_HUB_PWM_FUN ARG=M5IO_HUB_PWM_set LEN=1 payload[1]={u8 brightness}，应答 ARG=status LEN=0
 * DATA：无。背光固定 PA8/TIM1_CH1，没有读命令，主机侧缓存即权威值。
 */
int m5io_hub_pwm_config(struct m5io_hub *hub, unsigned int frequency, unsigned int brightness);

/**
 * m5io_hub_pwm_reset - 复位背光
 * @hub: hub 句柄
 *
 * 分区：FUNC=M5IO_HUB_PWM_FUN (PWM/背光)，无 DATA 通道
 * 请求：无。FUNC=M5IO_HUB_PWM_FUN 只有 ARG=M5IO_HUB_PWM_config 和 ARG=M5IO_HUB_PWM_set 两个子命令
 *       （m5io-hub-backlight.c:28-29，核心应答校验表同样只认这两个）；
 *       枚举里的 M5IO_HUB_PWM_reset 在本目录内找不到任何协议依据。
 * 应答：无
 * DATA：无
 * 要“复位”背光，可发 CONFIG 重设频率 + SET 0 亮度，或直接发 MCU_RESET。
 */
int m5io_hub_pwm_reset(struct m5io_hub *hub);



#endif /* __LINUX_M5IO_HUB_API_H */
