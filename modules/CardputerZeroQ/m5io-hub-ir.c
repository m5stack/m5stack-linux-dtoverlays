// SPDX-License-Identifier: GPL-2.0
/*
 * M5Stack M5IO-HUB 红外收发驱动（rc-core / LIRC 桥）
 *
 * 硬件分工：MCU 只负责红外电平翻转，以及引导电平/结束电平的识别；收发内容
 * 在 SoC 与 MCU 之间都是“逻辑字节”（1..13 字节，字节内高位先发）。协议解析
 * 放在 SoC 侧完成，所以本驱动把 MCU 上报的逻辑字节还原成标准 raw pulse/space
 * 事件交给 rc-core，由内核软件解码器（NEC 等）解析成 key/scancode；发送方向
 * 反过来，把 rc-core 交来的 pulse/space 波形按同一套时序解回逻辑字节，再经
 * m5io_hub_ir_send_data() 交给 MCU 重新调制。
 *
 * 与 core 的时序契约：m5io_hub_ir_config(hub, clk) 只携带载波频率，15 字节
 * 时序块由 core 内部补齐。本驱动按下面这组 NEC 时序展开/解析波形，因此 core
 * 补齐的 leader/bit MARK/SPACE 必须与 M5IO_IR_*_US 一致，否则收发都对不上
 * （IR-Chat / FactoryTest 用的也是同一组数值）。
 *
 * 本驱动把 core 的 IR API 接成标准 rc-core 设备（/sys/class/rc/rcN、
 * evdev input，内核开 CONFIG_LIRC 时还有 /dev/lircN），用户态协议解析与
 * 回放走标准 LIRC API。MCU 的 IR_TX 与 IR_RX 互斥，所以发送前后由驱动
 * stop/start 一次接收；这意味着 m5io_hub_ir_send_data() 必须在波形发送
 * 结束（而不是仅排队）后才返回，与 rc-core 对 tx_ir 的时序要求一致。
 * API 目前只有载波频率，占空比由 core 的固定配置决定，因此驱动不
 * advertise LIRC_SET_SEND_DUTY_CYCLE。
 */

#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#include <media/rc-core.h>

#include "m5io-hub.h"

#define M5IO_IR_DRIVER_NAME        "m5io-hub-ir"
#define M5IO_IR_DEVICE_NAME        "M5IO-HUB IR"
/* 固件单次红外逻辑消息上限（APP 限制，不是 DATA 帧限制）。 */
#define M5IO_IR_MAX_BYTES          13U
#define M5IO_IR_DEFAULT_CARRIER_HZ 38000U
#define M5IO_IR_CARRIER_MIN_HZ     20000U
#define M5IO_IR_CARRIER_MAX_HZ     60000U

/* NEC 风格通用二进制时序；必须与 core 补齐的 15 B IR_CONFIG 一致。 */
#define M5IO_IR_LEADER_MARK_US      9000U
#define M5IO_IR_LEADER_SPACE_US     4500U
#define M5IO_IR_BIT_MARK_US         560U
#define M5IO_IR_BIT_ZERO_SPACE_US   560U
#define M5IO_IR_BIT_ONE_SPACE_US    1690U
/* 与用户态 IR-Chat/FactoryTest 相同的判读窗口，用来解析外部波形。 */
#define M5IO_IR_LEADER_MARK_MIN_US  6300U
#define M5IO_IR_LEADER_MARK_MAX_US  11700U
#define M5IO_IR_LEADER_SPACE_MIN_US 3150U
#define M5IO_IR_LEADER_SPACE_MAX_US 5850U
#define M5IO_IR_BIT_MARK_MIN_US     300U
#define M5IO_IR_BIT_MARK_MAX_US     850U
#define M5IO_IR_BIT_ZERO_MAX_US     950U
#define M5IO_IR_BIT_ONE_MIN_US      1100U
#define M5IO_IR_BIT_ONE_MAX_US      2400U
/*
 * 帧尾还要给解码器一个足够长的 SPACE：rc-core 的 NEC 解码器在终止 MARK 之后
 * 要求 space >= NEC_TRAILER_SPACE(10*563us) 左右才收帧。MCU 的 frame_gap
 * 可以短到 3 ms，所以这里按协议允许的最大解码窗口取一个安全值。
 */
#define M5IO_IR_TRAILER_SPACE_US    11000U

struct m5io_hub_ir {
  struct m5io_hub *hub;
  struct rc_dev *rc;
  /* 串行化“重配载波 / 开关接收 / 发送”三件会互相踩硬件状态的事。 */
  struct mutex lock;
  unsigned int carrier_hz;
  bool rx_running;
  bool rx_registered;
  /* 发送期间丢弃迟到的接收帧，避免把 TX 尾巴当 RX 数据解码。 */
  bool tx_active;
};

