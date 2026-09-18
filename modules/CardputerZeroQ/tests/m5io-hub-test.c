// SPDX-License-Identifier: GPL-2.0
/* Real Linux drivers and MCU FUNC2 implementation, with simulated SPI/I2C pins.
 * This exercises the production state machines in a UML kernel, not on hardware.
 */
#include <kunit/test.h>
#include <kunit/of.h>
#include <linux/kthread.h>

#include "../m5io-hub-core.c"
#include "../m5io-hub-i2c.c"
#include "../../../.slave/cardputerq/code/CardputerQ-APP/Core/User/FUNC2_I2C/func2_i2c.c"

struct m5io_test_state {
    struct kunit *test;
    struct device *parent;
    struct spi_controller *ctlr;
    struct spi_device *spi;
    struct m5io_hub_i2c *bridge;
    struct m5io_hub *hub;
    u8 wire[2048];
    unsigned int head, tail, frames, data_frames, writes;
    u32 speeds[2];
    u8 written[2][255];
    unsigned int written_len[2];
    u16 reg;
    u8 reg_size;
    int forced_status;
    unsigned int config_calls, config_fail_after;
    bool progress, drop_token, drop_data, corrupt_data, wrong_pid, wrong_bus;
    bool duplicate_data;
    u8 gpio_level;
};

static struct m5io_test_state *m5io_test_active;

uint32_t HAL_GetTick(void)
{
    return jiffies_to_msecs(jiffies);
}

/* Independent bitwise reference, deliberately not Linux crc_itu_t(). */
static u16 wire_crc(const u8 *bytes, unsigned int len)
{
    u16 crc = 0xffff;
    unsigned int bit;

    while (len--) {
        crc ^= *bytes++ << 8;
        for (bit = 0; bit < 8; bit++)
            crc = (crc << 1) ^ ((crc & 0x8000) ? 0x1021 : 0);
    }
    return crc;
}

static void wire_push(u8 value)
{
    struct m5io_test_state *s = m5io_test_active;

    s->wire[s->tail++ % sizeof(s->wire)] = value;
}

static void wire_reply(u8 h0, u8 h1, const u8 *payload, u8 len, bool corrupt)
{
    u8 bytes[259] = { h0, h1 };
    u16 crc;
    unsigned int i;

    if (len)
        memcpy(bytes + 2, payload, len);
    crc = wire_crc(bytes, len + 2);
    if (corrupt)
        crc ^= 1;
    for (i = 0; i < 4; i++)
        wire_push(0xff);
    for (i = 0; i < len + 2; i++)
        wire_push(bytes[i]);
    wire_push(crc & 0xff);
    wire_push(crc >> 8);
}

protocol_result_t protocol_tx_enqueue_token(uint8_t tid, uint8_t p,
                                            uint8_t func, uint8_t arg,
                                            const uint8_t *payload,
                                            uint8_t length)
{
    if (!m5io_test_active->drop_token)
        wire_reply((func << 4) | (tid << 2) | (p << 1),
                    (arg << 4) | length, payload, length, false);
    return PROTOCOL_RESULT_OK;
}

protocol_result_t protocol_tx_enqueue_data(uint8_t pid, uint8_t channel,
                                           const uint8_t *payload,
                                           uint8_t length)
{
    struct m5io_test_state *s = m5io_test_active;
    u8 header;

    if (s->drop_data)
        return PROTOCOL_RESULT_OK;
    if (s->wrong_pid)
        pid = 1;
    if (s->wrong_bus)
        channel = channel == 1 ? 2 : 1;
    header = 1 | (pid << 1) | (channel << 3);
    wire_reply(header, length, payload, length, s->corrupt_data);
    if (s->duplicate_data)
        wire_reply(header, length, payload, length, false);
    return PROTOCOL_RESULT_OK;
}

static u8 read_pattern(unsigned int bus, unsigned int offset)
{
    return (bus * 17 + offset) & 0xff;
}

user_i2c_result_t user_i2c_set_speed(user_i2c_bus_t bus, user_i2c_speed_t speed)
{
    m5io_test_active->config_calls++;
    if (m5io_test_active->config_fail_after == m5io_test_active->config_calls)
        return USER_I2C_RESULT_BUSY;
    m5io_test_active->speeds[bus - 1] = speed ? 400000 : 100000;
    return USER_I2C_RESULT_OK;
}

user_i2c_result_t user_i2c_is_device_ready(user_i2c_bus_t bus, uint8_t address,
                                         uint32_t timeout_ms)
{
    return address == 0x50 ? USER_I2C_RESULT_OK : USER_I2C_RESULT_NACK;
}

