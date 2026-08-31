idf_component_register(
    SRCS
        "app_main.cpp"
        "tuya_lock_bridge.cpp"
        "door_lock_manager.cpp"
    INCLUDE_DIRS "."
    PRIV_INCLUDE_DIRS "$ENV{ESP_MATTER_PATH}/examples/common/utils"
)

target_compile_options(${COMPONENT_LIB} PRIVATE "-DCHIP_HAVE_CONFIG_H")
