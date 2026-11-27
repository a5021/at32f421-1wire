#include <stdint.h>
#include <string.h>

#ifdef __arm__
#include "at32f421.h"
#define NOW_CYCLES() (DWT->CYCCNT)
#else
#define NOW_CYCLES() (0u)
#endif

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

/* Search results are staged here and copied into roms[] only when the pass
 * completes, so a failed rescan cannot leave a partial list published. */
static uint8_t  roms_next[DEV_MAX][8];
static uint32_t search_next_count;
static uint32_t search_next_ds;

static int16_t  temp_raw[DEV_MAX];
static uint8_t  temp_status[DEV_MAX];
static uint8_t  sp_sel[9];
static uint8_t  eeprom_data[8];
static uint8_t  eeprom_status;

static uint32_t manager_state;
static uint8_t  pause_requested;
static uint8_t  paused;
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
static uint32_t wait_cycles;        /* MGR_WAIT passes since the last search */
static uint8_t  rescan_auto;        /* this search was periodic, keep readings */

/* A bridge that only searches at boot never notices a sensor that is plugged
 * in later.  Search again on a slow cadence, and keep searching while no
 * DS18B20 is known so a hot-plug is picked up within a cycle or two. */
#define RESCAN_CYCLES           60u

/* Latched by the I2C bridge at STOP; consumed by manager_poll(). */
static uint32_t g_cmd;

/* Shared register file, visible to app/main.c through the extern in
 * manager.h. */
uint8_t  g_regs[256] __attribute__((aligned(4)));
volatile uint32_t g_i2c_speed = 0;
static uint32_t roms_dirty = 1;

/* Three buffers.  map_live is what the master reads and is never written; the
 * build runs in map_build and the swap is one pointer store.  The buffer a
 * read may still be holding is parked in map_spare and reused only two
 * publishes later.  The 4 x 64-byte commit this replaces was visible to the
 * master as a run of 0xFF from the chunk boundary onwards whenever a read
 * overlapped it. */
static uint8_t  map_a[256] __attribute__((aligned(4)));
static uint8_t  map_b[256] __attribute__((aligned(4)));
static uint8_t  map_c[256] __attribute__((aligned(4)));
static uint8_t *map_live = map_a;
static uint8_t *map_build = map_b;
static uint8_t *map_spare = map_c;

static uint16_t crc_table[256];
static void crc_table_init(void);
static void crc_skip_table_init(void);

/* Number of ROMs actually stored in roms[] (the search counts more). */
static uint32_t rom_count(void)
{
    return (device_count < DEV_MAX) ? device_count : DEV_MAX;
}

/* Called by the I2C bridge when a command is latched at STOP. */
void manager_command(uint8_t cmd)
{
    /* 4 pauses the cycle, 5 resumes it.  Pause only takes effect in MGR_WAIT
     * so a conversion or an EEPROM read is never cut in half. */
    if (cmd == 4u) {
        pause_requested = 1u;
        return;
    }
    if (cmd == 5u) {
        pause_requested = 0u;
        paused = 0u;
        return;
    }
    g_cmd = cmd;
}

