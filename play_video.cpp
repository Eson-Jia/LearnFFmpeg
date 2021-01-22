extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_audio.h>
}

#include <iostream>
#include <list>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <functional>

using namespace std;


template<class T>
class List {
public:
    List() {
    }

    list<T> theList;
    condition_variable condition;
    mutex conditionMutex;

    T list_get(bool block = true) {
        unique_lock<mutex> locker(conditionMutex);
        while (theList.empty() && block) {
            condition.wait(locker);
        }
        if (theList.empty()) {
            return nullptr;
        }
        auto last = theList.back();
        theList.pop_back();
        return last;
    }

    void list_push(T t) {
        unique_lock<mutex> locker(conditionMutex);
        theList.emplace_front(t);
        condition.notify_one();
    }

    void list_limit_push(T t, int max, function<int()> beforeWait, function<int()> afterWait) {
        unique_lock<mutex> locker(conditionMutex);
        auto first = true;
        while (theList.size() >= max) {
            if (first) {
                beforeWait();
                first = false;
            }
            condition.wait(locker);
        }
        theList.emplace_front(t);
        if (!first)
            afterWait();
    }
};

class VideoInfo {
public:
    VideoInfo() {
        videoIndex = 0;
        audioIndex = 0;
    };
    List<AVPacket *> videoPacketList;
    List<AVPacket *> audioPacketList;
    int videoIndex;
    AVCodecContext *videoCodecContext;
    AVCodec *videoCodec;
    List<AVFrame *> videoFrameList;
    int audioIndex;
    AVCodecContext *audioCodecContext;
    AVCodec *audioCodec;
    SwrContext *resampleContext;
    shared_ptr<thread> decodeVideoThread;

};

void error_out(string msg) {
    cerr << msg << endl;
    exit(1);
}

void audio_callback(void *userdata, Uint8 *stream, int len) {
    auto videoInfo = static_cast<VideoInfo *>(userdata);
    while (true) {
        auto packet = videoInfo->audioPacketList.list_get();
    };
}

int main(int argc, char **argv) {
    AVFormatContext *formatContext = nullptr;
    auto ret = avformat_open_input(&formatContext, argv[1], nullptr, nullptr);
    if (ret < 0) {
        error_out("failed in open input");
    }
    if (avformat_find_stream_info(formatContext, nullptr) < 0) {
        error_out("failed in find stream info");
    }
    av_dump_format(formatContext, 0, argv[1], 0);
    auto videoInfo = new VideoInfo();
    thread demuxerThread([&formatContext, videoInfo]() -> void {
        int ret = -1;
        for (int i = 0; i < formatContext->nb_streams; ++i) {
            auto codecPar = formatContext->streams[i]->codecpar;
            if (codecPar->codec_type == AVMEDIA_TYPE_AUDIO ||
                codecPar->codec_type == AVMEDIA_TYPE_VIDEO) {
                auto codecContext = avcodec_alloc_context3(nullptr);
                avcodec_parameters_to_context(codecContext, codecPar);
                auto codec = avcodec_find_decoder(codecPar->codec_id);
                if (codec == nullptr) {
                    error_out("failed in find decoder");
                }
                if (avcodec_open2(codecContext, codec, nullptr) < 0) {
                    error_out("failed in open avcodec");
                }
                switch (codecPar->codec_type) {
                    case AVMEDIA_TYPE_AUDIO:
                        videoInfo->audioIndex = i;
                        videoInfo->audioCodec = codec;
                        videoInfo->audioCodecContext = codecContext;
                        videoInfo->resampleContext = swr_alloc_set_opts(
                                nullptr,
                                codecContext->channel_layout,
                                AV_SAMPLE_FMT_FLT,
                                44100,
                                codecContext->channel_layout,
                                codecContext->sample_fmt,
                                codecContext->sample_rate,
                                1,
                                nullptr);
                        break;
                    case AVMEDIA_TYPE_VIDEO:
                        videoInfo->videoIndex = i;
                        videoInfo->audioCodec = codec;
                        videoInfo->videoCodecContext = codecContext;
                        videoInfo->decodeVideoThread = make_shared<thread>([](VideoInfo *videoInfo) -> void {
                            auto frame = av_frame_alloc();
                            while (true) {
                                auto packet = videoInfo->videoPacketList.list_get();
                                if (packet == nullptr) {
                                    break;
                                }
                                avcodec_send_packet(videoInfo->videoCodecContext, packet);
                                int ret;
                                do {
                                    ret = avcodec_receive_frame(videoInfo->videoCodecContext, frame);
                                    if (ret == AVERROR_EOF || AVERROR(EAGAIN) == ret) {
                                        break;
                                    }
                                    if (ret < 0) {
                                        error_out("decode video:failed in receive frame");
                                    }
                                    videoInfo->videoFrameList.list_push(frame);
                                } while (ret == 0);
                            }
                        }, videoInfo);
                }
            }
        }
        auto packet = av_packet_alloc();
        while (true) {
            ret = av_read_frame(formatContext, packet);
            if (ret == AVERROR(EAGAIN) ||
                ret == AVERROR_EOF) {
                break;
            }
            if (ret < 0) {
                error_out("failed in read packet");
                return;
            }
            if (packet->stream_index == videoInfo->videoIndex) {
                videoInfo->videoPacketList.list_push(packet);
            } else if (packet->stream_index == videoInfo->audioIndex) {
                videoInfo->audioPacketList.list_limit_push(packet, 100, [&formatContext]() -> int {
                    cout<<"before wait"<<endl;
                    return 0;
                }, [&formatContext]() -> int {
                    cout<<"after wait"<<endl;
                    return 0;
                });
            } else {
                cerr << "" << endl;
                av_packet_unref(packet);
            }
        }
    });
    if (SDL_Init(SDL_INIT_AUDIO | SDL_INIT_EVENTS | SDL_INIT_VIDEO | SDL_INIT_TIMER) < 0) {
        error_out("failed in init sdl");
    }
    SDL_AudioSpec wantSpec, gotSpec;
    wantSpec.channels = 2;
    wantSpec.freq = 44100;
    wantSpec.format = AUDIO_F32;
    wantSpec.silence = 0;
    wantSpec.size = 1024;
    wantSpec.userdata = &videoInfo;
    wantSpec.callback = audio_callback;
    if (SDL_OpenAudio(&wantSpec, &gotSpec) == -1) {
        error_out("failed in open audio");
    }
    demuxerThread.join();
//    thread save_picture([](VideoInfo *videoInfo) -> void {
//
//    }, videoInfo)
}




















