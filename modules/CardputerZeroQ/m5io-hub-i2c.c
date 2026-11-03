// SPDX-License-Identifier: GPL-2.0
/*
 * M5Stack M5IO-HUB I2C bridge driver
 *
 * 新 API 把 FUNC=2 的 TOKEN（配置/复位）与 DATA 通道（0xAA 写、0xAB 读/合并
 * 写读、0xAC Quick）都收进了 core：驱动只交 Linux i2c_msg 数组和总线号，
 * 不再自己拼 payload、选 DATA 通道或校验应答。
 *
 * 与旧接口的差异（迁移要点）：
 *   m5io_hub_exec(FUNC=2, ARG=CONFIG)      -> m5io_hub_i2c_config()
 *   m5io_hub_i2c_data_sync(chn, ...)       -> m5io_hub_i2c_transfer(hub, bus, msgs, num)
 *   （新 API 增加了 bus 参数，两路适配器不再靠“最近一次配置”猜总线）
 *   新增 m5io_hub_i2c_quick()/quick_write()/quick_read()：协议这次真的支持
 *   SMBus Quick，适配器可以 advertise I2C_FUNC_SMBUS_QUICK，i2cdetect 默认的
 *   地址探测走 0xAC 而不是 0 字节写。
 *
 * 每次传输前重放一次 CONFIG 是旧驱动就有的自愈手段：MCU_RESET/OTA 会清掉
 * FUNC=2 的 configured 位，而 Linux 适配器对象不会随之重建；重放 CONFIG
 * 能让总线在 MCU 复位后无需重新 probe 即可恢复。CONFIG 只作用于自己的
 * 总线，两路适配器并发时不会互相干扰。
 */

#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#include "m5io-hub.h"

#define M5IO_I2C_BUS_COUNT 2U
/* 协议 DATA 事务的外层预算，沿用旧驱动的宽松取值。 */
#define M5IO_I2C_ADAPTER_TIMEOUT_MS 27500U

struct m5io_hub_i2c_bus {
  struct m5io_hub *hub;
  struct i2c_adapter adap;
  u8 bus;
  u8 speed;
};

struct m5io_hub_i2c {
  struct m5io_hub_i2c_bus buses[M5IO_I2C_BUS_COUNT];
};

static int m5io_hub_i2c_replay_config(struct m5io_hub_i2c_bus *bus)
{
  return m5io_hub_i2c_config(bus->hub, bus->bus, bus->speed);
}

/* SMBus Quick：i2c 核心会用一条零长度消息把它模拟到 master_xfer。 */
static int m5io_hub_i2c_quick_msg(struct m5io_hub_i2c_bus *bus,
                                  struct i2c_msg *msg)
{
  bool read = !!(msg->flags & I2C_M_RD);
  int ret;

  if (msg->flags & ~(I2C_M_RD | I2C_M_DMA_SAFE))
    return -EOPNOTSUPP;
  if (msg->addr < 0x08 || msg->addr > 0x77)
    return -EINVAL;

  ret = m5io_hub_i2c_replay_config(bus);
  if (ret)
    return ret;

  /* 0 = 从机 ACK；-ENXIO = NAK/无应答（i2cdetect 需要的语义）。 */
  ret = m5io_hub_i2c_quick(bus->hub, bus->bus, msg->addr, read);
  if (ret)
    return ret;
  return 1;
}

/*
 * 协议只认 7 bit 地址的普通写/读。10 bit 地址、NOSTART、RECV_LEN 等特性
 * 协议里都没有对应字段，提前拒绝，避免把无意义 flags 交给 core。
 */
static int m5io_hub_i2c_validate(struct i2c_msg *msgs, int num)
{
  const u16 supported = I2C_M_RD | I2C_M_DMA_SAFE;
  int i;

  for (i = 0; i < num; i++) {
    if (msgs[i].flags & ~supported)
      return -EOPNOTSUPP;
    if (msgs[i].addr < 0x08 || msgs[i].addr > 0x77)
      return -EINVAL;
    if (msgs[i].len && !msgs[i].buf)
      return -EINVAL;
  }
  return 0;
}

