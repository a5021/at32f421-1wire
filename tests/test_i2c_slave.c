#include "test.h"
#include "i2c_hw.h"
#include "i2c_hw_mock.h"
#include "i2c_slave.h"
#include "manager.h"
#include "ow_hal_mock.h"

#define STS1_ADDRF      0x00000002u
#define STS1_STOPF      0x00000010u
#define STS1_STARTF     0x00000001u
#define STS1_RDBF       0x00000040u
#define STS1_TDBE       0x00000080u
#define STS1_BUSERR     0x00000100u
#define STS1_ARLOST     0x00000200u
#define STS1_ACKFAIL    0x00000400u
#define STS1_OUF        0x00000800u
#define STS2_DIRF       0x00000004u
#define STS2_BUSYF      0x00000002u

static void reset_all(void)
{
    mock_reset();
    i2c_hw_mock_reset();
    manager_init();
    i2c_slave_init(i2c_hw_reset_cause());
    i2c_slave_set_cycles_per_us(120u);
}

static void start_read(void)
{
    i2c_hw_mock_set_status(STS1_ADDRF, STS2_DIRF, 0x00000003u);
    i2c_slave_poll();
}

static void start_write(void)
{
    i2c_hw_mock_set_status(STS1_ADDRF, 0u, 0x00000003u);
    i2c_slave_poll();
}

static void write_register(uint8_t addr, uint8_t value)
{
    i2c_hw_mock_set_dt(addr);
    i2c_hw_mock_set_status(STS1_RDBF, 0u, 0x00000003u);
    i2c_slave_poll();
    i2c_hw_mock_set_dt(value);
    i2c_hw_mock_set_status(STS1_RDBF, 0u, 0x00000003u);
    i2c_slave_poll();
}

static void stop_frame(void)
{
    i2c_hw_mock_set_status(STS1_STOPF, 0u, 0x00000003u);
    i2c_slave_poll();
}

static void test_stop_address_order(void)
{
    reset_all();
    start_write();
    write_register(REG_SELECTED, 3u);

    i2c_hw_mock_reset();
    i2c_hw_mock_set_status(STS1_STOPF | STS1_ADDRF, STS2_DIRF, 0x00000003u);
    i2c_slave_poll();

    TF_CHECK(i2c_slave_busy());
    TF_CHECK_EQ(2u, i2c_slave_reg_byte(REG_FRAMES));
    TF_CHECK_EQ(0u, g_regs[REG_SELECTED]);
    TF_CHECK_EQ(6u, i2c_mock_event_count);
    TF_CHECK_EQ(MOCK_EVT_STS1, i2c_hw_mock_event(0).type);
    TF_CHECK_EQ(MOCK_EVT_STS1, i2c_hw_mock_event(1).type);
    TF_CHECK_EQ(MOCK_EVT_STOP_REARM, i2c_hw_mock_event(2).type);
    TF_CHECK_EQ(MOCK_EVT_STS1, i2c_hw_mock_event(3).type);
    TF_CHECK_EQ(MOCK_EVT_STS2, i2c_hw_mock_event(4).type);
    TF_CHECK_EQ(MOCK_EVT_DT_WRITE, i2c_hw_mock_event(5).type);
}

static void test_repeated_start(void)
{
    reset_all();
    start_read();

    i2c_hw_mock_set_status(STS1_STARTF, 0u, 0x00000003u);
    i2c_slave_poll();
    i2c_slave_poll();
    TF_CHECK_EQ(1u, i2c_slave_reg_byte(REG_DIAG_START));
    TF_CHECK(i2c_slave_busy());
    TF_CHECK_EQ(0u, i2c_hw_mock_count(MOCK_EVT_ABORT));

    i2c_hw_mock_set_status(STS1_ADDRF, STS2_DIRF, 0x00000003u);
    i2c_slave_poll();
    TF_CHECK_EQ(2u, i2c_slave_reg_byte(REG_FRAMES));
    TF_CHECK(i2c_slave_busy());
}

