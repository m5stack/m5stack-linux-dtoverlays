/**
 * @file main.cpp
 * @author Forairaaaaa
 * @brief 
 * @version 0.1
 * @date 2023-02-08
 * 
 * @copyright Copyright (c) 2023
 * 
 */
/* Using SDL2 */
#define DISP_USING_SDL      0
/* Using framebuffer directly */
/* you can define your path to FBDEV_PATH in lv_drv_conf.h */
#define DISP_USING_FB       1
/* Enable touch pad */
#define ENABLE_TOUCH_PAD    1


#include <iostream>
#include <unistd.h>
#include "lvgl/lvgl.h"
#include "lvgl/demos/lv_demos.h"
#include "lv_porting/lv_port_disp.h"
#include "lv_porting/lv_port_indev.h"


int main(int argc, char const *argv[])
{
    printf("> CM4Stack Lvgl demo <\n> Press ctrl + c to quit\n");

    /* Lvgl init */
    lv_init();

    /* Display init */
    #if DISP_USING_SDL
        lv_port_disp_init(0);
        lv_port_indev_init(0, ENABLE_TOUCH_PAD);
    #elif DISP_USING_FB
        lv_port_disp_init(1);
        lv_port_indev_init(1, ENABLE_TOUCH_PAD);
    #endif
    

    /* Lvgl offical demos */
    lv_demo_widgets();
    // lv_demo_stress();
    // lv_demo_benchmark();
    // lv_demo_music();


    while(1)
    {
        lv_timer_handler();
        usleep(5000);
    }
    return 0;
}

