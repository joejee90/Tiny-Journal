#pragma once
#include <thread>
#include <chrono>
using TaskHandle_t=void*;
#define pdPASS 1
#define pdTRUE 1
#define portMAX_DELAY 0
#define pdMS_TO_TICKS(x) (x)
inline void vTaskDelay(unsigned long n){std::this_thread::sleep_for(std::chrono::milliseconds(n));}
inline int xTaskCreate(void(*)(void*),const char*,unsigned,void*,int,TaskHandle_t*t){*t=(void*)1;return pdPASS;}
