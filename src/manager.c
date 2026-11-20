#include <stdint.h>
#include <string.h>

#include "manager.h"
#include "onewire.h"
#include "ds18b20.h"
#include "ds2431.h"
#include "ow_hal.h"

static ow_search_t search;
static ds18b20_t   sensor;
static ds2431_t    eeprom;

/* device_count counts every device the search finds; roms[] stores at most
 * DEV_MAX of them.  REG_DEVICES still reports the total. */
static uint8_t  roms[DEV_MAX][8];
static uint32_t device_count;
static uint32_t ds18b20_count;

static int16_t  temp_raw[DEV_MAX];
static uint8_t  temp_status[DEV_MAX];
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
static uint8_t  pub_seq;

/* Latched by the I2C bridge at STOP; consumed by manager_poll(). */
static uint32_t g_cmd;

/* Shared register file, visible to app/main.c through the extern in
 * manager.h. */
uint8_t  g_regs[256];
volatile uint32_t g_i2c_speed = 0;
static uint32_t roms_dirty = 1;

/* Number of ROMs actually stored in roms[] (the search counts more). */
static uint32_t rom_count(void)
{
    return (device_count < DEV_MAX) ? device_count : DEV_MAX;
}

/* Called by the I2C bridge when a command is latched at STOP. */
void manager_command(uint8_t cmd)
{
    g_cmd = cmd;
}

/* Test helper: force the manager state (tests only). */
void manager_set_state_for_test(uint32_t state)
{
    manager_state = state;
}

static void clear_results(void);

/* Weak stub for the host build; app/main.c overrides it with the real
 * I2C2 register read. */
bool __attribute__((weak)) manager_i2c_busy(void)
{
    return false;
}

void manager_init(void)
{
    memset((void *)g_regs, 0, sizeof(g_regs));
    g_regs[REG_MAGIC] = 0x5Au;
    g_regs[REG_VERSION] = 0x01u;
    ds18b20_init(&sensor, true);
    ds2431_init(&eeprom);
    clear_results();

    manager_state = MGR_SEARCH;
    search_started = 0;
    roms_dirty = 1;
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

    g_regs[REG_SEQ] = ++pub_seq;        /* odd: update in progress */
    cycle++;
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

    /* A selection outside the DS18B20 table must not expose a stale
     * scratchpad: publish the invalid marker instead. */
    if ((g_regs[REG_SELECTED] & 0x0Fu) >= ds18b20_count) {
        memset(sp_sel, 0xFF, sizeof(sp_sel));
    }

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
    g_regs[REG_SEQ] = ++pub_seq;        /* even: snapshot stable */
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

void manager_poll(void)
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

    case MGR_EEPROM: {
        uint32_t i;
        uint32_t found = 0;

        if (eeprom_started == 0u) {
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
    }

    case MGR_PUBLISH:
        /* I2C bridge must not be in the middle of a transaction. */
        if (manager_i2c_busy()) {
            return;
        }
        publish();
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
