idf_component_register(
    SRCS "blufi_app.c" "blufi_init.c" "blufi_security.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES bt nvs_flash mbedtls esp_wifi esp_event
    REQUIRES brookesia_core
)
