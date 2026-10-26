// SPDX-License-Identifier: GPL-2.0
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include "m5io-hub.h"

#define M5IO_I2C_CONFIG   1
#define M5IO_I2C_BUSES    2

struct m5io_hub_i2c_bus {
    struct m5io_hub *hub;
    struct i2c_adapter adap;
    u8 bus;
    u8 speed;
};

struct m5io_hub_i2c {
    struct m5io_hub_i2c_bus buses[M5IO_I2C_BUSES];
};

static int m5io_hub_i2c_xfer(struct i2c_adapter *adap,
                            struct i2c_msg *msgs, int num)
{
    struct m5io_hub_i2c_bus *bus = i2c_get_adapdata(adap);
    u8 payload[M5IO_HUB_DATA_PAYLOAD_MAX];
    u8 result[M5IO_HUB_DATA_PAYLOAD_MAX];
    unsigned int result_len = 0;
    struct i2c_msg *w = NULL, *r = NULL;
    unsigned int payload_len;
    int ret;

    if (!bus || !bus->hub)
        return -ENODEV;
    if (!msgs || num < 1 || num > 2)
        return -EOPNOTSUPP;

    /* MCU_RESET clears FUNC=2's configured bits.  The Linux adapter object
     * survives that maintenance command, so replay CONFIG before every DATA
     * transaction.  This also makes a transfer self-healing after an OTA or
     * any other MCU-side transport reset without requiring a Linux reprobe. */
    {
        u8 config_payload[2] = { bus->bus, bus->speed };
        struct m5io_hub_request config = {
            .func = M5IO_HUB_FUNC_I2C,
            .arg = M5IO_I2C_CONFIG,
            .payload = config_payload,
            .payload_len = sizeof(config_payload),
        };

        ret = m5io_hub_exec(bus->hub, &config);
        if (ret)
            return ret;
        if (config.status != 0 || config.reply_len)
            return config.status == 4 ? -EBUSY :
                   (config.status == 3 ? -EINVAL : -EPROTO);
    }
    if (num == 1) {
        if (msgs[0].addr < 0x08 || msgs[0].addr > 0x77 ||
            msgs[0].len == 0 || msgs[0].len > 253 ||
            (msgs[0].flags & ~(I2C_M_RD | I2C_M_DMA_SAFE)) || !msgs[0].buf)
            return -EINVAL;
        if (msgs[0].flags & I2C_M_RD) {
            payload[0] = 0xAB;
            payload[1] = msgs[0].addr;
            payload[2] = msgs[0].len;
            payload_len = 3;
            ret = m5io_hub_i2c_data_sync(bus->hub, bus->bus, payload,
                                         payload_len, result, msgs[0].len + 2,
                                         &result_len);
            if (ret)
                return ret;
            if (result_len != msgs[0].len + 2U || result[0] != 0xAB ||
                result[1] != msgs[0].addr)
                return -EPROTO;
            memcpy(msgs[0].buf, &result[2], msgs[0].len);
        } else {
            payload[0] = 0xAA;
            payload[1] = msgs[0].addr;
            memcpy(&payload[2], msgs[0].buf, msgs[0].len);
            ret = m5io_hub_i2c_data_sync(bus->hub, bus->bus, payload,
                                         msgs[0].len + 2, NULL, 0, NULL);
            if (ret)
                return ret;
        }
        return 1;
    }

    w = &msgs[0]; r = &msgs[1];
    if ((w->flags & ~I2C_M_DMA_SAFE) ||
        !(r->flags & I2C_M_RD) ||
        (r->flags & ~(I2C_M_RD | I2C_M_DMA_SAFE)) ||
        w->addr != r->addr || w->addr < 0x08 || w->addr > 0x77 ||
        !w->len || w->len > 252 || !r->len || r->len > 253 ||
        !w->buf || !r->buf)
        return -EOPNOTSUPP;
    payload[0] = 0xAB;
    payload[1] = w->addr;
    memcpy(&payload[2], w->buf, w->len);
    payload[2 + w->len] = r->len;
    payload_len = 3 + w->len;
    ret = m5io_hub_i2c_data_sync(bus->hub, bus->bus, payload, payload_len,
                                 result, r->len + 2, &result_len);
    if (ret)
        return ret;
    if (result_len != r->len + 2U || result[0] != 0xAB || result[1] != r->addr)
        return -EPROTO;
    memcpy(r->buf, &result[2], r->len);
    return 2;
}

