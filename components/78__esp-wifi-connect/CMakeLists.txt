set(hosted_requires "")
# Requirements are expanded before sdkconfig variables are available.
if(IDF_TARGET STREQUAL "esp32p4" OR IDF_TARGET STREQUAL "esp32h2")
    list(APPEND hosted_requires espressif__esp_hosted)
endif()

idf_component_register(
    SRCS
        "wifi_configuration_ap.cc"
        "wifi_station.cc"
        "ssid_manager.cc"
        "dns_server.cc"
    INCLUDE_DIRS
        "include"
    EMBED_TXTFILES
        "assets/wifi_configuration.html"
        "assets/wifi_configuration_done.html"
    REQUIRES
        "esp_timer"
        "esp_http_server"
        "esp_wifi"
        "nvs_flash"
        "json"
    PRIV_REQUIRES ${hosted_requires}
)
