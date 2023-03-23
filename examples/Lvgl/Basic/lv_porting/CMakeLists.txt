# submodule
add_subdirectory(lv_drivers)

add_library(lvgl_porting
    lv_port_disp.c
    lv_port_disp.h
    lv_port_indev.c
    lv_port_indev.h
)
find_package(SDL2 REQUIRED SDL2)
include_directories(${SDL2_INCLUDE_DIRS})
target_link_libraries(lvgl_porting lvgl_drivers ${SDL2_LIBRARIES})
