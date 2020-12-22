cmake_minimum_required(VERSION 3.16)
project(LearnFFmpeg)

set(CMAKE_CXX_STANDARD 11)

add_executable(LearnFFmpeg main.cpp)

target_link_libraries(
        LearnFFmpeg
        avformat
        avcodec
        avdevice
        avfilter
        avresample
        avutil
        swscale
)