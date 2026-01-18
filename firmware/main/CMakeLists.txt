file(GLOB SOURCES "src/*.c")
file(GLOB DRIVERS "drivers/*.c")
# file(GLOB SSD "ssd1306/*.c")

idf_component_register(
    SRCS ${SOURCES} ${SSD} ${DRIVERS}
    INCLUDE_DIRS "lib/" "drivers/"
    REQUIRES 
        nvs_flash 
        esp_event 
        esp_netif 
        esp_wifi 
        mbedtls 
        bt
        aw9523 
        hnr26_badge
        esp_adc
        esp_driver_gpio
        esp_driver_i2c
        esp_driver_tsens
)