user_i2c_result_t user_i2c_read(user_i2c_bus_t bus, uint8_t address,
                               uint8_t *data, uint16_t length, uint32_t timeout_ms)
{
    unsigned int i;

    if (address != 0x50)
        return USER_I2C_RESULT_NACK;
    for (i = 0; i < length; i++)
        data[i] = read_pattern(bus, i);
    return USER_I2C_RESULT_OK;
}

user_i2c_result_t user_i2c_write(user_i2c_bus_t bus, uint8_t address,
                                const uint8_t *data, uint16_t length,
                                uint32_t timeout_ms)
{
    struct m5io_test_state *s = m5io_test_active;

    if (address != 0x50)
        return USER_I2C_RESULT_NACK;
    memcpy(s->written[bus - 1], data, length);
    s->written_len[bus - 1] = length;
    s->writes++;
    return USER_I2C_RESULT_OK;
}

user_i2c_result_t user_i2c_mem_read(user_i2c_bus_t bus, uint8_t address,
                                   uint16_t reg, user_i2c_mem_address_size_t size,
                                   uint8_t *data, uint16_t length,
                                   uint32_t timeout_ms)
{
    unsigned int i;

    m5io_test_active->reg = reg;
    m5io_test_active->reg_size = size;
    if (address != 0x50)
        return USER_I2C_RESULT_NACK;
    for (i = 0; i < length; i++)
        data[i] = read_pattern(bus, reg + i);
    return USER_I2C_RESULT_OK;
}

user_i2c_result_t user_i2c_mem_write(user_i2c_bus_t bus, uint8_t address,
                                    uint16_t reg, user_i2c_mem_address_size_t size,
                                    const uint8_t *data, uint16_t length,
                                    uint32_t timeout_ms)
{
    m5io_test_active->reg = reg;
    m5io_test_active->reg_size = size;
    return user_i2c_write(bus, address, data, length, timeout_ms);
}

static int fake_spi_message(struct spi_controller *ctlr, struct spi_message *msg)
{
    struct m5io_test_state *s = m5io_test_active;
    struct spi_transfer *xfer;
    protocol_frame_t frame = {0};
    unsigned int len, i;
    u16 crc;
    int ret = 0;

    list_for_each_entry(xfer, &msg->transfers, transfer_list) {
        const u8 *tx = xfer->tx_buf;
        u8 *rx = xfer->rx_buf;

        KUNIT_EXPECT_EQ(s->test, xfer->speed_hz, 20000000U);
        if (tx[0] != 0xff) {
            /* Exactly one frame per NSS, including its two CRC bytes. */
            len = (tx[0] & 1) ? tx[1] : tx[1] & 15;
            if (xfer->len != len + 4) {
                ret = -EMSGSIZE;
                break;
            }
            crc = wire_crc(tx, len + 2);
            if (tx[len + 2] != (crc & 0xff) || tx[len + 3] != (crc >> 8)) {
                ret = -EBADMSG;
                break;
            }
            frame.header = tx[0] | (tx[1] << 8);
            frame.payload_length = len;
            memcpy(frame.payload, tx + 2, len);
            s->frames++;
            if (tx[0] & 1) {
                frame.type = PROTOCOL_FRAME_DATA;
                frame.data.value = frame.header;
                s->data_frames++;
                KUNIT_EXPECT_EQ(s->test, (unsigned int)frame.data.bits.pid, 0U);
                func2_i2c_process(&frame);
            } else {
                frame.type = PROTOCOL_FRAME_TOKEN;
                frame.token.value = frame.header;
                KUNIT_EXPECT_LE(s->test, (unsigned int)frame.token.bits.func, 2U);
                if (frame.token.bits.func == PROTOCOL_FUNC_GPIO) {
                    if (frame.token.bits.arg == 2)
                        s->gpio_level = frame.payload[1];
                    protocol_tx_enqueue_token(frame.token.bits.tid, 1, 1, 0,
                        &s->gpio_level, frame.token.bits.arg == 3 ? 1 : 0);
                } else if (s->forced_status >= 0) {
                    protocol_tx_enqueue_token(frame.token.bits.tid, 1, 2,
                                               s->forced_status, NULL, 0);
                } else {
                    if (s->progress)
                        protocol_tx_enqueue_token(frame.token.bits.tid, 1, 2,
                                                   1, NULL, 0);
                    func2_i2c_process(&frame);
                }
            }
        }
        for (i = 0; i < xfer->len; i++)
            rx[i] = s->head == s->tail ? 0xff : s->wire[s->head++ % sizeof(s->wire)];
        /* Emulate level-low HOST_READY: FF padding at a transfer boundary
         * must not hide a queued completion TOKEN from the IRQ-less fixture.
         */
        if (s->head != s->tail)
            m5io_hub_rpc_transfer(spi_get_drvdata(msg->spi));
        msg->actual_length += xfer->len;
    }
    KUNIT_EXPECT_EQ(s->test, ret, 0);
    msg->status = ret;
    spi_finalize_current_message(ctlr);
    return 0;
}