uint8_t manager_state_reg(void)
{
    return (uint8_t)(manager_state | (paused ? 0x80u : 0u));
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
    crc_table_init();
    crc_skip_table_init();
    g_regs[REG_MAGIC] = 0x5Au;
    g_regs[REG_VERSION] = BRIDGE_MAP_VERSION;
    memset(map_a, 0, sizeof(map_a));
    map_a[REG_MAGIC] = 0x5Au;
    map_a[REG_VERSION] = BRIDGE_MAP_VERSION;
    ds18b20_init(&sensor, true);
    ds2431_init(&eeprom);
    clear_results();

    manager_state = MGR_SEARCH;
    search_started = 0;
    roms_dirty = 1;
    g_cmd = 0u;                 /* a latched command belongs to no session */
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

/* Table-driven CRC-16/CCITT-FALSE.  The bitwise version blocked the CPU for
 * ~150 us inside publish(), and the I2C slave stretches while the CPU is busy:
 * the master then corrupts the byte it was transferring.  The table is built
 * once at init and the loop drops to ~25 us. */
static void crc_table_init(void)
{
    for (uint32_t i = 0u; i < 256u; i++) {
        uint16_t c = (uint16_t)(i << 8);

        for (int b = 0; b < 8; b++) {
            c = ((c & 0x8000u) != 0u)
                ? (uint16_t)((uint16_t)(c << 1) ^ 0x1021u)
                : (uint16_t)(c << 1);
        }
        crc_table[i] = c;
    }
}

/* Build state for the snapshot in progress. */
static uint8_t  pub_phase;
static uint32_t pub_pos;
static uint16_t pub_crc;
static uint16_t cycle_next;
uint32_t pub_phase_max[3];
uint32_t pub_phase_sum[3];
uint32_t pub_phase_cnt[3];
static uint32_t eep_start_max;
static uint32_t eep_poll_max;
static uint8_t diag_crc_ffff;
static uint8_t  crc_skip_tab[256];

static bool crc_skip(uint32_t i)
{
    return (i == REG_FLAGS) || (i == REG_SEQ) ||
           ((i >= REG_FRAMES) && (i <= REG_SPEED)) ||
           ((i >= REG_CMD) && (i <= REG_DIAG_REASON)) ||
           ((i >= REG_DIAG_GATE) && (i <= REG_DIAG_GATE_POS)) ||
           ((i >= REG_DIAG_START) && (i <= REG_DIAG_BUS)) ||
           ((i >= REG_MAX_STEP) && (i <= REG_EEP_POLL));
}

/* One 64-byte quarter of the 256-byte map, copied in words.  The whole map is
 * four chunks: a full copy measured 14 us, which sits on the slave's ~20 us
 * service budget at 400 kHz. */
static void map_copy_chunk(uint8_t *dst, const uint8_t *src, uint32_t pos)
{
    uint32_t *d = (uint32_t *)(void *)(dst + pos);
    const uint32_t *s = (const uint32_t *)(const void *)(src + pos);

    for (uint32_t i = 0u; i < 16u; i++) {
        d[i] = s[i];
    }
}

static void crc_skip_table_init(void)
{
    for (uint32_t i = 0u; i < 256u; i++) {
        crc_skip_tab[i] = crc_skip(i) ? 1u : 0u;
    }
}

/* Build the next snapshot into the map_build buffer. */
static void publish_fill(uint8_t *dst)
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

    cycle_next = (uint16_t)(cycle + 1u);
    dst[REG_MAGIC] = 0x5Au;
    dst[REG_VERSION] = BRIDGE_MAP_VERSION;
    dst[REG_FLAGS] = flags;
    dst[REG_DEVICES] = (uint8_t)((device_count > 255u) ? 255u : device_count);
    dst[REG_DS18B20] = (uint8_t)((ds18b20_count > 255u) ? 255u : ds18b20_count);
    dst[REG_ERROR] = (uint8_t)last_error;
    dst[REG_CYCLE_L] = (uint8_t)(cycle_next & 0xFFu);
    dst[REG_CYCLE_H] = (uint8_t)(cycle_next >> 8);
    dst[REG_SPEED] = (uint8_t)g_i2c_speed;

    for (uint32_t i = 0u; (i < ds18b20_count) && (i < DEV_MAX); i++) {
        dst[REG_TEMP + 2u * i] = (uint8_t)((uint16_t)temp_raw[i] & 0xFFu);
        dst[REG_TEMP + 2u * i + 1u] = (uint8_t)((uint16_t)temp_raw[i] >> 8);
        dst[REG_TEMP_STATUS + i] = temp_status[i];
    }
    /* Slots this cycle did not fill must not keep the previous cycle's
     * readings: a rescan can leave fewer sensors than before. */
    for (uint32_t i = ds18b20_count; i < DEV_MAX; i++) {
        dst[REG_TEMP + 2u * i] = 0x00u;
        dst[REG_TEMP + 2u * i + 1u] = 0x80u;      /* 0x8000 = invalid */
        dst[REG_TEMP_STATUS + i] = 0xFFu;
    }

    memcpy((void *)&dst[REG_EEPROM], eeprom_data, 8u);
    dst[REG_EEPROM_STATUS] = eeprom_status;

    /* The host owns REG_SELECTED, so the snapshot carries the value that is
     * current at build time: the scratchpad below must match what the master
     * reads back from REG_SELECTED itself. */
    dst[REG_SELECTED] = g_regs[REG_SELECTED];

    /* A selection outside the DS18B20 table must not expose a stale
     * scratchpad: publish the invalid marker instead. */
    if ((dst[REG_SELECTED] & 0x0Fu) >= ds18b20_count) {
        memset(sp_sel, 0xFF, sizeof(sp_sel));
    }

    memcpy((void *)&dst[REG_SCRATCH], sp_sel, 9u);

    if (roms_dirty != 0u) {
        for (uint32_t i = 0u; (i < device_count) && (i < DEV_MAX); i++) {
            memcpy((void *)&dst[REG_ROMS + 8u * i], roms[i], 8u);
        }
        for (uint32_t i = device_count; i < DEV_MAX; i++) {
            memset((void *)&dst[REG_ROMS + 8u * i], 0xFF, 8u);
        }
        roms_dirty = 0u;
    }
}

