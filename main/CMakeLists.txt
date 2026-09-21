file(GLOB icon_srcs "assets/icons/*.c")

idf_component_register(SRCS "main.cpp" 
                            "SdUsbManager.cpp"
                            ${icon_srcs}
                       INCLUDE_DIRS "."
                       REQUIRES lvgl nvs_flash esp_wifi app_update)

target_compile_options(${COMPONENT_LIB} PRIVATE "-Wno-format")