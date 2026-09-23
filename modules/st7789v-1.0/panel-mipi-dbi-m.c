// SPDX-License-Identifier: GPL-2.0
/*
 * DRM driver for MIPI DBI compatible display panels
 *
 * Copyright 2022 Noralf Trønnes
 */

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/property.h>
#include <linux/regulator/consumer.h>
#include <linux/spi/spi.h>

#include <drm/clients/drm_client_setup.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_fbdev_dma.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_managed.h>
#include <drm/drm_mipi_dbi.h>
#include <drm/drm_modes.h>
#include <drm/drm_modeset_helper.h>

#include <video/mipi_display.h>


/*
 * TE support is intentionally disabled for now. Keep these definitions here
 * so TE can be enabled again when the panel wiring and timing are verified.
 */
#if 0
#ifndef MIPI_DCS_SET_TEAR_ON
#define MIPI_DCS_SET_TEAR_ON		0x35
#endif
#define MIPI_DCS_TEAR_MODE_VBLANK	0x00
#endif


struct panel_mipi_dbi_format {
    const char *name;
    u32 fourcc;
    unsigned int bpp;
};

static const struct panel_mipi_dbi_format panel_mipi_dbi_formats[] = {
    { "r5g6b5", DRM_FORMAT_RGB565, 16 },
    { "b6x2g6x2r6x2", DRM_FORMAT_RGB888, 24 },
};

/* dbidev must be the first member so container_of can cast it safely. */
struct panel_mipi_dbi_device {
    struct mipi_dbi_dev dbidev;

    /*
     * TE support is intentionally disabled for now and may be enabled later.
     * struct gpio_desc *te;
     * int te_irq;
     * struct completion te_completion;
     */
    struct mutex display_lock;		/* Serialize reset and display transfers */
};

static inline struct panel_mipi_dbi_device *
to_panel_mipi_dbi_device(struct mipi_dbi_dev *dbidev)
{
    return container_of(dbidev, struct panel_mipi_dbi_device, dbidev);
}

static int panel_mipi_dbi_get_format(struct device *dev, u32 *formats, unsigned int *bpp)
{
    const char *format_name;
    unsigned int i;
    int ret;

    formats[1] = DRM_FORMAT_XRGB8888;

    ret = device_property_read_string(dev, "format", &format_name);
    if (ret) {
        /* Old Device Trees don't have this property */
        formats[0] = DRM_FORMAT_RGB565;
        *bpp = 16;
        return 0;
    }

    for (i = 0; i < ARRAY_SIZE(panel_mipi_dbi_formats); i++) {
        const struct panel_mipi_dbi_format *format = &panel_mipi_dbi_formats[i];

        if (strcmp(format_name, format->name))
            continue;

        formats[0] = format->fourcc;
        *bpp = format->bpp;
        return 0;
    }

    dev_err(dev, "Pixel format is not supported: '%s'\n", format_name);

    return -EINVAL;
}

static const u8 panel_mipi_dbi_magic[15] = { 'M', 'I', 'P', 'I', ' ', 'D', 'B', 'I',
                         0, 0, 0, 0, 0, 0, 0 };

/*
 * The display controller configuration is stored in a firmware file.
 * The Device Tree 'compatible' property value with a '.bin' suffix is passed
 * to request_firmware() to fetch this file.
 */
struct panel_mipi_dbi_config {
    /* Magic string: panel_mipi_dbi_magic */
    u8 magic[15];

    /* Config file format version */
    u8 file_format_version;

    /*
     * MIPI commands to execute when the display pipeline is enabled.
     * This is used to configure the display controller.
     *
     * The commands are stored in a byte array with the format:
     *     command, num_parameters, [ parameter, ...], command, ...
     *
     * Some commands require a pause before the next command can be received.
     * Inserting a delay in the command sequence is done by using the NOP command with one
     * parameter: delay in miliseconds (the No Operation command is part of the MIPI Display
     * Command Set where it has no parameters).
     *
     * Example:
     *     command 0x11
     *     sleep 120ms
     *     command 0xb1 parameters 0x01, 0x2c, 0x2d
     *     command 0x29
     *
     * Byte sequence:
     *     0x11 0x00
     *     0x00 0x01 0x78
     *     0xb1 0x03 0x01 0x2c 0x2d
     *     0x29 0x00
     */
    u8 commands[];
};

