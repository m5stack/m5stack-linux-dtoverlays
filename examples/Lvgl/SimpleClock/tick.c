/**
 * @file tick.c
 * @author Forairaaaaa
 * @brief 
 * @version 0.1
 * @date 2023-02-09
 * 
 * @copyright Copyright (c) 2023
 * 
 */
#include "tick.h"

int64_t lvgl_get_tick()
{    
    struct timeval tv;    
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000 + tv.tv_usec / 1000;    
}
