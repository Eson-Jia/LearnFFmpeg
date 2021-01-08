extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/>
#include <SDL2/SDL_audio.h>
#include <SDL2/SDL.h>
}

#include <iostream>
#include <thread>

#define SDL_AUDIO_BUFFER_SIZE 4096
#define MAX_AUDIO_FRAME_SIZE 192000
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

int audio_resample(){

}

int audio_decode_frame(AVCodecContext *audioCtx, uint8_t *audio_buf, int buf_size) {
    static int ret = -1;
    auto packet = av_packet_alloc();
    auto frame = av_frame_alloc();
    while (true) {
        while (ret >= 0) {
            ret = avcodec_receive_frame(audioCtx, frame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                break;
            else if (ret < 0) {
                cerr << "Error during decoding" << endl;
                exit(1);
            }
            auto data_size = av_samples_get_buffer_size(nullptr,
                                                        audioCtx->channels,
                                                        frame->nb_samples,
                                                        audioCtx->sample_fmt,
                                                        1);
            if (data_size > buf_size) {
                cerr << "data_size > buf_size" << endl;
                exit(1);
            }
            if (data_size <= 0) {
                continue;
            }
            memcpy(audio_buf, frame->data[0], data_size);
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
    auto audioStreamIndex = 0;
    AVCodecParameters *parameters = avcodec_parameters_alloc();
    for (int i = 0; i < formatCtx->nb_streams; ++i) {
        if (formatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            audioStreamIndex = i;
            if (avcodec_parameters_copy(parameters, formatCtx->streams[i]->codecpar) < 0) {
                cerr << "failed in copy parameter" << endl;
                exit(1);
            }
            break;
        }
    }
    AVCodec *audioCodec = avcodec_find_decoder(parameters->codec_id);
    auto audioCodecCtx = avcodec_alloc_context3(audioCodec);
    ret = avcodec_parameters_to_context(audioCodecCtx, parameters);
    if (ret < 0) {
        cerr << "failed in parameters to context" << endl;
        exit(1);
    }
    ret = avcodec_open2(audioCodecCtx, audioCodec, nullptr);
    if (ret < 0) {
        cerr << "failed in codec open" << endl;
        exit(1);
    }
    ret = SDL_Init(SDL_INIT_AUDIO | SDL_INIT_TIMER);
    if (ret < 0) {
        cerr << "failed in sdl init" << endl;
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
    while (true) {
        auto ret = av_read_frame(formatCtx, &packet);
        this_thread::sleep_for(chrono::milliseconds(10));
        if (ret == AVERROR_EOF || ret == AVERROR(EAGAIN)) {
            cout << "read frame finished" << endl;
            this_thread::sleep_for(chrono::minutes(10));
        }
        if (ret < 0) {
            cerr << "failed in read packet" << endl;
            break;
        }
        if (packet.stream_index == audioStreamIndex) {
            packet_queue_put(&packetQueue, &packet);
        }
        SDL_PollEvent(&event);
        switch (event.type) {
            case SDL_QUIT:
                SDL_Quit();
                exit(0);
                break;
            default:
                break;
        }
    }
}