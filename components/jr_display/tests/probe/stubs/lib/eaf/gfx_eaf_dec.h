#pragma once
/* Host stand-in for the engine-private EAF decoder (src/lib/eaf). Only the
 * calls jr_display.c's one-shot dial decode makes; see idf_stubs.inc. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
typedef void *eaf_format_handle_t;
typedef enum { EAF_FORMAT_VALID = 0, EAF_FORMAT_INVALID = 2 } eaf_format_type_t;
typedef struct {
    uint8_t bit_depth;
    uint16_t width;
    uint16_t height;
} eaf_header_t;
esp_err_t eaf_init(const uint8_t *data, size_t data_len, eaf_format_handle_t *ret_parser);
esp_err_t eaf_deinit(eaf_format_handle_t handle);
eaf_format_type_t eaf_get_frame_info(eaf_format_handle_t handle, int frame_index,
                                     eaf_header_t *frame_info);
void eaf_free_header(eaf_header_t *header);
esp_err_t eaf_frame_decode(eaf_format_handle_t handle, int frame_index,
                           uint8_t *frame_buffer, size_t frame_buffer_size,
                           bool swap_bytes);
