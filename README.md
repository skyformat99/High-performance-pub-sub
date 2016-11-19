# High performance pub/sub
This repository displays a very basic broadcasting coalescing algorithm used to minimize the amount of TCP sends performed when dealing with problems like pub/sub. The algorithm is written in C++11 and what it does in **89 milliseconds** takes an equivalent Socket.IO based pub/sub implementation **4.5 minutes**. That's a difference, of this particular problem size, of **over 3000x in performance**.

When I started looking into pub/sub I noticed the same thing I had seen when looking into WebSockets in general: **nobody is doing it efficiently**. Basic and very easy-to-understand concepts like minimizing the amount of broadcasts, sends, framing, branches, copies and the like are complely ignored in most of the pub/sub implementations I have looked at.

The following algorithm is not in any way optimized to some crazy degree, but rather displays the most fundamental design decisions that could be made to massively improve performance. Naturally you first have to select a high performance transport system, in this case we use **µWebSockets**. Again, this is not the gold standard in pub/sub but rather a basic guide.

```c++
#include <uWS/uWS.h>
#include <iostream>
#include <vector>
#include <map>

struct Gap {
    int start;
    int length;
};

std::vector<uWS::WebSocket<uWS::SERVER>> sockets;
std::map<uWS::WebSocket<uWS::SERVER>, std::vector<Gap>> uniqueSenders;
std::string sharedMessage;
uv_prepare_t prepare;
uv_check_t checker;
int ready_polls = 0;
uv_timer_t timer;
bool delaying = false;
int batched = 0;

void broadcast() {
    std::vector<std::pair<void *, std::string>> uniqueMessages(uniqueSenders.size());
    int i = 0;
    for (auto uniqueSender : uniqueSenders) {
        uniqueMessages[i].first = uniqueSender.first.getPollHandle();
        uniqueMessages[i].second = sharedMessage.substr(0, uniqueSender.second[0].start);
        int lastStop = uniqueSender.second[0].start + uniqueSender.second[0].length;
        for (int j = 1; j < uniqueSender.second.size(); j++) {
            uniqueMessages[i].second += sharedMessage.substr(lastStop, uniqueSender.second[j].start - lastStop);
            lastStop = uniqueSender.second[j].start + uniqueSender.second[j].length;
        }
        uniqueMessages[i].second += sharedMessage.substr(lastStop);
        i++;
    }

    auto preparedMessage = uWS::WebSocket<uWS::SERVER>::prepareMessage((char *) sharedMessage.data(), sharedMessage.length(), uWS::OpCode::TEXT, false);
    int j = 0;
    for (auto socket : sockets) {
        if (j < uniqueMessages.size() && uniqueMessages[j].first == socket.getPollHandle()) {
            if (uniqueMessages[j].second.length()) {
                socket.send(uniqueMessages[j].second.data(), uniqueMessages[j].second.length(), uWS::OpCode::TEXT);
            }
            j++;
        } else {
            socket.sendPrepared(preparedMessage);
        }
    }
    uWS::WebSocket<uWS::SERVER>::finalizeMessage(preparedMessage);

    delaying = false;
    batched = 0;
    uniqueSenders.clear();
    sharedMessage.clear();
}

int main(int argc, char *argv[])
{
    uWS::Hub hub;
    uv_timer_init(hub.getLoop(), &timer);

    uv_prepare_init(hub.getLoop(), &prepare);
    uv_prepare_start(&prepare, [](uv_prepare_t *prepare) {
        // called before epoll_wait
        // reset number of messages for this iteration
        ready_polls = 0;
        // if in batching mode, start a noop timer of 1ms
        // this extends the batching window 1ms
        if (batched) {
            uv_timer_start(&timer, [](uv_timer_t *t) {}, 1, 0);
        }
    });

    uv_check_init(hub.getLoop(), &checker);
    uv_check_start(&checker, [](uv_check_t *checker) {
        // called after epoll_wait returns
        // if in batching mode and we got no new messages this iteration, end batch and broadcast!
        if (delaying && ready_polls == 0) {
            broadcast();
            delaying = false;
        }
    });

    hub.onConnection([](uWS::WebSocket<uWS::SERVER> ws, uWS::UpgradeInfo ui) {
        // keep a sorted container of sockets
        sockets.push_back(ws);
		std::sort(sockets.begin(), sockets.end(), [](const uWS::WebSocket<uWS::SERVER> &a, const uWS::WebSocket<uWS::SERVER> &b) {
		    return a < b;
		});

        // do whatever you need to establish correct state of your connection
        ws.send("1234567890123456");
    });

    hub.onMessage([](uWS::WebSocket<uWS::SERVER> ws, char *message, size_t length, uWS::OpCode cpCode) {
        // every batch has a shared message that most (listeners-only) will receive
        int start = sharedMessage.length();
        sharedMessage.append(message, length);
        // senders should not receive what they sent themselves, so we add a gap to this sender
        uniqueSenders[ws].push_back({start, length});

        // enter batching mode if not already
        if (!delaying) {
            delaying = true;
            batched = 0;
        }

        // update information of this batch and of this epoll_wait return
        ready_polls++;
        batched++;
    });

    hub.listen(6020);
    hub.run();
}


```
