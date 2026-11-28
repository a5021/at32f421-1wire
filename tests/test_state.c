#include "test.h"
#include "ow_hal_mock.h"
#include "onewire.h"
#include "ds18b20.h"
#include "ds2431.h"

static const uint8_t rom[8] = {
    0x28, 0x7F, 0x1E, 0x5B, 0x16, 0x03, 0x00, 0xFE
};

static ow_status_t run_power(ds18b20_t *temp, bool *parasite)
{
    int guard = 200;
    ow_status_t st;

    *parasite = true;
    if (!ds18b20_power_supply_start(temp, 0)) {
        return OW_PARAM;
    }
    while (((st = ds18b20_power_supply_poll(temp, parasite)) == OW_BUSY) &&
           (guard-- > 0)) {
    }
    return st;
}

void test_state_run(void)
{
    ds2431_t eep;
    ds18b20_t temp;
    bool parasite;

    /* Read Power Supply: external-only and mixed wired-AND replies. */
    mock_reset();
    (void)mock_add(MOCK_DS18B20, rom);
    mock_dev(0)->parasite = false;
    ds18b20_init(&temp, true);
    TF_CHECK_EQ(OW_OK, run_power(&temp, &parasite));
    TF_CHECK(!parasite);
    TF_CHECK(!mock_strong());

    mock_reset();
    (void)mock_add(MOCK_DS18B20, rom);
    mock_dev(0)->parasite = false;
    (void)mock_add(MOCK_DS18B20, rom);
    mock_dev(1)->parasite = true;
    ds18b20_init(&temp, false);
    TF_CHECK_EQ(OW_OK, run_power(&temp, &parasite));
    TF_CHECK(parasite);

    /* Injecting two BUSY polls per pass delays completion by exactly two. */
    mock_reset();
    (void)mock_add(MOCK_DS18B20, rom);
    mock_set_busy_ticks(2);
    {
        bool presence = false;
        int busy = 0;
        int i;

        ow_reset_start();
        for (i = 0; i < 8; i++) {
            ow_status_t st = ow_reset_poll(&presence);
            if (st == OW_BUSY) {
                busy++;
            } else {
                TF_CHECK_EQ(OW_OK, st);
                break;
            }
        }
        TF_CHECK_EQ(2, busy);
        TF_CHECK(presence);
    }

    /* Hardware-timed delay: same BUSY contract and exact duration. */
    mock_reset();
    mock_set_busy_ticks(1);
    ow_delay_start(1234u);
    TF_CHECK(!ow_delay_poll());
    TF_CHECK(ow_delay_poll());
    TF_CHECK_EQ(1234, mock_last_wait_us());

    /* Idle DS2431 polls are harmless. */
    mock_reset();
    ds2431_init(&eep);
    TF_CHECK_EQ(OW_OK, ds2431_read_poll(&eep));
    TF_CHECK_EQ(OW_OK, ds2431_write_poll(&eep));

    /* Wrong-mode poll after a completed read. */
    {
        uint8_t buf[4];
        int guard = 300;
        ow_status_t st;

        (void)mock_add(MOCK_DS2431, rom);
        TF_CHECK(ds2431_read_start(&eep, 0, 0u, buf, 4u));
        while (((st = ds2431_read_poll(&eep)) == OW_BUSY) && (guard-- > 0)) {
        }
        TF_CHECK_EQ(OW_OK, st);
        TF_CHECK_EQ(OW_PARAM, ds2431_write_poll(&eep));
    }

    /* The engine rejects writes while a pass is active. */
    mock_reset();
    mock_set_active(true);
    {
        uint8_t b = 0;
        TF_CHECK(!ow_write_start(&b, 1u));
        mock_set_active(false);
    }

    /* Completed sensor polls stay at OK. */
    mock_reset();
    (void)mock_add(MOCK_DS18B20, rom);
    ds18b20_init(&temp, false);
    {
        int guard = 300;
        ow_status_t st;

        ds18b20_convert_start(&temp, 0);
        while (((st = ds18b20_convert_poll(&temp)) == OW_BUSY) &&
               (guard-- > 0)) {
        }
        TF_CHECK_EQ(OW_OK, st);
        TF_CHECK_EQ(OW_OK, ds18b20_convert_poll(&temp));
        TF_CHECK_EQ(OW_OK, ds18b20_convert_poll(&temp));
    }

    /* A non-parasite convert never touches the strong pull-up. */
    TF_CHECK(!mock_strong());
    TF_CHECK(!mock_last_cmd_strong());
}
