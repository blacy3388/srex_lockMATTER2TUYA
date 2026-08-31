idf_component_register(
    SRCS
        "app_main.cpp"
        "tuya_lock_bridge.cpp"
        "${ESP_MATTER_PATH}/examples/door_lock/main/lock/door_lock_manager.cpp"
    INCLUDE_DIRS "."
    PRIV_INCLUDE_DIRS
        "${ESP_MATTER_PATH}/examples/common/utils"
        "${ESP_MATTER_PATH}/examples/door_lock/main/lock"
)

target_compile_options(${COMPONENT_LIB} PRIVATE "-DCHIP_HAVE_CONFIG_H")