static int m5io_hub_i2c_xfer(struct i2c_adapter *adap, struct i2c_msg *msgs,
                             int num)
{
  struct m5io_hub_i2c_bus *bus = i2c_get_adapdata(adap);
  int ret;

  if (!bus || !bus->hub)
    return -ENODEV;
  if (!msgs || num < 1 || num > 2)
    return -EOPNOTSUPP;

  /* 协议里 Quick 只走地址阶段，DATA 帧最小长度 2 字节，不能当普通写发。 */
  if (num == 1 && !msgs[0].len)
    return m5io_hub_i2c_quick_msg(bus, &msgs[0]);

  ret = m5io_hub_i2c_validate(msgs, num);
  if (ret)
    return ret;

  /* 先把本总线重新配置一次：既保证速率正确，也覆盖 MCU 复位后的掉配置。 */
  ret = m5io_hub_i2c_replay_config(bus);
  if (ret)
    return ret;

  ret = m5io_hub_i2c_transfer(bus->hub, bus->bus, msgs, num);
  if (ret < 0)
    return ret;
  return num;
}

static u32 m5io_hub_i2c_functionality(struct i2c_adapter *adap)
{
  /*
   * Quick 由本驱动实现；其余 SMBus 操作靠 i2c 核心用 master_xfer 模拟，
   * 所以这里列出的都是模拟得出来的类型。块读需要 I2C_M_RECV_LEN，协议
   * 没有对应字段，不 advertise。
   */
  return I2C_FUNC_I2C | I2C_FUNC_SMBUS_QUICK | I2C_FUNC_SMBUS_BYTE |
         I2C_FUNC_SMBUS_BYTE_DATA | I2C_FUNC_SMBUS_WORD_DATA |
         I2C_FUNC_SMBUS_PROC_CALL | I2C_FUNC_SMBUS_WRITE_BLOCK_DATA |
         I2C_FUNC_SMBUS_I2C_BLOCK;
}

static const struct i2c_adapter_quirks m5io_hub_i2c_quirks = {
    /* 组合读只支持“先写后读”；零长度消息留给 Quick（已去掉 NO_ZERO_LEN）。 */
    .flags = I2C_AQ_COMB_WRITE_THEN_READ,
    .max_num_msgs = 2,
    /* 0xAA/0xAB 各占 1 字节操作码 + 1 字节地址；组合读末尾再占 1 字节读长。 */
    .max_write_len = 253,
    .max_read_len = 253,
    .max_comb_1st_msg_len = 252,
    .max_comb_2nd_msg_len = 253,
};

static const struct i2c_algorithm m5io_hub_i2c_algo = {
    .master_xfer = m5io_hub_i2c_xfer,
    .functionality = m5io_hub_i2c_functionality,
};