static bool m5io_hub_ir_in_range(unsigned int value, unsigned int min,
                                 unsigned int max)
{
  return value >= min && value <= max;
}

static bool m5io_hub_ir_is_leader_mark(unsigned int duration)
{
  return m5io_hub_ir_in_range(duration, M5IO_IR_LEADER_MARK_MIN_US,
                              M5IO_IR_LEADER_MARK_MAX_US);
}

static bool m5io_hub_ir_is_leader_space(unsigned int duration)
{
  return m5io_hub_ir_in_range(duration, M5IO_IR_LEADER_SPACE_MIN_US,
                              M5IO_IR_LEADER_SPACE_MAX_US);
}

static bool m5io_hub_ir_is_bit_mark(unsigned int duration)
{
  return m5io_hub_ir_in_range(duration, M5IO_IR_BIT_MARK_MIN_US,
                              M5IO_IR_BIT_MARK_MAX_US);
}

/* 逻辑 0/1 的唯一区别是 SPACE 宽度：窄=0，宽=1。 */
static bool m5io_hub_ir_decode_bit_space(unsigned int duration, bool *one)
{
  if (m5io_hub_ir_in_range(duration, M5IO_IR_BIT_MARK_MIN_US,
                           M5IO_IR_BIT_ZERO_MAX_US)) {
    *one = false;
    return true;
  }
  if (m5io_hub_ir_in_range(duration, M5IO_IR_BIT_ONE_MIN_US,
                           M5IO_IR_BIT_ONE_MAX_US)) {
    *one = true;
    return true;
  }
  return false;
}

static void m5io_hub_ir_store(struct rc_dev *rc, unsigned int duration_us,
                              bool pulse)
{
  struct ir_raw_event ev = {
      .duration = duration_us,
      .pulse = pulse,
  };

  if (ir_raw_event_store(rc, &ev))
    dev_warn_ratelimited(&rc->dev, "IR raw event FIFO full\n");
}

/*
 * RX：MCU 已经完成引导码/终止 MARK 的识别并去掉它们，只上报逻辑字节。
 * 这里按同样的 NEC 时序把字节还原成完整波形，交给 rc-core 的软件解码器。
 */
static void m5io_hub_ir_rx(void *data, const u8 *buf, unsigned int len)
{
  struct m5io_hub_ir *ir = data;
  unsigned int byte, bit;

  if (!ir || !buf || !len)
    return;
  if (len > M5IO_IR_MAX_BYTES)
    return;
  if (READ_ONCE(ir->tx_active))
    return;

  m5io_hub_ir_store(ir->rc, M5IO_IR_LEADER_MARK_US, true);
  m5io_hub_ir_store(ir->rc, M5IO_IR_LEADER_SPACE_US, false);
  for (byte = 0; byte < len; byte++) {
    for (bit = 0; bit < 8; bit++) {
      bool one = !!(buf[byte] & (0x80U >> bit));

      m5io_hub_ir_store(ir->rc, M5IO_IR_BIT_MARK_US, true);
      m5io_hub_ir_store(ir->rc,
                        one ? M5IO_IR_BIT_ONE_SPACE_US :
                              M5IO_IR_BIT_ZERO_SPACE_US,
                        false);
    }
  }
  /*
   * 协议规定终止 MARK 不属于逻辑数据，但解码器需要它标记数据结束；后面再补
   * 一段 SPACE，rc-core 的 NEC 解码器只有看到“终止 MARK + 足够长的 SPACE”
   * 才会收帧（真实接收头同样会把这帧结束后的空闲间隙报成 space）。
   */
  m5io_hub_ir_store(ir->rc, M5IO_IR_BIT_MARK_US, true);
  m5io_hub_ir_store(ir->rc, M5IO_IR_TRAILER_SPACE_US, false);
  ir_raw_event_handle(ir->rc);
}

/*
 * TX：rc-core 交来的是 LIRC pulse 模式的时长数组，奇数个元素、MARK 开头、
 * 终止 MARK 结尾。解成逻辑字节后由 MCU 重新加引导码并按当前时序调制。
 */