static int match_i2c_child(struct device *dev, const void *unused)
{
    return dev->driver == &m5io_hub_i2c_driver.driver;
}

static int match_spi_child(struct device *dev, const void *unused)
{
    return dev->bus == &spi_bus_type;
}

static int m5io_test_init_common(struct kunit *test, bool use_of)
{
    struct spi_board_info info = {
        .modalias = "m5io-hub",
        .max_speed_hz = 20000000,
    };
    struct m5io_test_state *s;
    struct device *child;
    int ret;

    s = kunit_kzalloc(test, sizeof(*s), GFP_KERNEL);
    if (!s)
        return -ENOMEM;
    test->priv = s;
    s->test = test;
    s->forced_status = -1;
    m5io_test_active = s;
    func2_i2c_init();
    s->parent = root_device_register("m5io-test");
    if (IS_ERR(s->parent))
        return PTR_ERR(s->parent);
    s->ctlr = spi_alloc_host(s->parent, 0);
    if (!s->ctlr) {
        ret = -ENOMEM;
        goto free_parent;
    }
    s->ctlr->transfer_one_message = fake_spi_message;
    s->ctlr->num_chipselect = 1;
    s->ctlr->bits_per_word_mask = SPI_BPW_MASK(8);
    if (use_of) {
        ret = of_overlay_apply_kunit(test, m5io_hub_fixture);
        if (ret)
            goto put_ctlr;
        s->ctlr->dev.of_node = of_find_node_by_path("/m5io-test-spi");
        if (!s->ctlr->dev.of_node) {
            ret = -ENODEV;
            goto put_ctlr;
        }
        of_node_put_kunit(test, s->ctlr->dev.of_node);
    }
    ret = spi_register_controller(s->ctlr);
    if (ret)
        goto put_ctlr;
    if (use_of) {
        child = device_find_child(&s->ctlr->dev, NULL, match_spi_child);
        s->spi = child ? to_spi_device(child) : NULL;
        put_device(child);
    } else {
        s->spi = spi_new_device(s->ctlr, &info);
    }
    if (!s->spi) {
        ret = -ENODEV;
        goto free_ctlr;
    }
    s->hub = spi_get_drvdata(s->spi);
    child = device_find_child(&s->spi->dev, NULL, match_i2c_child);
    if (!child || !s->hub) {
        put_device(child);
        ret = -ENODEV;
        goto free_spi;
    }
    s->bridge = dev_get_drvdata(child);
    put_device(child);
    return 0;

free_spi:
    spi_unregister_device(s->spi);
free_ctlr:
    spi_unregister_controller(s->ctlr);
    goto free_parent;
put_ctlr:
    spi_controller_put(s->ctlr);
free_parent:
    root_device_unregister(s->parent);
    return ret;
}

static int m5io_test_init(struct kunit *test)
{
    return m5io_test_init_common(test, false);
}

static int m5io_test_of_init(struct kunit *test)
{
    return m5io_test_init_common(test, true);
}

static void m5io_test_exit(struct kunit *test)
{
    struct m5io_test_state *s = test->priv;

    spi_unregister_device(s->spi);
    spi_unregister_controller(s->ctlr);
    root_device_unregister(s->parent);
    m5io_test_active = NULL;
}

static int test_xfer(struct m5io_test_state *s, int bus, struct i2c_msg *msgs, int n)
{
    return i2c_transfer(&s->bridge->buses[bus - 1].adap, msgs, n);
}

static void m5io_reads_writes(struct kunit *test)
{
    static const unsigned int lengths[] = { 1, 12, 13, 15, 16, 254, 255 };
    struct m5io_test_state *s = test->priv;
    u8 buf[255];
    struct i2c_msg msg = { .addr = 0x50, .buf = buf };
    unsigned int bus, j, i;

    for (bus = 1; bus <= 2; bus++) {
        KUNIT_EXPECT_EQ(test, s->speeds[bus - 1], 100000U);
        for (j = 0; j < ARRAY_SIZE(lengths); j++) {
            msg.len = lengths[j];
            msg.flags = 0;
            for (i = 0; i < msg.len; i++)
                buf[i] = i ^ (0xaa + bus);
            KUNIT_ASSERT_EQ(test, test_xfer(s, bus, &msg, 1), 1);
            KUNIT_EXPECT_EQ(test, s->written_len[bus - 1], (unsigned int)msg.len);
            KUNIT_EXPECT_MEMEQ(test, s->written[bus - 1], buf, msg.len);
            msg.flags = I2C_M_RD;
            memset(buf, 0xee, sizeof(buf));
            KUNIT_ASSERT_EQ(test, test_xfer(s, bus, &msg, 1), 1);
            for (i = 0; i < msg.len; i++)
                KUNIT_EXPECT_EQ(test, buf[i], read_pattern(bus, i));
        }
    }
    KUNIT_EXPECT_EQ(test, s->data_frames, 10U);
}

