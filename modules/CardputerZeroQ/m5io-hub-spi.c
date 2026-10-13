// SPDX-License-Identifier: GPL-2.0
/* SPI2 bridge for CardputerQ FUNC3 (fixed active-low PB12 chip select).
 * The MCU executes one continuous exchange per RPC, releasing CS afterwards.
 * Coalesce transfers up to a CS boundary; never split a CS assertion into
 * several RPCs. Unsupported timing/configuration is rejected before any I/O.
 */
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/spi/spi.h>
#include <linux/unaligned.h>

#include "m5io-hub.h"

#define M5IO_SPI_CONFIG         1
#define M5IO_SPI_FULL_DUPLEX    2
#define M5IO_SPI_SHORT_LEN      13
#define M5IO_SPI_MIN_HZ         250000
#define M5IO_SPI_MAX_HZ         20000000

struct m5io_hub_spi {
    struct m5io_hub *hub;
    u8 tx[M5IO_HUB_SPI_MAX_LEN];
    u8 rx[M5IO_HUB_SPI_MAX_LEN];
};

static int m5io_hub_spi_setup(struct spi_device *spi)
{
    if (spi_get_chipselect(spi, 0) || (spi->mode & ~(SPI_CPOL | SPI_CPHA)) ||
        (spi->bits_per_word != 8 && spi->bits_per_word != 16))
        return -EINVAL;
    if (spi->max_speed_hz < M5IO_SPI_MIN_HZ)
        return -EINVAL;
    if (spi->word_delay.value || spi->cs_setup.value ||
        spi->cs_hold.value || spi->cs_inactive.value)
        return -EOPNOTSUPP;
    return 0;
}

static size_t m5io_hub_spi_max_transfer_size(struct spi_device *spi)
{
    return M5IO_HUB_SPI_MAX_LEN;
}

/* Validate the whole message, including later CS groups, before CONFIG. */
static int m5io_hub_spi_validate(struct spi_message *msg)
{
    struct spi_transfer *t;
    unsigned int len = 0, speed = 0, bits = 0;
    int ret;

    ret = m5io_hub_spi_setup(msg->spi);
    if (ret)
        return ret;
    list_for_each_entry(t, &msg->transfers, transfer_list) {
        if (!t->len || t->len > M5IO_HUB_SPI_MAX_LEN ||
            (t->bits_per_word != 8 && t->bits_per_word != 16) ||
            (t->bits_per_word == 16 && (t->len & 1)) ||
            t->speed_hz < M5IO_SPI_MIN_HZ || t->speed_hz > M5IO_SPI_MAX_HZ)
            return -EINVAL;
        if (t->cs_off || t->delay.value || t->word_delay.value ||
            t->tx_nbits > SPI_NBITS_SINGLE || t->rx_nbits > SPI_NBITS_SINGLE ||
            (list_is_last(&t->transfer_list, &msg->transfers) && t->cs_change))
            return -EOPNOTSUPP;
        if (len && (t->speed_hz != speed || t->bits_per_word != bits))
            return -EOPNOTSUPP;
        speed = t->speed_hz;
        bits = t->bits_per_word;
        len += t->len;
        if (len > M5IO_HUB_SPI_MAX_LEN)
            return -EMSGSIZE;
        if (t->cs_change)
            len = 0;
    }
    return 0;
}

static int m5io_hub_spi_configure(struct m5io_hub_spi *ms,
                                 struct spi_device *spi,
                                 struct spi_transfer *t)
{
    u8 payload[4] = { spi->mode & (SPI_CPOL | SPI_CPHA), t->bits_per_word };
    struct m5io_hub_request req = {
        .func = M5IO_HUB_FUNC_SPI,
        .arg = M5IO_SPI_CONFIG,
        .payload = payload,
        .payload_len = sizeof(payload),
    };
    int ret;

    /* Firmware rounds down to a hardware prescaler. Never round Hz up. */
    put_unaligned_le16(t->speed_hz / 1000, payload + 2);
    ret = m5io_hub_exec(ms->hub, &req);
    if (ret)
        return ret;
    return req.reply_len ? -EPROTO : 0;
}

static int m5io_hub_spi_exchange(struct m5io_hub_spi *ms, unsigned int len)
{
    u8 payload[M5IO_HUB_TOKEN_PAYLOAD_MAX];
    struct m5io_hub_request req = {
        .func = M5IO_HUB_FUNC_SPI,
        .arg = M5IO_SPI_FULL_DUPLEX,
        .payload = payload,
        .payload_len = 2,
    };
    int ret;

    put_unaligned_le16(len, payload);
    if (len <= M5IO_SPI_SHORT_LEN) {
        memcpy(payload + 2, ms->tx, len);
        req.payload_len += len;
    } else {
        req.data_chn = M5IO_HUB_CHN_SPI;
        req.data_tx = ms->tx;
        req.data_rx = ms->rx;
        req.data_len = len;
    }
    ret = m5io_hub_exec(ms->hub, &req);
    if (ret)
        return ret;
    if (len <= M5IO_SPI_SHORT_LEN) {
        if (req.reply_len != len)
            return -EPROTO;
        memcpy(ms->rx, req.reply, len);
    }
    return 0;
}

