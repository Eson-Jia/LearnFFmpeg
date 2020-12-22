#include <iostream>
#include <thread>
#include <chrono>
extern "C"{
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavdevice/avdevice.h>
#include <libavfilter/avfilter.h>
#include <libavresample/avresample.h>
#include <libavutil/avutil.h>
}
using namespace std;
int main() {
    int avformatVersion = avformat_version();
    cout << "avformat version:" << avformatVersion << endl;
    int codecVersion = avcodec_version();
    cout << "avcodec version:" << codecVersion << endl;
    int deviceVersion = avdevice_version();
    cout << "avdevice version:" << deviceVersion << endl;
    int filterVersion = avfilter_version();
    cout << "avfilter version:" << filterVersion << endl;
    int resampleVersion = avresample_version();
    cout << "avresample version:" << resampleVersion << endl;
    int utilVersion = avutil_version();
    cout << "avutil version:" << utilVersion << endl;
    this_thread::sleep_for(chrono::milliseconds (1));
    return 0;
}