/* One short step of the publish.  The CRC is spread over several calls: the
 * I2C slave can only keep the CPU busy for about eight bit times before the
 * master corrupts the byte it is transferring, and the whole 256-byte CRC is
 * far longer than that. */
static void publish_step(void){
    uint32_t t0 = NOW_CYCLES();
    uint8_t ph_in = pub_phase;

    switch (pub_phase) {
    case 0u:
        map_copy_chunk(map_build, map_live, pub_pos);
        pub_pos += 64u;
        if (pub_pos >= 256u) {
            publish_fill(map_build);
            pub_crc = 0xFFFFu;
            pub_pos = 0u;
            pub_phase = 1u;
        }
        break;

    case 1u: {
        uint32_t end = pub_pos + 32u;

        if (end > 256u) {
            end = 256u;
        }
        for (uint32_t i = pub_pos; i < end; i++) {
            if (crc_skip_tab[i] != 0u) {
                continue;
            }
            pub_crc = (uint16_t)((uint16_t)(pub_crc << 8) ^
                       crc_table[(uint8_t)((pub_crc >> 8) ^ map_build[i])]);
        }
        pub_pos = end;
        if (pub_pos >= 256u) {
            map_build[REG_MAP_CRC_L] = (uint8_t)(pub_crc & 0xFFu);
            map_build[REG_MAP_CRC_H] = (uint8_t)(pub_crc >> 8);
            pub_pos = 0u;
            pub_phase = 2u;
        }
        break;
    }

    default:
        if ((g_regs[REG_SELECTED] & 0x0Fu) != map_build[REG_SELECTED]) {
            /* A selection change restarts the build. */
            pub_phase = 0u;
            pub_pos = 0u;
            break;
        }
        if (pub_crc == 0xFFFFu) {
            if (diag_crc_ffff != 0xFFu) {
                diag_crc_ffff++;
            }
        }
        /* Even SEQ: the snapshot is stable.  A publish is now a pointer swap,
         * so there is no odd "update in progress" phase any more. */
        pub_seq = (uint8_t)(pub_seq + 2u);
        map_build[REG_SEQ] = pub_seq;
        {
            uint8_t *t = map_live;

            map_live = map_build;   /* single store: readers see one map */
            map_build = map_spare;
            map_spare = t;          /* buffer a running read may still hold */
        }
        cycle = cycle_next;
        pub_pos = 0u;
        pub_phase = 0u;
        manager_state = MGR_WAIT;
        ow_delay_start(200000u);
        break;
    }

    {
        uint32_t d = NOW_CYCLES() - t0;

        if (ph_in > 2u) {
            ph_in = 2u;
        }
        if (d > pub_phase_max[ph_in]) {
            pub_phase_max[ph_in] = d;
        }
        pub_phase_sum[ph_in] += d;
        pub_phase_cnt[ph_in]++;
    }
}

bool manager_publish_busy(void)
{
    return pub_phase != 0u;
}

uint8_t manager_crc_ffff_count(void)
{
    return diag_crc_ffff;
}

/* The map a read must serve.  Latched by the caller at the address match so a
 * swap in the middle of a transaction cannot mix two snapshots. */
const uint8_t *manager_map(void)
{
    return map_live;
}

/* The host owns the selection.  A change invalidates the scratchpad: what the
 * map publishes must belong to the index the master reads back, and the
 * previous sensor's scratchpad is a lie.  It becomes valid again when the
 * manager reads the newly selected sensor. */
void manager_select(uint8_t idx)
{
    g_regs[REG_SELECTED] = (uint8_t)(idx & 0x0Fu);
    memset(sp_sel, 0xFF, sizeof(sp_sel));
}

uint32_t manager_eep_start_max(void)
{
    return eep_start_max;
}

uint32_t manager_eep_poll_max(void)
{
    return eep_poll_max;
}

