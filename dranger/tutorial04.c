// tutorial04.c
// A pedagogical video player that will stream through every video frame as fast as it can,
// and play audio (out of sync).
//
// Code based on FFplay, Copyright (c) 2003 Fabrice Bellard, 
// and a tutorial by Martin Bohme (boehme@inb.uni-luebeckREMOVETHIS.de)
// Tested on Gentoo, CVS version 5/01/07 compiled with GCC 4.1.1
// With updates from https://github.com/chelyaev/ffmpeg-tutorial
// Updates tested on:
// LAVC 54.59.100, LAVF 54.29.104, LSWS 2.1.101, SDL 1.2.15
// on GCC 4.7.2 in Debian February 2015
// Use
//
// gcc -o tutorial04 tutorial04.c -lavformat -lavcodec -lswscale -lz -lm `sdl-config --cflags --libs`
// to build (assuming libavformat and libavcodec are correctly installed, 
// and assuming you have sdl-config. Please refer to SDL docs for your installation.)
//
// Run using
// tutorial04 myvideofile.mpg
//
// to play the video stream on your screen.

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavutil/avstring.h>
#include <libavutil/imgutils.h>
#include <libswresample/swresample.h>
#include <libavutil/opt.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL_thread.h>

#ifdef __MINGW32__
#undef main /* Prevents SDL from overriding main() */
#endif

#include <stdio.h>
#include <assert.h>
#include <math.h>

// compatibility with newer API
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(55, 28, 1)
#define av_frame_alloc avcodec_alloc_frame
#define av_frame_free avcodec_free_frame
#endif

#define SDL_AUDIO_BUFFER_SIZE 1024
#define MAX_AUDIO_FRAME_SIZE 192000

#define MAX_AUDIOQ_SIZE (5 * 16 * 1024)
#define MAX_VIDEOQ_SIZE (5 * 256 * 1024)

#define FF_REFRESH_EVENT (SDL_USEREVENT)
#define FF_QUIT_EVENT (SDL_USEREVENT + 1)

#define VIDEO_PICTURE_QUEUE_SIZE 1

typedef struct PacketQueue {
    AVPacketList *first_pkt, *last_pkt;
    int nb_packets;
    int size;
    SDL_mutex *mutex;
    SDL_cond *cond;
} PacketQueue;


typedef struct VideoPicture {
    AVFrame *frame;
    int width, height; /* source height & width */
    int allocated;
} VideoPicture;

typedef struct VideoState {

    AVFormatContext *pFormatCtx;
    int videoStream, audioStream;
    AVStream *audio_st;
    AVCodecContext *audio_ctx;
    SwrContext *convert_ctx;
    PacketQueue audioq;
    uint8_t audio_buf[(MAX_AUDIO_FRAME_SIZE * 3) / 2];
    unsigned int audio_buf_size;
    unsigned int audio_buf_index;
    AVFrame audio_frame;
    AVPacket audio_pkt;
    uint8_t *audio_pkt_data;
    int audio_pkt_size;
    AVStream *video_st;
    AVCodecContext *video_ctx;
    PacketQueue videoq;
    struct SwsContext *sws_ctx;

    VideoPicture pictq[VIDEO_PICTURE_QUEUE_SIZE];
    int pictq_size, pictq_rindex, pictq_windex;
    SDL_mutex *pictq_mutex;
    SDL_cond *pictq_cond;

    SDL_Thread *parse_tid;
    SDL_Thread *video_tid;

    char filename[1024];
    int quit;
} VideoState;

SDL_Window *window;
SDL_Renderer *renderer;
SDL_Texture *texture;
SDL_mutex *screen_mutex;

/* Since we only have one decoding thread, the Big Struct
   can be global in case we need it. */
VideoState *global_video_state;

void packet_queue_init(PacketQueue *q) {
    memset(q, 0, sizeof(PacketQueue));
    q->mutex = SDL_CreateMutex();
    q->cond = SDL_CreateCond();
}

