# ESP-Matter's example manager sizes its persisted per-user credential matrix
# using the advertised count instead of the matrix stride. Our storage pool is
# deliberately wider (20 PIN slots) than the advertised three links per user,
# so compile a build-local copy with the complete matrix persisted.
set(DOOR_LOCK_MANAGER_SOURCE
    "${ESP_MATTER_PATH}/examples/door_lock/main/lock/door_lock_manager.cpp")
file(READ "${DOOR_LOCK_MANAGER_SOURCE}" DOOR_LOCK_MANAGER_CONTENT)
string(FIND "${DOOR_LOCK_MANAGER_CONTENT}"
    "sizeof(CredentialStruct) * LockParams.numberOfUsers * LockParams.numberOfCredentialsPerUser"
    CREDENTIAL_MATRIX_SIZE_EXPRESSION)
if(CREDENTIAL_MATRIX_SIZE_EXPRESSION EQUAL -1)
    message(FATAL_ERROR "ESP-Matter door lock manager persistence layout changed")
endif()
string(REPLACE
    "sizeof(CredentialStruct) * LockParams.numberOfUsers * LockParams.numberOfCredentialsPerUser"
    "sizeof(mCredentials)"
    DOOR_LOCK_MANAGER_CONTENT
    "${DOOR_LOCK_MANAGER_CONTENT}")
set(SIZED_DOOR_LOCK_MANAGER "${CMAKE_CURRENT_BINARY_DIR}/door_lock_manager_sized.cpp")
file(WRITE "${SIZED_DOOR_LOCK_MANAGER}" "${DOOR_LOCK_MANAGER_CONTENT}")

idf_component_register(
    SRCS
        "app_main.cpp"
        "tuya_lock_bridge.cpp"
        "${SIZED_DOOR_LOCK_MANAGER}"
    INCLUDE_DIRS "."
    PRIV_INCLUDE_DIRS
        "${ESP_MATTER_PATH}/examples/common/utils"
        "${ESP_MATTER_PATH}/examples/door_lock/main/lock"
)

target_compile_options(${COMPONENT_LIB} PRIVATE "-DCHIP_HAVE_CONFIG_H")
