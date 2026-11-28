#include "test.h"
#include "ow_hal_mock.h"
#include "manager.h"
#include "onewire.h"
#include "ds18b20.h"

static const uint8_t rom[8] = {
    0x28, 0x7F, 0x1E, 0x5B, 0x16, 0x03, 0x00, 0xFE
};

static bool pump_until_state(uint8_t want)
{
    int guard = 20000;

    while (guard-- > 0) {
        manager_poll();
        if ((manager_state_reg() & 0x0Fu) == want) {
            return true;
        }
    }
    return false;
}

static int mock_reset_count(void)
{
    int n = 0;
    int count = mock_event_count();
    const mock_event_t *events = mock_events();

    for (int i = 0; i < count; i++) {
        if (events[i].kind == MOCK_EV_RESET) {
            n++;
        }
    }
    return n;
}

static int mock_strong_true_count(void)
{
    int n = 0;
    int count = mock_event_count();
    const mock_event_t *events = mock_events();

    for (int i = 0; i < count; i++) {
        if ((events[i].kind == MOCK_EV_STRONG) && (events[i].a != 0u)) {
            n++;
        }
    }
    return n;
}

void test_manager_run(void)
{
    /* A command latched while the manager is waiting and the delay has
     * elapsed must be accepted and move the machine to MGR_SEARCH. */
    mock_reset();
    (void)mock_add(MOCK_DS18B20, rom);
    manager_init();
    manager_set_state_for_test(MGR_WAIT);
    mock_set_busy_ticks(1);
    ow_delay_start(200000u);
    TF_CHECK(!ow_delay_poll());
    mock_set_busy_ticks(0);
    TF_CHECK(ow_delay_poll());

    manager_command(1u);
    manager_poll();

    TF_CHECK(manager_map()[REG_FLAGS] & FLAG_BUSY);
    TF_CHECK(!(manager_map()[REG_FLAGS] & FLAG_SNAPSHOT));

    /* A command latched while the manager is busy must be ignored. */
    mock_reset();
    (void)mock_add(MOCK_DS18B20, rom);
    manager_init();
    manager_set_state_for_test(MGR_CONVERT);
    manager_command(2u);
    manager_poll();

    /* Still converting: BUSY flag must not have been set by the command. */
    TF_CHECK(!(manager_map()[REG_FLAGS] & FLAG_BUSY));

    /* The command is consumed: a second poll must not re-trigger it. */
    manager_poll();
    TF_CHECK(!(manager_map()[REG_FLAGS] & FLAG_BUSY));

    /* Idle passes must search again, otherwise a sensor plugged in after boot
     * stays invisible until the host sends a manual rescan. */
    mock_reset();
    manager_init();
    manager_set_state_for_test(MGR_WAIT);
    mock_set_busy_ticks(0);
    ow_delay_start(1u);
    manager_poll();
    TF_CHECK((manager_state_reg() & 0x0Fu) == MGR_SEARCH);

    /* A rescan that loses presence after finding devices must keep the last
     * good ROM list. OW_END means enumeration completed; OW_NO_PRESENCE is a
     * failed pass. */
    mock_reset();
    {
        static const uint8_t rom2[8] = {
            0x2D, 0xF8, 0x47, 0x60, 0x26, 0x00, 0x00, 0x69
        };
        uint8_t old_map[256];

        (void)mock_add(MOCK_DS18B20, rom);
        (void)mock_add(MOCK_DS2431, rom2);
        manager_init();
        mock_set_busy_ticks(0);
        TF_CHECK(pump_until_state(MGR_WAIT));
        TF_CHECK(manager_map()[REG_FLAGS] & FLAG_SNAPSHOT);
        memcpy(old_map, manager_map(), sizeof(old_map));
        TF_CHECK_EQ(2, old_map[REG_DEVICES]);

        mock_set_presence_drop_at_reset(mock_reset_count() + 2);
        ow_delay_start(1u);
        manager_command(1u);
        manager_poll();
        TF_CHECK_EQ(MGR_SEARCH, manager_state_reg() & 0x0Fu);
        TF_CHECK(pump_until_state(MGR_WAIT));
        TF_CHECK(manager_map()[REG_FLAGS] & FLAG_SEARCH_ERROR);
        TF_CHECK_EQ(old_map[REG_DEVICES], manager_map()[REG_DEVICES]);
        TF_CHECK_MEM(&old_map[REG_ROMS], &manager_map()[REG_ROMS], 16u);
    }

    /* External power is detected with Read Power Supply, so conversion must
     * not engage the PA10 strong pull-up. */
    mock_reset();
    (void)mock_add(MOCK_DS18B20, rom);
    mock_dev(0)->parasite = false;
    manager_init();
    mock_set_busy_ticks(0);
    TF_CHECK(pump_until_state(MGR_WAIT));
    TF_CHECK(!(manager_map()[REG_FLAGS] & FLAG_BUS_PARASITE));
    TF_CHECK_EQ(0, mock_strong_true_count());

    /* A parasite-powered bus keeps the strong pull-up and publishes the mode. */
    mock_reset();
    (void)mock_add(MOCK_DS18B20, rom);
    manager_init();
    mock_set_busy_ticks(0);
    TF_CHECK(pump_until_state(MGR_WAIT));
    TF_CHECK(manager_map()[REG_FLAGS] & FLAG_BUS_PARASITE);
    TF_CHECK(mock_strong_true_count() > 0);

    /* No DS18B20 means no parasite-power flag. */
    mock_reset();
    manager_init();
    mock_set_busy_ticks(0);
    TF_CHECK(pump_until_state(MGR_WAIT));
    TF_CHECK(!(manager_map()[REG_FLAGS] & FLAG_BUS_PARASITE));
}
