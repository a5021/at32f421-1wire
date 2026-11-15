#include <stdint.h>
#include <string.h>

#include "at32f421.h"
#include "clock.h"
#include "ow_hal.h"
#include "onewire.h"
#include "ds18b20.h"
#include "ds2431.h"

#define I2C_ADDR7               0x42u
#define I2C_OADDR1              ((uint16_t)(I2C_ADDR7 << 1))

#define REG_MAGIC               0x00u
#define REG_VERSION             0x01u
#define REG_FLAGS               0x02u
#define REG_DEVICES             0x03u
#define REG_DS18B20             0x04u
#define REG_ERROR               0x05u
#define REG_CYCLE_L             0x06u
#define REG_CYCLE_H             0x07u
#define REG_SPEED               0x0Fu
#define REG_TEMP                0x10u
#define REG_TEMP_STATUS         0x30u
#define REG_EEPROM              0x40u
#define REG_EEPROM_STATUS       0x48u
#define REG_CMD                 0x60u
#define REG_SELECTED            0x61u
#define REG_SCRATCH             0x70u
#define REG_ROMS                0x80u

#define FLAG_SEARCH_DONE        0x01u
#define FLAG_SEARCH_ERROR       0x02u
#define FLAG_BUSY               0x04u
#define FLAG_SNAPSHOT           0x08u
#define FLAG_EEPROM_OK          0x10u
#define FLAG_TEMP_ERROR         0x20u

#define STS1_ADDR7F             0x00000002u
#define STS1_STOPF              0x00000010u
#define STS1_RDBF               0x00000040u
#define STS1_TDBE               0x00000080u
#define STS1_BUSERR             0x00000100u
#define STS1_ARLOST             0x00000200u
#define STS1_ACKFAIL            0x00000400u
#define STS1_OUF                0x00000800u
#define STS2_DIRF               0x00000004u

#define DEV_MAX                 16u
_Static_assert(REG_ROMS + 8u * DEV_MAX <= 256u, "ROM map overflows the register file");

enum {
    MGR_SEARCH = 0,
    MGR_CONVERT,
    MGR_READ,
    MGR_EEPROM,
    MGR_PUBLISH,
    MGR_WAIT
};

static ow_search_t search;
static ds18b20_t   sensor;
static ds2431_t    eeprom;

static uint8_t  roms[DEV_MAX][8];
static uint32_t device_count;
static uint32_t ds18b20_count;

static int16_t  temp_raw[DEV_MAX];
static uint8_t  temp_status[DEV_MAX];

/* Number of ROMs actually stored in roms[] (the search counts more). */
static uint32_t rom_count(void)
{
    return (device_count < DEV_MAX) ? device_count : DEV_MAX;
}
static uint8_t  sp_sel[9];
static uint8_t  eeprom_data[8];
static uint8_t  eeprom_status;

static uint32_t manager_state;
static uint32_t search_started;
static uint32_t convert_started;
static uint32_t read_started;
static uint32_t read_dev;
static uint32_t read_sensor;
static uint32_t eeprom_started;
static uint32_t search_done;
static uint32_t search_error;
static uint32_t last_error;
static uint32_t ok_count;
static uint32_t err_count;
static uint32_t cycle;

volatile uint8_t  g_cmd;
volatile uint32_t g_i2c_speed;

static uint8_t  g_regs[256];
static uint32_t roms_dirty = 1;

static uint32_t reg_ptr;
static uint32_t rx_mode;
static uint32_t tx_mode;
static uint32_t first_byte;
static int      pending_speed = -1;
static uint8_t  pending_cmd;

/* Program the slave clocking from the APB1 frequency that is actually running.
 * speed is the SCL prescaler: APB1 / (2 * SCL) in standard mode and
 * APB1 / (3 * SCL) for fast mode with a 1:2 high:low ratio, which is what the
 * reference manual and the vendor BSP use. */
static void i2c_apply_speed(uint32_t scl_hz)
{
    uint32_t apb1 = clock_apb1_hz();
    uint32_t mhz = apb1 / 1000000u;
    uint32_t speed;
    uint32_t rise;

    I2C2->ctrl1_bit.i2cen = 0;
    I2C2->ctrl2_bit.clkfreq = mhz;

    if (scl_hz <= 100000u) {
        speed = apb1 / (2u * scl_hz);
        if (speed < 4u) {
            speed = 4u;
        }
        I2C2->clkctrl_bit.speedmode = 0;
        rise = mhz + 1u;
    } else {
        speed = apb1 / (3u * scl_hz);
        if (speed == 0u) {
            speed = 1u;
        }
        I2C2->clkctrl_bit.speedmode = 1;
        rise = (mhz * 300u) / 1000u + 1u;
    }

    if (speed > 0xFFFu) {
        speed = 0xFFFu;
    }
    if (rise > 0x3Fu) {
        rise = 0x3Fu;
    }

    I2C2->clkctrl_bit.dutymode = 0;             /* 1:2 high:low in fast mode */
    I2C2->clkctrl_bit.speed = speed;
    I2C2->tmrise_bit.risetime = rise;

    I2C2->ctrl1_bit.i2cen = 1;
    I2C2->ctrl1_bit.acken = 1;
    I2C2->ctrl1_bit.stretch = 1;

    g_i2c_speed = (scl_hz > 100000u) ? 1u : 0u;
}

