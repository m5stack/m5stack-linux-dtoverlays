// SPDX-License-Identifier: GPL-2.0-only
/*
 * M5IO-HUB LCD backlight driver
 *
 * The MCU exposes the fixed PA8/TIM1_CH1 backlight as FUNC=5.  Brightness is
 * sent as a logical 0..255 value with ARG=BACKLIGHT_SET (0x2).  The protocol
 * deliberately has no read command, so the Linux backlight class value and
 * the last successfully applied value are the authoritative cached state.
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

#define M5IO_HUB_BACKLIGHT_MAX_BRIGHTNESS 255U
#define M5IO_HUB_BACKLIGHT_DEFAULT_FREQ_HZ 30000U
#define M5IO_HUB_BACKLIGHT_MIN_FREQ_HZ 1U
#define M5IO_HUB_BACKLIGHT_MAX_FREQ_HZ 65535U

#define M5IO_HUB_BACKLIGHT_ARG_CONFIG 0x1U
#define M5IO_HUB_BACKLIGHT_ARG_SET 0x2U

struct m5io_hub_backlight {
  struct m5io_hub *hub;
  struct backlight_device *bl;
  u32 frequency_hz;
  u8 applied_brightness;
};

static int m5io_hub_backlight_configure(struct m5io_hub_backlight *backlight,
                                        u32 frequency_hz)
{
  struct m5io_hub_request request = {
      .func = M5IO_HUB_FUNC_BACKLIGHT,
      .arg = M5IO_HUB_BACKLIGHT_ARG_CONFIG,
  };
  u8 payload[2];
  u32 actual_frequency_hz;
  int ret;

  if (frequency_hz < M5IO_HUB_BACKLIGHT_MIN_FREQ_HZ ||
      frequency_hz > M5IO_HUB_BACKLIGHT_MAX_FREQ_HZ)
    return -EINVAL;

  payload[0] = frequency_hz & 0xff;
  payload[1] = frequency_hz >> 8;
  request.payload = payload;
  request.payload_len = sizeof(payload);

  ret = m5io_hub_exec(backlight->hub, &request);
  if (ret)
    return ret;
  if (request.status != 0 || request.reply_len != sizeof(payload))
    return -EPROTO;

  actual_frequency_hz = request.reply[0] |
                        ((u32)request.reply[1] << 8);
  if (actual_frequency_hz < M5IO_HUB_BACKLIGHT_MIN_FREQ_HZ ||
      actual_frequency_hz > M5IO_HUB_BACKLIGHT_MAX_FREQ_HZ)
    return -EPROTO;

  backlight->frequency_hz = actual_frequency_hz;
  return 0;
}

static int m5io_hub_backlight_set(struct m5io_hub_backlight *backlight,
                                  u8 brightness)
{
  struct m5io_hub_request request = {
      .func = M5IO_HUB_FUNC_BACKLIGHT,
      .arg = M5IO_HUB_BACKLIGHT_ARG_SET,
      .payload = &brightness,
      .payload_len = 1,
  };
  int ret;

  ret = m5io_hub_exec(backlight->hub, &request);
  if (ret)
    return ret;
  if (request.status != 0 || request.reply_len != 0)
    return -EPROTO;

  WRITE_ONCE(backlight->applied_brightness, brightness);
  return 0;
}

static int m5io_hub_backlight_update_status(struct backlight_device *bl)
{
  struct m5io_hub_backlight *backlight = bl_get_data(bl);
  u8 brightness = backlight_get_brightness(bl);

  /* The backlight core serializes update_status() with update_lock. */
  if (brightness == READ_ONCE(backlight->applied_brightness))
    return 0;

  return m5io_hub_backlight_set(backlight, brightness);
}

static int m5io_hub_backlight_get_brightness(struct backlight_device *bl)
{
  struct m5io_hub_backlight *backlight = bl_get_data(bl);

  /* FUNC=5 has no read command; return the last successful MCU update. */
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
  struct m5io_hub *hub = dev_get_drvdata(dev->parent);
  struct m5io_hub_backlight *backlight;
  struct backlight_properties properties = {};
  u32 default_brightness = M5IO_HUB_BACKLIGHT_MAX_BRIGHTNESS;
  u32 frequency_hz;
  int ret;

  if (!hub || !hub->spi)
    return -EPROBE_DEFER;

  backlight = devm_kzalloc(dev, sizeof(*backlight), GFP_KERNEL);
  if (!backlight)
    return -ENOMEM;
  backlight->hub = hub;

  ret = device_property_read_u32(dev, "default-brightness-level",
                                 &default_brightness);
  if (ret && ret != -EINVAL && ret != -ENODATA)
    return dev_err_probe(dev, ret, "failed to read default brightness\n");
  if (default_brightness > M5IO_HUB_BACKLIGHT_MAX_BRIGHTNESS)
    return dev_err_probe(dev, -EINVAL,
                        "default brightness %u exceeds %u\n",
                        default_brightness,
                        M5IO_HUB_BACKLIGHT_MAX_BRIGHTNESS);

  /* The MCU has a 30 kHz reset default.  Configure it only when the DT
   * supplies a value, allowing boards to use the MCU default without an
   * unnecessary transaction. */
  ret = device_property_read_u32(dev, "pwm-frequency-hz", &frequency_hz);
  if (!ret) {
    ret = m5io_hub_backlight_configure(backlight, frequency_hz);
    if (ret)
      return dev_err_probe(dev, ret, "failed to configure PWM frequency\n");
  } else if (ret != -EINVAL && ret != -ENODATA) {
    return dev_err_probe(dev, ret, "failed to read PWM frequency\n");
  } else {
    backlight->frequency_hz = M5IO_HUB_BACKLIGHT_DEFAULT_FREQ_HZ;
  }

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
  ret = backlight_update_status(backlight->bl);
  if (ret)
    return dev_err_probe(dev, ret, "failed to set initial brightness\n");

  dev_info(dev, "M5IO-HUB backlight registered, PWM %u Hz, brightness %u\n",
           backlight->frequency_hz, default_brightness);
  return 0;
}

static const struct of_device_id m5io_hub_backlight_of_match[] = {
    {.compatible = "m5stack,m5io-hub-backlight"},
    {}
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
