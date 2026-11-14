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

void test_headers_run(void)
{
    ow_status_t st = OW_OK;
    bool flag = true;

    TF_CHECK_EQ(0, st);
    TF_CHECK_EQ(1, flag);
    TF_CHECK(sizeof(ow_search_t) >= 16u);
    TF_CHECK(sizeof(ds18b20_t) >= 16u);
    TF_CHECK(sizeof(ds2431_t) >= 16u);
}
