//
// Created by Jia on 2022/2/25.
//
extern "C" {
#include "libavformat/avformat.h"
#include "libavcodec/avcodec.h"
#include "libavutil/imgutils.h"
#include "libavutil/log.h"
#include "libavdevice/avdevice.h"
#include "x264.h"
}

#include <fstream>
#include <iostream>
#include <vector>
#include "FFmpeg.h"

using namespace std;
//char av_error[AV_ERROR_MAX_STRING_SIZE] = {0};
//#define av_err2str(errnum) av_make_error_string(av_error, AV_ERROR_MAX_STRING_SIZE, errnum)

static int unwrap_packet(AVPacket *source, AVFrame *dest);

//int FFmpeg::test_h264() {
//    x264_param_t param;
//    x264_param_default(&param);
//    av_log_set_level(AV_LOG_TRACE);
//    auto codec = avcodec_find_encoder(AV_CODEC_ID_H264);
//    if (codec) {
//        std::cout << "success!!!" << codec->name << std::endl;
//    } else {
//        std::cout << "failed!!!" << std::endl;
//    }
//    return 0;
//}




void FFmpeg::record_video() {
    using namespace std;
    avdevice_register_all();
    const auto DEVICE = "video=Logitech HD Webcam C270";
    auto *fmt = av_find_input_format("dshow");
    AVDictionary *options = nullptr;
    av_dict_set(&options, "video_size", "640x360", 0);
    av_dict_set(&options, "pixel_format", "yuv420p", 0);
    av_dict_set(&options, "framerate", "30", 0);
    AVFormatContext *ctx = nullptr;
    auto res = avformat_open_input(&ctx, DEVICE, fmt, &options);
    if (res < 0) {
        av_log(nullptr, AV_LOG_ERROR, "%s", av_err2str(res));
        return;
    }
    AVCodecContext *codecContext = nullptr;

//    auto codec = avcodec_find_encoder_by_name("x264");
//    res = avcodec_open2(codecContext, codec, nullptr);
//    if (res < 0) {
////        av_log(nullptr, AV_LOG_ERROR, "%s", av_err2str(res));
////        return;
//    }
    auto params = ctx->streams[0]->codecpar;
    auto bufferSize = av_image_get_buffer_size((AVPixelFormat) (params->format), params->width, params->height, 1);
    auto passthroughDecoder = avcodec_find_decoder(AV_CODEC_ID_WRAPPED_AVFRAME);
    auto passthroughDecoderCtx = avcodec_alloc_context3(passthroughDecoder);
    std::ofstream file("./record_video.yuv");
    auto pkt = av_packet_alloc();
    auto frame = av_frame_alloc();
    auto count = 60;
    while (true) {
        auto ret = av_read_frame(ctx, pkt);
        if (ret == 0) {
            unwrap_packet(pkt, frame);
            res = avcodec_send_frame(codecContext, frame);
            avcodec_receive_packet(codecContext, pkt);
            if (res < 0) {
                av_log(nullptr, AV_LOG_ERROR, "%s", av_err2str(res));
                return;
            }
            av_packet_unref(pkt);
        } else if (ret == AVERROR(EAGAIN)) {
            continue;
        } else {
            break;
        }
        if (count-- <= 0) {
            break;
        }

    }
    av_packet_free(&pkt);
    file.close();
    avformat_close_input(&ctx);
}

void FFmpeg::test_flush() {
    auto url_list = vector<string>{
            "http://bmi-union-test.oss-cn-beijing.aliyuncs.com/1080p_2S_one0.ts",
            "http://bmi-union-test.oss-cn-beijing.aliyuncs.com/1080p_2S_one1.ts",
            "http://bmi-union-test.oss-cn-beijing.aliyuncs.com/1080p_2S_one2.ts",
            "http://bmi-union-test.oss-cn-beijing.aliyuncs.com/1080p_2S_one3.ts",
            "http://bmi-union-test.oss-cn-beijing.aliyuncs.com/1080p_2S_one4.ts",
    };
    av_log_set_level(AV_LOG_DEBUG);
    auto ctx = avformat_alloc_context();
    for (auto &url: url_list) {
        auto ret = avformat_open_input(&ctx, url.c_str(), nullptr, nullptr);
        if (ret < 0) {
            av_log(nullptr, AV_LOG_ERROR, "%s\n", av_err2str(ret));
            return;
        }
        auto pkt = av_packet_alloc();
        while (1) {
            ret = av_read_frame(ctx, pkt);
            if (ret == AVERROR(EAGAIN)) {
                continue;
            }
            if (ret == AVERROR_EOF) {
                av_log(nullptr, AV_LOG_INFO, "ts file end of file\n");
                break;
            }
            av_log(nullptr, AV_LOG_INFO, "pkt size:%d pkt pts:%d pkt duration:%d\n", pkt->size, pkt->pts,
                   pkt->duration);
        }
        av_packet_unref(pkt);
        av_packet_free(&pkt);
        avio_flush(ctx->pb);
        ret = avformat_flush(ctx);
        av_log(nullptr, AV_LOG_INFO, "flush ret:%d\n", ret);
        if (ret < 0) {
            av_log(nullptr, AV_LOG_ERROR, "%s\n", av_err2str(ret));
        }
    }
}


static int unwrap_packet(AVPacket *source, AVFrame *dest) {
    static bool opened = false;
    static auto passthroughDecoder = avcodec_find_decoder(AV_CODEC_ID_WRAPPED_AVFRAME);
    static auto passthroughDecoderCtx = avcodec_alloc_context3(passthroughDecoder);
    if (!opened) {
        opened = true;
        auto result = avcodec_open2(passthroughDecoderCtx, passthroughDecoder, nullptr);
        if (result < 0) {
            av_log(nullptr, AV_LOG_ERROR, "%s", av_err2str(result));
            return result;
        }
    }
    auto result = avcodec_receive_packet(passthroughDecoderCtx, source);
    if (result < 0) {
        av_log(nullptr, AV_LOG_ERROR, "%s", av_err2str(result));
        return result;
    }
    avcodec_receive_frame(passthroughDecoderCtx, dest);
    if (result < 0) {
        av_log(nullptr, AV_LOG_ERROR, "%s", av_err2str(result));
        return result;
    }
}