struct panel_mipi_dbi_commands {
    const u8 *buf;
    size_t len;
};

static struct panel_mipi_dbi_commands *
panel_mipi_dbi_check_commands(struct device *dev, const struct firmware *fw)
{
    const struct panel_mipi_dbi_config *config = (struct panel_mipi_dbi_config *)fw->data;
    struct panel_mipi_dbi_commands *commands;
    size_t size = fw->size, commands_len;
    unsigned int i = 0;

    if (size < sizeof(*config) + 2) { /* At least 1 command */
        dev_err(dev, "config: file size=%zu is too small\n", size);
        return ERR_PTR(-EINVAL);
    }

    if (memcmp(config->magic, panel_mipi_dbi_magic, sizeof(config->magic))) {
        dev_err(dev, "config: Bad magic: %15ph\n", config->magic);
        return ERR_PTR(-EINVAL);
    }

    if (config->file_format_version != 1) {
        dev_err(dev, "config: version=%u is not supported\n", config->file_format_version);
        return ERR_PTR(-EINVAL);
    }

    drm_dev_dbg(dev, DRM_UT_DRIVER, "size=%zu version=%u\n", size, config->file_format_version);

    commands_len = size - sizeof(*config);

    while ((i + 1) < commands_len) {
        u8 command = config->commands[i++];
        u8 num_parameters = config->commands[i++];
        const u8 *parameters = &config->commands[i];

        i += num_parameters;
        if (i > commands_len) {
            dev_err(dev, "config: command=0x%02x num_parameters=%u overflows\n",
                command, num_parameters);
            return ERR_PTR(-EINVAL);
        }

        if (command == 0x00 && num_parameters == 1)
            drm_dev_dbg(dev, DRM_UT_DRIVER, "sleep %ums\n", parameters[0]);
        else
            drm_dev_dbg(dev, DRM_UT_DRIVER, "command %02x %*ph\n",
                    command, num_parameters, parameters);
    }

    if (i != commands_len) {
        dev_err(dev, "config: malformed command array\n");
        return ERR_PTR(-EINVAL);
    }

    commands = devm_kzalloc(dev, sizeof(*commands), GFP_KERNEL);
    if (!commands)
        return ERR_PTR(-ENOMEM);

    commands->len = commands_len;
    commands->buf = devm_kmemdup(dev, config->commands, commands->len, GFP_KERNEL);
    if (!commands->buf)
        return ERR_PTR(-ENOMEM);

    return commands;
}

static struct panel_mipi_dbi_commands *panel_mipi_dbi_commands_from_fw(struct device *dev)
{
    struct panel_mipi_dbi_commands *commands;
    const struct firmware *fw;
    const char *compatible;
    char fw_name[40];
    int ret;

    ret = of_property_read_string_index(dev->of_node, "compatible", 0, &compatible);
    if (ret)
        return ERR_PTR(ret);

    snprintf(fw_name, sizeof(fw_name), "%s.bin", compatible);
    ret = request_firmware(&fw, fw_name, dev);
    if (ret) {
        dev_err(dev, "No config file found for compatible '%s' (error=%d)\n",
            compatible, ret);

        return ERR_PTR(ret);
    }

    commands = panel_mipi_dbi_check_commands(dev, fw);
    release_firmware(fw);

    return commands;
}

static void panel_mipi_dbi_commands_execute(struct mipi_dbi *dbi,
                        struct panel_mipi_dbi_commands *commands)
{
    unsigned int i = 0;

    if (!commands)
        return;

