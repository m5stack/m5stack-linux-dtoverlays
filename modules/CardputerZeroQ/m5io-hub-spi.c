// SPDX-License-Identifier: GPL-2.0
/*
 * M5Stack M5IO-HUB SPI host controller driver
 *
 * 新 API 把 FUNC=3 的全部协议细节收进了 core：配置、开窗/收尾、按 DATA 通道
 * 分片、MOSI 补零、MISO 回填、16 bit 模式下的大端拆拼、以及按 cs_change 的
 * CS 分组，都由 m5io_hub_spi_transfer() 一次完成。本驱动只负责 Linux SPI
 * 框架这一侧的接线：
 *   - 每路 SPI 总线（bus 0..3）一个 platform_device + 一个 spi_controller；
 *   - 在 setup 阶段挡掉 MCU 无法实现的能力（CS 时序、非 8/16 位字宽等），
 *     让错误在注册 spi_device 时暴露，而不是等到第一次传输；
 *   - transfer_one_message 直接转调 m5io_hub_spi_transfer()；遇到 -EBUSY
 *     按 API 约定用 m5io_hub_spi_flush() 清掉 MCU 侧未收尾的交换。
 *
 * 与旧接口的差异（迁移要点）：
 *   m5io_hub_exec(FUNC=3, ARG=CONFIG/DISABLE) + m5io_hub_send_spi_sync()
 *     -> m5io_hub_spi_transfer(hub, bus, msg)
 *        m5io_hub_spi_reset(hub, bus)   （旧 DISABLE）
 *        m5io_hub_spi_flush(hub, bus)   （旧 BUSY 恢复路径）
 *   旧驱动里的 m5io_hub_spi_configure/exchange/pack/unpack/validate 全部下移
 *   core；驱动里不再出现 struct m5io_hub_request、通道号或帧格式。
 *   SPI 总线的编号来自 DT（缺省 bus 0），不再假设“只有一路 SPI”。
 */

#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/spi/spi.h>

#include "m5io-hub.h"

#define M5IO_SPI_MIN_HZ       250000U
#define M5IO_SPI_MAX_HZ       20000000U
/* 协议单次交换上限：SPI_BEGIN 的 len 最大 512 字节。 */
#define M5IO_SPI_MAX_TRANSFER 512U
/* FUNC=3 支持的 SPI 总线数：bus 0..3 依次占用 DATA CH1..CH4。 */
#define M5IO_SPI_BUS_COUNT    4U

struct m5io_hub_spi {
  struct m5io_hub *hub;
  u8 bus;
};

static size_t m5io_hub_spi_max_transfer_size(struct spi_device *spi)
{
  return M5IO_SPI_MAX_TRANSFER;
}

/*
 * 设备级能力校验。协议层还会再校验一次每条 transfer（speed/字宽/CS 时序），
 * 但在这里提前拒绝可以把错误留在注册设备阶段，且能挡住 core 不认识的
 * cs_setup/cs_hold/cs_inactive/word_delay —— MCU 自己产生 CS 时序。
 */
static int m5io_hub_spi_setup(struct spi_device *spi)
{
  if (spi_get_chipselect(spi, 0))
    return -EINVAL;
  if (spi->mode & ~(SPI_CPOL | SPI_CPHA))
    return -EINVAL;
  if (spi->bits_per_word != 8 && spi->bits_per_word != 16)
    return -EINVAL;
  if (spi->max_speed_hz < M5IO_SPI_MIN_HZ)
    return -EINVAL;
  if (spi->word_delay.value || spi->cs_setup.value || spi->cs_hold.value ||
      spi->cs_inactive.value)
    return -EOPNOTSUPP;
  return 0;
}

static int m5io_hub_spi_transfer_message(struct spi_controller *ctlr,
                                        struct spi_message *msg)
{
  struct m5io_hub_spi *ms = spi_controller_get_devdata(ctlr);
  int ret, flush_ret;

  msg->actual_length = 0;
  ret = m5io_hub_spi_transfer(ms->hub, ms->bus, msg);
  if (ret < 0 && !msg->status)
    msg->status = ret;

  /*
   * API 约定：BEGIN 时 MCU 还持有上一笔未收尾的交换会回 -EBUSY。
   * SPI_FLUSH 是协议里唯一能解掉该状态的命令（不发它就只能等 MCU 复位），
   * 这里清一次并把原始错误交给上层；不自动重发，避免对可能已经部分
   * 上线的 MOSI 造成二次副作用。
   */
  if (ret == -EBUSY) {
    flush_ret = m5io_hub_spi_flush(ms->hub, ms->bus);
    dev_err_ratelimited(&ctlr->dev,
                        "SPI bus %u transfer is BUSY, flush returned %d\n",
                        ms->bus, flush_ret);
  }