static void i2c_hw_init(void)
{
    CRM->ahben_bit.gpioaen = 1;
    CRM->apb1en_bit.i2c2en = 1;

    GPIOA->cfgr = (GPIOA->cfgr & ~0x0000000Fu) | 0x0000000Au;
    GPIOA->omode |= 0x00000003u;
    GPIOA->pull &= ~0x0000000Fu;
    GPIOA->muxl = (GPIOA->muxl & ~0x000000FFu) | 0x00000044u;

    I2C2->ctrl1 = 0;
    I2C2->ctrl2 = 0;
    I2C2->oaddr1 = I2C_OADDR1;
    I2C2->oaddr1_bit.addr1mode = 0;

    i2c_apply_speed(100000u);
}

static void i2c_poll(void)
{
    uint32_t sts1 = I2C2->sts1;
    uint32_t relevant = sts1 & ~STS1_TDBE;

    if ((relevant == 0u) && (tx_mode == 0u)) {
        return;
    }

    if ((sts1 & STS1_ADDR7F) != 0u) {
        uint32_t sts2 = I2C2->sts2;

        (void)I2C2->sts1;
        (void)I2C2->sts2;

        if ((sts2 & STS2_DIRF) != 0u) {
            tx_mode = 1;
            rx_mode = 0;
            I2C2->dt = g_regs[reg_ptr & 0xFFu];
            reg_ptr = (reg_ptr + 1u) & 0xFFu;
        } else {
            rx_mode = 1;
            tx_mode = 0;
            first_byte = 1;
        }
        return;
    }

    if ((rx_mode != 0u) && ((sts1 & STS1_RDBF) != 0u)) {
        uint8_t b = (uint8_t)I2C2->dt;

        if (first_byte != 0u) {
            reg_ptr = b;
            first_byte = 0;
        } else {
            g_regs[reg_ptr] = b;
            if (reg_ptr == REG_SPEED && b <= 1u) {
                pending_speed = (int)b;
            } else if (reg_ptr == REG_CMD) {
                pending_cmd = b;
            }
            reg_ptr = (reg_ptr + 1u) & 0xFFu;
        }
        return;
    }

    if ((tx_mode != 0u) && ((sts1 & STS1_TDBE) != 0u)) {
        I2C2->dt = g_regs[reg_ptr & 0xFFu];
        reg_ptr = (reg_ptr + 1u) & 0xFFu;
        return;
    }

    if ((sts1 & STS1_STOPF) != 0u) {
        (void)I2C2->sts1;
        I2C2->ctrl1_bit.i2cen = 1;
        rx_mode = 0;
        tx_mode = 0;
        first_byte = 0;

        if (pending_cmd != 0u) {
            g_cmd = pending_cmd;
            pending_cmd = 0;
        }
        if (pending_speed >= 0) {
            i2c_apply_speed(pending_speed != 0 ? 400000u : 100000u);
            pending_speed = -1;
        }
        return;
    }

    if ((sts1 & STS1_ACKFAIL) != 0u) {
        I2C2->sts1 = (uint32_t)~(STS1_ACKFAIL & 0x0000DF00u);
        tx_mode = 0;
    }
    if ((sts1 & STS1_BUSERR) != 0u) {
        I2C2->sts1 = (uint32_t)~(STS1_BUSERR & 0x0000DF00u);
    }
    if ((sts1 & STS1_ARLOST) != 0u) {
        I2C2->sts1 = (uint32_t)~(STS1_ARLOST & 0x0000DF00u);
    }
    if ((sts1 & STS1_OUF) != 0u) {
        I2C2->sts1 = (uint32_t)~(STS1_OUF & 0x0000DF00u);
    }
    /* An aborted transfer must not leave a command or a speed change armed
     * for the next STOP, which belongs to somebody else's transaction. */
    if ((sts1 & (STS1_ACKFAIL | STS1_BUSERR | STS1_ARLOST | STS1_OUF)) != 0u) {
        pending_cmd = 0;
        pending_speed = -1;
        rx_mode = 0;
        tx_mode = 0;
        first_byte = 0;
    }
}

