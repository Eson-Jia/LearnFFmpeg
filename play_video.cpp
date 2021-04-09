extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <libavutil/avutil.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_audio.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersrc.h>
#include <libavfilter/buffersink.h>
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
#define FRAME_RING_QUEUE_MAX_SIZE 1
#define MAX_AUDIO_FRAME_SIZE 192000

using namespace std;

class FrameWithClock {
public:
    FrameWithClock() {
        frame = nullptr;
        clock = 0;
    }

    AVFrame *frame;
    double clock;
};

class PacketQueue {
public:
    PacketQueue() {
        this->first_packet = nullptr;
        this->last_packet = nullptr;
        this->nb_packets = 0;
        this->size = 0;
        this->mutex = SDL_CreateMutex();
        this->cond = SDL_CreateCond();
    }

    int get(AVPacket *packet, bool block) {
        auto ret = 0;
        SDL_LockMutex(this->mutex);
        while (true) {
            auto pkt = this->first_packet;
            if (pkt != nullptr) {
                this->first_packet = pkt->next;
                if (this->first_packet == nullptr) {
                    this->last_packet = nullptr;
                }
                this->nb_packets--;
                this->size -= pkt->pkt.size;
                *packet = pkt->pkt;
                av_free(pkt);
                ret = 1;
                break;
            } else if (block) {
                SDL_CondWait(this->cond, this->mutex);
            } else {
                break;
            }
        }
        SDL_UnlockMutex(this->mutex);
        return ret;
    }

/**
 *
 * @param pkt
 * @return
 */
    int put(AVPacket *pkt) {
        /**
         * 这个链表是需要加锁的,因为空链表读取操作会阻塞
         * 问题:操作 std::list 的时候是否需要加锁?
         */
        AVPacketList *current = static_cast<AVPacketList *>(av_malloc(sizeof(AVPacketList)));
        if (current == nullptr)
            return -1;
        current->pkt = *pkt;
        current->next = nullptr;

        SDL_LockMutex(this->mutex);
        if (this->last_packet == nullptr)
            this->first_packet = current;
        else
            this->last_packet->next = current;
        this->last_packet = current;
        this->nb_packets++;
        this->size += pkt->size;
        SDL_CondSignal(this->cond);
        SDL_UnlockMutex(this->mutex);
    }

private:
    AVPacketList *first_packet, *last_packet;
    int nb_packets;
    int size;
    SDL_mutex *mutex;
    SDL_cond *cond;
};


class VideoInfo {
public:
    VideoInfo() {
        videoIndex = -1;
        audioIndex = -1;
        quit = false;
        ringQMutex = SDL_CreateMutex();
        ringQCond = SDL_CreateCond();
        ringQSize = 0;
        ringQWriteIndex = 0;
        ringQReadIndex = 0;
        videoClock = 0;
        audioClock = 0;
    };
    AVFormatContext *formatContext;
    PacketQueue videoPacketList;
    int videoIndex;
    AVCodecContext *videoCodecContext;
    AVCodec *videoCodec;

    FrameWithClock frameRingQ[FRAME_RING_QUEUE_MAX_SIZE];
    SDL_mutex *ringQMutex;
    SDL_cond *ringQCond;
    int ringQSize;
    int ringQReadIndex;
    int ringQWriteIndex;

    shared_ptr<thread> decodeVideoThread;
    SwsContext *swsContext;
    AVFrame *YUVFrame;
    uint8_t *YUVOutBuffer;
    SDL_Renderer *renderer;
    SDL_Texture *texture;

    //filter
    AVFilterContext *bufferSrcFilterCtx;
    AVFilterContext *bufferSinkFilterCtx;
    string filterDescription;
    double videoClock;

    int audioIndex;
    AVCodecContext *audioCodecContext;
    AVCodec *audioCodec;
    SwrContext *resampleContext;
    PacketQueue audioPacketList;
    double audioClock;
    uint8_t audioBuffer[MAX_AUDIO_FRAME_SIZE * 3 / 2];
    unsigned int audioBufferSize;
    unsigned int audioBufferIndex;

    bool quit;
    bool audioNeedSendPacket;
};

void queue_frame(VideoInfo *videoInfo, AVFrame *frame, double pts_clock) {
    SDL_LockMutex(videoInfo->ringQMutex);
    while (videoInfo->ringQSize == FRAME_RING_QUEUE_MAX_SIZE) {
        SDL_CondWait(videoInfo->ringQCond, videoInfo->ringQMutex);
    }
    SDL_UnlockMutex(videoInfo->ringQMutex);
    auto ptsFrame = &videoInfo->frameRingQ[videoInfo->ringQWriteIndex];
    /**
     * 内存问题:谁申请谁释放
     * 这里如何进行内存管理
     */
    ptsFrame->frame = frame;
    ptsFrame->clock = pts_clock;
    if (++videoInfo->ringQWriteIndex == FRAME_RING_QUEUE_MAX_SIZE) {
        videoInfo->ringQWriteIndex = 0;
    }
    SDL_LockMutex(videoInfo->ringQMutex);
    videoInfo->ringQSize++;
    SDL_UnlockMutex(videoInfo->ringQMutex);
}