static void m5io_register_reads(struct kunit *test)
{
    struct m5io_test_state *s = test->priv;
    u8 reg[2] = { 0x12, 0x34 }, buf[255];
    struct i2c_msg msgs[2] = {
        { .addr = 0x50, .buf = reg, .len = 1 },
        { .addr = 0x50, .buf = buf, .flags = I2C_M_RD },
    };
    unsigned int size, len, i, bus;

    for (bus = 1; bus <= 2; bus++)
        for (size = 1; size <= 2; size++)
            for (len = 15; len <= 16; len++) {
                msgs[0].len = size;
                msgs[1].len = len;
                KUNIT_ASSERT_EQ(test, test_xfer(s, bus, msgs, 2), 2);
                KUNIT_EXPECT_EQ(test, s->reg_size, (u8)size);
                KUNIT_EXPECT_EQ(test, s->reg, (u16)(size == 1 ? 0x12 : 0x1234));
                for (i = 0; i < len; i++)
                    KUNIT_EXPECT_EQ(test, buf[i], read_pattern(bus, s->reg + i));
            }
}

static void m5io_detect_and_errors(struct kunit *test)
{
    struct m5io_test_state *s = test->priv;
    struct i2c_msg msg = { .addr = 0x50 };
    static const int errors[] = { -EOPNOTSUPP, -EINVAL, -EBUSY,
                                 -EALREADY, -EBADMSG, -EIO };
    unsigned int i;
    u8 data = 0xaa;

    KUNIT_EXPECT_EQ(test, test_xfer(s, 1, &msg, 1), 1);
    msg.addr = 0x51;
    KUNIT_EXPECT_EQ(test, test_xfer(s, 2, &msg, 1), -ENXIO);
    msg.buf = &data;
    msg.len = 1;
    msg.flags = I2C_M_RD;
    KUNIT_EXPECT_EQ(test, test_xfer(s, 1, &msg, 1), -EIO);
    KUNIT_EXPECT_EQ(test, data, (u8)0xaa);
    msg.addr = 0x50;
    for (i = 0; i < ARRAY_SIZE(errors); i++) {
        s->forced_status = i + 2;
        KUNIT_EXPECT_EQ(test, test_xfer(s, 1, &msg, 1), errors[i]);
        KUNIT_EXPECT_EQ(test, data, (u8)0xaa);
    }
    s->forced_status = -1;
    s->progress = true;
    KUNIT_EXPECT_EQ(test, test_xfer(s, 2, &msg, 1), 1);
    KUNIT_EXPECT_EQ(test, data, read_pattern(2, 0));
}

static void m5io_rejects_unsupported(struct kunit *test)
{
    struct m5io_test_state *s = test->priv;
    u8 buf[256] = {0};
    struct i2c_msg msgs[3] = {
        { .addr = 0x50, .len = 1, .buf = buf },
        { .addr = 0x51, .len = 1, .buf = buf, .flags = I2C_M_RD },
        { .addr = 0x50, .len = 1, .buf = buf },
    };
    const u16 flags[] = { I2C_M_TEN, I2C_M_NOSTART, I2C_M_IGNORE_NAK,
                         I2C_M_RECV_LEN, I2C_M_STOP, I2C_M_NO_RD_ACK };
    unsigned int frames = s->frames, i;

    KUNIT_EXPECT_EQ(test, test_xfer(s, 1, msgs, 3), -EOPNOTSUPP);
    KUNIT_EXPECT_EQ(test, test_xfer(s, 1, msgs, 2), -EOPNOTSUPP);
    for (i = 0; i < ARRAY_SIZE(flags); i++) {
        msgs[0].flags = flags[i];
        KUNIT_EXPECT_EQ(test, test_xfer(s, 1, msgs, 1), -EOPNOTSUPP);
    }
    msgs[0].flags = 0;
    msgs[0].len = 256;
    KUNIT_EXPECT_EQ(test, test_xfer(s, 1, msgs, 1), -EOPNOTSUPP);
    msgs[0].len = 1;
    msgs[0].addr = 7;
    KUNIT_EXPECT_EQ(test, test_xfer(s, 1, msgs, 1), -EINVAL);
    msgs[0].addr = 0x78;
    KUNIT_EXPECT_EQ(test, test_xfer(s, 1, msgs, 1), -EINVAL);
    KUNIT_EXPECT_EQ(test, s->frames, frames);
}

