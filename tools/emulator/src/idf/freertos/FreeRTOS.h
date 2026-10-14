#pragma once

#include <assert.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t TickType_t;
typedef int BaseType_t;
typedef unsigned int UBaseType_t;
typedef uint8_t StackType_t;

#define pdFALSE 0
#define pdTRUE  1
#define pdFAIL  0
#define pdPASS  1

#define portMAX_DELAY      ((TickType_t)0xffffffffu)
#define portTICK_PERIOD_MS 10u
#define pdMS_TO_TICKS(ms)  ((TickType_t)((ms) / portTICK_PERIOD_MS))
#define configASSERT(x)    assert(x)
#define tskIDLE_PRIORITY   0

// Critical sections guard against preemption; one thread has none.
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(mux) ((void)(mux))
#define portEXIT_CRITICAL(mux)  ((void)(mux))

#ifdef __cplusplus
}
#endif
