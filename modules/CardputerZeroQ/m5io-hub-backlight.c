// SPDX-License-Identifier: GPL-2.0-only
/*
 * M5IO-HUB LCD backlight driver
 *
 * core 的 m5io_hub_pwm_config(freq, brightness) 一条调用完成协议里的两步：
 * ① FUNC=5 ARG=CONFIG 下发 PWM 频率 ② FUNC=5 ARG=SET 下发 0..255 亮度。
 * 协议没有读回命令，所以 Linux backlight 类是权威缓存；驱动只缓存“最后
 * 一次下发成功”的亮度用于幂等判断，频率则随每次 pwm_config() 的 CONFIG
 * 步骤一起重发。
 */

#include <linux/backlight.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/types.h>

#include "m5io-hub.h"

#define M5IO_HUB_BACKLIGHT_MAX_BRIGHTNESS   255U
/* MCU 上电默认 30 kHz；DT 没给频率时用它作为 Linux 侧缓存的频率值。 */
#define M5IO_HUB_BACKLIGHT_DEFAULT_FREQ_HZ  30000U
#define M5IO_HUB_BACKLIGHT_MIN_FREQ_HZ      1U
#define M5IO_HUB_BACKLIGHT_MAX_FREQ_HZ      65535U

struct m5io_hub_backlight {
  struct m5io_hub *hub;
  struct backlight_device *bl;
  u32 frequency_hz;
  u8 applied_brightness;
};

static int m5io_hub_backlight_apply(struct m5io_hub_backlight *backlight,
                                    u8 brightness)
{
  int ret;

  ret = m5io_hub_pwm_config(backlight->hub, backlight->frequency_hz,
                            brightness);
  if (ret)
    return ret;

  WRITE_ONCE(backlight->applied_brightness, brightness);
  return 0;
}

/*
 * backlight 核心在 bd->update_lock mutex 下调用 update_status（见
 * backlight.h 的 update_lock 注释），所以这里可以走会睡眠的 core API；
 * pwm_config 内部是两条 TOKEN + 等待完成。
 */
static int m5io_hub_backlight_update_status(struct backlight_device *bl)
{
  struct m5io_hub_backlight *backlight = bl_get_data(bl);

  might_sleep();
  u8 brightness = backlight_get_brightness(bl);

  /* backlight 核心已经用 update_lock 串行化，这里再省掉重复下发。 */
  if (brightness == READ_ONCE(backlight->applied_brightness))
    return 0;

  return m5io_hub_backlight_apply(backlight, brightness);
}

static int m5io_hub_backlight_get_brightness(struct backlight_device *bl)
{
  struct m5io_hub_backlight *backlight = bl_get_data(bl);

  /* FUNC=5 没有读命令：返回最后一次成功下发的值。 */
  return READ_ONCE(backlight->applied_brightness);
}

static const struct backlight_ops m5io_hub_backlight_ops = {
    .options = BL_CORE_SUSPENDRESUME,
    .update_status = m5io_hub_backlight_update_status,
    .get_brightness = m5io_hub_backlight_get_brightness,
};

static int m5io_hub_backlight_probe(struct platform_device *pdev)
{
  struct device *dev = &pdev->dev;
  struct m5io_hub_backlight *backlight;
  struct backlight_properties properties = {};
  struct m5io_hub *hub;
  u32 default_brightness = M5IO_HUB_BACKLIGHT_MAX_BRIGHTNESS;
  u32 frequency_hz = M5IO_HUB_BACKLIGHT_DEFAULT_FREQ_HZ;
  bool configure_frequency = false;
  int ret;

  hub = dev_get_drvdata(dev->parent);
  if (!hub)
    return -EPROBE_DEFER;

  backlight = devm_kzalloc(dev, sizeof(*backlight), GFP_KERNEL);
  if (!backlight)
    return -ENOMEM;
  backlight->hub = hub;
  backlight->frequency_hz = frequency_hz;

  ret = device_property_read_u32(dev, "default-brightness-level",
                                 &default_brightness);
  if (ret && ret != -EINVAL && ret != -ENODATA)
    return dev_err_probe(dev, ret, "failed to read default brightness\n");
  if (default_brightness > M5IO_HUB_BACKLIGHT_MAX_BRIGHTNESS)
    return dev_err_probe(dev, -EINVAL,
                         "default brightness %u exceeds %u\n",
                         default_brightness,
                         M5IO_HUB_BACKLIGHT_MAX_BRIGHTNESS);

  /*
   * DT 显式给出频率时按它配置；否则按 MCU 上电默认的 30 kHz 处理。core API 只有
   * “CONFIG + SET” 一条组合接口，没有单独下发亮度的入口，所以无论 DT 是否
   * 给频率，首次亮度都必须经 pwm_config() 下发，频率与 MCU 默认值保持一致。
   */
  ret = device_property_read_u32(dev, "pwm-frequency-hz", &frequency_hz);
  if (!ret) {
    if (frequency_hz < M5IO_HUB_BACKLIGHT_MIN_FREQ_HZ ||
        frequency_hz > M5IO_HUB_BACKLIGHT_MAX_FREQ_HZ)
      return dev_err_probe(dev, -EINVAL,
                           "pwm-frequency-hz %u out of range\n",
                           frequency_hz);
    configure_frequency = true;
  } else if (ret != -EINVAL && ret != -ENODATA) {
    return dev_err_probe(dev, ret, "failed to read PWM frequency\n");
  }

  backlight->frequency_hz = frequency_hz;

  /*
   * 首次下发：配置频率并设置初始亮度。即使 DT 没给频率也必须走一次，
   * 因为初始亮度只能通过 m5io_hub_pwm_config() 的 SET 步骤下发；CONFIG
   * 带的就是 MCU 现有默认值，重复下发无副作用。
   */
  ret = m5io_hub_backlight_apply(backlight, (u8)default_brightness);
  if (ret)
    return dev_err_probe(dev, ret, "failed to apply backlight state\n");
  dev_dbg(dev, "initial backlight state: %u Hz, brightness %u%s\n",
          frequency_hz, default_brightness,
          configure_frequency ? "" : " (MCU default frequency kept)");

  properties.type = BACKLIGHT_RAW;
  properties.max_brightness = M5IO_HUB_BACKLIGHT_MAX_BRIGHTNESS;
  properties.brightness = default_brightness;
  properties.power = BACKLIGHT_POWER_ON;

  backlight->bl = devm_backlight_device_register(
      dev, dev_name(dev), dev, backlight, &m5io_hub_backlight_ops,
      &properties);
  if (IS_ERR(backlight->bl))
    return dev_err_probe(dev, PTR_ERR(backlight->bl),
                         "failed to register backlight\n");

  platform_set_drvdata(pdev, backlight);
  dev_info(dev, "M5IO-HUB backlight registered, PWM %u Hz, brightness %u\n",
           backlight->frequency_hz, backlight->applied_brightness);
  return 0;
}

static const struct of_device_id m5io_hub_backlight_of_match[] = {
    {.compatible = "m5stack,m5io-hub-backlight"},
    {},
};
MODULE_DEVICE_TABLE(of, m5io_hub_backlight_of_match);

static struct platform_driver m5io_hub_backlight_driver = {
    .probe = m5io_hub_backlight_probe,
    .driver = {
        .name = "m5io-hub-backlight",
        .of_match_table = m5io_hub_backlight_of_match,
    },
};

module_platform_driver(m5io_hub_backlight_driver);

MODULE_AUTHOR("M5Stack");
MODULE_DESCRIPTION("M5IO-HUB LCD backlight driver");
MODULE_LICENSE("GPL");
