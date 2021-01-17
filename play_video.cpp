extern "C"{
#include <libavformat/avformat.h>
}
#include <iostream>
#include <list>
#include <condition_variable>
#include <mutex>
#include <thread>
using namespace std;


template<class T>class List{
public:
    List(){
    }
    list<T> theList;
    condition_variable condition;
    mutex conditionMutex;
    T list_get(bool block=true){
        lock_guard<mutex> locker (conditionMutex);
        while(theList.empty()&& block){
            unique_lock<mutex> uniqueLock(conditionMutex);
            condition.wait(uniqueLock);
        }
        return theList.pop_back();
    }

    void list_push(T t){
        unique_lock<mutex> locker(conditionMutex);
        theList.emplace_front(t);
        condition.notify_one();
    }
};

class VideoInfo{
public:
    VideoInfo(){
        videoIndex=0;
        audioIndex=0;
    };
    List<AVPacket*> videoPacketList;
    List<AVPacket*> audioPacketList;
    int videoIndex;
    AVCodecContext* videoCodecContext;
    AVCodec * videoCodec;
    int audioIndex;
    AVCodecContext* audioCodecContext;
    AVCodec *audioCodec;
};

void error_out(string msg){
    cerr<<msg<<endl;
    exit(1);
}



int main(int argc,char ** argv){
    AVFormatContext * formatContext;
    auto ret = avformat_open_input(&formatContext,argv[1], nullptr, nullptr);
    if(ret <0){
        error_out("failed in open input");
    }
    VideoInfo videoInfo;
    thread decode_thread([&formatContext,&videoInfo]()->void{
        if (avformat_find_stream_info(formatContext, nullptr)<0){
            error_out("failed in find stream info");
        }
        for (int i = 0; i < formatContext->nb_streams; ++i) {
            auto codecPar= formatContext->streams[i]->codecpar;
            if()
        }
    });

}