#include "FreeRTOS.h"
typedef uint32_t *EventGroupHandle_t;
EventGroupHandle_t xEventGroupCreate(void);
uint32_t xEventGroupGetBits(EventGroupHandle_t group);
uint32_t xEventGroupSetBits(EventGroupHandle_t group, uint32_t bits);
uint32_t xEventGroupClearBits(EventGroupHandle_t group, uint32_t bits);
void vEventGroupDelete(EventGroupHandle_t group);