static void test_stretch_model(void)
{
    reset_all();
    start_read();
    i2c_hw_mock_reset();
    i2c_hw_mock_set_status(STS1_TDBE, STS2_DIRF, 0x00000003u);
    TF_CHECK((i2c_mock_gpio & 0x00000001u) == 0u);
    TF_CHECK(i2c_slave_busy());
    i2c_slave_poll();
    TF_CHECK((i2c_mock_gpio & 0x00000001u) != 0u);
    TF_CHECK((i2c_mock_sts1 & STS1_TDBE) == 0u);
    TF_CHECK_EQ(1u, i2c_hw_mock_count(MOCK_EVT_DT_WRITE));

    reset_all();
    start_write();
    i2c_hw_mock_set_dt(REG_MAGIC);
    i2c_hw_mock_set_status(STS1_RDBF, 0u, 0x00000003u);
    TF_CHECK((i2c_mock_gpio & 0x00000001u) == 0u);
    i2c_slave_poll();
    TF_CHECK((i2c_mock_gpio & 0x00000001u) != 0u);
    TF_CHECK((i2c_mock_sts1 & STS1_RDBF) == 0u);

    reset_all();
    start_read();
    i2c_hw_mock_reset();
    i2c_hw_mock_set_status(STS1_BUSERR | STS1_TDBE, STS2_DIRF,
                           0x00000003u);
    i2c_slave_poll();
    TF_CHECK(!i2c_slave_busy());
    TF_CHECK_EQ(1u, i2c_hw_mock_count(MOCK_EVT_ABORT));
    TF_CHECK_EQ(0u, i2c_hw_mock_count(MOCK_EVT_DT_WRITE));
    TF_CHECK_EQ(0x00000003u, i2c_mock_gpio);
}

static void test_nack_lifetime(void)
{
    reset_all();
    start_read();

    i2c_hw_mock_set_status(STS1_ACKFAIL, 0u, 0x00000003u);
    i2c_slave_poll();
    TF_CHECK(i2c_slave_busy());
    TF_CHECK_EQ(0u, i2c_hw_mock_count(MOCK_EVT_ABORT));
    TF_CHECK_EQ(0u, i2c_slave_reg_byte(REG_DIAG_EARLY_NACK));
    TF_CHECK_EQ(0u, i2c_slave_reg_byte(REG_DIAG_NACK_POS));

    stop_frame();
    TF_CHECK(!i2c_slave_busy());
}

static void test_early_and_final_nack_positions(void)
{
    reset_all();
    start_read();
    for (int i = 0; i < 128; i++) {
        i2c_hw_mock_set_status(STS1_TDBE, STS2_DIRF, 0x00000003u);
        i2c_slave_poll();
    }
    i2c_hw_mock_set_status(STS1_ACKFAIL, 0u, 0x00000003u);
    i2c_slave_poll();
    TF_CHECK_EQ(1u, i2c_slave_reg_byte(REG_DIAG_EARLY_NACK));
    TF_CHECK_EQ(129u, i2c_slave_reg_byte(REG_DIAG_EARLY_NACK_POS));
    TF_CHECK_EQ(129u, i2c_slave_reg_byte(REG_DIAG_NACK_POS));

    reset_all();
    start_read();
    for (int i = 0; i < 255; i++) {
        i2c_hw_mock_set_status(STS1_TDBE, STS2_DIRF, 0x00000003u);
        i2c_slave_poll();
    }
    i2c_hw_mock_set_status(STS1_ACKFAIL, 0u, 0x00000003u);
    i2c_slave_poll();
    TF_CHECK_EQ(0u, i2c_slave_reg_byte(REG_DIAG_EARLY_NACK));
}

static void test_stop_gate(void)
{
    reset_all();
    start_read();

    i2c_hw_mock_set_status(STS1_STOPF, 0u, 0u);
    i2c_slave_poll();
    TF_CHECK(i2c_slave_busy());
    TF_CHECK_EQ(1u, i2c_slave_reg_byte(REG_DIAG_GATE));
    TF_CHECK_EQ(1u, i2c_slave_reg_byte(REG_DIAG_GATE_POS));

    stop_frame();
    TF_CHECK(!i2c_slave_busy());
}