double syncing_video(VideoInfo *videoInfo, AVFrame *frame, double clock) {
    if (clock > 0) {
        videoInfo->videoClock = clock;
    } else {
        clock = videoInfo->videoClock;
    }
    auto delay = av_q2d(videoInfo->videoCodecContext->time_base);
    // extra delay  = repeat_pict / (2*fps) = repeat_pict * time_base * 0.5;
    videoInfo->videoClock += (frame->repeat_pict * delay * 0.5 + delay);
    return clock;
}

void error_out(string msg) {
    cerr << msg << endl;
    exit(1);
}

int init_filter(VideoInfo *videoInfo) {
    const AVFilter *bufferFilter = avfilter_get_by_name("buffer");
    const AVFilter *bufferSinkFilter = avfilter_get_by_name("buffersink");
    AVFilterInOut *inputs = avfilter_inout_alloc();
    AVFilterInOut *outputs = avfilter_inout_alloc();
    auto graph = avfilter_graph_alloc();
    AVRational videoTimeBase = videoInfo->formatContext->streams[videoInfo->videoIndex]->time_base;
    char args[512];
    snprintf(args, 512, "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
             videoInfo->videoCodecContext->width,
             videoInfo->videoCodecContext->height,
             AV_PIX_FMT_YUV420P,
             videoTimeBase.num,
             videoTimeBase.den,
             videoInfo->videoCodecContext->sample_aspect_ratio.num,
             videoInfo->videoCodecContext->sample_aspect_ratio.den);
    avfilter_graph_create_filter(&videoInfo->bufferSrcFilterCtx, bufferFilter, "in", args, nullptr, graph);
    avfilter_graph_create_filter(&videoInfo->bufferSinkFilterCtx, bufferSinkFilter, "out", "", nullptr, graph);

    outputs->filter_ctx = videoInfo->bufferSrcFilterCtx;
    outputs->name = av_strdup("in");
    outputs->next = nullptr;
    outputs->pad_idx = 0;

    inputs->filter_ctx = videoInfo->bufferSinkFilterCtx;
    inputs->name = av_strdup("out");
    outputs->next = nullptr;
    outputs->pad_idx = 0;

    auto ret = avfilter_graph_parse_ptr(graph, videoInfo->filterDescription.c_str(), &inputs, &outputs, nullptr);
    if (ret < 0) {
        goto end;
    }
    ret = avfilter_graph_config(graph, nullptr);
    if (ret < 0)
        goto end;
    end:
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);
    return ret;
}

int audio_decode_frame(VideoInfo *videoInfo, uint8_t *audio_buf, int buf_size) {
    int ret;
    for (;;) {
        while (!videoInfo->audioNeedSendPacket) {
            auto frame = av_frame_alloc();
            ret = avcodec_receive_frame(videoInfo->audioCodecContext, frame);
            videoInfo->audioClock = av_q2d(videoInfo->audioCodecContext->time_base) * frame->pts;
            if (ret == AVERROR_EOF ||
                ret == AVERROR(EAGAIN)) {
                videoInfo->audioNeedSendPacket = true;
                break;
            }
            ret = swr_convert(videoInfo->resampleContext,
                              &audio_buf,
                              frame->nb_samples,
                              (const uint8_t **) frame->data,
                              frame->nb_samples);
            if (ret < 0)
                return ret;
            auto convertedSize = av_samples_get_buffer_size(nullptr, frame->channels, ret,
                                                            videoInfo->audioCodecContext->sample_fmt, 0);
            av_frame_free(&frame);
            return convertedSize;
        }
        auto packet = av_packet_alloc();
        videoInfo->audioPacketList.get(packet, true);
        if (packet->pts != AV_NOPTS_VALUE)
            videoInfo->audioClock = packet->pts * av_q2d(videoInfo->audioCodecContext->time_base);
        ret = avcodec_send_packet(videoInfo->audioCodecContext, packet);
        av_packet_free(&packet);
        if (ret < 0)
            return ret;
        videoInfo->audioNeedSendPacket = false;
    }
}

double get_audio_clock(VideoInfo *videoInfo) {
    auto audioClock = videoInfo->audioClock;
    auto leftBufferSize = videoInfo->audioBufferSize - videoInfo->audioBufferIndex;
    auto n = videoInfo->audioCodecContext->channels * 4;
    auto bytesPerSecond = 0;
    if (videoInfo->audioCodecContext) {
        bytesPerSecond = videoInfo->audioCodecContext->sample_rate * n;
    }
    if (bytesPerSecond) {
        audioClock -= (double) leftBufferSize / bytesPerSecond;
    }
    return audioClock;
}

