extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <libavutil/avutil.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_audio.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
}

#include <iostream>
#include <list>
#include <mutex>
#include <thread>
#include <functional>
#include <condition_variable>
#include <chrono>

#define MAX_AUDIO_FRAME_SIZE 192000
#define FF_REFRESH_EVENT (SDL_USEREVENT)
#define  FF_QUIT_EVENT SDL_USEREVENT+1
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

    bool empty() {
        unique_lock<mutex> locker(conditionMutex);
        return theList.empty();
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
        quit = false;
    };
    List<AVPacket *> videoPacketList;

    int videoIndex;
    AVCodecContext *videoCodecContext;
    AVCodec *videoCodec;
    List<AVFrame *> videoFrameList;
    shared_ptr<thread> decodeVideoThread;
    SwsContext *swsContext;
    AVFrame *YUVFrame;
    uint8_t *YUVOutBuffer;
    SDL_Renderer *renderer;
    SDL_Texture *texture;

    int audioIndex;
    AVCodecContext *audioCodecContext;
    AVCodec *audioCodec;
    SwrContext *resampleContext;
    List<AVPacket *> audioPacketList;
    List<AVFrame *> audioFrameList;
    shared_ptr<thread> decodeAudioThread;
    bool quit;
};

void error_out(string msg) {
    cerr << msg << endl;
    exit(1);
}

int audio_decode_frame(VideoInfo *videoInfo, uint8_t *audio_buf, int buf_size) {
    static int ret = -1;
    auto frame = videoInfo->audioFrameList.list_get();
    auto data_size = av_samples_get_buffer_size(nullptr, frame->channels, frame->nb_samples,
                                                videoInfo->audioCodecContext->sample_fmt, 0);
    int convert_ret = swr_convert(videoInfo->resampleContext,
                                  &audio_buf,
                                  frame->nb_samples,
                                  (const uint8_t **) frame->data,
                                  frame->nb_samples);
//    av_frame_unref(frame); no need
    av_frame_free(&frame);
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

// SDL_AddTimer 的回调函数的传入参数是调用 SDL_AddTimer 时的参数:timer interval,用户定义的参数,返回值是下一个 timer interval.
// 如果返回值是 0 的话,这个 timer 就会被取消.
// 回调函数运行在一个单独的线程.
// Timer 回调函数的执行执行也会被计入下次迭代的总时间中,例如:如果回调函数执行了`250ms`然后返回了 1000(ms),那么 timer 在下一次迭代之前只会再等 750(ms).
// 参考 https://wiki.libsdl.org/SDL_AddTimer
Uint32 freshTimerCallback(Uint32 interval, void *opaque) {
    SDL_Event event;
    event.type = FF_REFRESH_EVENT;
    event.user.data1 = opaque;
    SDL_PushEvent(&event);
    return 0;
};

void scheduleRefresh(VideoInfo *videoInfo, int delay) {
    SDL_AddTimer(delay, freshTimerCallback, videoInfo);
};

void showFrame(VideoInfo *videoInfo) {
    auto frame = videoInfo->videoFrameList.list_get();
    sws_scale(videoInfo->swsContext,
              frame->data,
              frame->linesize,
              0,
              videoInfo->videoCodecContext->height,
              videoInfo->YUVFrame->data,
              videoInfo->YUVFrame->linesize);
    SDL_UpdateTexture(videoInfo->texture, nullptr, videoInfo->YUVFrame->data[0], videoInfo->YUVFrame->linesize[0]);
    SDL_RenderClear(videoInfo->renderer);
    SDL_RenderCopy(videoInfo->renderer, videoInfo->texture, nullptr, nullptr);
    SDL_RenderPresent(videoInfo->renderer);
//    av_frame_unref(frame); 不需要再调用这句话
    av_frame_free(&frame);
}

void videoRefreshTimer(void *data) {
    auto videoInfo = static_cast<VideoInfo *>(data);
    if (videoInfo->videoCodecContext != nullptr) {
        if (!videoInfo->videoFrameList.empty()) {
            scheduleRefresh(videoInfo, 40);
            showFrame(videoInfo);
        } else {
            scheduleRefresh(videoInfo, 10);
        }
    } else {
        scheduleRefresh(videoInfo, 100);
    }
}

void decodeVideo(VideoInfo *videoInfo) {
    while (!videoInfo->quit) {
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
            videoInfo->videoFrameList.list_limit_push(frame, 100, []() -> int {
                cout << "video frame is full, before wait" << endl;
            }, []() -> int {
                cout << "video frame is full, after wait" << endl;
            });
        } while (ret == 0);
        av_packet_free(&packet);
    }
    cerr << "video decode thread exit" << endl;
}

