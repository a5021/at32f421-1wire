#include "test.h"
#include "ow_hal_mock.h"
#include "ds18b20.h"

static const uint8_t rom[8] = {
    0x28, 0x7F, 0x1E, 0x5B, 0x16, 0x03, 0x00, 0xFE
};

static ow_status_t spin_convert(ds18b20_t *d)
{
    int guard = 400;
    ow_status_t st;

    while (((st = ds18b20_convert_poll(d)) == OW_BUSY) && (guard-- > 0)) {
    }
    return st;
}

static ow_status_t spin_read(ds18b20_t *d)
{
    int guard = 400;
    ow_status_t st;

    while (((st = ds18b20_read_poll(d)) == OW_BUSY) && (guard-- > 0)) {
    }
    return st;
}

static ow_status_t spin_temp(ds18b20_t *d, int16_t *raw)
{
    int guard = 800;
    ow_status_t st;

    while (((st = ds18b20_temp_poll(d, raw)) == OW_BUSY) && (guard-- > 0)) {
    }
    return st;
}

void test_ds18b20_run(void)
{
    ds18b20_t d;

    /* Init defaults. */
    mock_reset();
    ds18b20_init(&d, true);
    TF_CHECK(d.parasite);
    TF_CHECK(!d.match_rom);
    TF_CHECK_EQ(DS18B20_CONV_US_12, d.conv_us);

    /* Parasite convert: strong pull-up on for the command and the conversion,
     * Skip ROM, 750 ms wait, then released. */
    mock_reset();
    (void)mock_add(MOCK_DS18B20, rom);
    ds18b20_init(&d, true);
    ds18b20_convert_start(&d, 0);
    TF_CHECK_EQ(OW_OK, spin_convert(&d));
    {
        int n = 0;
        const uint8_t *tx = mock_last_tx(&n);

        TF_CHECK_EQ(2, n);
        TF_CHECK_EQ(0xCC, tx[0]);
        TF_CHECK_EQ(0x44, tx[1]);
    }
    TF_CHECK(mock_last_cmd_strong());
    TF_CHECK_EQ(750000, mock_last_wait_us());
    TF_CHECK(!mock_strong());
    TF_CHECK(!mock_read_while_strong());

    /* Address a single device with Match ROM. */
    mock_reset();
    (void)mock_add(MOCK_DS18B20, rom);
    ds18b20_init(&d, true);
    ds18b20_convert_start(&d, rom);
    TF_CHECK(d.match_rom);
    TF_CHECK_MEM(rom, d.rom, 8);
    TF_CHECK_EQ(OW_OK, spin_convert(&d));
    {
        int n = 0;
        const uint8_t *tx = mock_last_tx(&n);

        TF_CHECK_EQ(10, n);
        TF_CHECK_EQ(0x55, tx[0]);
        TF_CHECK_MEM(rom, &tx[1], 8);
        TF_CHECK_EQ(0x44, tx[9]);
    }

    /* Missing presence. */
    mock_reset();
    ds18b20_init(&d, true);
    ds18b20_convert_start(&d, 0);
    TF_CHECK_EQ(OW_NO_PRESENCE, spin_convert(&d));
    TF_CHECK(!mock_strong());

    /* Full temperature cycle, +25.0625 C. */
    mock_reset();
    {
        mock_dev_t *dev = mock_add(MOCK_DS18B20, rom);
        int16_t raw = 0;

        dev->temp_raw = 0x0191;
        ds18b20_init(&d, true);
        ds18b20_temp_start(&d, 0);
        TF_CHECK_EQ(OW_OK, spin_temp(&d, &raw));
        TF_CHECK_EQ(0x0191, raw);
        TF_CHECK_EQ(raw, d.raw);
        TF_CHECK_NEAR(25.0625, ds18b20_celsius(&d), 0.0001);
        TF_CHECK(ds18b20_crc_ok(&d));
    }

    /* Negative temperature, -10.0 C. */
    mock_reset();
    {
        mock_dev_t *dev = mock_add(MOCK_DS18B20, rom);
        int16_t raw = 0;

        dev->temp_raw = -160;
        ds18b20_init(&d, true);
        ds18b20_temp_start(&d, 0);
        TF_CHECK_EQ(OW_OK, spin_temp(&d, &raw));
        TF_CHECK_EQ(-160, raw);
        TF_CHECK_NEAR(-10.0, ds18b20_celsius(&d), 0.0001);
    }

    /* Power-on default, +85.0 C. */
    mock_reset();
    {
        int16_t raw = 0;

        (void)mock_add(MOCK_DS18B20, rom);
        ds18b20_init(&d, true);
        ds18b20_temp_start(&d, 0);
        TF_CHECK_EQ(OW_OK, spin_temp(&d, &raw));
        TF_CHECK_EQ(0x0550, raw);
        TF_CHECK_NEAR(85.0, ds18b20_celsius(&d), 0.0001);
    }

    /* Scratchpad CRC failure. */
    mock_reset();
    {
        mock_dev_t *dev = mock_add(MOCK_DS18B20, rom);

        dev->sp18[8] ^= 0xFFu;
        ds18b20_init(&d, true);
        ds18b20_read_start(&d, 0);
        TF_CHECK_EQ(OW_CRC_ERROR, spin_read(&d));
        TF_CHECK(!ds18b20_crc_ok(&d));
    }

    /* Read Scratchpad issues 0xBE and leaves the bus open-drain. */
    mock_reset();
    {
        (void)mock_add(MOCK_DS18B20, rom);
        ds18b20_init(&d, true);
        ds18b20_read_start(&d, rom);
        TF_CHECK_EQ(OW_OK, spin_read(&d));
        {
            int n = 0;
            const uint8_t *tx = mock_last_tx(&n);

            TF_CHECK_EQ(10, n);
            TF_CHECK_EQ(0x55, tx[0]);
            TF_CHECK_EQ(0xBE, tx[9]);
        }
        TF_CHECK(!mock_read_while_strong());
        TF_CHECK_EQ(-1, mock_last_wait_us());
    }

    /* Start functions are rejected while the engine is busy. */
    mock_reset();
    (void)mock_add(MOCK_DS18B20, rom);
    ds18b20_init(&d, true);
    mock_set_active(true);
    {
        int before = mock_event_count();

        ds18b20_temp_start(&d, 0);
        TF_CHECK_EQ(before, mock_event_count());
    }
    mock_set_active(false);

    /* Second start while a conversion is pending must not clobber the state. */
    mock_reset();
    (void)mock_add(MOCK_DS18B20, rom);
    ds18b20_init(&d, true);
    ds18b20_temp_start(&d, 0);
    {
        int before;
        ow_status_t st = ds18b20_temp_poll(&d, 0);

        TF_CHECK_EQ(OW_BUSY, st);
        before = mock_event_count();
        ds18b20_read_start(&d, 0);
        TF_CHECK_EQ(before, mock_event_count());
        TF_CHECK_EQ(OW_BUSY, ds18b20_temp_poll(&d, 0));
    }
    TF_CHECK_EQ(OW_OK, spin_temp(&d, 0));

    /* A failed convert must not turn into a read of the stale scratchpad. */
    mock_reset();
    ds18b20_init(&d, true);
    ds18b20_temp_start(&d, 0);
    {
        int16_t raw = 0x1234;

        TF_CHECK_EQ(OW_NO_PRESENCE, spin_temp(&d, &raw));
        TF_CHECK_EQ(0x1234, raw);                 /* untouched on failure */
    }

    /* The device comes back, but no conversion has run: polling must not
     * report the sensor's previous value as a fresh reading. */
    (void)mock_add(MOCK_DS18B20, rom);
    {
        int16_t raw = 0x1234;

        TF_CHECK_EQ(OW_PARAM, ds18b20_temp_poll(&d, &raw));
        TF_CHECK_EQ(0x1234, raw);
    }

    /* A new start recovers and reports a real reading. */
    ds18b20_temp_start(&d, 0);
    {
        int16_t raw = 0;

        TF_CHECK_EQ(OW_OK, spin_temp(&d, &raw));
        TF_CHECK_EQ(0x0550, raw);
    }

    /* NULL handling. */
    TF_CHECK_EQ(OW_PARAM, ds18b20_convert_poll(0));
    TF_CHECK_EQ(OW_PARAM, ds18b20_read_poll(0));
    TF_CHECK_EQ(OW_PARAM, ds18b20_temp_poll(0, 0));
    TF_CHECK(!ds18b20_crc_ok(0));
    TF_CHECK_NEAR(0.0, ds18b20_celsius(0), 0.0001);
    ds18b20_init(0, true);
    ds18b20_convert_start(0, 0);
    ds18b20_read_start(0, 0);
    ds18b20_temp_start(0, 0);
}