  spi_finalize_current_message(ctlr);
  return 0;
}

/*
 * 总线编号：新协议支持 bus 0..3，一路总线一个控制器节点。DT 里显式写
 * m5stack,spi-bus（或节点 reg）时用它，否则按板级现状默认 bus 0 —— 这样
 * 不加新属性也能继续用现有的单总线 overlay。
 */
static int m5io_hub_spi_of_bus(struct platform_device *pdev, u8 *bus)
{
  struct device_node *np = pdev->dev.of_node;
  u32 value;
  int ret;

  *bus = 0;
  if (!np)
    return 0;

  ret = of_property_read_u32(np, "m5stack,spi-bus", &value);
  if (ret == -EINVAL || ret == -ENODATA)
    ret = of_property_read_u32(np, "reg", &value);
  if (ret == -EINVAL || ret == -ENODATA)
    return 0;
  if (ret)
    return ret;
  if (value >= M5IO_SPI_BUS_COUNT)
    return -EINVAL;

  *bus = (u8)value;
  return 0;
}

static int m5io_hub_spi_probe(struct platform_device *pdev)
{
  struct device *dev = &pdev->dev;
  struct m5io_hub_spi *ms;
  struct spi_controller *ctlr;
  struct m5io_hub *hub;
  u8 bus;
  int ret;

  /* struct m5io_hub 是不透明句柄：父设备没挂 drvdata 就说明 core 还没起。 */
  hub = dev_get_drvdata(dev->parent);
  if (!hub)
    return -EPROBE_DEFER;

  ret = m5io_hub_spi_of_bus(pdev, &bus);
  if (ret)
    return dev_err_probe(dev, ret, "invalid SPI bus index\n");

  ctlr = devm_spi_alloc_host(dev, sizeof(*ms));
  if (!ctlr)
    return -ENOMEM;
  ms = spi_controller_get_devdata(ctlr);
  ms->hub = hub;
  ms->bus = bus;

  ctlr->dev.of_node = dev->of_node;
  ctlr->bus_num = -1;
  ctlr->num_chipselect = 1;
  ctlr->mode_bits = SPI_CPOL | SPI_CPHA;
  ctlr->bits_per_word_mask = SPI_BPW_MASK(8) | SPI_BPW_MASK(16);
  ctlr->min_speed_hz = M5IO_SPI_MIN_HZ;
  ctlr->max_speed_hz = M5IO_SPI_MAX_HZ;
  ctlr->max_transfer_size = m5io_hub_spi_max_transfer_size;
  ctlr->setup = m5io_hub_spi_setup;
  ctlr->transfer_one_message = m5io_hub_spi_transfer_message;

  platform_set_drvdata(pdev, ctlr);
  ret = devm_spi_register_controller(dev, ctlr);
  if (ret)
    return dev_err_probe(dev, ret, "failed to register SPI controller\n");

  dev_info(dev, "M5IO-HUB SPI bus %u registered as spi%u\n", bus,
           ctlr->bus_num);
  return 0;
}

static void m5io_hub_spi_remove(struct platform_device *pdev)
{
  struct spi_controller *ctlr = platform_get_drvdata(pdev);
  struct m5io_hub_spi *ms;
  int ret;

  if (!ctlr)
    return;
  ms = spi_controller_get_devdata(ctlr);
  /* 旧 DISABLE：释放 MCU 侧总线状态，避免模块重载后仍留着旧配置。 */
  ret = m5io_hub_spi_reset(ms->hub, ms->bus);
  if (ret)
    dev_warn(&pdev->dev, "failed to disable SPI bus %u: %d\n", ms->bus, ret);
}

static const struct of_device_id m5io_hub_spi_of_match[] = {
    {.compatible = "m5stack,m5io-hub-spi"},
    {},
};
MODULE_DEVICE_TABLE(of, m5io_hub_spi_of_match);

static struct platform_driver m5io_hub_spi_driver = {
    .driver =
        {
            .name = "m5io-hub-spi",
            .of_match_table = m5io_hub_spi_of_match,
        },
    .probe = m5io_hub_spi_probe,
    .remove = m5io_hub_spi_remove,
};
module_platform_driver(m5io_hub_spi_driver);

MODULE_ALIAS("platform:m5io-hub-spi");
MODULE_DESCRIPTION("M5Stack M5IO-HUB FUNC3 SPI controller");
MODULE_LICENSE("GPL");
