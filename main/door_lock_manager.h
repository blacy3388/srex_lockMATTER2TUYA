// Local sizing wrapper for ESP-Matter's door_lock_manager.cpp.
// The implementation is compiled from the pinned ESP-Matter v1.6 checkout;
// keeping this header local lets this product select realistic resource limits.
#pragma once

#include <app/clusters/door-lock-server/door-lock-server.h>
#include <lib/core/CHIPError.h>

#include <stdbool.h>
#include <stdint.h>

struct WeekDaysScheduleInfo {
    DlScheduleStatus status;
    EmberAfPluginDoorLockWeekDaySchedule schedule;
};

struct YearDayScheduleInfo {
    DlScheduleStatus status;
    EmberAfPluginDoorLockYearDaySchedule schedule;
};

struct HolidayScheduleInfo {
    DlScheduleStatus status;
    EmberAfPluginDoorLockHolidaySchedule schedule;
};

namespace ESP32DoorLock {
namespace ResourceRanges {

static constexpr uint16_t kMaxUsers                  = 20;
// The SDK uses this value both for the per-user credential list storage and
// for credential indices. Twenty slots allow one unique PIN per supported
// user; the Matter attribute below limits links to three per user.
static constexpr uint8_t kMaxCredentialsPerUser      = 20;
static constexpr uint8_t kMaxWeekdaySchedulesPerUser = 1;
static constexpr uint8_t kMaxYeardaySchedulesPerUser = 1;
static constexpr uint8_t kMaxHolidaySchedules        = 1;
static constexpr uint8_t kMaxCredentialSize          = 16;

// Only Programming PIN (type 0) and PIN (type 1) are advertised.
static constexpr uint16_t kSupportedCredentialTypes = 2;
static constexpr uint16_t kMaxCredentials = kSupportedCredentialTypes * kMaxCredentialsPerUser;
} // namespace ResourceRanges

namespace LockInitParams {

struct LockParam {
    uint16_t numberOfUsers                  = 0;
    uint8_t numberOfCredentialsPerUser      = 0;
    uint8_t numberOfWeekdaySchedulesPerUser = 0;
    uint8_t numberOfYeardaySchedulesPerUser = 0;
    uint8_t numberOfHolidaySchedules        = 0;
};

class ParamBuilder {
public:
    ParamBuilder &SetNumberOfUsers(uint16_t value)
    {
        lockParam_.numberOfUsers = value;
        return *this;
    }
    ParamBuilder &SetNumberOfCredentialsPerUser(uint8_t value)
    {
        lockParam_.numberOfCredentialsPerUser = value;
        return *this;
    }
    ParamBuilder &SetNumberOfWeekdaySchedulesPerUser(uint8_t value)
    {
        lockParam_.numberOfWeekdaySchedulesPerUser = value;
        return *this;
    }
    ParamBuilder &SetNumberOfYeardaySchedulesPerUser(uint8_t value)
    {
        lockParam_.numberOfYeardaySchedulesPerUser = value;
        return *this;
    }
    ParamBuilder &SetNumberOfHolidaySchedules(uint8_t value)
    {
        lockParam_.numberOfHolidaySchedules = value;
        return *this;
    }
    LockParam GetLockParam() { return lockParam_; }

private:
    LockParam lockParam_;
};

} // namespace LockInitParams
} // namespace ESP32DoorLock

using namespace chip;
using namespace ESP32DoorLock::ResourceRanges;

class BoltLockManager {
public:
    CHIP_ERROR InitLockState();
    CHIP_ERROR Init(chip::app::DataModel::Nullable<chip::app::Clusters::DoorLock::DlLockState> state,
                    ESP32DoorLock::LockInitParams::LockParam lockParam);

    bool ValidatePIN(chip::EndpointId endpointId, const Optional<chip::ByteSpan> &pin,
                     OperationErrorEnum &err) const;
    void Lock(chip::EndpointId endpointId, app::Clusters::DoorLock::OperationSourceEnum source);
    void Unlock(chip::EndpointId endpointId, app::Clusters::DoorLock::OperationSourceEnum source);

