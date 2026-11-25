#include "test.h"
#include "ow_hal_mock.h"
#include "manager.h"
#include "onewire.h"
#include "ds18b20.h"

static const uint8_t rom[8] = {
    0x28, 0x7F, 0x1E, 0x5B, 0x16, 0x03, 0x00, 0xFE
};

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
}
