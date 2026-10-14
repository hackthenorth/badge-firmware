#pragma once
using SemaphoreHandle_t = int *;
inline SemaphoreHandle_t xSemaphoreCreateMutex(){static int mutex;return &mutex;}
inline int xSemaphoreTake(SemaphoreHandle_t,unsigned){return 1;}
inline void xSemaphoreGive(SemaphoreHandle_t){}