static void manager_search(void)
{
    ow_status_t st;

    if (search_started == 0u) {
        ow_search_begin(&search);
        search_next_count = 0;
        search_next_ds = 0;
        search_error = 0;
        search_done = 0;
        search_started = 1;
        map_live[REG_FLAGS] |= FLAG_BUSY;
        return;
    }

    st = ow_search_poll(&search);
    if (st == OW_BUSY) {
        return;
    }
    if (st == OW_OK) {
        if (search_next_count < DEV_MAX) {
            memcpy(roms_next[search_next_count], search.rom, 8u);
            if (search.rom[0] == 0x28u) {
                search_next_ds++;
            }
        }
        search_next_count++;
        return;
    }

    search_started = 0;
    if (st == OW_END) {
        /* Only a complete pass replaces the published list: a failed rescan
         * leaves the last known-good devices in place instead of publishing a
         * truncated count over the old ROM map. */
        memcpy(roms, roms_next, sizeof(roms));
        device_count = search_next_count;
        ds18b20_count = search_next_ds;
        search_done = 1;
        roms_dirty = 1;
    } else {
        search_error = (uint32_t)st;
    }

    if (rescan_auto == 0u) {
        clear_results();
    }
    rescan_auto = 0u;
    manager_state = (ds18b20_count != 0u && search_error == 0u) ? MGR_CONVERT
                                                               : MGR_EEPROM;
}

void manager_poll(void)
{
    ow_status_t st;

    if ((manager_state == MGR_WAIT) && (pause_requested != 0u)) {
        paused = 1u;
    }
    if (paused != 0u) {
        return;
    }

    if ((g_cmd != 0u) && (manager_state == MGR_WAIT) && ow_delay_poll()) {
        uint8_t cmd = g_cmd;
        g_cmd = 0;

        if (cmd == 1u) {
            manager_state = MGR_SEARCH;
            search_started = 0;
            search_done = 0;
            map_live[REG_FLAGS] |= FLAG_BUSY;
            return;
        }
        if (cmd == 2u) {
            eeprom_started = 0;
            manager_state = MGR_EEPROM;
            map_live[REG_FLAGS] |= FLAG_BUSY;
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
            if (!ds18b20_convert_start(&sensor, 0)) {
                last_error = (uint32_t)OW_PARAM;
                err_count++;
                manager_state = MGR_EEPROM;
                return;
            }
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
            if (!ds18b20_read_start(&sensor, roms[read_dev])) {
                /* Refused start must not be published as a fresh reading of
                 * whatever the previous sensor left in sensor.raw. */
                temp_status[read_sensor] = (uint8_t)OW_PARAM;
                temp_raw[read_sensor] = (int16_t)0x8000;
                err_count++;
                last_error = (uint32_t)OW_PARAM;
                if (read_sensor == (uint32_t)(g_regs[REG_SELECTED] & 0x0Fu)) {
                    memset(sp_sel, 0xFF, sizeof(sp_sel));
                }
                read_dev++;
                read_sensor++;
                return;
            }
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
            if ((st == OW_OK) &&
                (read_sensor == (uint32_t)(g_regs[REG_SELECTED] & 0x0Fu))) {
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
            uint32_t t0 = NOW_CYCLES();
            uint32_t d;

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
            d = NOW_CYCLES() - t0;
            if (d > eep_start_max) {
                eep_start_max = d;
            }
            eeprom_started = 1;
            return;
        }
        {
            uint32_t t0 = NOW_CYCLES();
            uint32_t d;

            st = ds2431_read_poll(&eeprom);
            d = NOW_CYCLES() - t0;
            if (d > eep_poll_max) {
                eep_poll_max = d;
            }
        }
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
        publish_step();
        break;

    case MGR_WAIT:
        if (!ow_delay_poll()) {
            return;
        }
        wait_cycles++;
        if ((wait_cycles >= RESCAN_CYCLES) || (ds18b20_count == 0u)) {
            wait_cycles = 0u;
            search_started = 0u;
            search_done = 0u;
            rescan_auto = 1u;       /* keep the last readings while searching */
            manager_state = MGR_SEARCH;
            map_live[REG_FLAGS] |= FLAG_BUSY;
            break;
        }
        manager_state = (ds18b20_count != 0u) ? MGR_CONVERT : MGR_EEPROM;
        map_live[REG_FLAGS] |= FLAG_BUSY;
        break;

    default:
        manager_state = MGR_SEARCH;
        break;
    }
}