int packet_queue_put(PacketQueue *q, AVPacket *pkt) {

    AVPacketList *pkt1;
    if (av_dup_packet(pkt) < 0) {
        return -1;
    }
    pkt1 = av_malloc(sizeof(AVPacketList));
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

        if (global_video_state->quit) {
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

int audio_decode_frame(VideoState *is, uint8_t *audio_buf, int buf_size) {
    static int ret = -1;
    AVPacket *packet = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    int data_size = -1;
    while (1) {
        while (ret >= 0) {
            ret = avcodec_receive_frame(is->audio_ctx, frame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                break;
            else if (ret < 0) {
                fprintf(stderr, "Error during decoding\n");
                exit(1);
            }
            data_size = av_samples_get_buffer_size(NULL, frame->channels, frame->nb_samples,
                                                   is->audio_ctx->sample_fmt, 0);
            int convert_ret = swr_convert(is->convert_ctx,
                                          &audio_buf,
                                          frame->nb_samples,
                                          (const uint8_t **) frame->data,
                                          frame->nb_samples);
            if (convert_ret < 0) {
                fprintf(stderr, "failed in convert\n");
                exit(1);
            }
            return data_size;
        }
        if (packet->data) {
            av_packet_unref(packet);
        }
        if (packet_queue_get(&is->audioq, packet, 1) < 0) {
            return -1;
        }
        ret = avcodec_send_packet(is->audio_ctx, packet);
        if (ret < 0) {
            fprintf(stderr, "failed in send packet error:%d\n", AVERROR(ret));
            break;
        }
    }
}

void audio_callback(void *userdata, Uint8 *stream, int len) {

    VideoState *is = (VideoState *) userdata;
    int len1, audio_size;

    while (len > 0) {
        if (is->audio_buf_index >= is->audio_buf_size) {
            /* We have already sent all our data; get more */
            audio_size = audio_decode_frame(is, is->audio_buf, sizeof(is->audio_buf));
            if (audio_size < 0) {
                /* If error, output silence */
                is->audio_buf_size = 1024;
                memset(is->audio_buf, 0, is->audio_buf_size);
            } else {
                is->audio_buf_size = audio_size;
            }
            is->audio_buf_index = 0;
        }
        len1 = is->audio_buf_size - is->audio_buf_index;
        if (len1 > len)
            len1 = len;
        memcpy(stream, (uint8_t *) is->audio_buf + is->audio_buf_index, len1);
        len -= len1;
        stream += len1;
        is->audio_buf_index += len1;
    }
}

static Uint32 sdl_refresh_timer_cb(Uint32 interval, void *opaque) {
    SDL_Event event;
    event.type = FF_REFRESH_EVENT;
    event.user.data1 = opaque;
    SDL_PushEvent(&event);
    return 0; /* 0 means stop timer */
}

/* schedule a video refresh in 'delay' ms */
static void schedule_refresh(VideoState *is, int delay) {
    SDL_AddTimer(delay, sdl_refresh_timer_cb, is);
}

void video_display(VideoState *is) {

    VideoPicture *vp;
    vp = &is->pictq[is->pictq_rindex];
    if (vp->frame) {
        SDL_LockMutex(screen_mutex);
        SDL_UpdateTexture(renderer, NULL, vp->frame->data[0], vp->frame->linesize[0]);
        SDL_RenderClear(renderer);
        SDL_RenderCopy(renderer, texture, NULL, NULL);
        SDL_RenderPresent(renderer);
        SDL_UnlockMutex(screen_mutex);
    }
}

void video_refresh_timer(void *userdata) {

    VideoState *is = (VideoState *) userdata;
    VideoPicture *vp;

    if (is->video_st) {
        if (is->pictq_size == 0) {
            schedule_refresh(is, 1);
        } else {
            vp = &is->pictq[is->pictq_rindex];
            /* Now, normally here goes a ton of code
           about timing, etc. we're just going to
           guess at a delay for now. You can
           increase and decrease this value and hard code
           the timing - but I don't suggest that ;)
           We'll learn how to do it for real later.
            */
            schedule_refresh(is, 40);

            /* show the picture! */
            video_display(is);

            /* update queue for next picture! */
            if (++is->pictq_rindex == VIDEO_PICTURE_QUEUE_SIZE) {
                is->pictq_rindex = 0;
            }
            SDL_LockMutex(is->pictq_mutex);
            is->pictq_size--;
            SDL_CondSignal(is->pictq_cond);
            SDL_UnlockMutex(is->pictq_mutex);
        }
    } else {
        schedule_refresh(is, 100);
    }
}

void alloc_picture(void *userdata) {
    VideoState *is = (VideoState *) userdata;
    VideoPicture *vp;
    uint8_t *out_buffer;

    vp = &is->pictq[is->pictq_windex];
    if (vp->frame == NULL) {
        // we already have one make another, bigger/smaller
        vp->frame = av_frame_alloc();
    } else if (vp->frame->data) {
        av_frame_unref(vp->frame);
    }
    vp->width = is->video_ctx->width;
    vp->height = is->video_ctx->height;
    out_buffer = (unsigned char *) av_malloc(
            av_image_get_buffer_size(AV_PIX_FMT_YUV420P, vp->width, vp->height, 1));
    av_image_fill_arrays(vp->frame->data, vp->frame->linesize, out_buffer,
                         AV_PIX_FMT_YUV420P, vp->width, vp->height, 1);
    vp->allocated = 1;

}

int queue_picture(VideoState *is, AVFrame *pFrame) {

    VideoPicture *vp;
    /* wait until we have space for a new pic */
    SDL_LockMutex(is->pictq_mutex);
    while (is->pictq_size >= VIDEO_PICTURE_QUEUE_SIZE &&
           !is->quit) {
        SDL_CondWait(is->pictq_cond, is->pictq_mutex);
    }
    SDL_UnlockMutex(is->pictq_mutex);

    if (is->quit)
        return -1;

    // windex is set to 0 initially
    vp = &is->pictq[is->pictq_windex];

    /* allocate or resize the buffer! */
    if (!vp->frame ||
        vp->width != is->video_ctx->width ||
        vp->height != is->video_ctx->height) {
        SDL_Event event;

        vp->allocated = 0;
        alloc_picture(is);
        if (is->quit) {
            return -1;
        }
    }

    /* We have a place to put our picture on the queue */

    if (vp->frame) {
        /* point pict at the queue */

        // Convert the image into YUV format that SDL uses
        auto ret = sws_scale(is->sws_ctx, (uint8_t const *const *) pFrame->data,
                             pFrame->linesize, 0, is->video_ctx->height,
                             vp->frame->data, vp->frame->linesize);
        if (ret < 0) {
            fprintf(stderr, "failed in scale\n");
            exit(1);
        }
        /* now we inform our display thread that we have a pic ready */
        if (++is->pictq_windex == VIDEO_PICTURE_QUEUE_SIZE) {
            is->pictq_windex = 0;
        }
        SDL_LockMutex(is->pictq_mutex);
        is->pictq_size++;
        SDL_UnlockMutex(is->pictq_mutex);
    }
    return 0;
}

int video_thread(void *arg) {
    VideoState *is = (VideoState *) arg;
    AVPacket *packet = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    int ret = -1;
    while (1) {
        if (packet_queue_get(&is->videoq, packet, 1) < 0) {
            break;
        }
        ret = avcodec_send_packet(is->video_ctx, packet);
        if (ret == AVERROR_EOF || ret == AVERROR(EAGAIN)) {
            break;
        }
        if (ret < 0) {
            fprintf(stderr, "failed in send packet");
            exit(1);
        }
        do {
            ret = avcodec_receive_frame(is->video_ctx, frame);
            if (ret == AVERROR_EOF || ret == AVERROR(EAGAIN)) {
                break;
            }
            if (ret < 0) {
                fprintf(stderr, "failed in receive frame");
                exit(1);
            }
            ret = queue_picture(is, frame);
            if (ret < 0) {
                break;
            }
        } while (ret > 0);
        av_packet_unref(packet);
    }
    av_frame_unref(frame);
    av_frame_free(frame);
}

int stream_component_open(VideoState *is, int stream_index) {

    AVFormatContext *pFormatCtx = is->pFormatCtx;
    AVCodecContext *codecCtx = NULL;
    AVCodec *codec = NULL;
    SDL_AudioSpec wanted_spec, spec;

    if (stream_index < 0 || stream_index >= pFormatCtx->nb_streams) {
        return -1;
    }

    codec = avcodec_find_decoder(pFormatCtx->streams[stream_index]->codec->codec_id);
    if (!codec) {
        fprintf(stderr, "Unsupported codec!\n");
        return -1;
    }

    codecCtx = avcodec_alloc_context3(codec);
    if (avcodec_copy_context(codecCtx, pFormatCtx->streams[stream_index]->codec) != 0) {
        fprintf(stderr, "Couldn't copy codec context");
        return -1; // Error copying codec context
    }


    if (codecCtx->codec_type == AVMEDIA_TYPE_AUDIO) {
        is->convert_ctx = swr_alloc();
        av_opt_set_int(is->convert_ctx, "in_channel_layout", codecCtx->channel_layout, 0);
        av_opt_set_int(is->convert_ctx, "out_channel_layout", codecCtx->channel_layout, 0);
        av_opt_set_int(is->convert_ctx, "in_sample_rate", codecCtx->sample_rate, 0);
        av_opt_set_int(is->convert_ctx, "out_sample_rate", codecCtx->sample_rate, 0);
        av_opt_set_sample_fmt(is->convert_ctx, "in_sample_fmt", AV_SAMPLE_FMT_FLTP, 0);
        av_opt_set_sample_fmt(is->convert_ctx, "out_sample_fmt", AV_SAMPLE_FMT_FLT, 0);
        swr_init(is->convert_ctx);

        // Set audio settings from codec info
        wanted_spec.freq = codecCtx->sample_rate;
        wanted_spec.format = AUDIO_F32;
        wanted_spec.channels = codecCtx->channels;
        wanted_spec.silence = 0;
        wanted_spec.samples = SDL_AUDIO_BUFFER_SIZE;
        wanted_spec.callback = audio_callback;
        wanted_spec.userdata = is;

        if (SDL_OpenAudio(&wanted_spec, &spec) < 0) {
            fprintf(stderr, "SDL_OpenAudio: %s\n", SDL_GetError());
            return -1;
        }
    }
    if (avcodec_open2(codecCtx, codec, NULL) < 0) {
        fprintf(stderr, "Unsupported codec!\n");
        return -1;
    }

    switch (codecCtx->codec_type) {
        case AVMEDIA_TYPE_AUDIO:
            is->audioStream = stream_index;
            is->audio_st = pFormatCtx->streams[stream_index];
            is->audio_ctx = codecCtx;
            is->audio_buf_size = 0;
            is->audio_buf_index = 0;
            memset(&is->audio_pkt, 0, sizeof(is->audio_pkt));
            packet_queue_init(&is->audioq);
            SDL_PauseAudio(0);
            break;
        case AVMEDIA_TYPE_VIDEO:
            is->videoStream = stream_index;
            is->video_st = pFormatCtx->streams[stream_index];
            is->video_ctx = codecCtx;
            packet_queue_init(&is->videoq);
            is->video_tid = SDL_CreateThread(video_thread, "", is);
            is->sws_ctx = sws_getContext(is->video_ctx->width, is->video_ctx->height,
                                         is->video_ctx->pix_fmt, is->video_ctx->width,
                                         is->video_ctx->height, AV_PIX_FMT_YUV420P,
                                         0, NULL, NULL, NULL);
            break;
        default:
            break;
    }
}

int decode_thread(void *arg) {

    VideoState *is = (VideoState *) arg;
    AVFormatContext *pFormatCtx = avformat_alloc_context();
    AVPacket pkt1, *packet = &pkt1;

    int video_index = -1;
    int audio_index = -1;
    int i;

    is->videoStream = -1;
    is->audioStream = -1;

    global_video_state = is;

    // Open video file
    if (avformat_open_input(&pFormatCtx, is->filename, NULL, NULL) != 0)
        return -1; // Couldn't open file

    is->pFormatCtx = pFormatCtx;

    // Retrieve stream information
    if (avformat_find_stream_info(pFormatCtx, NULL) < 0)
        return -1; // Couldn't find stream information

    // Dump information about file onto standard error
    av_dump_format(pFormatCtx, 0, is->filename, 0);

    // Find the first video stream

    for (i = 0; i < pFormatCtx->nb_streams; i++) {
        if (pFormatCtx->streams[i]->codec->codec_type == AVMEDIA_TYPE_VIDEO &&
            video_index < 0) {
            video_index = i;
        }
        if (pFormatCtx->streams[i]->codec->codec_type == AVMEDIA_TYPE_AUDIO &&
            audio_index < 0) {
            audio_index = i;
        }
    }
    if (audio_index >= 0) {
        stream_component_open(is, audio_index);
    }
    if (video_index >= 0) {
        stream_component_open(is, video_index);
    }

    if (is->videoStream < 0 || is->audioStream < 0) {
        fprintf(stderr, "%s: could not open codecs\n", is->filename);
        goto fail;
    }

    // main decode loop

    for (;;) {
        if (is->quit) {
            break;
        }
        // seek stuff goes here
        if (is->audioq.size > MAX_AUDIOQ_SIZE ||
            is->videoq.size > MAX_VIDEOQ_SIZE) {
            SDL_Delay(10);
            continue;
        }
        if (av_read_frame(is->pFormatCtx, packet) < 0) {
            if (is->pFormatCtx->pb->error == 0) {
                SDL_Delay(100); /* no error; wait for user input */
                continue;
            } else {
                break;
            }
        }
        // Is this a packet from the video stream?
        if (packet->stream_index == is->videoStream) {
            packet_queue_put(&is->videoq, packet);
        } else if (packet->stream_index == is->audioStream) {
            packet_queue_put(&is->audioq, packet);
        } else {
            av_free_packet(packet);
        }
    }
    /* all done - wait for it */
    while (!is->quit) {
        SDL_Delay(100);
    }

    fail:
    if (1) {
        SDL_Event event;
        event.type = FF_QUIT_EVENT;
        event.user.data1 = is;
        SDL_PushEvent(&event);
    }
    return 0;
}

int main(int argc, char *argv[]) {

    SDL_Event event;

    VideoState *is;

    is = av_mallocz(sizeof(VideoState));

    if (argc < 2) {
        fprintf(stderr, "Usage: test <file>\n");
        exit(1);
    }
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER)) {
        fprintf(stderr, "Could not initialize SDL - %s\n", SDL_GetError());
        exit(1);
    }

    window = SDL_CreateWindow(
            "tutorial04",
            SDL_WINDOWPOS_UNDEFINED,
            SDL_WINDOWPOS_UNDEFINED,
            1280,
            720,
            SDL_WINDOW_OPENGL);
    renderer = SDL_CreateRenderer(window, -1, 0);
    texture = SDL_CreateTexture(
            renderer,
            SDL_PIXELFORMAT_IYUV,
            SDL_TEXTUREACCESS_STREAMING,
            1280,
            720);
    screen_mutex = SDL_CreateMutex();
    av_strlcpy(is->filename, argv[1], sizeof(is->filename));

    is->pictq_mutex = SDL_CreateMutex();
    is->pictq_cond = SDL_CreateCond();

    schedule_refresh(is, 40);

    is->parse_tid = SDL_CreateThread(decode_thread, NULL, is);
    if (!is->parse_tid) {
        av_free(is);
        return -1;
    }
    for (;;) {

        SDL_WaitEvent(&event);
        switch (event.type) {
            case FF_QUIT_EVENT:
            case SDL_QUIT:
                is->quit = 1;
                SDL_Quit();
                return 0;
                break;
            case FF_REFRESH_EVENT:
                video_refresh_timer(event.user.data1);
                break;
            default:
                break;
        }
    }
    return 0;

}
