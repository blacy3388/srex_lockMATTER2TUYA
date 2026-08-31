#include <esp_err.h>
#include <esp_log.h>
#include <nvs_flash.h>
#include <cstring>

#include <esp_matter.h>
#include <esp_matter_console.h>
#include <esp_matter_ota.h>
#include <common_macros.h>

#if CHIP_DEVICE_CONFIG_ENABLE_THREAD
#include <platform/ESP32/OpenthreadLauncher.h>
#include "esp_openthread_types.h"
#endif

#include <app/clusters/door-lock-server/door-lock-server.h>
#include <app-common/zap-generated/attributes/Accessors.h>
#include <app/server/CommissioningWindowManager.h>
#include <app/server/Server.h>
#include <platform/PlatformManager.h>

#include "door_lock_manager.h"
#include "tuya_lock_bridge.h"

static const char *TAG = "srex_matter_lock";
static uint16_t s_lock_endpoint_id = 0;
static uint16_t s_power_source_endpoint_id = 0;

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

static void matter_apply_battery(intptr_t arg)
{
    if (!s_power_source_endpoint_id) return;

    const uint8_t percent = static_cast<uint8_t>(arg);
    // Matter represents battery percentage in half-percent units (0..200).
    esp_matter_attr_val_t value = esp_matter_nullable_uint8(nullable<uint8_t>(percent * 2));
    esp_err_t err = attribute::update(s_power_source_endpoint_id, PowerSource::Id,
                                      PowerSource::Attributes::BatPercentRemaining::Id, &value);
    const uint8_t charge_level = percent <= 10 ? 2 : (percent <= 20 ? 1 : 0);
    esp_matter_attr_val_t level_value =
        esp_matter_attr_val(charge_level, esp_matter_attr_val::uint_sub_type::k_enum);
    attribute::update(s_power_source_endpoint_id, PowerSource::Id,
                      PowerSource::Attributes::BatChargeLevel::Id, &level_value);
    esp_matter_attr_val_t replacement_value = esp_matter_bool(percent <= 10);
    attribute::update(s_power_source_endpoint_id, PowerSource::Id,
                      PowerSource::Attributes::BatReplacementNeeded::Id, &replacement_value);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to update battery percentage: %d", err);
    } else {
        ESP_LOGI(TAG, "Matter battery <= %u%%", percent);
    }
}

