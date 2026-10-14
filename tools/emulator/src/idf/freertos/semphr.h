#pragma once

#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct EmuSemaphore *SemaphoreHandle_t;

// Uncontended by construction: radio callbacks and app ticks interleave on
// one thread, never preempt each other.
SemaphoreHandle_t xSemaphoreCreateMutex(void);
void vSemaphoreDelete(SemaphoreHandle_t sem);

static inline BaseType_t xSemaphoreTake(SemaphoreHandle_t sem, TickType_t wait)
{
    (void)wait;
    return sem ? pdTRUE : pdFALSE;
}

static inline BaseType_t xSemaphoreGive(SemaphoreHandle_t sem)
{
    return sem ? pdTRUE : pdFALSE;
}

#ifdef __cplusplus
}
#endif