static void m5io_smbus_transfers(struct kunit *test)
{
    static const u8 lengths[] = { 1, 10, 11, 12, 15, 16, 32 };
    struct m5io_test_state *s = test->priv;
    union i2c_smbus_data data;
    u8 expected[34];
    unsigned int bus, i, j;

    /* Use the Linux SMBus emulator so command/count bytes and little-endian
     * words are verified across the real I2C message conversion path.
     */
    for (bus = 1; bus <= 2; bus++) {
        struct i2c_adapter *adap = &s->bridge->buses[bus - 1].adap;

        KUNIT_ASSERT_EQ(test, i2c_smbus_xfer(adap, 0x50, 0, I2C_SMBUS_WRITE,
                            0x24, I2C_SMBUS_BYTE, NULL), 0);
        KUNIT_EXPECT_EQ(test, s->written_len[bus - 1], 1U);
        KUNIT_EXPECT_EQ(test, s->written[bus - 1][0], (u8)0x24);
        KUNIT_ASSERT_EQ(test, i2c_smbus_xfer(adap, 0x50, 0, I2C_SMBUS_READ,
                            0, I2C_SMBUS_BYTE, &data), 0);
        KUNIT_EXPECT_EQ(test, data.byte, read_pattern(bus, 0));

        data.byte = 0xa5;
        expected[0] = 0x24;
        expected[1] = data.byte;
        KUNIT_ASSERT_EQ(test, i2c_smbus_xfer(adap, 0x50, 0, I2C_SMBUS_WRITE,
                            0x24, I2C_SMBUS_BYTE_DATA, &data), 0);
        KUNIT_EXPECT_EQ(test, s->written_len[bus - 1], 2U);
        KUNIT_EXPECT_MEMEQ(test, s->written[bus - 1], expected, 2);
        KUNIT_ASSERT_EQ(test, i2c_smbus_xfer(adap, 0x50, 0, I2C_SMBUS_READ,
                            0x24, I2C_SMBUS_BYTE_DATA, &data), 0);
        KUNIT_EXPECT_EQ(test, data.byte, read_pattern(bus, 0x24));

        data.word = 0x1234;
        expected[1] = 0x34;
        expected[2] = 0x12;
        KUNIT_ASSERT_EQ(test, i2c_smbus_xfer(adap, 0x50, 0, I2C_SMBUS_WRITE,
                            0x24, I2C_SMBUS_WORD_DATA, &data), 0);
        KUNIT_EXPECT_EQ(test, s->written_len[bus - 1], 3U);
        KUNIT_EXPECT_MEMEQ(test, s->written[bus - 1], expected, 3);
        KUNIT_ASSERT_EQ(test, i2c_smbus_xfer(adap, 0x50, 0, I2C_SMBUS_READ,
                            0x24, I2C_SMBUS_WORD_DATA, &data), 0);
        KUNIT_EXPECT_EQ(test, data.word, (u16)(read_pattern(bus, 0x24) |
                                             (read_pattern(bus, 0x25) << 8)));

        for (j = 0; j < ARRAY_SIZE(lengths); j++) {
            data.block[0] = lengths[j];
            for (i = 1; i <= lengths[j]; i++)
                data.block[i] = i ^ 0xa5;
            memcpy(expected + 1, data.block, lengths[j] + 1);
            KUNIT_ASSERT_EQ(test, i2c_smbus_xfer(adap, 0x50, 0, I2C_SMBUS_WRITE,
                                0x24, I2C_SMBUS_BLOCK_DATA, &data), 0);
            KUNIT_EXPECT_EQ(test, s->written_len[bus - 1], lengths[j] + 2U);
            KUNIT_EXPECT_MEMEQ(test, s->written[bus - 1], expected, lengths[j] + 2);

            memcpy(expected + 1, data.block + 1, lengths[j]);
            KUNIT_ASSERT_EQ(test, i2c_smbus_xfer(adap, 0x50, 0, I2C_SMBUS_WRITE,
                                0x24, I2C_SMBUS_I2C_BLOCK_DATA, &data), 0);
            KUNIT_EXPECT_EQ(test, s->written_len[bus - 1], lengths[j] + 1U);
            KUNIT_EXPECT_MEMEQ(test, s->written[bus - 1], expected, lengths[j] + 1);
            KUNIT_ASSERT_EQ(test, i2c_smbus_xfer(adap, 0x50, 0, I2C_SMBUS_READ,
                                0x24, I2C_SMBUS_I2C_BLOCK_DATA, &data), 0);
            KUNIT_EXPECT_EQ(test, data.block[0], lengths[j]);
            for (i = 0; i < lengths[j]; i++)
                KUNIT_EXPECT_EQ(test, data.block[i + 1], read_pattern(bus, 0x24 + i));
        }
    }
}