static int m5io_hub_i2c_bus_setup(struct platform_device *pdev,
                                  struct m5io_hub_i2c_bus *bus,
                                  unsigned int id, struct device_node *node)
{
  struct device *dev = &pdev->dev;
  u32 frequency = I2C_MAX_STANDARD_MODE_FREQ;
  u8 speed = 0;
  int ret;

  bus->hub = dev_get_drvdata(dev->parent);
  if (!bus->hub)
    return -EPROBE_DEFER;
  bus->bus = (u8)id;

  if (node && of_find_property(node, "clock-frequency", NULL)) {
    ret = of_property_read_u32(node, "clock-frequency", &frequency);
    if (ret)
      return dev_err_probe(dev, ret,
                           "I2C%u clock-frequency read failed\n", id);
  }
  if (frequency == I2C_MAX_FAST_MODE_FREQ)
    speed = 1;
  else if (frequency != I2C_MAX_STANDARD_MODE_FREQ)
    return dev_err_probe(dev, -EINVAL,
                         "I2C%u supports only 100/400 kHz, got %u Hz\n", id,
                         frequency);
  bus->speed = speed;

  ret = m5io_hub_i2c_config(bus->hub, bus->bus, speed);
  if (ret)
    return dev_err_probe(dev, ret, "I2C%u configuration failed\n", id);

  bus->adap.owner = THIS_MODULE;
  bus->adap.algo = &m5io_hub_i2c_algo;
  bus->adap.quirks = &m5io_hub_i2c_quirks;
  bus->adap.dev.parent = dev;
  bus->adap.timeout = msecs_to_jiffies(M5IO_I2C_ADAPTER_TIMEOUT_MS);
  bus->adap.retries = 0;
  if (node) {
    /*
     * 适配器要靠 of_node 才能实例化 i2c@1/2 下面的 client（TCA8418 等）。
     * 这个引用由父 platform_device 的 DT 生命周期持有，适配器先于父设备
     * 销毁，所以这里直接引用即可，不需要额外 of_node_get()。
     */
    bus->adap.dev.of_node = node;
  }
  snprintf(bus->adap.name, sizeof(bus->adap.name), "m5io-hub-i2c%u", id);
  i2c_set_adapdata(&bus->adap, bus);

  ret = devm_i2c_add_adapter(dev, &bus->adap);
  if (ret)
    return ret;

  dev_info(dev, "I2C%u registered as i2c-%d at %s\n", id, bus->adap.nr,
           speed ? "400 kHz" : "100 kHz");
  return 0;
}

static int m5io_hub_i2c_probe(struct platform_device *pdev)
{
  struct device *dev = &pdev->dev;
  struct m5io_hub_i2c *mi;
  unsigned long enabled = 0;
  unsigned long registered = 0;
  unsigned int id;
  int ret;

  if (!dev_get_drvdata(dev->parent))
    return -EPROBE_DEFER;

  mi = devm_kzalloc(dev, sizeof(*mi), GFP_KERNEL);
  if (!mi)
    return -ENOMEM;

  if (of_get_child_count(dev->of_node)) {
    for_each_available_child_of_node_scoped(dev->of_node, node) {
      ret = of_property_read_u32(node, "reg", &id);
      if (ret || id < 1 || id > M5IO_I2C_BUS_COUNT ||
          test_and_set_bit(id - 1, &enabled))
        return dev_err_probe(dev, -EINVAL, "invalid I2C child node\n");

      ret = m5io_hub_i2c_bus_setup(pdev, &mi->buses[id - 1], id, node);
      /*
       * 板级可能只接了其中一路 MCU I2C。DT 声明的另一路在 MCU 上不可用时，
       * 记一条告警继续注册可用总线；DT 本身写错或驱动类错误仍然硬失败。
       */
      if (ret == -ETIMEDOUT || ret == -ENXIO || ret == -EIO) {
        dev_warn(dev, "I2C%u unavailable (%d); continuing\n", id, ret);
        continue;
      }
      if (ret)
        return ret;
      set_bit(id - 1, &registered);
    }
    if (!registered)
      return -ENODEV;
  } else {
    /* 兼容旧 overlay：没有子节点时把两路都按 100 kHz 暴露出来。 */
    for (id = 1; id <= M5IO_I2C_BUS_COUNT; id++) {
      ret = m5io_hub_i2c_bus_setup(pdev, &mi->buses[id - 1], id, NULL);
      if (ret)
        return ret;
    }
  }

  platform_set_drvdata(pdev, mi);
  return 0;
}

static const struct of_device_id m5io_hub_i2c_of_match[] = {
    {.compatible = "m5stack,m5io-hub-i2c"},
    {},
};
MODULE_DEVICE_TABLE(of, m5io_hub_i2c_of_match);

static struct platform_driver m5io_hub_i2c_driver = {
    .driver =
        {
            .name = "m5io-hub-i2c",
            .of_match_table = m5io_hub_i2c_of_match,
        },
    .probe = m5io_hub_i2c_probe,
};
module_platform_driver(m5io_hub_i2c_driver);

MODULE_DESCRIPTION("M5Stack M5IO-HUB I2C bridge driver");
MODULE_LICENSE("GPL");
