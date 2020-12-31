//
// Created by ubuntu on 2020/12/30.
//
extern "C" {
#include <libavcodec/avcodec.h>
}

#include <iostream>
#include <fstream>
#include <sstream>

#define INBUF_SIZE 4096
#define PREFIX_SIZE 256
using namespace std;


static void pgm_save(string out_file, unsigned char *buff, int wrap, int xsize, int ysize) {
    ofstream out(out_file, ios_base::out);
    char prefix_buff[PREFIX_SIZE];
    snprintf(prefix_buff, PREFIX_SIZE, "P5\n%d %d\n%d\n", xsize, ysize, 255);
    out.write(prefix_buff, strlen(prefix_buff));
    for (int i = 0; i < ysize; ++i) {
        out.write(reinterpret_cast<const char *>(buff + wrap * i), xsize);
    }
    out.close();
}

static void decode(AVCodecContext *ctx, AVPacket *packet, AVFrame *frame, string out_file_prefix) {
    auto ret = avcodec_send_packet(ctx, packet);
    if (ret < 0) {
        cerr << "Failed in send packet" << endl;
        exit(1);
    }
    while (ret >= 0) {
        ret = avcodec_receive_frame(ctx, frame);
        if (ret == AVERROR_EOF || ret == AVERROR(EAGAIN)) {
            return;
        }
        if (ret < 0) {
            cerr << "Failed in receive frame" << endl;
            exit(1);
        }
        stringstream ss;
        ss << out_file_prefix << "-" << ctx->frame_number;
        pgm_save(ss.str(), frame->data[0], frame->linesize[0], frame->width, frame->height);
    }
}


int main(int argc, char **argv) {
    if (argc <= 3) {
        cerr << "argc too less " << endl;
        exit(1);
    }
    ifstream src_file(argv[1]);
    auto codec_name = argv[2];
    string out_file_prefix(argv[3]);
    auto codec = avcodec_find_decoder_by_name(codec_name);
    if (codec == nullptr) {
        cerr << "Could not found codec" << codec_name << endl;
        exit(1);
    }
    auto ctx = avcodec_alloc_context3(codec);
    if (ctx == nullptr) {
        cerr << "Could not allocate context" << endl;
        exit(1);
    }
    if (avcodec_open2(ctx, codec, nullptr) < 0) {
        exit(1);
    }
    auto parser = av_parser_init(codec->id);
    if (parser == nullptr) {
        cerr << "Failed in init parser" << endl;
        exit(1);
    }
    auto frame = av_frame_alloc();
    if (frame == nullptr) {
        cerr << "Could not allocate frame" << endl;
        exit(1);
    }
    auto packet = av_packet_alloc();
    if (packet == nullptr) {
        cerr << "Could not allocate packet" << endl;
        exit(1);
    }
    uint8_t inbuf[INBUF_SIZE + AV_INPUT_BUFFER_PADDING_SIZE];
    memset(inbuf + INBUF_SIZE, 0, AV_INPUT_BUFFER_PADDING_SIZE);
    while (!src_file.eof()) {
        src_file.read(reinterpret_cast<char *>(inbuf), INBUF_SIZE);
        uint8_t *data = inbuf;
        auto read_length = src_file.gcount();
        while (read_length > 0) {
            auto parse_len = av_parser_parse2(parser, ctx, &packet->data, &packet->size, data, read_length,
                                              AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
            data += parse_len;
            read_length -= parse_len;
            if (packet->size > 0) {
                decode(ctx, packet, frame,out_file_prefix);
            }
        }
    }
    decode(ctx, nullptr, frame,out_file_prefix);
    src_file.close();
    avcodec_close(ctx);
    av_parser_close(parser);
    av_frame_free(&frame);
    av_packet_free(&packet);
    return 0;
}