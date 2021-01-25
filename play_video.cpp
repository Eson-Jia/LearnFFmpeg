extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <libavutil/avutil.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_audio.h>
}

#include <iostream>
#include <list>
#include <mutex>
#include <thread>
#include <functional>
#include <condition_variable>

#define MAX_AUDIO_FRAME_SIZE 192000
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
        condition.notify_one();
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
        condition.notify_one();
    }
};

class VideoInfo {
public:
    VideoInfo() {
        videoIndex = -1;
        audioIndex = -1;
    };
    List<AVPacket *> videoPacketList;

    int videoIndex;
    AVCodecContext *videoCodecContext;
    AVCodec *videoCodec;
    List<AVFrame *> videoFrameList;
    shared_ptr<thread> decodeVideoThread;

    int audioIndex;
    AVCodecContext *audioCodecContext;
    AVCodec *audioCodec;
    SwrContext *resampleContext;
    List<AVPacket *> audioPacketList;
    List<AVFrame *> audioFrameList;
    shared_ptr<thread> decodeAudioThread;
};

void error_out(string msg) {
    cerr << msg << endl;
    exit(1);
}

//void audio_callback(void *userdata, Uint8 *stream, int len) {
//    auto videoInfo = static_cast<VideoInfo *>(userdata);
//    cout<<"callback len: "<<len<<endl;
//    auto frame = videoInfo->audioFrameList.list_get();
//    if(frame!= nullptr){
//        int convert_ret = swr_convert(videoInfo->resampleContext,
//                                      &stream,
//                                      frame->nb_samples,
//                                      (const uint8_t **) frame->data,
//                                      frame->nb_samples);
//        av_frame_unref(frame);
//        av_frame_free(&frame);
//        if(convert_ret<0){
//            cerr<<"failed in convert audio: "<<av_err2str(convert_ret)<<endl;
//        }
//    }
//}

int audio_decode_frame(VideoInfo *videoInfo, uint8_t *audio_buf, int buf_size) {
    static int ret = -1;
    auto frame = *videoInfo->audioFrameList.list_get();
    auto data_size = av_samples_get_buffer_size(nullptr, frame.channels, frame.nb_samples,
                                                videoInfo->audioCodecContext->sample_fmt, 0);
    int convert_ret = swr_convert(videoInfo->resampleContext,
                                  &audio_buf,
                                  frame.nb_samples,
                                  (const uint8_t **) frame.data,
                                  frame.nb_samples);
    if (convert_ret >= 0) {
        return data_size;
    }
    return 0;
}