static void m5io_smbus_unsupported(struct kunit *test)
{
    struct m5io_test_state *s = test->priv;
    struct i2c_adapter *adap = &s->bridge->buses[0].adap;
    union i2c_smbus_data data = { .word = 1 };
    unsigned int frames = s->frames;
    u32 unsupported = I2C_FUNC_SMBUS_QUICK | I2C_FUNC_SMBUS_PEC |
                      I2C_FUNC_SMBUS_PROC_CALL | I2C_FUNC_SMBUS_READ_BLOCK_DATA |
                      I2C_FUNC_SMBUS_BLOCK_PROC_CALL | I2C_FUNC_10BIT_ADDR;

    KUNIT_EXPECT_EQ(test, i2c_get_functionality(adap) & unsupported, 0U);
    KUNIT_EXPECT_EQ(test, i2c_smbus_xfer(adap, 0x50, 0, I2C_SMBUS_WRITE,
                        0x24, I2C_SMBUS_PROC_CALL, &data), -EOPNOTSUPP);
    KUNIT_EXPECT_EQ(test, i2c_smbus_xfer(adap, 0x50, 0, I2C_SMBUS_READ,
                        0x24, I2C_SMBUS_BLOCK_DATA, &data), -EOPNOTSUPP);
    KUNIT_EXPECT_EQ(test, i2c_smbus_xfer(adap, 0x50, I2C_CLIENT_PEC, I2C_SMBUS_READ,
                        0x24, I2C_SMBUS_WORD_DATA, &data), -EOPNOTSUPP);
    KUNIT_EXPECT_EQ(test, s->frames, frames);
}

static void m5io_core_mem_write_and_scan(struct kunit *test)
{
    struct m5io_test_state *s = test->priv;
    u8 payload[15] = { 2, 0x50, 2, 0x34, 0x12, 9 };
    u8 data[255];
    struct m5io_hub_request req = {
        .func = 2, .arg = 6, .payload = payload, .payload_len = 15,
    };
    unsigned int i;

    memset(data, 0xa5, sizeof(data));
    memcpy(payload + 6, data, 9);
    KUNIT_ASSERT_EQ(test, m5io_hub_exec(s->hub, &req), 0);
    KUNIT_EXPECT_EQ(test, req.reply_len, 0U);
    KUNIT_EXPECT_EQ(test, s->reg, (u16)0x1234);
    KUNIT_EXPECT_MEMEQ(test, s->written[1], data, 9);
    req.payload_len = 6;
    req.data_chn = 2;
    req.data_tx = data;
    for (i = 10; i <= 255; i += 245) {
        payload[5] = i;
        req.data_len = i;
        KUNIT_ASSERT_EQ(test, m5io_hub_exec(s->hub, &req), 0);
        KUNIT_EXPECT_MEMEQ(test, s->written[1], data, i);
    }
    req.data_tx = NULL;
    req.data_len = 0;
    req.arg = 4;
    req.payload_len = 1;
    KUNIT_ASSERT_EQ(test, m5io_hub_exec(s->hub, &req), 0);
    KUNIT_ASSERT_EQ(test, req.reply_len, 14U);
    for (i = 0; i < 14; i++)
        KUNIT_EXPECT_EQ(test, req.reply[i], (u8)(i == 9 ? 1 : 0));
    req.arg = 1;
    req.payload_len = 2;
    payload[1] = 1;
    KUNIT_ASSERT_EQ(test, m5io_hub_exec(s->hub, &req), 0);
    KUNIT_EXPECT_EQ(test, s->speeds[1], 400000U);
}

static void check_failed_read(struct kunit *test, int error)
{
    struct m5io_test_state *s = test->priv;
    u8 buf[16], expected[16];
    struct i2c_msg msg = { .addr = 0x50, .len = 16, .buf = buf, .flags = I2C_M_RD };

    memset(buf, 0xa5, sizeof(buf));
    memset(expected, 0xa5, sizeof(expected));
    KUNIT_EXPECT_EQ(test, test_xfer(s, 1, &msg, 1), error);
    KUNIT_EXPECT_MEMEQ(test, buf, expected, sizeof(buf));
    KUNIT_EXPECT_EQ(test, test_xfer(s, 2, &msg, 1), -EPIPE);
}

static void m5io_missing_data(struct kunit *test)
{
    ((struct m5io_test_state *)test->priv)->drop_data = true;
    check_failed_read(test, -EPROTO);
}

static void m5io_corrupt_data(struct kunit *test)
{
    ((struct m5io_test_state *)test->priv)->corrupt_data = true;
    check_failed_read(test, -EPROTO);
}

static void m5io_wrong_pid(struct kunit *test)
{
    ((struct m5io_test_state *)test->priv)->wrong_pid = true;
    check_failed_read(test, -EPROTO);
}

static void m5io_wrong_bus(struct kunit *test)
{
    ((struct m5io_test_state *)test->priv)->wrong_bus = true;
    check_failed_read(test, -EPROTO);
}

