extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <SDL2/SDL_audio.h>
#include <SDL2/SDL.h>
}

#include <iostream>
#include <thread>

#define SDL_AUDIO_BUFFER_SIZE 4096
#define MAX_AUDIO_FRAME_SIZE 192000
#define SFM_REFRESH_EVENT  (SDL_USEREVENT + 1)
int quit = 0;
using namespace std;

typedef struct {
    AVPacketList *first_pkt, *last_pkt;
    int nb_packets;
    int size;
    SDL_mutex *mutex;
    SDL_cond *cond;
} PacketQueue;

PacketQueue packetQueue;
SwrContext *swrContext = nullptr;

void init_queue(PacketQueue *pqueue) {
    memset(pqueue, 0, sizeof(PacketQueue));
    packetQueue.mutex = SDL_CreateMutex();
    packetQueue.cond = SDL_CreateCond();
}

int packet_queue_put(PacketQueue *q, AVPacket *pkt) {

    AVPacketList *pkt1;
    if (av_dup_packet(pkt) < 0) {
        return -1;
    }
    pkt1 = static_cast<AVPacketList *>(av_malloc(sizeof(AVPacketList)));
    if (!pkt1)
        return -1;
    pkt1->pkt = *pkt;
    pkt1->next = NULL;


    SDL_LockMutex(q->mutex);

    if (!q->last_pkt)
        q->first_pkt = pkt1;
    else
        q->last_pkt->next = pkt1;
    q->last_pkt = pkt1;
    q->nb_packets++;
    q->size += pkt1->pkt.size;
    SDL_CondSignal(q->cond);

    SDL_UnlockMutex(q->mutex);
    return 0;
}

static int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block) {
    AVPacketList *pkt1;
    int ret;

    SDL_LockMutex(q->mutex);

    for (;;) {

        if (quit) {
            ret = -1;
            break;
        }

        pkt1 = q->first_pkt;
        if (pkt1) {
            q->first_pkt = pkt1->next;
            if (!q->first_pkt)
                q->last_pkt = NULL;
            q->nb_packets--;
            q->size -= pkt1->pkt.size;
            *pkt = pkt1->pkt;
            av_free(pkt1);
            ret = 1;
            break;
        } else if (!block) {
            ret = 0;
            break;
        } else {
            SDL_CondWait(q->cond, q->mutex);
        }
    }
    SDL_UnlockMutex(q->mutex);
    return ret;
}

int init_swrcontext(AVCodecContext *codecCtx, SwrContext **swrContext) {
    *swrContext = swr_alloc();
    //
    av_opt_set_int(*swrContext, "in_channel_layout", codecCtx->channel_layout, 0);
    av_opt_set_int(*swrContext, "out_channel_layout", codecCtx->channel_layout, 0);
    av_opt_set_int(*swrContext, "in_sample_rate", codecCtx->sample_rate, 0);
    av_opt_set_int(*swrContext, "out_sample_rate", codecCtx->sample_rate, 0);
    av_opt_set_sample_fmt(*swrContext, "in_sample_fmt", codecCtx->sample_fmt, 0);
    av_opt_set_sample_fmt(*swrContext, "out_sample_fmt", AV_SAMPLE_FMT_FLT, 0);
    return swr_init(*swrContext);
}

