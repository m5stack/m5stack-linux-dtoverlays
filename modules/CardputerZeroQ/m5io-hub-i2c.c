// SPDX-License-Identifier: GPL-2.0
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include "m5io-hub.h"

#define M5IO_I2C_CONFIG   1
#define M5IO_I2C_READ     2
#define M5IO_I2C_WRITE    3
#define M5IO_I2C_DETECT   4
#define M5IO_I2C_MEM_READ 5
#define M5IO_I2C_BUSES    2

struct m5io_hub_i2c_bus {
    struct m5io_hub *hub;
    struct i2c_adapter adap;
    u8 bus;
};

struct m5io_hub_i2c {
    struct m5io_hub_i2c_bus buses[M5IO_I2C_BUSES];
};

static int m5io_hub_i2c_xfer(struct i2c_adapter *adap,
                            struct i2c_msg *msgs, int num)
{
    struct m5io_hub_i2c_bus *bus = i2c_get_adapdata(adap);
    struct m5io_hub_request request = { .func = M5IO_HUB_FUNC_I2C };
    struct i2c_msg *msg;
    u8 payload[M5IO_HUB_TOKEN_PAYLOAD_MAX];
    int i, ret;

    /* Validate the whole vector before sending. A repeated START is available
     * only for a 1/2-byte write followed by a read of the same address.
     */
    if (!msgs || num < 1 || num > 2)
        return -EOPNOTSUPP;
    for (i = 0; i < num; i++) {
        if (msgs[i].flags & ~(I2C_M_RD | I2C_M_DMA_SAFE))
            return -EOPNOTSUPP;
        if (msgs[i].addr < 0x08 || msgs[i].addr > 0x77)
            return -EINVAL;
        if (msgs[i].len > M5IO_HUB_DATA_PAYLOAD_MAX)
            return -EOPNOTSUPP;
        if (msgs[i].len && !msgs[i].buf)
            return -EINVAL;
        if (!msgs[i].len && (msgs[i].flags & I2C_M_RD))
            return -EOPNOTSUPP;
    }
    if (num == 2 && ((msgs[0].flags & I2C_M_RD) ||
                    !(msgs[1].flags & I2C_M_RD) ||
                    msgs[0].addr != msgs[1].addr ||
                    msgs[0].len < 1 || msgs[0].len > 2))
        return -EOPNOTSUPP;

    msg = &msgs[num - 1];
    payload[0] = bus->bus;
    payload[1] = msg->addr;
    request.payload = payload;
    request.data_chn = bus->bus;
    if (num == 2) {
        request.arg = M5IO_I2C_MEM_READ;
        request.payload_len = 6;
        payload[2] = msgs[0].len;
        /* I2C register bytes are MSB first; RPC encodes the value little-endian. */
        payload[3] = msgs[0].buf[msgs[0].len - 1];
        payload[4] = msgs[0].len == 2 ? msgs[0].buf[0] : 0;
        payload[5] = msg->len;
    } else if (!msg->len) {
        request.arg = M5IO_I2C_DETECT;
        request.payload_len = 2;
    } else {
        request.arg = (msg->flags & I2C_M_RD) ? M5IO_I2C_READ : M5IO_I2C_WRITE;
        request.payload_len = 3;
        payload[2] = msg->len;
    }

    if (msg->flags & I2C_M_RD) {
        if (msg->len > M5IO_HUB_TOKEN_PAYLOAD_MAX) {
            request.data_rx = msg->buf;
            request.data_len = msg->len;
        }
    } else if (msg->len > 12) {
        request.data_tx = msg->buf;
        request.data_len = msg->len;
    } else if (msg->len) {
        memcpy(payload + 3, msg->buf, msg->len);
        request.payload_len += msg->len;
    }

    ret = m5io_hub_exec(bus->hub, &request);
    if (ret)
        return ret;
    if (request.arg == M5IO_I2C_DETECT) {
        if (request.reply_len != 1 || request.reply[0] > 1)
            return -EPROTO;
        if (!request.reply[0])
            return -ENXIO;
    } else if ((msg->flags & I2C_M_RD) && !request.data_rx) {
        if (request.reply_len != msg->len)
            return -EPROTO;
        memcpy(msg->buf, request.reply, msg->len);
    } else if (request.reply_len) {
        return -EPROTO;
    }
    return num;
}

static u32 m5io_hub_i2c_functionality(struct i2c_adapter *adap)
{
    return I2C_FUNC_I2C | I2C_FUNC_SMBUS_BYTE | I2C_FUNC_SMBUS_BYTE_DATA |
           I2C_FUNC_SMBUS_WORD_DATA | I2C_FUNC_SMBUS_WRITE_BLOCK_DATA |
           I2C_FUNC_SMBUS_I2C_BLOCK;
}

static const struct i2c_adapter_quirks m5io_hub_i2c_quirks = {
    .flags = I2C_AQ_COMB_WRITE_THEN_READ | I2C_AQ_NO_ZERO_LEN_READ,
    .max_num_msgs = 2,
    .max_write_len = M5IO_HUB_DATA_PAYLOAD_MAX,
    .max_read_len = M5IO_HUB_DATA_PAYLOAD_MAX,
    .max_comb_1st_msg_len = 2,
    .max_comb_2nd_msg_len = M5IO_HUB_DATA_PAYLOAD_MAX,
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

    if (node && of_find_property(node, "clock-frequency", NULL)) {
        ret = of_property_read_u32(node, "clock-frequency", &frequency);
        if (ret)
            return ret;
    }
    if (frequency == I2C_MAX_FAST_MODE_FREQ)
        payload[1] = 1;
    else if (frequency != I2C_MAX_STANDARD_MODE_FREQ)
        return dev_err_probe(&pdev->dev, -EINVAL,
                             "I2C%u supports only 100/400 kHz\n", id);

    bus->hub = dev_get_drvdata(pdev->dev.parent);
    bus->bus = id;
    ret = m5io_hub_exec(bus->hub, &request);
    if (ret)
        return dev_err_probe(&pdev->dev, ret, "I2C%u configuration failed\n", id);
    if (request.reply_len)
        return -EPROTO;

    bus->adap.owner = THIS_MODULE;
    bus->adap.algo = &m5io_hub_i2c_algo;
    bus->adap.quirks = &m5io_hub_i2c_quirks;
    bus->adap.dev.parent = &pdev->dev;
    bus->adap.timeout = msecs_to_jiffies(2000);
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

    if (!dev_get_drvdata(pdev->dev.parent))
        return -ENODEV;
    mi = devm_kzalloc(&pdev->dev, sizeof(*mi), GFP_KERNEL);
    if (!mi)
        return -ENOMEM;

    if (of_get_child_count(pdev->dev.of_node)) {
        for_each_available_child_of_node_scoped(pdev->dev.of_node, node) {
            ret = of_property_read_u32(node, "reg", &id);
            if (ret || id < 1 || id > M5IO_I2C_BUSES ||
                test_and_set_bit(id - 1, &enabled))
                return -EINVAL;
            ret = m5io_hub_i2c_add_bus(pdev, &mi->buses[id - 1], id, node);
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
