#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*tuya_lock_state_cb_t)(bool locked, void *ctx);

esp_err_t tuya_lock_bridge_init(tuya_lock_state_cb_t cb, void *ctx);
bool tuya_lock_request_unlock(void);
bool tuya_lock_request_lock(void);

#ifdef __cplusplus
}
#endif