static int m5io_hub_ir_tx(struct rc_dev *rc, unsigned int *txbuf,
                          unsigned int count)
{
  struct m5io_hub_ir *ir = rc->priv;
  u8 data[M5IO_IR_MAX_BYTES] = {};
  unsigned int bits, bytes, index;
  int ret = 0;

  /* 布局：[leader mark][leader space]([bit mark][bit space])*N[trail mark] */
  if (count < 5 || !(count & 1))
    return -EINVAL;
  bits = (count - 3) / 2;
  if (!bits || (bits & 7) || bits / 8 > M5IO_IR_MAX_BYTES)
    return -EINVAL;
  bytes = bits / 8;

  if (!m5io_hub_ir_is_leader_mark(txbuf[0]) ||
      !m5io_hub_ir_is_leader_space(txbuf[1]) ||
      !m5io_hub_ir_is_bit_mark(txbuf[count - 1]))
    return -EINVAL;

  for (index = 0; index < bits; index++) {
    unsigned int mark = txbuf[2 + index * 2];
    unsigned int space = txbuf[3 + index * 2];
    bool one;

    if (!m5io_hub_ir_is_bit_mark(mark) ||
        !m5io_hub_ir_decode_bit_space(space, &one))
      return -EINVAL;
    if (one)
      data[index / 8] |= (u8)(0x80U >> (index % 8));
  }

  mutex_lock(&ir->lock);
  WRITE_ONCE(ir->tx_active, true);

  /*
   * 固件规定 TX 必须在接收关闭时才开始（否则 BUSY）。rc-core 在设备打开
   * 期间一直开着 RX，所以这里临时关一次、发完再恢复。
   */
  if (ir->rx_running) {
    ret = m5io_hub_ir_stop_reace(ir->hub);
    if (!ret)
      ir->rx_running = false;
  }
  if (!ret)
    ret = m5io_hub_ir_send_data(ir->hub, data, bytes);

  if (!ir->rx_running && ir->rx_registered) {
    int restart = m5io_hub_ir_start_reace(ir->hub);

    if (restart) {
      dev_warn_ratelimited(&rc->dev,
                           "failed to restart IR receiver: %d\n", restart);
      if (!ret)
        ret = restart;
    } else {
      ir->rx_running = true;
    }
  }

  WRITE_ONCE(ir->tx_active, false);
  mutex_unlock(&ir->lock);

  /* rc-core 要求返回实际发送的脉冲时长个数。 */
  return ret ? ret : (int)count;
}

static int m5io_hub_ir_set_carrier(struct rc_dev *rc, u32 carrier)
{
  struct m5io_hub_ir *ir = rc->priv;
  int ret, restart;

  if (carrier < M5IO_IR_CARRIER_MIN_HZ ||
      carrier > M5IO_IR_CARRIER_MAX_HZ)
    return -EINVAL;

  mutex_lock(&ir->lock);
  if (!ir->rx_running) {
    /* 设备没打开：只记住载波，下一次 open 时和时序一起下发。 */
    ir->carrier_hz = carrier;
    mutex_unlock(&ir->lock);
    return 0;
  }

  /* 重配必须让红外空闲：关 RX -> CONFIG -> 开 RX。 */
  ret = m5io_hub_ir_stop_reace(ir->hub);
  if (ret) {
    mutex_unlock(&ir->lock);
    return ret;
  }
  ir->rx_running = false;

  ret = m5io_hub_ir_config(ir->hub, carrier);
  if (!ret)
    ir->carrier_hz = carrier;

  restart = m5io_hub_ir_start_reace(ir->hub);
  if (restart) {
    dev_warn_ratelimited(&rc->dev, "failed to re-enable IR receiver: %d\n",
                         restart);
    if (!ret)
      ret = restart;
  } else {
    ir->rx_running = true;
  }

  mutex_unlock(&ir->lock);
  return ret;
}

static int m5io_hub_ir_open(struct rc_dev *rc)
{
  struct m5io_hub_ir *ir = rc->priv;
  int ret;

  mutex_lock(&ir->lock);
  /* 按当前载波重新配置一次：上次 close 时已经 reset，配置不再可信。 */
  ret = m5io_hub_ir_config(ir->hub, ir->carrier_hz);
  if (ret)
    goto out_unlock;

  if (!ir->rx_registered) {
    ret = m5io_hub_ir_register_rx(ir->hub, m5io_hub_ir_rx, ir);
    if (ret)
      goto out_unlock;
    ir->rx_registered = true;
  }

  ret = m5io_hub_ir_start_reace(ir->hub);
  if (ret) {
    m5io_hub_ir_unregister_rx(ir->hub);
    ir->rx_registered = false;
    goto out_unlock;
  }
  ir->rx_running = true;
  dev_dbg(&rc->dev, "receiver enabled, carrier %u Hz\n", ir->carrier_hz);

out_unlock:
  mutex_unlock(&ir->lock);
  return ret;
}

static void m5io_hub_ir_close(struct rc_dev *rc)
{
  struct m5io_hub_ir *ir = rc->priv;

  mutex_lock(&ir->lock);
  if (ir->rx_running) {
    m5io_hub_ir_stop_reace(ir->hub);
    ir->rx_running = false;
  }
  if (ir->rx_registered) {
    m5io_hub_ir_unregister_rx(ir->hub);
    ir->rx_registered = false;
  }
  /* 关掉收发并清配置，避免离开的用户态进程留下一个开着的发射通道。 */
  m5io_hub_ir_reset(ir->hub);
  mutex_unlock(&ir->lock);
}

