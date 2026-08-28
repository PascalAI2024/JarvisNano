#pragma once
#include "esp_err.h"
#include "esp_log.h"
#define ESP_RETURN_ON_ERROR(x, tag, ...) do { esp_err_t __e = (x); if (__e != ESP_OK) { (void)(tag); return __e; } } while (0)
#define ESP_RETURN_ON_FALSE(x, err, tag, ...) do { if (!(x)) { (void)(tag); return (err); } } while (0)