    bool GetUser(chip::EndpointId endpointId, uint16_t userIndex,
                 EmberAfPluginDoorLockUserInfo &user);
    bool SetUser(chip::EndpointId endpointId, uint16_t userIndex, chip::FabricIndex creator,
                 chip::FabricIndex modifier, const chip::CharSpan &userName, uint32_t uniqueId,
                 UserStatusEnum userStatus, UserTypeEnum userType,
                 CredentialRuleEnum credentialRule, const CredentialStruct *credentials,
                 size_t totalCredentials);
    bool GetCredential(chip::EndpointId endpointId, uint16_t credentialIndex,
                       CredentialTypeEnum credentialType,
                       EmberAfPluginDoorLockCredentialInfo &credential);
    bool SetCredential(chip::EndpointId endpointId, uint16_t credentialIndex,
                       chip::FabricIndex creator, chip::FabricIndex modifier,
                       DlCredentialStatus credentialStatus, CredentialTypeEnum credentialType,
                       const chip::ByteSpan &credentialData);

    DlStatus GetWeekdaySchedule(chip::EndpointId endpointId, uint8_t weekdayIndex,
                                uint16_t userIndex, EmberAfPluginDoorLockWeekDaySchedule &schedule);
    DlStatus SetWeekdaySchedule(chip::EndpointId endpointId, uint8_t weekdayIndex,
                                uint16_t userIndex, DlScheduleStatus status, DaysMaskMap daysMask,
                                uint8_t startHour, uint8_t startMinute, uint8_t endHour,
                                uint8_t endMinute);
    DlStatus GetYeardaySchedule(chip::EndpointId endpointId, uint8_t yearDayIndex,
                                uint16_t userIndex, EmberAfPluginDoorLockYearDaySchedule &schedule);
    DlStatus SetYeardaySchedule(chip::EndpointId endpointId, uint8_t yearDayIndex,
                                uint16_t userIndex, DlScheduleStatus status,
                                uint32_t localStartTime, uint32_t localEndTime);
    DlStatus GetHolidaySchedule(chip::EndpointId endpointId, uint8_t holidayIndex,
                                EmberAfPluginDoorLockHolidaySchedule &schedule);
    DlStatus SetHolidaySchedule(chip::EndpointId endpointId, uint8_t holidayIndex,
                                DlScheduleStatus status, uint32_t localStartTime,
                                uint32_t localEndTime, OperatingModeEnum operatingMode);

    bool IsValidUserIndex(uint16_t userIndex);
    bool IsValidCredentialIndex(uint16_t credentialIndex, CredentialTypeEnum type);
    uint16_t CredentialStorageIndex(uint16_t credentialIndex, CredentialTypeEnum type) const;
    bool IsValidWeekdayScheduleIndex(uint8_t scheduleIndex);
    bool IsValidYeardayScheduleIndex(uint8_t scheduleIndex);
    bool IsValidHolidayScheduleIndex(uint8_t scheduleIndex);
    const char *lockStateToString(DlLockState lockState) const;
    bool ReadConfigValues();

private:
    friend BoltLockManager &BoltLockMgr();

    EmberAfPluginDoorLockUserInfo mLockUsers[kMaxUsers];
    EmberAfPluginDoorLockCredentialInfo mLockCredentials[kMaxCredentials];
    WeekDaysScheduleInfo mWeekdaySchedule[kMaxUsers][kMaxWeekdaySchedulesPerUser];
    YearDayScheduleInfo mYeardaySchedule[kMaxUsers][kMaxYeardaySchedulesPerUser];
    HolidayScheduleInfo mHolidaySchedule[kMaxHolidaySchedules];
    char mUserNames[kMaxUsers][DOOR_LOCK_MAX_USER_NAME_SIZE];
    uint8_t mCredentialData[kMaxCredentials][kMaxCredentialSize];
    CredentialStruct mCredentials[kMaxUsers][kMaxCredentialsPerUser];

    static BoltLockManager sLock;
    ESP32DoorLock::LockInitParams::LockParam LockParams;
};

inline BoltLockManager &BoltLockMgr()
{
    return BoltLockManager::sLock;
}