static int m5io_hub_ir_probe(struct platform_device *pdev)
{
  struct device *dev = &pdev->dev;
  struct device_node *np = dev->of_node;
  struct m5io_hub_ir *ir;
  struct m5io_hub *hub;
  struct rc_dev *rc;
  u32 carrier = M5IO_IR_DEFAULT_CARRIER_HZ;
  int ret;

  hub = dev_get_drvdata(dev->parent);
  if (!hub)
    return -EPROBE_DEFER;

  ir = devm_kzalloc(dev, sizeof(*ir), GFP_KERNEL);
  if (!ir)
    return -ENOMEM;
  mutex_init(&ir->lock);
  ir->hub = hub;

  if (np) {
    ret = of_property_read_u32(np, "carrier-frequency-hz", &carrier);
    if (!ret) {
      if (carrier < M5IO_IR_CARRIER_MIN_HZ ||
          carrier > M5IO_IR_CARRIER_MAX_HZ)
        return dev_err_probe(dev, -EINVAL,
                             "carrier-frequency-hz %u out of range\n",
                             carrier);
    } else if (ret != -EINVAL && ret != -ENODATA) {
      return dev_err_probe(dev, ret, "failed to read carrier frequency\n");
    }
  }
  ir->carrier_hz = carrier;

  rc = devm_rc_allocate_device(dev, RC_DRIVER_IR_RAW);
  if (!rc)
    return -ENOMEM;
  rc->priv = ir;
  rc->driver_name = M5IO_IR_DRIVER_NAME;
  rc->device_name = M5IO_IR_DEVICE_NAME;
  /*
   * 逻辑字节由驱动按固定 NEC 时序还原，只有 NEC 家族的软件解码器对得上；
   * 原始波形回放（LIRC raw）不受 allowed_protocols 限制。
   */
  rc->allowed_protocols = RC_PROTO_BIT_NEC | RC_PROTO_BIT_NECX |
                          RC_PROTO_BIT_NEC32;
  rc->map_name = np ? of_get_property(np, "linux,rc-map-name", NULL) : NULL;
  if (!rc->map_name)
    rc->map_name = RC_MAP_EMPTY;
  /* 驱动上报的时长单位是 1 µs，没有硬件超时。 */
  rc->rx_resolution = 1;
  rc->min_timeout = 1;
  rc->timeout = IR_DEFAULT_TIMEOUT;
  rc->max_timeout = 10 * IR_DEFAULT_TIMEOUT;
  rc->open = m5io_hub_ir_open;
  rc->close = m5io_hub_ir_close;
  rc->s_tx_carrier = m5io_hub_ir_set_carrier;
  rc->tx_ir = m5io_hub_ir_tx;
  ir->rc = rc;

  ret = devm_rc_register_device(dev, rc);
  if (ret)
    return dev_err_probe(dev, ret, "failed to register rc device\n");

  platform_set_drvdata(pdev, ir);
  dev_info(dev, "M5IO-HUB IR registered, carrier %u Hz, max %u bytes/frame\n",
           ir->carrier_hz, M5IO_IR_MAX_BYTES);
  return 0;
}

static void m5io_hub_ir_remove(struct platform_device *pdev)
{
  struct m5io_hub_ir *ir = platform_get_drvdata(pdev);

  if (!ir)
    return;
  mutex_lock(&ir->lock);
  if (ir->rx_running) {
    m5io_hub_ir_stop_reace(ir->hub);
    ir->rx_running = false;
  }
  if (ir->rx_registered) {
    m5io_hub_ir_unregister_rx(ir->hub);
    ir->rx_registered = false;
  }
  m5io_hub_ir_reset(ir->hub);
  mutex_unlock(&ir->lock);
}

static const struct of_device_id m5io_hub_ir_of_match[] = {
    {.compatible = "m5stack,m5io-hub-ir"},
    {},
};
MODULE_DEVICE_TABLE(of, m5io_hub_ir_of_match);

static struct platform_driver m5io_hub_ir_driver = {
    .probe = m5io_hub_ir_probe,
    .remove = m5io_hub_ir_remove,
    .driver =
        {
            .name = M5IO_IR_DRIVER_NAME,
            .of_match_table = m5io_hub_ir_of_match,
        },
};
module_platform_driver(m5io_hub_ir_driver);

MODULE_DESCRIPTION("M5Stack M5IO-HUB infrared rc-core bridge");
MODULE_LICENSE("GPL");
