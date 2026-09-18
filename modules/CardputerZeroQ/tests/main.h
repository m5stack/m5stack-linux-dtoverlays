/* Test-only replacement for the STM32 HAL entry header. */
#ifndef M5IO_TEST_MAIN_H
#define M5IO_TEST_MAIN_H
#include <linux/types.h>
#include <linux/string.h>
uint32_t HAL_GetTick(void);
#endif
