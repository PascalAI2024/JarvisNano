#pragma once
#include <stdio.h>
#define ESP_LOGE(tag, fmt, ...) printf(fmt "\n", ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) printf(fmt "\n", ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) printf(fmt "\n", ##__VA_ARGS__)