    while (i < commands->len) {
        u8 command = commands->buf[i++];
        u8 num_parameters = commands->buf[i++];
        const u8 *parameters = &commands->buf[i];

        if (command == 0x00 && num_parameters == 1)
            msleep(parameters[0]);
        else if (num_parameters)
            mipi_dbi_command_stackbuf(dbi, command, parameters, num_parameters);
        else
            mipi_dbi_command(dbi, command);

        i += num_parameters;
    }
}

static int panel_mipi_dbi_reinitialize(struct panel_mipi_dbi_device *panel)
{
    struct mipi_dbi_dev *dbidev = &panel->dbidev;
    struct mipi_dbi *dbi = &dbidev->dbi;
    int ret;

    /* Balance the temporary regulator references after initialization. */
    ret = mipi_dbi_poweron_reset(dbidev);
    if (ret)
        return ret;

    panel_mipi_dbi_commands_execute(dbi, dbidev->driver_private);

    /* TE support is intentionally disabled; re-enable this with TE support. */
    /*
    if (panel->te)
        mipi_dbi_command(dbi, MIPI_DCS_SET_TEAR_ON,
                 MIPI_DCS_TEAR_MODE_VBLANK);
    */

    if (dbidev->regulator)
        regulator_disable(dbidev->regulator);
    if (dbidev->io_regulator)
        regulator_disable(dbidev->io_regulator);

    return 0;
}

static ssize_t panel_mipi_dbi_reset_store(struct device *dev,
                      struct device_attribute *attr,
                      const char *buf, size_t count)
{
    struct spi_device *spi = to_spi_device(dev);
    struct drm_device *drm = spi_get_drvdata(spi);
    struct panel_mipi_dbi_device *panel;
    unsigned int value;
    int idx, ret;

    ret = kstrtouint(buf, 0, &value);
    if (ret)
        return ret;

    if (value != 1)
        return -EINVAL;

    if (!drm)
        return -ENODEV;

    panel = to_panel_mipi_dbi_device(drm_to_mipi_dbi_dev(drm));

    if (!drm_dev_enter(drm, &idx))
        return -ENODEV;

    mutex_lock(&panel->display_lock);
    ret = panel_mipi_dbi_reinitialize(panel);
    mutex_unlock(&panel->display_lock);

    drm_dev_exit(idx);

    return ret ? ret : count;
}

static DEVICE_ATTR(reset, 0200, NULL, panel_mipi_dbi_reset_store);

static struct attribute *panel_mipi_dbi_attrs[] = {
    &dev_attr_reset.attr,
    NULL,
};

static const struct attribute_group panel_mipi_dbi_attr_group = {
    .attrs = panel_mipi_dbi_attrs,
};

/*
 * TE support is intentionally disabled for now. Keep the handler and wait
 * logic here for possible later re-enablement when TE timing is verified.
 */
#if 0
/* TE interrupt handler: wake waiters when a TE signal arrives */
static irqreturn_t panel_mipi_dbi_te_isr(int irq, void *data)
{
    struct panel_mipi_dbi_device *panel = data;

    complete(&panel->te_completion);

    return IRQ_HANDLED;
}

/* Wait for the panel TE sync signal before flushing */
static void panel_mipi_dbi_wait_for_te(struct panel_mipi_dbi_device *panel)
{
    if (!panel->te)
        return;

    reinit_completion(&panel->te_completion);

    /* Enable the IRQ only while waiting; keep it disabled otherwise to save power */
    enable_irq(panel->te_irq);

    if (!wait_for_completion_timeout(&panel->te_completion,
                     msecs_to_jiffies(100)))
        dev_warn_once(panel->dbidev.drm.dev,
                  "Timeout waiting for TE signal\n");

    disable_irq(panel->te_irq);
}
#endif

static void panel_mipi_dbi_enable(struct drm_simple_display_pipe *pipe,
                  struct drm_crtc_state *crtc_state,
                  struct drm_plane_state *plane_state)
{
    struct mipi_dbi_dev *dbidev = drm_to_mipi_dbi_dev(pipe->crtc.dev);
    struct panel_mipi_dbi_device *panel = to_panel_mipi_dbi_device(dbidev);
    struct mipi_dbi *dbi = &dbidev->dbi;
    int ret, idx;