static void m5io_hub_spi_pack(u8 *dst, const struct spi_transfer *t)
{
    const u8 *src = t->tx_buf;
    unsigned int i;

    /* Linux SPI specifies zeroes on MOSI for an RX-only transfer. */
    if (!src)
        memset(dst, 0, t->len);
    else if (t->bits_per_word == 8)
        memcpy(dst, src, t->len);
    else
        for (i = 0; i < t->len; i += 2)
            put_unaligned_be16(get_unaligned((const u16 *)(src + i)), dst + i);
}

static void m5io_hub_spi_unpack(struct spi_transfer *t, const u8 *src)
{
    u8 *dst = t->rx_buf;
    unsigned int i;

    if (!dst)
        return;
    if (t->bits_per_word == 8)
        memcpy(dst, src, t->len);
    else
        for (i = 0; i < t->len; i += 2)
            put_unaligned(get_unaligned_be16(src + i), (u16 *)(dst + i));
}

static int m5io_hub_spi_transfer_message(struct spi_controller *ctlr,
                                        struct spi_message *msg)
{
    struct m5io_hub_spi *ms = spi_controller_get_devdata(ctlr);
    struct spi_transfer *t, *first, *rx;
    unsigned int len = 0, offset;
    int ret;

    msg->actual_length = 0;
    ret = m5io_hub_spi_validate(msg);
    if (ret)
        goto done;
    first = list_first_entry(&msg->transfers, struct spi_transfer, transfer_list);
    list_for_each_entry(t, &msg->transfers, transfer_list) {
        m5io_hub_spi_pack(ms->tx + len, t);
        len += t->len;
        if (!t->cs_change && !list_is_last(&t->transfer_list, &msg->transfers))
            continue;
        /* Reconfigure each CS group, so MCU resets cannot stale a cache. */
        ret = m5io_hub_spi_configure(ms, msg->spi, first);
        if (ret)
            break;
        ret = m5io_hub_spi_exchange(ms, len);
        if (ret)
            break;
        offset = 0;
        rx = first;
        for (;;) {
            m5io_hub_spi_unpack(rx, ms->rx + offset);
            offset += rx->len;
            if (rx == t)
                break;
            rx = list_next_entry(rx, transfer_list);
        }
        msg->actual_length += len;
        len = 0;
        if (t->cs_change) {
            spi_transfer_cs_change_delay_exec(msg, t);
            first = list_next_entry(t, transfer_list);
        }
    }
done:
    msg->status = ret;
    spi_finalize_current_message(ctlr);
    return 0;
}

static int m5io_hub_spi_probe(struct platform_device *pdev)
{
    struct m5io_hub *hub = dev_get_drvdata(pdev->dev.parent);
    struct spi_controller *ctlr;
    struct m5io_hub_spi *ms;

    if (!hub)
        return -ENODEV;
    ctlr = devm_spi_alloc_host(&pdev->dev, sizeof(*ms));
    if (!ctlr)
        return -ENOMEM;
    ms = spi_controller_get_devdata(ctlr);
    ms->hub = hub;
    ctlr->dev.of_node = pdev->dev.of_node;
    ctlr->bus_num = -1;
    ctlr->num_chipselect = 1;
    ctlr->mode_bits = SPI_CPOL | SPI_CPHA;
    ctlr->bits_per_word_mask = SPI_BPW_MASK(8) | SPI_BPW_MASK(16);
    ctlr->min_speed_hz = M5IO_SPI_MIN_HZ;
    ctlr->max_speed_hz = M5IO_SPI_MAX_HZ;
    ctlr->setup = m5io_hub_spi_setup;
    ctlr->max_transfer_size = m5io_hub_spi_max_transfer_size;
    ctlr->transfer_one_message = m5io_hub_spi_transfer_message;
    platform_set_drvdata(pdev, ctlr);
    return devm_spi_register_controller(&pdev->dev, ctlr);
}

static const struct of_device_id m5io_hub_spi_of_match[] = {
    { .compatible = "m5stack,m5io-hub-spi" },
    { }
};
MODULE_DEVICE_TABLE(of, m5io_hub_spi_of_match);

static struct platform_driver m5io_hub_spi_driver = {
    .driver = {
        .name = "m5io-hub-spi",
        .of_match_table = m5io_hub_spi_of_match,
    },
    .probe = m5io_hub_spi_probe,
};
module_platform_driver(m5io_hub_spi_driver);

MODULE_ALIAS("platform:m5io-hub-spi");
MODULE_DESCRIPTION("M5Stack M5IO-HUB FUNC3 SPI controller");
MODULE_LICENSE("GPL");
