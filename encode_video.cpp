//
// Created by ubuntu on 2020/12/29.
//
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
}

#include <iostream>
#include <fstream>

using namespace std;

static void encode(AVCodecContext *enc_ctx, AVFrame *frame, AVPacket *packet, ofstream *out_file_stream) {
    if (frame) {
        printf("Send frame %3" PRId64"\n", frame->pts);
    }
    auto ret = avcodec_send_frame(enc_ctx, frame);
    if (ret < 0) {
        cerr << "Error sending a frame fro encoding" << endl;
        exit(1);
    }
    while (ret >= 0) {
        ret = avcodec_receive_packet(enc_ctx, packet);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            return;
        } else if (ret < 0) {
            cerr << "Error during encoding" << endl;
            exit(1);
        }
        printf("Write packet %3" PRId64" (size=%5d)\n", packet->pts, packet->size);
        out_file_stream->write(reinterpret_cast<const char *>(packet->data), packet->size);
        av_packet_unref(packet);
    }
}

int main(int argc, char **argv) {
    if (argc <= 2) {
        fprintf(stderr, "Usage: %s <output file> <codec name>\n", argv[0]);
        exit(0);
    }
    auto file_name = string(argv[1]);
    auto codec_name = argv[2];
    ofstream out_file(file_name);
    auto codec = avcodec_find_encoder_by_name(codec_name);
    if (codec == nullptr) {
        cerr << "Codec \'" << codec_name << "\' not found" << endl;
        exit(1);
    }
    auto c = avcodec_alloc_context3(codec);
    if (c == nullptr) {
        cerr << "Allocate Context" << endl;
        exit(1);
    }
    c->bit_rate = 400000;
    c->width = 352;
    c->height = 288;
    c->time_base = (AVRational) {1, 25};
    c->framerate = (AVRational) {25, 1};
    c->gop_size = 10;
    c->max_b_frames = 1;
    c->pix_fmt = AV_PIX_FMT_YUV420P;
    if (codec->id == AV_CODEC_ID_H264)
        av_opt_set(c->priv_data, "preset", "slow", 0);
    auto ret = avcodec_open2(c, codec, NULL);
    if (ret < 0) {
        cerr << "Could not open codec" << endl;
    }
    auto packet = av_packet_alloc();
    if (packet == nullptr) {
        cerr << "Could not allocate packet" << endl;
        exit(1);
    }
    auto frame = av_frame_alloc();
    if (frame == nullptr) {
        cerr << "Could not allocate frame" << endl;
        exit(1);
    }
    frame->format = c->pix_fmt;
    frame->width = c->width;
    frame->height = c->height;
    ret = av_frame_get_buffer(frame, 0);
    if (ret < 0) {
        cerr << "Could not allocate the video frame data" << endl;
        exit(1);
    }
    for (int i = 0; i < 25; ++i) {
        ret = av_frame_make_writable(frame);
        if (ret < 0) {
            cerr << "Could not make frame writable" << endl;
            exit(1);
        }
        for (int y = 0; y < c->height; ++y) {
            for (int x = 0; x < c->width; ++x) {
                frame->data[0][y * frame->linesize[0] + x] = y + y + i * 3;
            }
        }
        for (int y = 0; y < c->height / 2; ++y) {
            for (int x = 0; x < c->width / 2; ++x) {
                frame->data[1][y * frame->linesize[1] + x] = 128 + y + i * 2;
                frame->data[2][y * frame->linesize[2] + x] = 64 + x + i * 5;
            }
        }
        frame->pts = i;
        encode(c, frame, packet, &out_file);
    }
    encode(c, nullptr, packet, &out_file);
    if (codec->id == AV_CODEC_ID_MPEG1VIDEO || codec->id == AV_CODEC_ID_MPEG2VIDEO) {
        uint8_t endcode[] = {0, 0, 1, 0xb7};
        out_file << endcode;
    }
    out_file.close();
    avcodec_free_context(&c);
    av_frame_free(&frame);
    av_packet_free(&packet);
    return 0;
}