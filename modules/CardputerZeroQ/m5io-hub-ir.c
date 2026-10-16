// SPDX-License-Identifier: GPL-2.0
/* M5IO-HUB FUNC=6 infrared protocol adapter.
 *
 * IR timing/control commands remain TOKENs.  Actual transmit and receive
 * bytes are DATA on the shared CH=6 window; the removed IR_SEND/IR_RX_EVENT
 * TOKENs are deliberately never generated here.
 */
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/string.h>

#include "m5io-hub.h"

#define M5IO_IR_CONFIG       1
#define M5IO_IR_RECEIVE      3
#define M5IO_IR_RESET        5
#define M5IO_IR_DISABLE      7
#define M5IO_IR_CONFIG_LEN   15

static int m5io_hub_ir_token(struct m5io_hub *hub, u8 arg,
                             const u8 *payload, unsigned int payload_len,
                             u8 *reply, unsigned int *reply_len)
{
    struct m5io_hub_request req = {
        .func = M5IO_HUB_FUNC_IR,
        .arg = arg,
        .payload = payload,
        .payload_len = payload_len,
    };
    int ret;

    if (payload_len > M5IO_HUB_TOKEN_PAYLOAD_MAX ||
        (payload_len && !payload))
        return -EINVAL;
    ret = m5io_hub_exec(hub, &req);
    if (ret)
        return ret;
    if (reply && reply_len) {
        if (*reply_len < req.reply_len)
            return -ENOSPC;
        memcpy(reply, req.reply, req.reply_len);
        *reply_len = req.reply_len;
    } else if (reply_len) {
        *reply_len = req.reply_len;
    }
    /* Firmware reports protocol status in ARG.  Keep errno semantics for
     * callers while preserving the status in req for diagnostics. */
    if (req.status == 3)
        return -EINVAL;
    if (req.status == 4)
        return -EBUSY;
    if (req.status == 7)
        return -EIO;
    return req.status ? -EPROTO : 0;
}

int m5io_hub_ir_config(struct m5io_hub *hub, const u8 payload[ M5IO_IR_CONFIG_LEN ],
                       u8 actual[M5IO_IR_CONFIG_LEN])
{
    unsigned int len = M5IO_IR_CONFIG_LEN;
    int ret;

    if (!hub || !payload)
        return -EINVAL;
    ret = m5io_hub_ir_token(hub, M5IO_IR_CONFIG, payload,
                            M5IO_IR_CONFIG_LEN, actual, &len);
    if (!ret && actual && len != M5IO_IR_CONFIG_LEN)
        return -EPROTO;
    return ret;
}
EXPORT_SYMBOL_GPL(m5io_hub_ir_config);

int m5io_hub_ir_receive(struct m5io_hub *hub, bool enable)
{
    u8 payload = enable ? 1 : 0;

    return m5io_hub_ir_token(hub, M5IO_IR_RECEIVE, &payload, 1, NULL, NULL);
}
EXPORT_SYMBOL_GPL(m5io_hub_ir_receive);

int m5io_hub_ir_reset(struct m5io_hub *hub)
{
    return m5io_hub_ir_token(hub, M5IO_IR_RESET, NULL, 0, NULL, NULL);
}
EXPORT_SYMBOL_GPL(m5io_hub_ir_reset);

int m5io_hub_ir_disable(struct m5io_hub *hub)
{
    return m5io_hub_ir_token(hub, M5IO_IR_DISABLE, NULL, 0, NULL, NULL);
}
EXPORT_SYMBOL_GPL(m5io_hub_ir_disable);

int m5io_hub_ir_send(struct m5io_hub *hub, const u8 *data, unsigned int len)
{
    if (!hub || !data || !len || len > M5IO_HUB_DATA_PAYLOAD_MAX)
        return -EINVAL;
    return m5io_hub_send_data_async(hub, M5IO_HUB_CHN_IR, data, len);
}
EXPORT_SYMBOL_GPL(m5io_hub_ir_send);

int m5io_hub_ir_register_rx(struct m5io_hub *hub,
                             m5io_hub_chn_data_handler_t handler, void *data)
{
    return m5io_hub_register_chn_data_handler(hub, M5IO_HUB_CHN_IR,
                                               handler, data);
}
EXPORT_SYMBOL_GPL(m5io_hub_ir_register_rx);

void m5io_hub_ir_unregister_rx(struct m5io_hub *hub,
                               m5io_hub_chn_data_handler_t handler, void *data)
{
    m5io_hub_unregister_chn_data_handler(hub, M5IO_HUB_CHN_IR, handler, data);
}
EXPORT_SYMBOL_GPL(m5io_hub_ir_unregister_rx);

static int m5io_hub_ir_probe(struct platform_device *pdev)
{
    struct m5io_hub *hub = dev_get_drvdata(pdev->dev.parent);

    if (!hub)
        return -EPROBE_DEFER;
    platform_set_drvdata(pdev, hub);
    dev_info(&pdev->dev, "M5IO-HUB infrared DATA CH=6 adapter ready\n");
    return 0;
}

static const struct of_device_id m5io_hub_ir_of_match[] = {
    { .compatible = "m5stack,m5io-hub-ir" },
    { }
};
MODULE_DEVICE_TABLE(of, m5io_hub_ir_of_match);

static struct platform_driver m5io_hub_ir_driver = {
    .probe = m5io_hub_ir_probe,
    .driver = {
        .name = "m5io-hub-ir",
        .of_match_table = m5io_hub_ir_of_match,
    },
};
module_platform_driver(m5io_hub_ir_driver);

MODULE_DESCRIPTION("M5Stack M5IO-HUB infrared FUNC6 DATA adapter");
MODULE_LICENSE("GPL");
