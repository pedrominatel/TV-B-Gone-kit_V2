idf_component_register(
    SRCS "tvbgone_core.c" "WORLDcodes.cpp"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES driver esp_driver_rmt
)