static void m5io_duplicate_data(struct kunit *test)
{
    ((struct m5io_test_state *)test->priv)->duplicate_data = true;
    check_failed_read(test, -EPROTO);
}

static void m5io_missing_completion(struct kunit *test)
{
    ((struct m5io_test_state *)test->priv)->drop_token = true;
    check_failed_read(test, -ETIMEDOUT);
}

static void m5io_gpio_regression(struct kunit *test)
{
    struct m5io_test_state *s = test->priv;
    const u8 hello[] = { 0x02, 0x10 };

    KUNIT_EXPECT_EQ(test, wire_crc(hello, sizeof(hello)), (u16)0x695c);
    KUNIT_EXPECT_EQ(test, m5io_hub_pinMode(s->hub, 3, 0), 0);
    KUNIT_EXPECT_EQ(test, m5io_hub_digitalWrite(s->hub, 3, 1), 0);
    KUNIT_EXPECT_EQ(test, m5io_hub_digitalRead(s->hub, 3), 1);
    KUNIT_EXPECT_EQ(test, m5io_hub_digitalWrite(s->hub, 3, 0), 0);
    KUNIT_EXPECT_EQ(test, m5io_hub_digitalRead(s->hub, 3), 0);
}

struct concurrent_xfer {
    struct m5io_test_state *s;
    int bus;
    int result;
    struct completion done;
};

static int concurrent_worker(void *data)
{
    struct concurrent_xfer *job = data;
    u8 buf[255];
    struct i2c_msg msg = { .addr = 0x50, .len = sizeof(buf), .buf = buf };
    unsigned int i, j;

    for (i = 0; i < 16; i++) {
        memset(buf, job->bus + i, sizeof(buf));
        msg.flags = 0;
        if (test_xfer(job->s, job->bus, &msg, 1) != 1) {
            job->result = -EIO;
            break;
        }
        msg.flags = I2C_M_RD;
        if (test_xfer(job->s, job->bus, &msg, 1) != 1) {
            job->result = -EIO;
            break;
        }
        for (j = 0; j < sizeof(buf); j++)
            if (buf[j] != read_pattern(job->bus, j))
                job->result = -EBADMSG;
    }
    complete(&job->done);
    while (!kthread_should_stop())
        schedule_timeout_interruptible(1);
    return job->result;
}

static void m5io_concurrent_buses(struct kunit *test)
{
    struct concurrent_xfer jobs[2];
    struct task_struct *tasks[2];
    unsigned int i;

    for (i = 0; i < 2; i++) {
        jobs[i] = (struct concurrent_xfer){ .s = test->priv, .bus = i + 1 };
        init_completion(&jobs[i].done);
        tasks[i] = kthread_run(concurrent_worker, &jobs[i], "m5io-test-%u", i);
        if (IS_ERR(tasks[i])) {
            if (i)
                kthread_stop(tasks[0]);
            KUNIT_FAIL(test, "cannot create test thread");
            return;
        }
    }
    for (i = 0; i < 2; i++) {
        KUNIT_EXPECT_NE(test, wait_for_completion_timeout(&jobs[i].done, 20 * HZ), 0UL);
        KUNIT_EXPECT_EQ(test, kthread_stop(tasks[i]), 0);
    }
}

static struct kunit_case m5io_cases[] = {
    KUNIT_CASE(m5io_reads_writes),
    KUNIT_CASE(m5io_register_reads),
    KUNIT_CASE(m5io_detect_and_errors),
    KUNIT_CASE(m5io_rejects_unsupported),
    KUNIT_CASE(m5io_smbus_transfers),
    KUNIT_CASE(m5io_smbus_unsupported),
    KUNIT_CASE(m5io_core_mem_write_and_scan),
    KUNIT_CASE(m5io_missing_data),
    KUNIT_CASE(m5io_corrupt_data),
    KUNIT_CASE(m5io_wrong_pid),
    KUNIT_CASE(m5io_wrong_bus),
    KUNIT_CASE(m5io_duplicate_data),
    KUNIT_CASE(m5io_missing_completion),
    KUNIT_CASE(m5io_gpio_regression),
    KUNIT_CASE(m5io_concurrent_buses),
    {}
};

static struct kunit_suite m5io_suite = {
    .name = "m5io-hub",
    .init = m5io_test_init,
    .exit = m5io_test_exit,
    .test_cases = m5io_cases,
};

