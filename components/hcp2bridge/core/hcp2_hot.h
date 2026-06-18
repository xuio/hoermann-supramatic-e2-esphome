#pragma once

#if defined(HCP2_ESP32_REALTIME_HOT_PATH) && defined(ESP_PLATFORM)
#include "esp_attr.h"
#define HCP2_HOT_TEXT IRAM_ATTR
#define HCP2_HOT_DATA DRAM_ATTR
#else
#define HCP2_HOT_TEXT
#define HCP2_HOT_DATA
#endif
