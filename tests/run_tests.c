#include "test.h"
#include "suites.h"

int main(void)
{
    TF_RUN(test_crc_run);
    TF_RUN(test_timing_run);
    TF_RUN(test_encode_run);
    TF_RUN(test_bus_run);
    TF_RUN(test_search_run);
    TF_RUN(test_ds18b20_run);
    TF_RUN(test_ds2431_run);
    TF_RUN(test_state_run);
    TF_RUN(test_headers_run);
    TF_RUN(test_i2c_slave_run);
    TF_RUN(test_manager_run);
    return tf_summary();
}
