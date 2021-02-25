#include<string>
#include <iostream>
#include <thread>

#include <SDL2/SDL.h>
#include <SDL2/SDL_timer.h>
#include <SDL2/SDL_events.h>


using namespace std;

// 使用 event loop 能保证 my_function 一直在同一个线程(这里是主线程)中运行
void my_function(string param) {
    cout << "do work: " << param << "in thread:" << this_thread::get_id() << endl;
}

Uint32 callbackfunc(Uint32 interval, void *opaque) {
    SDL_Event event;
    SDL_UserEvent userEvent;

    /* In this example, our callback pushes a function
    into the queue, and causes our callback to be called again at the
    same interval: */

    userEvent.type = SDL_USEREVENT;// 注意在使用 userEvent 的时候需要设置 type,否则在 event loop 中取出的 event.type 为 SDL_QUIT
    userEvent.data1 = reinterpret_cast<void *>(&my_function);
    userEvent.data2 = opaque;

    event.type = SDL_USEREVENT;
    event.user = userEvent;
    SDL_PushEvent(&event);
    // interval > 0 下次迭代的周期 = 0 取消该定时器
    return interval;
}

int main() {
    // 使用定时器必须在 SDL_Init 中传入该 flag
    auto ret = SDL_Init(SDL_INIT_TIMER);
    if (ret < 0) {
        fprintf(stderr, "Could not initialize SDL - %s\n", SDL_GetError());
        exit(1);
    }
    string theWork("timer interval work");
    SDL_AddTimer(1000, callbackfunc, &theWork);
    SDL_Event getEvent;
    for (;;) {
        SDL_WaitEvent(&getEvent);
        switch (getEvent.type) {
            case SDL_USEREVENT:
                reinterpret_cast<typeof(my_function) * >(getEvent.user.data1)(
                        *static_cast<string *>(getEvent.user.data2));
                break;
            case SDL_QUIT:
                SDL_Quit();
                return 0;
        }
    }
}