static u32 m5io_hub_i2c_functionality(struct i2c_adapter *adap)
{
    /* The I2C core emulates these SMBus operations through master_xfer.
     * QUICK needs an address-only transaction and SMBus block reads need
     * I2C_M_RECV_LEN; neither can be represented by this wire revision.
     * In particular, advertise READ_BYTE so i2cdetect -r can scan normally. */
    return I2C_FUNC_I2C | I2C_FUNC_SMBUS_BYTE |
           I2C_FUNC_SMBUS_BYTE_DATA | I2C_FUNC_SMBUS_WORD_DATA |
           I2C_FUNC_SMBUS_PROC_CALL | I2C_FUNC_SMBUS_WRITE_BLOCK_DATA |
           I2C_FUNC_SMBUS_I2C_BLOCK;
}

static const struct i2c_adapter_quirks m5io_hub_i2c_quirks = {
    .flags = I2C_AQ_COMB_WRITE_THEN_READ | I2C_AQ_NO_ZERO_LEN,
    .max_num_msgs = 2,
    /* AA/AB reserve two bytes for opcode/address; combined requests also
     * carry a final read length. Limits apply before any bus side effects. */
    .max_write_len = 253,
    .max_read_len = 253,
    .max_comb_1st_msg_len = 252,
    .max_comb_2nd_msg_len = 253,
};

static const struct i2c_algorithm m5io_hub_i2c_algo = {
    .master_xfer = m5io_hub_i2c_xfer,
    .functionality = m5io_hub_i2c_functionality,
};

static void m5io_hub_i2c_put_node(void *data)
{
    of_node_put(data);
}

static int m5io_hub_i2c_add_bus(struct platform_device *pdev,
                               struct m5io_hub_i2c_bus *bus,
                               unsigned int id, struct device_node *node)
{
    u8 payload[2] = { id, 0 };
    struct m5io_hub_request request = {
        .func = M5IO_HUB_FUNC_I2C,
        .arg = M5IO_I2C_CONFIG,
        .payload = payload,
        .payload_len = sizeof(payload),
    };
    u32 frequency = I2C_MAX_STANDARD_MODE_FREQ;
    int ret;

    m5io_dbg("m5io-hub-i2c: add bus=%u node=%p\n", id, node);

    if (node && of_find_property(node, "clock-frequency", NULL)) {
        ret = of_property_read_u32(node, "clock-frequency", &frequency);
        if (ret) {
            m5io_dbg("m5io-hub-i2c: bus=%u clock-frequency read failed ret=%d\n",
                    id, ret);
            return ret;
        }
    }
    m5io_dbg("m5io-hub-i2c: bus=%u requested frequency=%u\n", id, frequency);
    if (frequency == I2C_MAX_FAST_MODE_FREQ)
        payload[1] = 1;
    else if (frequency != I2C_MAX_STANDARD_MODE_FREQ) {
        m5io_dbg("m5io-hub-i2c: bus=%u unsupported frequency=%u\n", id,
                frequency);
        return dev_err_probe(&pdev->dev, -EINVAL,
                             "I2C%u supports only 100/400 kHz\n", id);
    }

    bus->hub = dev_get_drvdata(pdev->dev.parent);
    bus->bus = id;
    bus->speed = payload[1];
    ret = m5io_hub_exec(bus->hub, &request);
    m5io_dbg("m5io-hub-i2c: bus=%u config exec ret=%d reply_len=%u\n",
            id, ret, request.reply_len);
    if (ret)
        return dev_err_probe(&pdev->dev, ret, "I2C%u configuration failed\n", id);
    if (request.reply_len) {
        m5io_dbg("m5io-hub-i2c: bus=%u config unexpected reply_len=%u\n",
                id, request.reply_len);
        return -EPROTO;
    }

    bus->adap.owner = THIS_MODULE;
    bus->adap.algo = &m5io_hub_i2c_algo;
    bus->adap.quirks = &m5io_hub_i2c_quirks;
    bus->adap.dev.parent = &pdev->dev;
    /* The protocol's I2C DATA transaction has a 27.5 s outer budget. */
    bus->adap.timeout = msecs_to_jiffies(27500);
    bus->adap.retries = 0;
    if (node) {
        ret = devm_add_action_or_reset(&pdev->dev, m5io_hub_i2c_put_node,
                                       of_node_get(node));
        if (ret)
            return ret;
        bus->adap.dev.of_node = node;
    }
    snprintf(bus->adap.name, sizeof(bus->adap.name), "m5io-hub-i2c%u", id);
    i2c_set_adapdata(&bus->adap, bus);
    ret = devm_i2c_add_adapter(&pdev->dev, &bus->adap);
    if (!ret)
        dev_info(&pdev->dev, "I2C%u registered as i2c-%d at %u Hz\n",
                 id, bus->adap.nr, frequency);
    return ret;
}

