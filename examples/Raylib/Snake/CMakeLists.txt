cmake_minimum_required(VERSION 3.0.0)
project(cm4RaylibDemo VERSION 0.1.0)


# raylib
add_subdirectory(raylib)


add_executable(cm4RaylibDemo main.cpp)
target_link_libraries(cm4RaylibDemo raylib)
