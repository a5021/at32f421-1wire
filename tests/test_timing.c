#include "test.h"
#include "ow_config.h"

void test_timing_run(void)
{
    /* Slot structure: low1 + low0 + guard. */
    TF_CHECK_EQ(OW_T_LOW1 + OW_T_LOW0 + OW_T_GUARD, OW_T_SLOT);
    TF_CHECK_EQ(165, OW_T_SLOT);

    /* Reset is 480 us low within a 960 us window. */
    TF_CHECK_EQ(480, OW_T_RESET_LOW);
    TF_CHECK_EQ(960, OW_T_RESET_PERIOD);
    TF_CHECK(OW_T_RESET_LOW < OW_T_RESET_PERIOD);
    TF_CHECK(OW_T_RESET_PERIOD >= 480u + 480u);

    /* The read sample threshold must sit between the two low times. */
    TF_CHECK(OW_T_SAMPLE_MAX >= OW_T_LOW1);
    TF_CHECK(OW_T_SAMPLE_MAX < OW_T_LOW0);
    TF_CHECK(OW_T_LOW1 < OW_T_LOW0);

    /* Worst-case reply fits the hardware pass limits. */
    TF_CHECK_EQ(256, OW_MAX_SLOTS);
    TF_CHECK_EQ(32, OW_MAX_BYTES);
    TF_CHECK((9u * 8u) <= OW_MAX_SLOTS);      /* DS18B20 scratchpad */
    TF_CHECK((13u * 8u) <= OW_MAX_SLOTS);     /* DS2431 scratchpad */
    TF_CHECK((OW_MAX_BYTES * 8u) == OW_MAX_SLOTS);

    /* Capture filter and guard sanity. */
    TF_CHECK(OW_C4DF <= 0xFu);
    TF_CHECK(OW_T_GUARD >= 60u);
}