int audio_decode_frame(AVCodecContext *audioCtx, uint8_t *audio_buf, int buf_size) {
    static int ret = -1;
    auto packet = av_packet_alloc();
    static AVFrame frame;
    while (true) {
        while (ret >= 0) {
            ret = avcodec_receive_frame(audioCtx, &frame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                break;
            else if (ret < 0) {
                cerr << "Error during decoding" << endl;
                exit(1);
            }
            auto data_size = av_samples_get_buffer_size(nullptr, frame.channels, frame.nb_samples,
                                                        audioCtx->sample_fmt, 0);
            int convert_ret = swr_convert(swrContext,
                                          &audio_buf,
                                          frame.nb_samples,
                                          (const uint8_t **) frame.data,
                                          frame.nb_samples);
            if (convert_ret < 0) {
                cerr << "failed in convert" << endl;
                exit(1);
            }
            return data_size;
        }
        if (packet->data) {
            av_packet_unref(packet);
        }
        if (packet_queue_get(&packetQueue, packet, 1) < 0) {
            return -1;
        }
        ret = avcodec_send_packet(audioCtx, packet);
        if (ret < 0) {
            cerr << "failed in send packet error: " << AVERROR(ret) << endl;
            break;
        }
    }
}

void audio_callback(void *userdata, Uint8 *stream, int len) {
    AVCodecContext *aCodecCtx = (AVCodecContext *) userdata;
    int len1, audio_size;

    static uint8_t audio_buf[(MAX_AUDIO_FRAME_SIZE * 3) / 2];
    static unsigned int audio_buf_size = 0;
    static unsigned int audio_buf_index = 0;

    while (len > 0) {
        if (audio_buf_index >= audio_buf_size) {
            /* We have already sent all our data; get more */
            audio_size = audio_decode_frame(aCodecCtx, audio_buf, sizeof(audio_buf));
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
        len -= len1;
        stream += len1;
        audio_buf_index += len1;
    }
}

int main(int argc, char **argv) {
    AVFormatContext *formatCtx = nullptr;
    auto ret = avformat_open_input(&formatCtx, argv[1], nullptr, nullptr);
    if (ret < 0) {
        cerr << "failed in open input" << argv[1] << endl;
        exit(1);
    }
    ret = avformat_find_stream_info(formatCtx, nullptr);
    if (ret < 0) {
        cerr << "failed in find stream info" << endl;
        exit(1);
    }
    av_dump_format(formatCtx, 0, argv[1], 0);
    auto audioStreamIndex = -1, videoStreamIndex = -1;
    AVCodecParameters *audioParameters = avcodec_parameters_alloc();
    AVCodecParameters *videoParameters = avcodec_parameters_alloc();
    for (int i = 0; i < formatCtx->nb_streams; ++i) {
        if (formatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            audioStreamIndex = i;
            if (avcodec_parameters_copy(audioParameters, formatCtx->streams[i]->codecpar) < 0) {
                cerr << "failed in copy parameter" << endl;
                exit(1);
            }
        }
        if (formatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            videoStreamIndex = i;
            if (avcodec_parameters_copy(videoParameters, formatCtx->streams[i]->codecpar) < 0) {
                cerr << "failed in copy video parameter" << endl;
            }
        }
    }

    // audio codec context
    AVCodec *audioCodec = avcodec_find_decoder(audioParameters->codec_id);
    // 注意 avcodec_alloc_context3 和 avcodec_open2 可以在一个位置传入 audioCodec,但是如果两个位置都传入的话需要两个 codec 相等
    auto audioCodecCtx = avcodec_alloc_context3(nullptr);
    ret = avcodec_parameters_to_context(audioCodecCtx, audioParameters);
    if (ret < 0) {
        cerr << "failed in audio Parameters to context" << endl;
        exit(1);
    }
    ret = avcodec_open2(audioCodecCtx, audioCodec, nullptr);
    if (ret < 0) {
        cerr << "failed in codec open" << endl;
        exit(1);
    }

    // video codec context
    AVCodec *videoCodec = avcodec_find_decoder(videoParameters->codec_id);
    auto videoCodecCtx = avcodec_alloc_context3(nullptr);
    ret = avcodec_parameters_to_context(videoCodecCtx, videoParameters);
    if (ret < 0) {
        cerr << "failed in audio Parametes to context" << endl;
        exit(1);
    }
    ret = avcodec_open2(videoCodecCtx, videoCodec, nullptr);
    if (ret < 0) {
        cerr << "failed in video Parametes to context" << endl;
        exit(1);
    }

    ret = SDL_Init(SDL_INIT_AUDIO | SDL_INIT_TIMER | SDL_INIT_VIDEO);
    if (ret < 0) {
        cerr << "failed in sdl init" << endl;
        exit(1);
    }
    if (init_swrcontext(audioCodecCtx, &swrContext) < 0) {
        cerr << "failed in init swr context" << endl;
        exit(1);
    }
    SDL_AudioSpec audioSpec, spec;
    audioSpec.freq = audioCodecCtx->sample_rate;
    audioSpec.format = AUDIO_F32;
    audioSpec.channels = audioCodecCtx->channels;
    audioSpec.silence = 0;
    audioSpec.samples = SDL_AUDIO_BUFFER_SIZE;

    audioSpec.callback = audio_callback;
    audioSpec.userdata = audioCodecCtx;
    if (SDL_OpenAudio(&audioSpec, &spec) != 0) {
        cerr << "failed in open audio" << endl;
        exit(1);
    }
    SDL_PauseAudio(0);
    init_queue(&packetQueue);
    SDL_Event event;
    AVPacket packet;
    auto srcWith = videoCodecCtx->width;
    auto srcHeight = videoCodecCtx->height;
    auto image_convert_context = sws_getContext(
            srcWith,
            srcHeight,
            videoCodecCtx->pix_fmt,
            srcWith,
            srcHeight,
            AV_PIX_FMT_YUV420P,
            0,
            nullptr,
            nullptr,
            nullptr);
    auto originFrame = av_frame_alloc();
    auto YUVFrame = av_frame_alloc();
    auto outBUffer = (uint8_t *) av_malloc(av_image_get_buffer_size(AV_PIX_FMT_YUV420P, srcWith, srcHeight, 1));
    av_image_fill_arrays(YUVFrame->data, YUVFrame->linesize, outBUffer, AV_PIX_FMT_YUV420P, srcWith, srcHeight, 1);
    auto window = SDL_CreateWindow("player",
                                   SDL_WINDOWPOS_UNDEFINED,
                                   SDL_WINDOWPOS_UNDEFINED,
                                   srcWith,
                                   srcHeight,
                                   SDL_WINDOW_OPENGL);
    if (window == nullptr) {
        cerr << "failed in create window" << endl;
        exit(1);
    }
    auto render = SDL_CreateRenderer(
            window,
            -1,
            0);
    auto texture = SDL_CreateTexture(
            render,
            SDL_PIXELFORMAT_IYUV,
            SDL_TEXTUREACCESS_STREAMING,
            srcWith,
            srcHeight);
    while (true) {
        auto ret = av_read_frame(formatCtx, &packet);
        if (ret == AVERROR_EOF || ret == AVERROR(EAGAIN)) {
            cout << "read frame finished" << endl;
            this_thread::sleep_for(chrono::milliseconds(100));
            continue;
        }
        if (ret < 0) {
            cerr << "failed in read packet" << endl;
            break;
        }
        if (packet.stream_index == audioStreamIndex) {
            packet_queue_put(&packetQueue, &packet);
            continue;
        }
        if (packet.stream_index == videoStreamIndex) {
            auto result = avcodec_send_packet(videoCodecCtx, &packet);
            if (result == AVERROR_EOF || result == AVERROR(EAGAIN)) {
                break;
            }
            if (result < 0) {
                cerr << "failed in send packet" << endl;
                exit(1);
            }
            do {
                ret = avcodec_receive_frame(
                        videoCodecCtx,
                        originFrame);
                if (ret == AVERROR_EOF || ret == AVERROR(EAGAIN)) {
                    break;
                }
                sws_scale(
                        image_convert_context,
                        originFrame->data,
                        originFrame->linesize,
                        0,
                        srcHeight,
                        YUVFrame->data,
                        YUVFrame->linesize);
                SDL_UpdateTexture(texture, nullptr, YUVFrame->data[0], YUVFrame->linesize[0]);
                SDL_RenderClear(render);
                SDL_RenderCopy(render, texture, nullptr, nullptr);
                SDL_RenderPresent(render);
            } while (ret >= 0);
            av_packet_unref(&packet);
        }
        SDL_PollEvent(&event);
        switch (event.type) {
            case SDL_QUIT:
                SDL_Quit();
                exit(0);
            default:
                break;
        }
    }
}