    if (!drm_dev_enter(pipe->crtc.dev, &idx))
        return;

    drm_dbg(pipe->crtc.dev, "\n");

    mutex_lock(&panel->display_lock);

    ret = mipi_dbi_poweron_conditional_reset(dbidev);
    if (ret < 0)
        goto out_exit;
    if (!ret)
        panel_mipi_dbi_commands_execute(dbi, dbidev->driver_private);

    /* TE support is intentionally disabled; re-enable this with TE support. */
    /*
    if (panel->te)
        mipi_dbi_command(dbi, MIPI_DCS_SET_TEAR_ON,
                 MIPI_DCS_TEAR_MODE_VBLANK);
    */

    mipi_dbi_enable_flush(dbidev, crtc_state, plane_state);
out_exit:
    mutex_unlock(&panel->display_lock);
    drm_dev_exit(idx);
}

/* Update callback remains locked to serialize normal transfers with reset. */
static void panel_mipi_dbi_pipe_update(struct drm_simple_display_pipe *pipe,
                       struct drm_plane_state *old_state)
{
    struct mipi_dbi_dev *dbidev = drm_to_mipi_dbi_dev(pipe->crtc.dev);
    struct panel_mipi_dbi_device *panel = to_panel_mipi_dbi_device(dbidev);

    mutex_lock(&panel->display_lock);
    mipi_dbi_pipe_update(pipe, old_state);
    mutex_unlock(&panel->display_lock);
}

static void panel_mipi_dbi_disable(struct drm_simple_display_pipe *pipe)
{
    struct mipi_dbi_dev *dbidev = drm_to_mipi_dbi_dev(pipe->crtc.dev);
    struct panel_mipi_dbi_device *panel = to_panel_mipi_dbi_device(dbidev);

    mutex_lock(&panel->display_lock);
    mipi_dbi_pipe_disable(pipe);
    mutex_unlock(&panel->display_lock);
}

static const struct drm_simple_display_pipe_funcs panel_mipi_dbi_pipe_funcs = {
    DRM_MIPI_DBI_SIMPLE_DISPLAY_PIPE_FUNCS(panel_mipi_dbi_enable),
    .disable = panel_mipi_dbi_disable,
    .update = panel_mipi_dbi_pipe_update,
};

DEFINE_DRM_GEM_DMA_FOPS(panel_mipi_dbi_fops);

static const struct drm_driver panel_mipi_dbi_driver = {
    .driver_features	= DRIVER_GEM | DRIVER_MODESET | DRIVER_ATOMIC,
    .fops			= &panel_mipi_dbi_fops,
    DRM_GEM_DMA_DRIVER_OPS_VMAP,
    DRM_FBDEV_DMA_DRIVER_OPS,
    .debugfs_init		= mipi_dbi_debugfs_init,
    .name			= "panel-mipi-dbi",
    .desc			= "MIPI DBI compatible display panel",
    .major			= 1,
    .minor			= 0,
};

static int panel_mipi_dbi_get_mode(struct mipi_dbi_dev *dbidev, struct drm_display_mode *mode)
{
    struct device *dev = dbidev->drm.dev;
    u16 hback_porch, vback_porch;
    int ret;

    ret = of_get_drm_panel_display_mode(dev->of_node, mode, NULL);
    if (ret) {
        dev_err(dev, "%pOF: failed to get panel-timing (error=%d)\n", dev->of_node, ret);
        return ret;
    }

    mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;

    hback_porch = mode->htotal - mode->hsync_end;
    vback_porch = mode->vtotal - mode->vsync_end;

    /*
     * Make sure width and height are set and that only back porch and
     * pixelclock are set in the other timing values. Also check that
     * width and height don't exceed the 16-bit value specified by MIPI DCS.
     */
    if (!mode->hdisplay || !mode->vdisplay || mode->flags ||
        mode->hsync_end > mode->hdisplay || (hback_porch + mode->hdisplay) > 0xffff ||
        mode->vsync_end > mode->vdisplay || (vback_porch + mode->vdisplay) > 0xffff) {
        dev_err(dev, "%pOF: panel-timing out of bounds\n", dev->of_node);
        return -EINVAL;
    }

    /* The driver doesn't use the pixel clock but it is mandatory so fake one if not set */
    if (!mode->clock)
        mode->clock = mode->htotal * mode->vtotal * 60 / 1000;

    dbidev->top_offset = vback_porch;
    dbidev->left_offset = hback_porch;

    return 0;
}

