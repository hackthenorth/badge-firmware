#pragma once

#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void *TaskHandle_t;

typedef struct {
    const char *pcTaskName;
    UBaseType_t uxCurrentPriority;
    uint32_t usStackHighWaterMark;
} TaskStatus_t;

// There is one thread, so a delay cannot yield. It advances the virtual clock
// instead, which keeps tick-based timeouts in firmware code finite.
void vTaskDelay(TickType_t ticks);
TickType_t xTaskGetTickCount(void);

UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t task);
UBaseType_t uxTaskGetNumberOfTasks(void);
UBaseType_t uxTaskGetSystemState(TaskStatus_t *out, UBaseType_t capacity,
                                 uint32_t *total_run_time);

#ifdef __cplusplus
}
#endif
