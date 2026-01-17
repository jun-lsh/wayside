file(GLOB SOURCES "src/*.c")
# file(GLOB SSD "ssd1306/*.c")

idf_component_register(
    SRCS ${SOURCES} ${SSD}
    INCLUDE_DIRS "lib/"
    REQUIRES nvs_flash esp_event esp_netif esp_wifi mbedtls bt esp_driver_i2c esp_driver_gpio aw9523 hnr26_badge
)