static void clear_results(void)
{
    for (uint32_t i = 0u; i < DEV_MAX; i++) {
        temp_raw[i] = (int16_t)0x8000;
        temp_status[i] = 0xFFu;
    }
    memset(sp_sel, 0, sizeof(sp_sel));
    memset(eeprom_data, 0, sizeof(eeprom_data));
    eeprom_status = 0xFFu;
    last_error = 0u;
    ok_count = 0u;
    err_count = 0u;
}

static void publish(void)
{
    uint8_t flags = FLAG_SNAPSHOT | FLAG_SEARCH_DONE;

    if (search_error != 0u) {
        flags |= FLAG_SEARCH_ERROR;
    }
    if (eeprom_status == 0u) {
        flags |= FLAG_EEPROM_OK;
    }
    if (err_count != 0u) {
        flags |= FLAG_TEMP_ERROR;
    }

    g_regs[REG_MAGIC] = 0x5Au;
    g_regs[REG_VERSION] = 0x01u;
    g_regs[REG_FLAGS] = flags;
    g_regs[REG_DEVICES] = (uint8_t)((device_count > 255u) ? 255u : device_count);
    g_regs[REG_DS18B20] = (uint8_t)((ds18b20_count > 255u) ? 255u : ds18b20_count);
    g_regs[REG_ERROR] = (uint8_t)last_error;
    g_regs[REG_CYCLE_L] = (uint8_t)(cycle & 0xFFu);
    g_regs[REG_CYCLE_H] = (uint8_t)(cycle >> 8);
    g_regs[REG_SPEED] = (uint8_t)g_i2c_speed;

    for (uint32_t i = 0u; (i < ds18b20_count) && (i < DEV_MAX); i++) {
        g_regs[REG_TEMP + 2u * i] = (uint8_t)((uint16_t)temp_raw[i] & 0xFFu);
        g_regs[REG_TEMP + 2u * i + 1u] = (uint8_t)((uint16_t)temp_raw[i] >> 8);
        g_regs[REG_TEMP_STATUS + i] = temp_status[i];
    }
    /* Slots this cycle did not fill must not keep the previous cycle's
     * readings: a rescan can leave fewer sensors than before. */
    for (uint32_t i = ds18b20_count; i < DEV_MAX; i++) {
        g_regs[REG_TEMP + 2u * i] = 0x00u;
        g_regs[REG_TEMP + 2u * i + 1u] = 0x80u;      /* 0x8000 = invalid */
        g_regs[REG_TEMP_STATUS + i] = 0xFFu;
    }

    memcpy((void *)&g_regs[REG_EEPROM], eeprom_data, 8u);
    g_regs[REG_EEPROM_STATUS] = eeprom_status;

    memcpy((void *)&g_regs[REG_SCRATCH], sp_sel, 9u);

    if (roms_dirty != 0u) {
        for (uint32_t i = 0u; (i < device_count) && (i < DEV_MAX); i++) {
            memcpy((void *)&g_regs[REG_ROMS + 8u * i], roms[i], 8u);
        }
        for (uint32_t i = device_count; i < DEV_MAX; i++) {
            memset((void *)&g_regs[REG_ROMS + 8u * i], 0xFF, 8u);
        }
        roms_dirty = 0u;
    }
}

static void manager_search(void)
{
    ow_status_t st;

    if (search_started == 0u) {
        ow_search_begin(&search);
        device_count = 0;
        ds18b20_count = 0;
        search_error = 0;
        search_done = 0;
        search_started = 1;
        g_regs[REG_FLAGS] |= FLAG_BUSY;
        return;
    }

    st = ow_search_poll(&search);
    if (st == OW_BUSY) {
        return;
    }
    if (st == OW_OK) {
        if (device_count < DEV_MAX) {
            memcpy(roms[device_count], search.rom, 8u);
            if (search.rom[0] == 0x28u) {
                ds18b20_count++;
            }
        }
        device_count++;
        return;
    }

    search_started = 0;
    if (st == OW_END) {
        search_done = 1;
        roms_dirty = 1;
    } else {
        search_error = (uint32_t)st;
    }

    clear_results();
    manager_state = (ds18b20_count != 0u && search_error == 0u) ? MGR_CONVERT
                                                               : MGR_EEPROM;
}

