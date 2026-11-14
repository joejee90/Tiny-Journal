#include "task.h"
#pragma once
#include <vector>
#include <cstring>
struct FakeQueue{size_t size;std::vector<unsigned char> last;};
using QueueHandle_t=FakeQueue*;
inline QueueHandle_t xQueueCreate(size_t,size_t size){return new FakeQueue{size,{}};}
inline int xQueueSend(QueueHandle_t q,const void*p,int){q->last.assign((const unsigned char*)p,(const unsigned char*)p+q->size);return pdTRUE;}
inline int xQueueReceive(QueueHandle_t,void*,int){return 0;}
