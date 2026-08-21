idf_component_register(
    SRCS
        "src/raop_core.c"
        "src/raop.c"
        "src/rtp.c"
        "src/util.c"
        "src/dmap_parser.c"
        "src/audio_buffer.c"
    INCLUDE_DIRS
        "include"
    PRIV_INCLUDE_DIRS
        "src"
        "private_include"
    REQUIRES
        esp_netif
        mdns
        mbedtls
        nvs_flash
    PRIV_REQUIRES
        esp_timer
        lwip
        pthread
)

target_include_directories(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/private_include"
)

target_link_libraries(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/lib/libalac.a"
)