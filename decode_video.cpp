extern "C" {
#include <libavcodec/avcodec.h>
}

#include<string>
#include <iostream>
#include <fstream>

#define INBUF_SIZE 4096
using namespace std;

static void pgm_save(unsigned char *buf, int wrap, int xsize, int ysize, string filename) {
    ofstream o(filename);
    char buff[1024];
    sprintf(buff, "P5\n%d %d\n%d\n", xsize, ysize, 255);
    o << buff << endl;
    for (int i = 0; i < ysize; ++i) {
        o.write(reinterpret_cast<const char *>(buf + i * wrap), xsize);
    }
    o.close();
}

static void decode(AVCodecContext *dec_ctx, AVFrame *frame, AVPacket *pkt,string outfile) {
    auto ret = avcodec_send_packet(dec_ctx, pkt);
    if (ret < 0) {
        cerr << "Error sending a packet for decoding" << endl;
        exit(1);
    }
    while (ret >= 0) {
        ret = avcodec_receive_frame(dec_ctx, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            return;
        else if (ret < 0) {
            cerr << "Error during decoding" << endl;
            exit(1);
        }
        cout << "saving frame" << dec_ctx->frame_number << endl;
        char buf[1024];
        snprintf(buf, sizeof(buf), "%s-%d", outfile.c_str(), dec_ctx->frame_number);
        pgm_save(frame->data[0], frame->linesize[0], frame->width, frame->height, buf);

    }
}


int main(int argc, char **argv) {
    string input, output;
    if (argc <= 2) {
        fprintf(stderr, "Usage: %s <input file> <output file>\n"
                        "And check your input file is encoded by mpeg1video please.\n", argv[0]);
    }
    input = string(argv[1]);
    output = string(argv[2]);
    AVPacket *pkg = av_packet_alloc();
    if (pkg == nullptr) {
        exit(1);
    }
    uint8_t inbuf[INBUF_SIZE + AV_INPUT_BUFFER_PADDING_SIZE];
    memset(inbuf + INBUF_SIZE, 0, AV_INPUT_BUFFER_PADDING_SIZE);
    AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (codec == nullptr) {
        cerr << "Codec not found" << endl;
        exit(1);
    }
    AVCodecParserContext *parser = av_parser_init(codec->id);
    if (parser == nullptr) {
        cerr << "parser not found" << endl;
        exit(1);
    }
    AVCodecContext *c = avcodec_alloc_context3(codec);
    if (c == nullptr) {
        cerr << "Could not allocate video codec context" << endl;
    }
    if (avcodec_open2(c, codec, NULL) < 0) {
        cerr << "Could not open codec" << endl;
        exit(1);
    }
    ifstream f(input, ios_base::in);
    AVPacket *pkt = av_packet_alloc();
    if (pkt == nullptr) {
        cerr << "Could not allocate video packet" << endl;
        exit(1);
    }
    AVFrame *frame = av_frame_alloc();
    if (frame == nullptr) {
        cerr << "Could not allocate video frame" << endl;
        exit(1);
    }
    while (!f.eof()) {
        f.read(reinterpret_cast<char *>(inbuf), INBUF_SIZE);
        uint8_t *data = inbuf;
        auto data_size = f.gcount();
        while (data_size > 0) {
            auto ret = av_parser_parse2(parser, c, &pkt->data, &pkt->size,
                                        data, data_size, AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
            if (ret < 0) {
                cerr << "Error while parsing" << endl;
                exit(1);
            }
            data += ret;
            data_size -= ret;
            if (pkt->size) {
                decode(c, av_frame_alloc(), pkt, output);
            }
        }
    }
    decode(c, frame, nullptr, output);
    f.close();
    av_parser_close(parser);
    avcodec_free_context(&c);
    av_frame_free(&frame);
    av_packet_free(&pkg);
    return 0;
}