static void m5io_of_check_devices(struct kunit *test, bool present)
{
    struct m5io_test_state *s = test->priv;
    struct device_node *i2c = of_get_child_by_name(s->spi->dev.of_node, "i2c");

    KUNIT_ASSERT_NOT_NULL(test, i2c);
    of_node_put_kunit(test, i2c);
    for_each_available_child_of_node_scoped(i2c, node) {
        struct i2c_adapter *adap = of_find_i2c_adapter_by_node(node);
        struct device_node *peripheral = of_get_child_by_name(node, "peripheral");
        struct i2c_client *client;
        u32 id = 0;

        KUNIT_EXPECT_EQ(test, !!adap, present);
        if (adap) {
            KUNIT_EXPECT_EQ(test, of_property_read_u32(node, "reg", &id), 0);
            KUNIT_EXPECT_TRUE(test, s->bridge && id >= 1 && id <= 2);
            if (s->bridge && id >= 1 && id <= 2)
                KUNIT_EXPECT_PTR_EQ(test, adap, &s->bridge->buses[id - 1].adap);
            put_device(&adap->dev);
        }
        KUNIT_EXPECT_NOT_NULL(test, peripheral);
        if (!peripheral)
            continue;
        client = of_find_i2c_device_by_node(peripheral);
        of_node_put(peripheral);
        KUNIT_EXPECT_EQ(test, !!client, present);
        if (client) {
            KUNIT_EXPECT_EQ(test, client->addr, (u16)0x50);
            put_device(&client->dev);
        }
    }
}

static void m5io_of_enumeration(struct kunit *test)
{
    struct m5io_test_state *s = test->priv;
    u8 reg = 0x24, buf[32];
    struct i2c_msg msgs[] = {
        { .addr = 0x50, .len = 1, .buf = &reg },
        { .addr = 0x50, .len = sizeof(buf), .buf = buf, .flags = I2C_M_RD },
    };
    unsigned int bus, i;

    KUNIT_EXPECT_EQ(test, s->spi->max_speed_hz, 20000000U);
    KUNIT_EXPECT_EQ(test, s->spi->cs_setup.value, (u16)10000);
    KUNIT_EXPECT_EQ(test, s->spi->cs_setup.unit, (u8)SPI_DELAY_UNIT_NSECS);
    KUNIT_EXPECT_EQ(test, s->spi->cs_inactive.value, (u16)100);
    KUNIT_EXPECT_EQ(test, s->spi->cs_inactive.unit, (u8)SPI_DELAY_UNIT_USECS);
    KUNIT_EXPECT_EQ(test, s->speeds[0], 100000U);
    KUNIT_EXPECT_EQ(test, s->speeds[1], 400000U);
    m5io_of_check_devices(test, true);
    for (bus = 1; bus <= 2; bus++) {
        KUNIT_ASSERT_EQ(test, test_xfer(s, bus, msgs, 2), 2);
        for (i = 0; i < sizeof(buf); i++)
            KUNIT_EXPECT_EQ(test, buf[i], read_pattern(bus, reg + i));
    }
}

KUNIT_DEFINE_ACTION_WRAPPER(m5io_put_device, put_device, struct device *);

static void m5io_of_reprobe(struct kunit *test, bool fail_config)
{
    struct m5io_test_state *s = test->priv;
    struct device *child = device_find_child(&s->spi->dev, NULL, match_i2c_child);
    unsigned int i;

    KUNIT_ASSERT_NOT_NULL(test, child);
    KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, m5io_put_device, child), 0);
    for (i = 0; i < 3; i++) {
        device_release_driver(child);
        s->bridge = NULL;
        m5io_of_check_devices(test, false);
        if (fail_config) {
            /* Fail the second CONFIG regardless of OF child traversal order. */
            s->config_calls = 0;
            s->config_fail_after = 2;
            KUNIT_EXPECT_EQ(test, device_attach(child), 0);
            KUNIT_EXPECT_EQ(test, s->config_calls, 2U);
            KUNIT_EXPECT_PTR_EQ(test, child->driver, NULL);
            m5io_of_check_devices(test, false);
            s->config_fail_after = 0;
        }
        KUNIT_ASSERT_EQ(test, device_attach(child), 1);
        s->bridge = dev_get_drvdata(child);
        KUNIT_ASSERT_NOT_NULL(test, s->bridge);
        m5io_of_enumeration(test);
    }
}

static void m5io_of_lifecycle(struct kunit *test)
{
    m5io_of_reprobe(test, false);
}

static void m5io_of_config_failure(struct kunit *test)
{
    m5io_of_reprobe(test, true);
}

static struct kunit_case m5io_of_cases[] = {
    KUNIT_CASE(m5io_of_enumeration),
    KUNIT_CASE(m5io_of_lifecycle),
    KUNIT_CASE(m5io_of_config_failure),
    {}
};

static struct kunit_suite m5io_of_suite = {
    .name = "m5io-hub-of",
    .init = m5io_test_of_init,
    .exit = m5io_test_exit,
    .test_cases = m5io_of_cases,
};
kunit_test_suites(&m5io_suite, &m5io_of_suite);
