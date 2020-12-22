#include <iostream>
extern "C"{
#include <libavformat/avformat.h>
}

int main() {
    int version = avformat_version();
    std::cout << "avformat version:" << version << std::endl;
    return 0;
}
