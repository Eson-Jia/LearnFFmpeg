#include <iostream>
#include <thread>
#include <chrono>
extern "C"{
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavdevice/avdevice.h>
#include <libavfilter/avfilter.h>
#include <libavutil/avutil.h>
#include <libswscale/swscale.h>
#include <libpostproc/postprocess.h>
}
#include "FFmpeg.h"
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
//    int resampleVersion = avresample_version();
//    cout << "avresample version:" << resampleVersion << endl;
    int utilVersion = avutil_version();
    cout << "avutil version:" << utilVersion << endl;
    int swscaleVersion = swscale_version();
    cout << "swscale version:" << swscaleVersion <<endl;
    int postprocVersion = postproc_version();
    cout << "postproc version:" << postprocVersion <<endl;
    this_thread::sleep_for(chrono::milliseconds (1));
    FFmpeg::test_flush();
}