static int panel_mipi_dbi_spi_probe(struct spi_device *spi)
{
    struct device *dev = &spi->dev;
    struct panel_mipi_dbi_device *panel;
    struct drm_display_mode mode;
    struct mipi_dbi_dev *dbidev;
    struct drm_device *drm;
    struct mipi_dbi *dbi;
    struct gpio_desc *dc;
    unsigned int bpp;
    size_t buf_size;
    u32 formats[2];
    int ret;

    panel = devm_drm_dev_alloc(dev, &panel_mipi_dbi_driver,
                   struct panel_mipi_dbi_device, dbidev.drm);
    if (IS_ERR(panel))
        return PTR_ERR(panel);

    dbidev = &panel->dbidev;
    dbi = &dbidev->dbi;
    drm = &dbidev->drm;
    mutex_init(&panel->display_lock);

    ret = panel_mipi_dbi_get_mode(dbidev, &mode);
    if (ret)
        return ret;

    dbidev->regulator = devm_regulator_get(dev, "power");
    if (IS_ERR(dbidev->regulator))
        return dev_err_probe(dev, PTR_ERR(dbidev->regulator),
                     "Failed to get regulator 'power'\n");

    dbidev->io_regulator = devm_regulator_get(dev, "io");
    if (IS_ERR(dbidev->io_regulator))
        return dev_err_probe(dev, PTR_ERR(dbidev->io_regulator),
                     "Failed to get regulator 'io'\n");

    dbidev->backlight = devm_of_find_backlight(dev);
    if (IS_ERR(dbidev->backlight))
        return dev_err_probe(dev, PTR_ERR(dbidev->backlight), "Failed to get backlight\n");

    dbi->reset = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_HIGH);
    if (IS_ERR(dbi->reset))
        return dev_err_probe(dev, PTR_ERR(dbi->reset), "Failed to get GPIO 'reset'\n");

    /* Multiple panels can share the "dc" GPIO, but only if they are on the same SPI bus! */
    dc = devm_gpiod_get_optional(dev, "dc", GPIOD_OUT_LOW | GPIOD_FLAGS_BIT_NONEXCLUSIVE);
    if (IS_ERR(dc))
        return dev_err_probe(dev, PTR_ERR(dc), "Failed to get GPIO 'dc'\n");

    ret = mipi_dbi_spi_init(spi, dbi, dc);
    if (ret)
        return ret;

    if (device_property_present(dev, "write-only"))
        dbi->read_commands = NULL;

    /*
     * TE support is intentionally disabled for now. Keep GPIO and IRQ setup
     * commented out so it can be enabled later after TE timing is verified.
     */
#if 0
    /* Get the optional TE pin and request its IRQ */
    panel->te = devm_gpiod_get_optional(dev, "te", GPIOD_IN);
    if (IS_ERR(panel->te))
        return dev_err_probe(dev, PTR_ERR(panel->te), "Failed to get GPIO 'te'\n");

    if (panel->te) {
        init_completion(&panel->te_completion);

        panel->te_irq = gpiod_to_irq(panel->te);
        if (panel->te_irq < 0)
            return dev_err_probe(dev, panel->te_irq,
                         "Failed to get TE irq\n");

        /*
         * Use IRQF_NO_AUTOEN so the IRQ stays disabled after it is requested,
         * and enable it only when wait_for_te() needs it.
         */
        ret = devm_request_irq(dev, panel->te_irq, panel_mipi_dbi_te_isr,
                       IRQF_TRIGGER_RISING | IRQF_NO_AUTOEN,
                       dev_name(dev), panel);
        if (ret)
            return dev_err_probe(dev, ret, "Failed to request TE irq\n");
    }
