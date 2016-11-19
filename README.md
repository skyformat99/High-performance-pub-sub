# High performance pub/sub
This repository displays a very basic broadcasting coalescing algorithm used to minimize the amount of TCP sends performed when dealing with problems like pub/sub. The algorithm is written in C++11 and what it does in **89 milliseconds** takes an equivalent Socket.IO based pub/sub implementation **4.5 minutes**. That's a difference, of this particular problem size, of **over 3000x in performance**.

When I started looking into pub/sub I noticed the same thing I had seen when looking into WebSockets in general: **nobody is doing it efficiently**. Basic and very easy-to-understand concepts like minimizing the amount of broadcasts, sends, framing, branches, copies and the like are complely ignored in most of the pub/sub implementations I have looked at.

The following algorithm is not in any way optimized to some crazy degree, but rather displays the most fundamental design decisions that could be made to massively improve performance. Naturally you first have to select a high performance transport system, in this case we use **µWebSockets**. Again, this is not the gold standard in pub/sub but rather a basic guide.

```c++
// Zlib licensed braindump, (C) 2016 by Alex Hultman

#include <uWS/uWS.h>
#include <iostream>
#include <vector>
#include <map>

// one batch per room
struct Room {
    // a Gap refers to a gap in the sharedMessage string
    struct Gap {
        int start;
        int length;
    };
    // senders have gaps in relation to the sharedMessage
    std::map<uWS::WebSocket<uWS::SERVER>, std::vector<Gap>> uniqueSenders;
    // the sharedMessage holds the complete batch that will be sent in verbatim
    // to sockets that didn't send anything for this batch (they are listeners-only)
    std::string sharedMessage;

    // you should probably use a doubly linked list of sockets, or any other container instead
    // for this example a simple vector or sorted sockets are used, your implementation can vary
    std::vector<uWS::WebSocket<uWS::SERVER>> sockets;

    // each iteration that resuls in a new pub should extend the time window of the batching
    bool gotPubsThisIteration = false;
    // total number of batched pubs this entire time window
    int batched = 0;

    // broadcasting ends the current batch and makes sure to transport the enqueued publications
    // to all subscribed-to-this-room sockets
    void broadcast() {
        // generate all the unique messages based on the unique senders and their gaps
        // in relation to the shared message of this room's batch
        std::vector<std::pair<void *, std::string>> uniqueMessages(uniqueSenders.size());
        int i = 0;
        for (auto uniqueSender : uniqueSenders) {
            // this entire loop could absolutely be optimized to remove redundant copies and appends
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

        // simply send correct messages to the right recipients
        // make sure to prepare the common shared message since that message will be sent
        // to the most sockets, in verbatim
        auto preparedMessage = uWS::WebSocket<uWS::SERVER>::prepareMessage((char *) sharedMessage.data(), sharedMessage.length(), uWS::OpCode::TEXT, false);
        int j = 0;
        for (auto socket : sockets) {
            // with a sorted vector of sockets you can make assumptions to your search algorithm
            // you could also implement this with a double linked list where you rearrange sockets based on
            // what and if they sent
            if (j < uniqueMessages.size() && uniqueMessages[j].first == socket.getPollHandle()) {
                if (uniqueMessages[j].second.length()) {
                    socket.send(uniqueMessages[j].second.data(), uniqueMessages[j].second.length(), uWS::OpCode::TEXT);
                }
                j++;
            } else {
                // most common path should be a fast path with a prepared message
                socket.sendPrepared(preparedMessage);
            }
        }
        uWS::WebSocket<uWS::SERVER>::finalizeMessage(preparedMessage);

        // mark this batch as finished, ready for a new time window
        batched = 0;
        uniqueSenders.clear();
        sharedMessage.clear();
    }

    void publish(uWS::WebSocket<uWS::SERVER> sender, const char *message, size_t length) {
        // every batch has a shared message that most (listeners-only) will receive
        int start = sharedMessage.length();
        sharedMessage.append(message, length);
        // senders should not receive what they sent themselves, so we add a gap to this sender
        uniqueSenders[sender].push_back({start, length});
        // update batching counters
        gotPubsThisIteration = true;
        batched++;
    }

    void addSubscriber(uWS::WebSocket<uWS::SERVER> ws) {
        // keep a sorted container of sockets (you can do much better than this!)
        sockets.push_back(ws);
        // at least do sorted insertion and not a full sort here
        std::sort(sockets.begin(), sockets.end(), [](const uWS::WebSocket<uWS::SERVER> &a, const uWS::WebSocket<uWS::SERVER> &b) {
            return a < b;
        });
    }

// let's call this the defaultRoom
} defaultRoom;

int main(int argc, char *argv[])
{
    uWS::Hub hub;

    // we need a timer to implement the broadcast coalescing time window
    uv_timer_t timer;
    uv_timer_init(hub.getLoop(), &timer);

    // register callback for before epoll_wait blocks
    uv_prepare_t prepare;
    prepare.data = &timer;
    uv_prepare_init(hub.getLoop(), &prepare);
    uv_prepare_start(&prepare, [](uv_prepare_t *prepare) {
        if (defaultRoom.batched) {
            // start a noop timer of 1ms to force epoll_wait to wake up in a while
            uv_timer_start((uv_timer_t *) prepare->data, [](uv_timer_t *t) {}, 1, 0);
            // clear this iteration, marking a potential end to this batch
            defaultRoom.gotPubsThisIteration = false;
        }
    });

    // register callback for after epoll_wait returned and all polls have been handled
    uv_check_t checker;
    uv_check_init(hub.getLoop(), &checker);
    uv_check_start(&checker, [](uv_check_t *checker) {
        // if in batching mode and we got no new pubs this iteration
        if (defaultRoom.batched && !defaultRoom.gotPubsThisIteration) {
            // end batch and broadcast
            defaultRoom.broadcast();
        }
    });

    hub.onConnection([](uWS::WebSocket<uWS::SERVER> ws, uWS::UpgradeInfo ui) {
        // let's assume every connection will subscribe to the default room
        defaultRoom.addSubscriber(ws);

        // do whatever you need to establish correct state of your connection
        // obviously depends on what protocol you are implementing
        ws.send("1234567890123456");
    });

    hub.onMessage([](uWS::WebSocket<uWS::SERVER> ws, char *message, size_t length, uWS::OpCode cpCode) {
        // in this example protocol we simply publish whatever message we get sent to us
        // we publish to the default room which all connected sockets are subscribed to
        defaultRoom.publish(ws, message, length);
    });

    // this code is compatible with the event benchmark available at https://github.com/deepstreamIO/deepstream.io-benchmarks
    hub.listen(6020);
    hub.run();
}
```