static void manager_poll(void)
{
    ow_status_t st;

    if ((g_cmd != 0u) && (manager_state == MGR_WAIT) && ow_delay_poll()) {
        uint8_t cmd = g_cmd;
        g_cmd = 0;

        if (cmd == 1u) {
            manager_state = MGR_SEARCH;
            search_started = 0;
            search_done = 0;
            g_regs[REG_FLAGS] |= FLAG_BUSY;
            return;
        }
        if (cmd == 2u) {
            eeprom_started = 0;
            manager_state = MGR_EEPROM;
            g_regs[REG_FLAGS] |= FLAG_BUSY;
            return;
        }
        if (cmd == 3u) {
            cycle = 0;
            clear_results();
        }
        return;
    }

    switch (manager_state) {
    case MGR_SEARCH:
        manager_search();
        break;

    case MGR_CONVERT:
        if (convert_started == 0u) {
            if (ow_hal_busy()) {
                return;
            }
            clear_results();
            ds18b20_convert_start(&sensor, 0);
            convert_started = 1;
            return;
        }
        st = ds18b20_convert_poll(&sensor);
        if (st == OW_BUSY) {
            return;
        }
        convert_started = 0;
        if (st != OW_OK) {
            last_error = (uint32_t)st;
            err_count++;
            manager_state = MGR_EEPROM;
            return;
        }
        read_dev = 0;
        read_sensor = 0;
        read_started = 0;
        manager_state = MGR_READ;
        break;

    case MGR_READ:
        /* device_count counts every device the search found, while roms[]
         * stores at most DEV_MAX of them: walk only the stored range. */
        while ((read_dev < rom_count()) && (roms[read_dev][0] != 0x28u)) {
            read_dev++;
        }
        if ((read_dev >= rom_count()) || (read_sensor >= DEV_MAX)) {
            manager_state = MGR_EEPROM;
            return;
        }
        if (read_started == 0u) {
            if (ow_hal_busy()) {
                return;
            }
            ds18b20_read_start(&sensor, roms[read_dev]);
            read_started = 1;
            return;
        }
        {
            st = ds18b20_read_poll(&sensor);
            if (st == OW_BUSY) {
                return;
            }
            temp_status[read_sensor] = (uint8_t)st;
            if (st == OW_OK) {
                temp_raw[read_sensor] = sensor.raw;
                ok_count++;
            } else {
                temp_raw[read_sensor] = (int16_t)0x8000;
                err_count++;
                last_error = (uint32_t)st;
            }
            if (read_sensor == (uint32_t)(g_regs[REG_SELECTED] & 0x0Fu)) {
                memcpy(sp_sel, sensor.sp, 9u);
            }
            read_dev++;
            read_sensor++;
            read_started = 0;
        }
        break;

    case MGR_EEPROM:
        if (eeprom_started == 0u) {
            uint32_t i;
            uint32_t found = 0;

            for (i = 0u; (i < device_count) && (i < DEV_MAX); i++) {
                if (roms[i][0] == 0x2Du) {
                    found = 1;
                    break;
                }
            }
            if ((found == 0u) || ow_hal_busy()) {
                if (found == 0u) {
                    eeprom_status = 0xFFu;
                    manager_state = MGR_PUBLISH;
                }
                return;
            }
            if (!ds2431_read_start(&eeprom, roms[i], 0u, eeprom_data, 8u)) {
                eeprom_status = 0xFFu;
                manager_state = MGR_PUBLISH;
                return;
            }
            eeprom_started = 1;
            return;
        }
        st = ds2431_read_poll(&eeprom);
        if (st == OW_BUSY) {
            return;
        }
        eeprom_status = (uint8_t)st;
        eeprom_started = 0;
        manager_state = MGR_PUBLISH;
        break;

    case MGR_PUBLISH:
        if ((I2C2->sts2 & 0x00000002u) != 0u) {
            return;
        }
        publish();
        cycle++;
        manager_state = MGR_WAIT;
        ow_delay_start(200000u);
        break;

    case MGR_WAIT:
        if (!ow_delay_poll()) {
            return;
        }
        manager_state = (ds18b20_count != 0u) ? MGR_CONVERT : MGR_EEPROM;
        g_regs[REG_FLAGS] |= FLAG_BUSY;
        break;

    default:
        manager_state = MGR_SEARCH;
        break;
    }
}

int main(void)
{
    memset((void *)g_regs, 0, sizeof(g_regs));
    g_regs[REG_MAGIC] = 0x5Au;
    g_regs[REG_VERSION] = 0x01u;

    clock_config();
    ow_hal_init();
    i2c_hw_init();

    ds18b20_init(&sensor, true);
    ds2431_init(&eeprom);
    clear_results();

    manager_state = MGR_SEARCH;
    search_started = 0;

    for (;;) {
        i2c_poll();
        manager_poll();
    }
}

void HardFault_Handler(void)
{
    for (;;) {
    }
}

void MemManage_Handler(void) __attribute__((alias("HardFault_Handler")));
void BusFault_Handler(void) __attribute__((alias("HardFault_Handler")));
void UsageFault_Handler(void) __attribute__((alias("HardFault_Handler")));