#endif

    dbidev->driver_private = panel_mipi_dbi_commands_from_fw(dev);
    if (IS_ERR(dbidev->driver_private))
        return PTR_ERR(dbidev->driver_private);

    ret = panel_mipi_dbi_get_format(dev, formats, &bpp);
    if (ret)
        return ret;

    buf_size = DIV_ROUND_UP(mode.hdisplay * mode.vdisplay * bpp, 8);
    ret = mipi_dbi_dev_init_with_formats(dbidev, &panel_mipi_dbi_pipe_funcs,
                         formats, ARRAY_SIZE(formats),
                         &mode, 0, buf_size);
    if (ret)
        return ret;

    drm_mode_config_reset(drm);

    ret = drm_dev_register(drm, 0);
    if (ret)
        return ret;

    spi_set_drvdata(spi, drm);

    ret = devm_device_add_group(dev, &panel_mipi_dbi_attr_group);
    if (ret) {
        drm_dev_unplug(drm);
        drm_atomic_helper_shutdown(drm);
        return ret;
    }

    if (bpp == 16)
        drm_client_setup_with_fourcc(drm, DRM_FORMAT_RGB565);
    else
        drm_client_setup_with_fourcc(drm, DRM_FORMAT_RGB888);

    return 0;
}

static void panel_mipi_dbi_spi_remove(struct spi_device *spi)
{
    struct drm_device *drm = spi_get_drvdata(spi);

    drm_dev_unplug(drm);
    drm_atomic_helper_shutdown(drm);
}

static void panel_mipi_dbi_spi_shutdown(struct spi_device *spi)
{
    drm_atomic_helper_shutdown(spi_get_drvdata(spi));
}

static int __maybe_unused panel_mipi_dbi_pm_suspend(struct device *dev)
{
    return drm_mode_config_helper_suspend(dev_get_drvdata(dev));
}

static int __maybe_unused panel_mipi_dbi_pm_resume(struct device *dev)
{
    drm_mode_config_helper_resume(dev_get_drvdata(dev));

    return 0;
}

static const struct dev_pm_ops panel_mipi_dbi_pm_ops = {
    SET_SYSTEM_SLEEP_PM_OPS(panel_mipi_dbi_pm_suspend, panel_mipi_dbi_pm_resume)
};

static const struct of_device_id panel_mipi_dbi_spi_of_match[] = {
    { .compatible = "panel-mipi-dbi-spi" },
    { .compatible = "panel-mipi-dbi-m5stack" },
    {},
};
MODULE_DEVICE_TABLE(of, panel_mipi_dbi_spi_of_match);

static const struct spi_device_id panel_mipi_dbi_spi_id[] = {
    { "panel-mipi-dbi-spi", 0 },
    { "panel-mipi-dbi-m5stack", 0 },
    { },
};
MODULE_DEVICE_TABLE(spi, panel_mipi_dbi_spi_id);

static struct spi_driver panel_mipi_dbi_spi_driver = {
    .driver = {
        .name = "panel-mipi-dbi-spi",
        .of_match_table = panel_mipi_dbi_spi_of_match,
        .pm = &panel_mipi_dbi_pm_ops,
    },
    .id_table = panel_mipi_dbi_spi_id,
    .probe = panel_mipi_dbi_spi_probe,
    .remove = panel_mipi_dbi_spi_remove,
    .shutdown = panel_mipi_dbi_spi_shutdown,
};
module_spi_driver(panel_mipi_dbi_spi_driver);

MODULE_DESCRIPTION("MIPI DBI compatible display panel driver");
MODULE_AUTHOR("Noralf Trønnes");
MODULE_LICENSE("GPL");
