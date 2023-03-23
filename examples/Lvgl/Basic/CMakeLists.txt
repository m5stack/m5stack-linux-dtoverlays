cmake_minimum_required(VERSION 3.0.0)
project(cm4LvglDemo VERSION 0.1.0)


include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}
)


# tick
add_library(tick tick.c)


# lvgl
add_subdirectory(lvgl)
add_subdirectory(lv_porting)


add_executable(cm4LvglDemo main.cpp)
target_link_libraries(cm4LvglDemo
    lvgl
    lvgl_porting
    lvgl_examples
    lvgl_demos
    tick
)
