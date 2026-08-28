#pragma once
#include <stdint.h>
typedef int BaseType_t;
typedef unsigned UBaseType_t;
typedef uint32_t TickType_t;
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
typedef struct { int lock; } portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED {0}
void taskENTER_CRITICAL(portMUX_TYPE *m);
void taskEXIT_CRITICAL(portMUX_TYPE *m);
