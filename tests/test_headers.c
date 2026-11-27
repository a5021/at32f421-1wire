#include "test.h"

/* Include every public header twice: guards must make this harmless. */
#include "ow_config.h"
#include "ow_config.h"
#include "ow_crc.h"
#include "ow_crc.h"
#include "ow_hal.h"
#include "ow_hal.h"
#include "onewire.h"
#include "onewire.h"
#include "ds18b20.h"
#include "ds18b20.h"
#include "ds2431.h"
#include "ds2431.h"
#include "clock.h"
#include "clock.h"
#include "i2c_latch.h"
#include "manager.h"
#include "manager.h"
#include "regmap.h"
#include "regmap.h"

void test_headers_run(void)
{
    ow_status_t st = OW_OK;
    bool flag = true;

    TF_CHECK_EQ(0, st);
    TF_CHECK_EQ(1, flag);
    TF_CHECK(sizeof(ow_search_t) >= 16u);
    TF_CHECK(sizeof(ds18b20_t) >= 16u);
    TF_CHECK(sizeof(ds2431_t) >= 16u);
    TF_CHECK_EQ(0x05, BRIDGE_MAP_VERSION);

    {
        i2c_select_latch_t latch = { 0u, false };
        uint8_t selected = 0xFFu;

        i2c_select_latch_write(&latch, 0x1Fu);
        TF_CHECK(latch.pending);
        TF_CHECK_EQ(0x0Fu, latch.value);
        TF_CHECK(i2c_select_latch_commit(&latch, &selected));
        TF_CHECK_EQ(0x0Fu, selected);
        TF_CHECK(!i2c_select_latch_commit(&latch, &selected));

        i2c_select_latch_write(&latch, 2u);
        i2c_select_latch_abort(&latch);
        TF_CHECK(!i2c_select_latch_commit(&latch, &selected));
    }
}
