#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
typedef struct { const char *base_path; const char *partition_label; int max_files; bool format_if_mount_failed; } esp_vfs_spiffs_conf_t;
esp_err_t esp_vfs_spiffs_register(const esp_vfs_spiffs_conf_t *c);
esp_err_t esp_spiffs_info(const char *label, size_t *total, size_t *used);
