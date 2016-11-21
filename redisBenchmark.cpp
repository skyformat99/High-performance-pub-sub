#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

#include <hiredis/hiredis.h>
#include <hiredis/async.h>
#include <hiredis/adapters/libuv.h>

int remainingSubs = 9000;
int remainingPubs = 1000;
uv_loop_t *loop;
void nextConnection();
#include <vector>
#include <iostream>
#include <chrono>
std::vector<redisAsyncContext *> pubs;
int remainingMessages = remainingSubs * remainingPubs;
int numMessages = remainingMessages;
int iterations = 0;
std::chrono::time_point<std::chrono::high_resolution_clock> start;

void publish() {
    for (auto *c : pubs) {
        redisAsyncCommand(c, NULL, NULL, "PUBLISH eventName a");
    }
}

void subCallback(redisAsyncContext *c, void *reply, void *privdata) {
    if (remainingSubs) {
        remainingSubs--;
    }

    if (!remainingSubs && !remainingPubs) {
        if (!--remainingMessages) {
            int ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::high_resolution_clock::now() - start).count();
            iterations++;
            std::cout << "Latency: " << (float(ms) / iterations) << " ms" << std::endl;
            remainingMessages = numMessages;
            publish();
        }
    } else {
        nextConnection();
    }
}

void connectCallback(const redisAsyncContext *c, int status) {
    pubs.push_back((redisAsyncContext *) c);
    remainingPubs--;
    if (!remainingSubs && !remainingPubs) {
        start = std::chrono::high_resolution_clock::now();
        publish();
    } else {
        nextConnection();
    }
}

void nextConnection() {
    redisAsyncContext *c = redisAsyncConnect("127.0.0.1", 6379);
    redisLibuvAttach(c,loop);

    if (remainingSubs) {
        redisAsyncCommand((redisAsyncContext *) c, subCallback, NULL, "SUBSCRIBE eventName");
    } else if (remainingPubs) {
        redisAsyncSetConnectCallback(c,connectCallback);
    }
}

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);
    loop = uv_default_loop();
    nextConnection();
    uv_run(loop, UV_RUN_DEFAULT);
    return 0;
}

