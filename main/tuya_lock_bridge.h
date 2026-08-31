#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*tuya_lock_state_cb_t)(bool locked, void *ctx);
typedef void (*tuya_lock_battery_cb_t)(uint8_t percent, void *ctx);

esp_err_t tuya_lock_bridge_init(tuya_lock_state_cb_t state_cb,
                                tuya_lock_battery_cb_t battery_cb,
                                void *ctx);
bool tuya_lock_request_unlock(void);
bool tuya_lock_request_lock(void);

#ifdef __cplusplus
}
#endif
