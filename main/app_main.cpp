#include <esp_err.h>
#include <esp_log.h>
#include <nvs_flash.h>

#include <esp_matter.h>
#include <esp_matter_console.h>
#include <esp_matter_ota.h>
#include <common_macros.h>

#if CHIP_DEVICE_CONFIG_ENABLE_THREAD
#include <platform/ESP32/OpenthreadLauncher.h>
#include "esp_openthread_types.h"
#endif

#include <app/clusters/door-lock-server/door-lock-server.h>
#include <app/server/CommissioningWindowManager.h>
#include <app/server/Server.h>
#include <platform/PlatformManager.h>

#include "tuya_lock_bridge.h"

static const char *TAG = "srex_matter_lock";
static uint16_t s_lock_endpoint_id = 0;

using namespace esp_matter;
using namespace esp_matter::endpoint;
using namespace chip;
using namespace chip::app::Clusters;
using namespace chip::app::Clusters::DoorLock;

#if CHIP_DEVICE_CONFIG_ENABLE_THREAD
#define ESP_OPENTHREAD_DEFAULT_RADIO_CONFIG() { .radio_mode = RADIO_MODE_NATIVE }
#define ESP_OPENTHREAD_DEFAULT_HOST_CONFIG() { .host_connection_mode = HOST_CONNECTION_MODE_NONE }
#define ESP_OPENTHREAD_DEFAULT_PORT_CONFIG() { .storage_partition_name = "nvs", .netif_queue_size = 10, .task_queue_size = 10 }
#endif

static void matter_apply_lock_state(intptr_t arg)
{
    bool locked = arg != 0;
    if (!s_lock_endpoint_id) return;
    DoorLockServer::Instance().SetLockState(
        s_lock_endpoint_id,
        locked ? DlLockState::kLocked : DlLockState::kUnlocked,
        OperationSourceEnum::kManual);
    ESP_LOGI(TAG, "Matter LockState <= %s", locked ? "LOCKED" : "UNLOCKED");
}

static void physical_state_cb(bool locked, void *)
{
    CHIP_ERROR err = chip::DeviceLayer::PlatformMgr().ScheduleWork(matter_apply_lock_state, locked ? 1 : 0);
    if (err != CHIP_NO_ERROR) {
        ESP_LOGE(TAG, "Failed to schedule Matter state update: %" CHIP_ERROR_FORMAT, err.Format());
    }
}

static void app_event_cb(const ChipDeviceEvent *event, intptr_t)
{
    switch (event->Type) {
    case chip::DeviceLayer::DeviceEventType::kCommissioningComplete:
        ESP_LOGI(TAG, "Matter commissioning complete");
        break;
    case chip::DeviceLayer::DeviceEventType::kCommissioningSessionStarted:
        ESP_LOGI(TAG, "Commissioning session started");
        break;
    case chip::DeviceLayer::DeviceEventType::kCommissioningSessionStopped:
        ESP_LOGI(TAG, "Commissioning session stopped");
        break;
    case chip::DeviceLayer::DeviceEventType::kInterfaceIpAddressChanged:
        ESP_LOGI(TAG, "Thread/IP address changed");
        break;
    default:
        break;
    }
}

static esp_err_t app_identification_cb(identification::callback_type_t type, uint16_t endpoint_id,
                                       uint8_t effect_id, uint8_t effect_variant, void *)
{
    ESP_LOGI(TAG, "Identify: type=%u endpoint=%u effect=%u variant=%u", type, endpoint_id, effect_id, effect_variant);
    return ESP_OK;
}

static esp_err_t app_attribute_update_cb(attribute::callback_type_t, uint16_t, uint32_t, uint32_t,
                                         esp_matter_attr_val_t *, void *)
{
    return ESP_OK;
}

// Matter Door Lock server callbacks. These are called by Matter lock/unlock commands.
void emberAfDoorLockClusterInitCallback(EndpointId endpoint)
{
    DoorLockServer::Instance().InitServer(endpoint);
    DoorLockServer::Instance().SetLockState(endpoint, DlLockState::kLocked);
    ESP_LOGI(TAG, "Door Lock server initialized on endpoint %u", endpoint);
}

bool emberAfPluginDoorLockOnDoorLockCommand(EndpointId endpointId, const Nullable<FabricIndex> &,
                                             const Nullable<NodeId> &, const Optional<ByteSpan> &,
                                             OperationErrorEnum &)
{
    ESP_LOGI(TAG, "Matter LOCK command endpoint=%u", endpointId);
    return tuya_lock_request_lock();
}

bool emberAfPluginDoorLockOnDoorUnlockCommand(EndpointId endpointId, const Nullable<FabricIndex> &,
                                               const Nullable<NodeId> &, const Optional<ByteSpan> &,
                                               OperationErrorEnum &)
{
    ESP_LOGI(TAG, "Matter UNLOCK command endpoint=%u", endpointId);
    return tuya_lock_request_unlock();
}

void emberAfPluginDoorLockOnAutoRelock(EndpointId)
{
    ESP_LOGI(TAG, "Matter auto-relock request");
    tuya_lock_request_lock();
}

extern "C" void app_main()
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    node::config_t node_config;
    node_t *node = node::create(&node_config, app_attribute_update_cb, app_identification_cb);
    ABORT_APP_ON_FAILURE(node != nullptr, ESP_LOGE(TAG, "Failed to create Matter node"));

    door_lock::config_t lock_config;
    lock_config.door_lock.lock_state = chip::to_underlying(DlLockState::kLocked);
    endpoint_t *endpoint = door_lock::create(node, &lock_config, ENDPOINT_FLAG_NONE, nullptr);
    ABORT_APP_ON_FAILURE(endpoint != nullptr, ESP_LOGE(TAG, "Failed to create Door Lock endpoint"));
    s_lock_endpoint_id = endpoint::get_id(endpoint);
    ESP_LOGI(TAG, "SREX Door Lock endpoint=%u", s_lock_endpoint_id);

#if CHIP_DEVICE_CONFIG_ENABLE_THREAD
    esp_openthread_platform_config_t ot_config = {
        .radio_config = ESP_OPENTHREAD_DEFAULT_RADIO_CONFIG(),
        .host_config = ESP_OPENTHREAD_DEFAULT_HOST_CONFIG(),
        .port_config = ESP_OPENTHREAD_DEFAULT_PORT_CONFIG(),
    };
    set_openthread_platform_config(&ot_config);
#endif

    err = esp_matter::start(app_event_cb);
    ABORT_APP_ON_FAILURE(err == ESP_OK, ESP_LOGE(TAG, "Matter start failed: %d", err));

    ESP_LOGI(TAG, "=== MATTER COMMISSIONING ===");
    ESP_LOGI(TAG, "Device is ready for commissioning via BLE");
    ESP_LOGI(TAG, "Look for setup code in serial output above");
    ESP_LOGI(TAG, "===========================");

    err = tuya_lock_bridge_init(physical_state_cb, nullptr);
    ABORT_APP_ON_FAILURE(err == ESP_OK, ESP_LOGE(TAG, "Tuya UART init failed: %d", err));

#if CONFIG_ENABLE_OTA_REQUESTOR
    esp_matter_ota_requestor_init();
#endif

#if CONFIG_ENABLE_CHIP_SHELL
    esp_matter::console::diagnostics_register_commands();
    esp_matter::console::factoryreset_register_commands();
#if CONFIG_OPENTHREAD_CLI
    esp_matter::console::otcli_register_commands();
#endif
    esp_matter::console::init();
#endif

    ESP_LOGI(TAG, "SREX ESP32-C6 Matter-over-Thread Tuya lock bridge READY");
}
