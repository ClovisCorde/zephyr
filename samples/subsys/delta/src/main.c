#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
 
#if defined(CONFIG_DELTA_UPDATE)
   #include <zephyr/delta/delta.h>
#endif
#include "delta_apply.h"

LOG_MODULE_REGISTER(delta_sample, LOG_LEVEL_INF);
 
static struct delta_api_t delta_api;

int main(void)
{
    int ret = 0;

    LOG_INF("Delta sample");
    ret = delta_apply(&delta_api);
    if(ret != 0){
        LOG_ERR("Error while applying delta update algorithm");
    }
    return ret;
}
