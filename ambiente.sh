# Prepares the terminal to build M5tabOS with ESP-IDF 5.5.
#
#   source ambiente.sh
#   idf.py build
#
# Standard ESP-IDF installation: set IDF_PATH (or install in ~/esp/esp-idf)
# and this script loads its export.sh. For a non-standard environment
# (another folder, an IDF from another tool), create ambiente.local.sh next to this
# file: if it exists, it is used instead of export.sh. It does not go into the
# repository (.gitignore).

AQUI="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)"

if [ -f "$AQUI/ambiente.local.sh" ]; then
    source "$AQUI/ambiente.local.sh"
else
    : "${IDF_PATH:=$HOME/esp/esp-idf}"
    if [ ! -f "$IDF_PATH/export.sh" ]; then
        echo "ESP-IDF not found in $IDF_PATH - install ESP-IDF 5.5 or set IDF_PATH."
        return 1 2>/dev/null || exit 1
    fi
    source "$IDF_PATH/export.sh" > /dev/null
fi

# CRITICAL FOR WIFI. The esp_wifi_remote Kconfig does
#   orsource "./Kconfig.idf_v$ESP_IDF_VERSION.in"
# and orsource SILENTLY IGNORES a file that does not exist. Without this variable
# set to "5.5", the CONFIG_WIFI_RMT_* options do not exist and the build dies with
# "CONFIG_WIFI_RMT_STATIC_RX_BUFFER_NUM undeclared".
export ESP_IDF_VERSION=5.5