static void battery_state_cb(uint8_t percent, void *)
{
    CHIP_ERROR err = chip::DeviceLayer::PlatformMgr().ScheduleWork(matter_apply_battery, percent);
    if (err != CHIP_NO_ERROR) {
        ESP_LOGE(TAG, "Failed to schedule Matter battery update: %" CHIP_ERROR_FORMAT, err.Format());
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
    if (BoltLockMgr().InitLockState() != CHIP_NO_ERROR) {
        ESP_LOGE(TAG, "Persistent Matter user database initialization failed");
    }
    ESP_LOGI(TAG, "Door Lock server initialized on endpoint %u", endpoint);
}

bool emberAfPluginDoorLockOnDoorLockCommand(EndpointId endpointId, const Nullable<FabricIndex> &,
                                             const Nullable<NodeId> &, const Optional<ByteSpan> &pinCode,
                                             OperationErrorEnum &operationError)
{
    ESP_LOGI(TAG, "Matter LOCK command endpoint=%u", endpointId);
    if (!BoltLockMgr().ValidatePIN(endpointId, pinCode, operationError)) return false;
    return tuya_lock_request_lock();
}

bool emberAfPluginDoorLockOnDoorUnlockCommand(EndpointId endpointId, const Nullable<FabricIndex> &,
                                               const Nullable<NodeId> &, const Optional<ByteSpan> &pinCode,
                                               OperationErrorEnum &operationError)
{
    ESP_LOGI(TAG, "Matter UNLOCK command endpoint=%u", endpointId);
    if (!BoltLockMgr().ValidatePIN(endpointId, pinCode, operationError)) return false;
    return tuya_lock_request_unlock();
}

bool emberAfPluginDoorLockGetCredential(EndpointId endpointId, uint16_t credentialIndex,
                                        CredentialTypeEnum credentialType,
                                        EmberAfPluginDoorLockCredentialInfo &credential)
{
    return BoltLockMgr().GetCredential(endpointId, credentialIndex, credentialType, credential);
}

bool emberAfPluginDoorLockSetCredential(EndpointId endpointId, uint16_t credentialIndex,
                                        FabricIndex creator, FabricIndex modifier,
                                        DlCredentialStatus credentialStatus,
                                        CredentialTypeEnum credentialType,
                                        const ByteSpan &credentialData)
{
    return BoltLockMgr().SetCredential(endpointId, credentialIndex, creator, modifier,
                                       credentialStatus, credentialType, credentialData);
}

bool emberAfPluginDoorLockGetUser(EndpointId endpointId, uint16_t userIndex,
                                  EmberAfPluginDoorLockUserInfo &user)
{
    return BoltLockMgr().GetUser(endpointId, userIndex, user);
}

bool emberAfPluginDoorLockSetUser(EndpointId endpointId, uint16_t userIndex,
                                  FabricIndex creator, FabricIndex modifier,
                                  const CharSpan &userName, uint32_t uniqueId,
                                  UserStatusEnum userStatus, UserTypeEnum userType,
                                  CredentialRuleEnum credentialRule,
                                  const CredentialStruct *credentials, size_t totalCredentials)
{
    return BoltLockMgr().SetUser(endpointId, userIndex, creator, modifier, userName,
                                 uniqueId, userStatus, userType, credentialRule,
                                 credentials, totalCredentials);
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

    cluster_t *door_lock_cluster = cluster::get(endpoint, DoorLock::Id);
    cluster::door_lock::feature::credential_over_the_air_access::config_t cota_config;
    cluster::door_lock::feature::pin_credential::config_t pin_config;
    cluster::door_lock::feature::user::config_t user_config;
    cota_config.require_pin_for_remote_operation = false;
    pin_config.number_pin_users_supported = 20;
    pin_config.min_pin_code_length = 4;
    pin_config.max_pin_code_length = 8;
    pin_config.wrong_code_entry_limit = 5;
    pin_config.user_code_temporary_disable_time = 60;
    pin_config.require_pin_for_remote_operation = false;
    user_config.number_of_total_user_supported = 20;
    user_config.number_of_credentials_supported_per_user = 3;
    ABORT_APP_ON_FAILURE(
        cluster::door_lock::feature::credential_over_the_air_access::add(door_lock_cluster, &cota_config) == ESP_OK,
        ESP_LOGE(TAG, "Failed to enable credential-over-the-air access"));
    ABORT_APP_ON_FAILURE(
        cluster::door_lock::feature::pin_credential::add(door_lock_cluster, &pin_config) == ESP_OK,
        ESP_LOGE(TAG, "Failed to enable PIN credentials"));
    ABORT_APP_ON_FAILURE(
        cluster::door_lock::feature::user::add(door_lock_cluster, &user_config) == ESP_OK,
        ESP_LOGE(TAG, "Failed to enable Matter users"));
    cluster::door_lock::attribute::create_auto_relock_time(door_lock_cluster, 5);

    power_source::config_t power_config;
    power_config.power_source.feature_flags = cluster::power_source::feature::battery::get_id();
    std::strncpy(power_config.power_source.description, "Lock battery",
                 sizeof(power_config.power_source.description) - 1);
    endpoint_t *power_endpoint = power_source::create(node, &power_config, ENDPOINT_FLAG_NONE, nullptr);
    ABORT_APP_ON_FAILURE(power_endpoint != nullptr, ESP_LOGE(TAG, "Failed to create battery endpoint"));
    endpoint::set_parent_endpoint(power_endpoint, endpoint);
    s_power_source_endpoint_id = endpoint::get_id(power_endpoint);
    cluster_t *power_cluster = cluster::get(power_endpoint, PowerSource::Id);
    cluster::power_source::attribute::create_bat_percent_remaining(
        power_cluster, nullable<uint8_t>(),
        nullable<uint8_t>(static_cast<uint8_t>(0)),
        nullable<uint8_t>(static_cast<uint8_t>(200)));
    cluster::power_source::attribute::create_bat_present(power_cluster, true);
    ESP_LOGI(TAG, "Battery Power Source endpoint=%u", s_power_source_endpoint_id);

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

    err = tuya_lock_bridge_init(physical_state_cb, battery_state_cb, nullptr);
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