static void test_buserr_precedes_tdbe(void)
{
    reset_all();
    start_read();
    i2c_hw_mock_reset();

    i2c_hw_mock_set_status(STS1_BUSERR | STS1_TDBE, STS2_DIRF, 0x00000003u);
    i2c_slave_poll();
    TF_CHECK(!i2c_slave_busy());
    TF_CHECK_EQ(1u, i2c_hw_mock_count(MOCK_EVT_ABORT));
    TF_CHECK_EQ(0u, i2c_hw_mock_count(MOCK_EVT_DT_WRITE));
    TF_CHECK_EQ(1u, i2c_hw_mock_count(MOCK_EVT_CLEAR_FLAGS));
    TF_CHECK_EQ(1u, g_i2c_err_bus);
    TF_CHECK_EQ(1u, i2c_slave_reg_byte(REG_ERR_BUS));
    TF_CHECK_EQ(HEALTH_BUSERR, i2c_slave_reg_byte(REG_HEALTH));
    TF_CHECK_EQ(1u, i2c_slave_reg_byte(REG_DIAG_BUS));
}

static void test_arlost_precedes_rdbf(void)
{
    reset_all();
    start_write();
    i2c_hw_mock_reset();

    i2c_hw_mock_set_status(STS1_ARLOST | STS1_RDBF, 0u, 0x00000003u);
    i2c_slave_poll();
    TF_CHECK(!i2c_slave_busy());
    TF_CHECK_EQ(1u, i2c_hw_mock_count(MOCK_EVT_ABORT));
    TF_CHECK_EQ(0u, i2c_hw_mock_count(MOCK_EVT_DT_READ));
    TF_CHECK_EQ(1u, g_i2c_err_arl);
    TF_CHECK_EQ(HEALTH_ARLOST, i2c_slave_reg_byte(REG_HEALTH));
}

static void test_ouf_position_and_priority(void)
{
    reset_all();
    start_read();
    i2c_hw_mock_reset();

    i2c_hw_mock_set_status(STS1_OUF | STS1_TDBE, STS2_DIRF, 0x00000003u);
    i2c_slave_poll();
    TF_CHECK(!i2c_slave_busy());
    TF_CHECK_EQ(1u, i2c_hw_mock_count(MOCK_EVT_ABORT));
    TF_CHECK_EQ(0u, i2c_hw_mock_count(MOCK_EVT_DT_WRITE));
    TF_CHECK_EQ(1u, g_i2c_err_ouf);
    TF_CHECK_EQ(1u, i2c_slave_reg_byte(REG_DIAG_REASON));
    TF_CHECK_EQ(1u, i2c_slave_reg_byte(REG_OUF_POS));
    TF_CHECK_EQ(1u, i2c_slave_reg_byte(REG_OUF_PTR));
    TF_CHECK_EQ(HEALTH_OUF, i2c_slave_reg_byte(REG_HEALTH));
}

static void test_register_write_policy(void)
{
    uint8_t before;

    reset_all();
    before = i2c_slave_reg_byte(REG_TEMP);
    start_write();
    write_register(REG_TEMP, 0x55u);
    stop_frame();
    TF_CHECK_EQ(before, i2c_slave_reg_byte(REG_TEMP));

    reset_all();
    start_write();
    write_register(REG_SELECTED, 0xFFu);
    TF_CHECK_EQ(0u, g_regs[REG_SELECTED]);
    i2c_slave_dispatch_pending();
    TF_CHECK_EQ(0u, g_regs[REG_SELECTED]);
    stop_frame();
    TF_CHECK_EQ(0u, g_regs[REG_SELECTED]);
    i2c_slave_dispatch_pending();
    TF_CHECK_EQ(15u, g_regs[REG_SELECTED]);
}

static void test_speed_defer_and_gates(void)
{
    reset_all();
    i2c_hw_mock_set_cycles(1000u);
    start_write();
    write_register(REG_SPEED, 1u);
    stop_frame();
    TF_CHECK_EQ(0u, i2c_hw_mock_count(MOCK_EVT_APPLY_SPEED));

    i2c_hw_mock_set_cycles(7001u);
    i2c_hw_mock_set_status(0u, STS2_BUSYF, 0x00000003u);
    i2c_slave_poll();
    TF_CHECK_EQ(0u, i2c_hw_mock_count(MOCK_EVT_APPLY_SPEED));

    i2c_hw_mock_set_status(0u, 0u, 0x00000003u);
    i2c_slave_poll();
    TF_CHECK_EQ(1u, i2c_hw_mock_count(MOCK_EVT_APPLY_SPEED));
    TF_CHECK_EQ(400000u, i2c_mock_speed_hz);
    TF_CHECK_EQ(1u, g_i2c_speed);
}