static int m5io_hub_i2c_probe(struct platform_device *pdev)
{
    struct m5io_hub_i2c *mi;
    unsigned long enabled = 0;
    unsigned long registered = 0;
    unsigned int id;
    int ret;

    m5io_dbg("m5io-hub-i2c: probe start device=%s\n", dev_name(&pdev->dev));

    if (!dev_get_drvdata(pdev->dev.parent)) {
        m5io_dbg("m5io-hub-i2c: probe parent drvdata missing\n");
        return -ENODEV;
    }
    mi = devm_kzalloc(&pdev->dev, sizeof(*mi), GFP_KERNEL);
    if (!mi)
        return -ENOMEM;

    m5io_dbg("m5io-hub-i2c: probe child_count=%d\n",
            of_get_child_count(pdev->dev.of_node));

    if (of_get_child_count(pdev->dev.of_node)) {
        for_each_available_child_of_node_scoped(pdev->dev.of_node, node) {
            ret = of_property_read_u32(node, "reg", &id);
            if (ret || id < 1 || id > M5IO_I2C_BUSES ||
                test_and_set_bit(id - 1, &enabled)) {
                m5io_dbg("m5io-hub-i2c: invalid child reg id=%u ret=%d\n",
                        id, ret);
                return -EINVAL;
            }
            ret = m5io_hub_i2c_add_bus(pdev, &mi->buses[id - 1], id, node);
            m5io_dbg("m5io-hub-i2c: bus=%u add result=%d\n", id, ret);
            /* Some M5IO-HUB firmware/boards expose only one MCU I2C bus.
             * Keep available DT-declared buses usable when another bus
             * rejects its CONFIG request, while still failing hard for
             * malformed DT or unexpected driver errors.
             */
            if (ret == -ETIMEDOUT || ret == -ENXIO || ret == -EIO) {
                dev_warn(&pdev->dev,
                         "I2C%u unavailable (%d); continuing with other buses\n",
                         id, ret);
                continue;
            }
            if (ret)
                return ret;
            set_bit(id - 1, &registered);
        }
        if (!registered)
            return -ENODEV;
    } else {
        /* Legacy child without bus nodes exposes both MCU buses at 100 kHz. */
        for (id = 1; id <= M5IO_I2C_BUSES; id++) {
            ret = m5io_hub_i2c_add_bus(pdev, &mi->buses[id - 1], id, NULL);
            if (ret)
                return ret;
        }
    }
    platform_set_drvdata(pdev, mi);
    m5io_dbg("m5io-hub-i2c: probe complete enabled=%#lx registered=%#lx\n",
            enabled, registered);
    return 0;
}

static const struct of_device_id m5io_hub_i2c_of_match[] = {
    { .compatible = "m5stack,m5io-hub-i2c" },
    { }
};
MODULE_DEVICE_TABLE(of, m5io_hub_i2c_of_match);

static struct platform_driver m5io_hub_i2c_driver = {
    .driver = {
        .name = "m5io-hub-i2c",
        .of_match_table = m5io_hub_i2c_of_match,
    },
    .probe = m5io_hub_i2c_probe,
};
module_platform_driver(m5io_hub_i2c_driver);

MODULE_DESCRIPTION("M5Stack M5IO-HUB I2C bridge driver");
MODULE_LICENSE("GPL");