void decodeAudio(VideoInfo *videoInfo) {
    while (!videoInfo->quit) {
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
            videoInfo->audioFrameList.list_limit_push(frame, 100, []() -> int {
                cout << "audio frame is full, before wait" << endl;
                return 0;
            }, []() -> int {
                cout << "audio frame is full, after wait" << endl;
                return 0;
            });
        } while (ret == 0);
        av_packet_free(&packet);
    }
}

void demuxerFunction(AVFormatContext *formatContext, VideoInfo *videoInfo) {
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
                    if (videoInfo->audioIndex != -1)
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
                    videoInfo->decodeAudioThread = make_shared<thread>(decodeAudio, videoInfo);
                    break;
                case AVMEDIA_TYPE_VIDEO:
                    if (videoInfo->videoIndex != -1)
                        continue;
                    videoInfo->videoIndex = i;
                    videoInfo->videoCodec = codec;
                    videoInfo->videoCodecContext = codecContext;
                    if (videoInfo->decodeVideoThread == nullptr) {
                        videoInfo->decodeVideoThread = make_shared<thread>(decodeVideo, videoInfo);
                    }
                    videoInfo->swsContext = sws_getContext(
                            //src
                            codecContext->width,
                            codecContext->height,
                            codecContext->pix_fmt,
                            //dest
                            codecContext->width,
                            codecContext->height,
                            AV_PIX_FMT_YUV420P,
                            0,
                            nullptr,
                            nullptr,
                            nullptr);
                    videoInfo->YUVFrame = av_frame_alloc();
                    videoInfo->YUVOutBuffer = (uint8_t *) av_malloc(av_image_get_buffer_size(
                            AV_PIX_FMT_YUV420P,
                            codecContext->width,
                            codecContext->height,
                            1));
                    if (av_image_fill_arrays(
                            videoInfo->YUVFrame->data,
                            videoInfo->YUVFrame->linesize,
                            videoInfo->YUVOutBuffer,
                            AV_PIX_FMT_YUV420P,
                            codecContext->width,
                            codecContext->height,
                            1) < 0) {
                        error_out("failed in fill arrays");
                    }

            }
        }
    }
    while (!videoInfo->quit) {
        auto packet = av_packet_alloc();
        ret = av_read_frame(formatContext, packet);
        if (ret == AVERROR(EAGAIN) ||
            ret == AVERROR_EOF) {
            SDL_Delay(10);
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
//            cout << "after push video packet" << endl;
        } else if (packet->stream_index == videoInfo->audioIndex) {
            videoInfo->audioPacketList.list_limit_push(packet, 100, []() -> int {
                cout << "audio before wait" << endl;
                return 0;
            }, []() -> int {
                cout << "audio after wait" << endl;
                return 0;
            });
//            cout << "after push audio packet" << endl;
        } else {
            cerr << "" << endl;
            // 解引用被 packet 引用的 buffer,并将成员变量重置为默认值
            // av_packet_unref(packet);
            // 释放 packet 内存,如果 packet 还在被引用计数的话,就先解引用,所以如果要调用 free 那么在其之前就没有必要调用 av_packet_unref
            av_packet_free(&packet);
        }
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
    thread demuxerThread(demuxerFunction, formatContext, videoInfo);
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
    auto windows = SDL_CreateWindow("main", 0, 0, 1080, 720, SDL_WINDOW_OPENGL);
    if (windows == nullptr) {
        error_out("failed in create windows");
    }
    auto render = SDL_CreateRenderer(windows, -1, 0);
    if (render == nullptr) {
        error_out("failed in create windows");
    }
    auto texture = SDL_CreateTexture(render, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING, 1080, 720);
    if (texture == nullptr) {
        error_out("failed in create texture");
    }
    videoInfo->renderer = render;
    videoInfo->texture = texture;
    SDL_Event event;
    scheduleRefresh(videoInfo, 40);
    while (true) {
        if (SDL_WaitEvent(&event) == 0) {
            error_out("failed in sdl wait event");
            break;
        }
        switch (event.type) {
            case FF_REFRESH_EVENT:
                videoRefreshTimer(event.user.data1);
                break;
            case FF_QUIT_EVENT:
            case SDL_QUIT:
                videoInfo->quit = true;
                SDL_Quit();
                return 0;
                break;
        }
    }
    demuxerThread.join();
    videoInfo->decodeVideoThread->join();
    videoInfo->decodeAudioThread->join();
}
