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
        avutil
        swscale
        postproc
)

add_executable(avio_reading avio_reading.c)
target_link_libraries(
        avio_reading
        avformat
        avcodec
        avutil
)

add_executable(decode_audio decode_audio.c)
target_link_libraries(
        decode_audio
        avcodec
        avutil
)

add_executable(decode_video decode_video.c)
target_link_libraries(
        decode_video
        avcodec
        avutil
)

add_executable(remuxing remuxing.c)
target_link_libraries(
        remuxing
        avformat
        avutil
        avcodec
)

add_executable(encode_video encode_video.c)
target_link_libraries(
        encode_video
        avformat
        avutil
        avcodec
)

add_executable(encode_video_cpp encode_video.cpp)

target_link_libraries(
        encode_video_cpp
        avcodec
        avutil
)

add_executable(demuxing_decoding demuxing_decoding.c)

target_link_libraries(
        demuxing_decoding
        avformat
        avutil
        avcodec
)