void audio_callback(void *userdata, Uint8 *stream, int len) {
    auto videoInfo = static_cast<VideoInfo *>(userdata);
    cout << "callback len: " << len << endl;
    int len1, audio_size;

    static uint8_t audio_buf[(MAX_AUDIO_FRAME_SIZE * 3) / 2];
    static unsigned int audio_buf_size = 0;
    static unsigned int audio_buf_index = 0;

    while (len > 0) {
        if (audio_buf_index >= audio_buf_size) {
            /* We have already sent all our data; get more */
            audio_size = audio_decode_frame(videoInfo, audio_buf, sizeof(audio_buf));
            if (audio_size < 0) {
                /* If error, output silence */
                audio_buf_size = 1024; // arbitrary?
                memset(audio_buf, 0, audio_buf_size);
            } else {
                audio_buf_size = audio_size;
            }
            audio_buf_index = 0;
        }
        len1 = audio_buf_size - audio_buf_index;
        if (len1 > len)
            len1 = len;
        memcpy(stream, (uint8_t *) audio_buf + audio_buf_index, len1);
        cout << "memcpy: " << len1 << endl;
        len -= len1;
        stream += len1;
        audio_buf_index += len1;
    }
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
                        if (videoInfo->audioIndex != -1 || i != 2)
                            continue;
                        videoInfo->audioIndex = i;
                        videoInfo->audioCodec = codec;
                        videoInfo->audioCodecContext = codecContext;
                        videoInfo->resampleContext = swr_alloc_set_opts(
                                nullptr,
                                codecContext->channel_layout,
                                AV_SAMPLE_FMT_FLT,
                                codecContext->sample_rate,
                                codecContext->channel_layout,
                                codecContext->sample_fmt,
                                codecContext->sample_rate,
                                1,
                                nullptr);
                        if (swr_init(videoInfo->resampleContext) < 0) {
                            error_out("failed in init swr");
                        }
                        videoInfo->decodeAudioThread = make_shared<thread>([](VideoInfo *videoInfo) -> void {
                            while (true) {
                                auto packet = videoInfo->audioPacketList.list_get();
                                if (packet == nullptr) {
                                    break;
                                }
                                avcodec_send_packet(videoInfo->audioCodecContext, packet);
                                int ret;
                                do {
                                    auto frame = av_frame_alloc();
                                    ret = avcodec_receive_frame(videoInfo->audioCodecContext, frame);
                                    if (ret == AVERROR_EOF ||
                                        AVERROR(EAGAIN) == ret) {
                                        break;
                                    }
                                    if (ret < 0) {
                                        error_out("decode audio:failed in receive frame");
                                    }
                                    videoInfo->audioFrameList.list_limit_push(frame, 10, []() -> int {
                                        cout << "audio frame is full, before wait" << endl;
                                        return 0;
                                    }, []() -> int {
                                        cout << "audio frame is full, after wait" << endl;
                                        return 0;
                                    });
                                } while (ret == 0);
                            }
                        }, videoInfo);
                        break;
                    case AVMEDIA_TYPE_VIDEO:
                        if (videoInfo->videoIndex != -1)
                            continue;
                        videoInfo->videoIndex = i;
                        videoInfo->audioCodec = codec;
                        videoInfo->videoCodecContext = codecContext;
                        if (videoInfo->decodeVideoThread == nullptr) {
                            videoInfo->decodeVideoThread = make_shared<thread>([](VideoInfo *videoInfo) -> void {
                                while (true) {
                                    auto packet = videoInfo->videoPacketList.list_get();
                                    if (packet == nullptr) {
                                        break;
                                    }
                                    avcodec_send_packet(videoInfo->videoCodecContext, packet);
                                    int ret;
                                    do {
                                        auto frame = av_frame_alloc();
                                        ret = avcodec_receive_frame(videoInfo->videoCodecContext, frame);
                                        if (ret == AVERROR_EOF || AVERROR(EAGAIN) == ret) {
                                            break;
                                        }
                                        if (ret < 0) {
                                            error_out("decode video:failed in receive frame");
                                        }
//                                        videoInfo->videoFrameList.list_limit_push(frame,10,[]()->int{
//                                            return 0;
//                                        },[]()->int{
//                                            return 0;
//                                        });
                                    } while (ret == 0);
                                }
                                cerr << "video decode thread exit" << endl;
                            }, videoInfo);
                        }
                }
            }
        }
        while (true) {
            auto packet = av_packet_alloc();
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
                videoInfo->videoPacketList.list_limit_push(packet, 100, []() -> int {
                    cout << "video before wait" << endl;
                    return 0;
                }, []() -> int {
                    cout << "video after wait" << endl;
                    return 0;
                });
                cout << "" << endl;
            } else if (packet->stream_index == videoInfo->audioIndex) {
                videoInfo->audioPacketList.list_limit_push(packet, 100, []() -> int {
                    cout << "audio before wait" << endl;
                    return 0;
                }, []() -> int {
                    cout << "audio after wait" << endl;
                    return 0;
                });
                cout << "after push audio packet" << endl;
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
    wantSpec.freq = 48000;
    wantSpec.format = AUDIO_F32;
    wantSpec.channels = 2;
    wantSpec.silence = 0;
    wantSpec.samples = 4096;

    wantSpec.userdata = videoInfo;
    wantSpec.callback = audio_callback;
    if (SDL_OpenAudio(&wantSpec, &gotSpec) == -1) {
        error_out("failed in open audio");
    }
    SDL_PauseAudio(0);
    demuxerThread.join();
}




