static void test_abort_cancels_speed(void)
{
    reset_all();
    i2c_hw_mock_set_cycles(1000u);
    start_write();
    write_register(REG_SPEED, 1u);
    i2c_hw_mock_set_status(STS1_BUSERR, 0u, 0x00000003u);
    i2c_slave_poll();
    i2c_hw_mock_set_cycles(10000u);
    i2c_hw_mock_set_status(0u, 0u, 0x00000003u);
    i2c_slave_poll();
    TF_CHECK_EQ(0u, i2c_hw_mock_count(MOCK_EVT_APPLY_SPEED));
    TF_CHECK(!i2c_slave_busy());
}

static void test_clear_command(void)
{
    reset_all();
    start_read();
    i2c_hw_mock_set_status(STS1_BUSERR, 0u, 0x00000003u);
    i2c_slave_poll();
    TF_CHECK_EQ(1u, i2c_slave_reg_byte(REG_HEALTH));

    start_write();
    write_register(REG_CMD, 3u);
    stop_frame();
    i2c_slave_dispatch_pending();
    TF_CHECK_EQ(0u, i2c_slave_reg_byte(REG_FRAMES));
    TF_CHECK_EQ(0u, i2c_slave_reg_byte(REG_HEALTH));
    TF_CHECK_EQ(0u, g_i2c_err_bus);
}

static void test_fault_commit(void)
{
    reset_all();
    start_write();
    write_register(REG_FAULT, FAULT_BIT_FLIP);
    stop_frame();
    start_write();
    i2c_hw_mock_set_dt(0u);
    i2c_hw_mock_set_status(STS1_RDBF, 0u, 0x00000003u);
    i2c_slave_poll();
    stop_frame();
    i2c_hw_mock_reset();
    start_read();
    TF_CHECK_EQ(0x5Au, i2c_hw_mock_event(i2c_mock_event_count - 1u).a);

    reset_all();
    i2c_hw_mock_set_cycles(0u);
    start_write();
    write_register(REG_FAULT, FAULT_UNLOCK_KEY);
    stop_frame();
    start_write();
    write_register(REG_FAULT, FAULT_BIT_FLIP);
    stop_frame();
    start_write();
    i2c_hw_mock_set_dt(0u);
    i2c_hw_mock_set_status(STS1_RDBF, 0u, 0x00000003u);
    i2c_slave_poll();
    stop_frame();
    i2c_hw_mock_reset();
    start_read();
    TF_CHECK_EQ(0x5Bu, i2c_hw_mock_event(i2c_mock_event_count - 1u).a);

    reset_all();
    i2c_hw_mock_set_cycles(0u);
    start_write();
    write_register(REG_FAULT, FAULT_UNLOCK_KEY);
    stop_frame();
    i2c_hw_mock_set_cycles(FAULT_UNLOCK_WINDOW_US * 120u + 1u);
    start_write();
    write_register(REG_FAULT, FAULT_BIT_FLIP);
    stop_frame();
    start_write();
    i2c_hw_mock_set_dt(0u);
    i2c_hw_mock_set_status(STS1_RDBF, 0u, 0x00000003u);
    i2c_slave_poll();
    stop_frame();
    i2c_hw_mock_reset();
    start_read();
    TF_CHECK_EQ(0x5Au, i2c_hw_mock_event(i2c_mock_event_count - 1u).a);
}

static void test_fault_abort(void)
{
    reset_all();
    i2c_hw_mock_set_cycles(0u);
    start_write();
    write_register(REG_FAULT, FAULT_UNLOCK_KEY);
    stop_frame();
    start_write();
    write_register(REG_FAULT, FAULT_ABORT);
    stop_frame();

    i2c_hw_mock_reset();
    start_read();
    TF_CHECK(!i2c_slave_busy());
    TF_CHECK_EQ(1u, i2c_hw_mock_count(MOCK_EVT_ABORT));
    TF_CHECK_EQ(0u, i2c_hw_mock_count(MOCK_EVT_DT_WRITE));
    TF_CHECK_EQ(1u, i2c_slave_reg_byte(REG_DIAG_ABORT));
    TF_CHECK_EQ(1u, i2c_slave_reg_byte(REG_DIAG_ABORT_POS));
}

