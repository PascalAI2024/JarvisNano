#pragma once
#include "freertos/FreeRTOS.h"
typedef struct tskTaskControlBlock *TaskHandle_t;
BaseType_t xTaskCreatePinnedToCore(void (*fn)(void *), const char *name, uint32_t stack, void *arg, UBaseType_t prio, TaskHandle_t *out, BaseType_t core);
void xTaskNotifyGive(TaskHandle_t t);
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t ticks);
TaskHandle_t xTaskGetCurrentTaskHandle(void);
TickType_t xTaskGetTickCount(void);
void vTaskDelay(TickType_t t);
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t t);