void audio_callback(void *userdata, Uint8 *stream, int len) {
    auto videoInfo = static_cast<VideoInfo *>(userdata);
    int len1, audio_size;
    while (len > 0) {
        if (videoInfo->audioBufferIndex >= videoInfo->audioBufferSize) {
            /* We have already sent all our data; get more */
            audio_size = audio_decode_frame(videoInfo, videoInfo->audioBuffer, sizeof(videoInfo->audioBuffer));
            if (audio_size < 0) {
                /* If error, output silence */
                videoInfo->audioBufferSize = 1024; // arbitrary?
                memset(videoInfo->audioBuffer, 0, videoInfo->audioBufferSize);
            } else {
                videoInfo->audioBufferSize = audio_size;
            }
            videoInfo->audioBufferIndex = 0;
        }
        len1 = videoInfo->audioBufferSize - videoInfo->audioBufferIndex;
        if (len1 > len)
            len1 = len;
        memcpy(stream, (uint8_t *) videoInfo->audioBuffer + videoInfo->audioBufferIndex, len1);
        len -= len1;
        stream += len1;
        videoInfo->audioBufferIndex += len1;
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
    SDL_LockMutex(videoInfo->ringQMutex);
    auto frame = &videoInfo->frameRingQ[videoInfo->ringQReadIndex];
    sws_scale(videoInfo->swsContext,
              frame->frame->data,
              frame->frame->linesize,
              0,
              videoInfo->videoCodecContext->height,
              videoInfo->YUVFrame->data,
              videoInfo->YUVFrame->linesize);
    SDL_UpdateTexture(videoInfo->texture, nullptr, videoInfo->YUVFrame->data[0], videoInfo->YUVFrame->linesize[0]);
    SDL_RenderClear(videoInfo->renderer);
    SDL_RenderCopy(videoInfo->renderer, videoInfo->texture, nullptr, nullptr);
    SDL_RenderPresent(videoInfo->renderer);
//    av_frame_unref(frame); 不需要再调用这句话
    av_frame_free(&frame->frame);
    SDL_UnlockMutex(videoInfo->ringQMutex);
}

void videoRefreshTimer(void *data) {
    auto videoInfo = static_cast<VideoInfo *>(data);
    if (videoInfo->videoCodecContext != nullptr) {
        if (videoInfo->ringQSize > 0) {
            auto PTSFrame = &videoInfo->frameRingQ[videoInfo->ringQReadIndex];
//            auto delay = PTSFrame->clock - videoInfo.audioClock;
            scheduleRefresh(videoInfo, 40);
            showFrame(videoInfo);
            if (++videoInfo->ringQReadIndex == FRAME_RING_QUEUE_MAX_SIZE) {
                videoInfo->ringQReadIndex = 0;
                SDL_LockMutex(videoInfo->ringQMutex);
                videoInfo->ringQSize--;
                SDL_CondSignal(videoInfo->ringQCond);
                SDL_UnlockMutex(videoInfo->ringQMutex);
            }
        } else {
            scheduleRefresh(videoInfo, 10);
        }
    } else {
        scheduleRefresh(videoInfo, 100);
    }
}

void decodeVideo(VideoInfo *videoInfo) {
    while (!videoInfo->quit) {
        auto packet = av_packet_alloc();
        videoInfo->videoPacketList.get(packet, true);
        avcodec_send_packet(videoInfo->videoCodecContext, packet);
        int ret;
        AVFrame frame;
        do {
            ret = avcodec_receive_frame(videoInfo->videoCodecContext, &frame);
            if (ret == AVERROR_EOF || AVERROR(EAGAIN) == ret) {
                break;
            }
            if (ret < 0) {
                error_out("decode video:failed in receive frame");
            }
            ret = av_buffersrc_add_frame_flags(videoInfo->bufferSrcFilterCtx, &frame, AV_BUFFERSRC_FLAG_KEEP_REF);
            if (ret < 0) {
                error_out("failed in buffersrc add frame");
            }
            auto filteredFrame = av_frame_alloc();
            do {
                ret = av_buffersink_get_frame(videoInfo->bufferSinkFilterCtx, filteredFrame);
                if (AVERROR(EAGAIN) == ret || AVERROR_EOF == ret) {
                    break;
                }
                if (ret < 0) {
                    error_out("failed iin buffer sink get frame");
                }
                double framePTSClock =
                        av_q2d(videoInfo->videoCodecContext->time_base) * filteredFrame->best_effort_timestamp;
                auto ptsClock = syncing_video(videoInfo, filteredFrame, framePTSClock);
                queue_frame(videoInfo, filteredFrame, ptsClock);
            } while (ret == 0);
        } while (ret == 0);
        av_packet_free(&packet);
    }
    cerr << "video decode thread exit" << endl;
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
                    init_filter(videoInfo);

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
            videoInfo->videoPacketList.put(packet);
        } else if (packet->stream_index == videoInfo->audioIndex) {
            videoInfo->audioPacketList.put(packet);
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
    if (argc < 3) {
        error_out("malformed parameter");
    }
    auto ret = avformat_open_input(&formatContext, argv[1], nullptr, nullptr);
    if (ret < 0) {
        error_out("failed in open input");
    }
    if (avformat_find_stream_info(formatContext, nullptr) < 0) {
        error_out("failed in find stream info");
    }
    av_dump_format(formatContext, 0, argv[1], 0);
    auto videoInfo = new VideoInfo();
    videoInfo->filterDescription = string(argv[2]);
    videoInfo->formatContext = formatContext;
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
}