static void test_recovery_policy(void)
{
    reset_all();
    start_read();
    i2c_hw_mock_set_status(0u, 0u, 0u);
    i2c_slave_recover(300u);
    TF_CHECK(i2c_slave_busy());
    TF_CHECK_EQ(0u, i2c_hw_mock_count(MOCK_EVT_RECOVER));
    TF_CHECK_EQ(0u, i2c_slave_reg_byte(REG_HEALTH));

    reset_all();
    i2c_hw_mock_set_status(0u, 0u, 0x00000001u);
    i2c_slave_health_poll(110u);
    i2c_slave_health_poll(120u);
    TF_CHECK_EQ(1u, i2c_hw_mock_count(MOCK_EVT_RECOVER));
    TF_CHECK_EQ(HEALTH_SDA_STUCK | HEALTH_RECOVERED,
                i2c_slave_reg_byte(REG_HEALTH));

    i2c_hw_mock_set_recover_result(false);
    i2c_hw_mock_set_status(0u, 0u, 0x00000001u);
    i2c_slave_health_poll(220u);
    i2c_slave_health_poll(230u);
    TF_CHECK_EQ(2u, i2c_hw_mock_count(MOCK_EVT_RECOVER));
    TF_CHECK((i2c_slave_reg_byte(REG_HEALTH) & HEALTH_RECOVERY_FAILED) != 0u);
}

static void test_reset_cause(void)
{
    reset_all();
    i2c_hw_mock_set_reset_cause(HEALTH_WDT_RESET | HEALTH_SW_RESET);
    i2c_slave_init(i2c_hw_reset_cause());
    TF_CHECK_EQ(HEALTH_WDT_RESET | HEALTH_SW_RESET,
                i2c_slave_reg_byte(REG_HEALTH));
    TF_CHECK_EQ(0u, i2c_hw_reset_cause());
}

static void test_frame_timeout_and_stats(void)
{
    reset_all();
    i2c_slave_health_poll(0u);
    start_read();
    TF_CHECK(!i2c_slave_recovery_due(1000u));
    TF_CHECK(!i2c_slave_recovery_due(1001u));
    TF_CHECK(!i2c_slave_recovery_due(1010u));
    TF_CHECK(i2c_slave_recovery_due(1011u));

    i2c_hw_mock_set_status(0u, 0u, 0u);
    TF_CHECK(!i2c_slave_recovery_due(1011u));
    i2c_hw_mock_set_status(0u, 0u, 0x00000001u);
    TF_CHECK(!i2c_slave_recovery_due(1011u));
    i2c_hw_mock_set_status(STS1_TDBE, 0u, 0x00000002u);
    TF_CHECK(!i2c_slave_recovery_due(1011u));
    TF_CHECK(i2c_slave_recovery_due(1021u));
    i2c_hw_mock_set_status(STS1_STARTF, 0u, 0x00000003u);
    TF_CHECK(!i2c_slave_recovery_due(1021u));

    i2c_slave_note_manager_poll(1200u, 4u);
    i2c_slave_note_manager_poll(600u, 2u);
    TF_CHECK_EQ(10u, i2c_slave_reg_byte(REG_MAX_STEP));
    TF_CHECK_EQ(0u, i2c_slave_reg_byte(REG_MAX_STEP_H));
    TF_CHECK_EQ(4u, i2c_slave_reg_byte(REG_MAX_STATE));
}

void test_i2c_slave_run(void)
{
    test_stop_address_order();
    test_repeated_start();
    test_stretch_model();
    test_nack_lifetime();
    test_early_and_final_nack_positions();
    test_stop_gate();
    test_buserr_precedes_tdbe();
    test_arlost_precedes_rdbf();
    test_ouf_position_and_priority();
    test_register_write_policy();
    test_speed_defer_and_gates();
    test_abort_cancels_speed();
    test_clear_command();
    test_fault_commit();
    test_fault_abort();
    test_recovery_policy();
    test_reset_cause();
    test_frame_timeout_and_stats();
}
