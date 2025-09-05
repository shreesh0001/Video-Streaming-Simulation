#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <unistd.h>
#include <cstring>
#include <iostream>
#include <thread>
#include <string>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <random>
#include <algorithm>
#include <chrono>

struct CtrlMsg { int type; int length; char data[256]; };
struct Packet { int seq; char payload[1024]; };

enum Mode { TCP, UDP };

struct Session {
    int sock;
    sockaddr_in addr;
    socklen_t addrlen;
    Mode mode;
    std::string resolution;
    int packetsToSend;
    int sentPackets = 0;
    std::chrono::high_resolution_clock::time_point startTime;
};

std::deque<Session*> queue;
std::mutex qmx;
std::condition_variable cv;
bool running = true;
std::string schedulingPolicy = "FCFS";

int packetsForResolution(const std::string& res) {
    if (res == "480p") return 50;
    if (res == "720p") return 100;
    return 150; // default 1080p
}

void scheduler(int udpSock) {
    int quantum = 10;
    while (running) {
        Session* s = nullptr;
        {
            std::unique_lock<std::mutex> lk(qmx);
            if (queue.empty()) {
                cv.wait(lk);
                continue;
            }
            s = queue.front();
            queue.pop_front();
        }

        if (!s) continue;

        if (s->sentPackets == 0) {
            s->startTime = std::chrono::high_resolution_clock::now();
        }

        if (s->mode == TCP) {
            int limit = (schedulingPolicy == "FCFS") ?
                        s->packetsToSend :
                        std::min(s->sentPackets + quantum, s->packetsToSend);

            for (; s->sentPackets < limit; s->sentPackets++) {
                Packet pkt{};
                pkt.seq = s->sentPackets + 1;
                strcpy(pkt.payload, "VIDEO_PACKET_TCP");
                send(s->sock, (char*)&pkt, sizeof(pkt), 0);
                std::cout << "[" << schedulingPolicy << "] Sent TCP packet #" << pkt.seq
                          << " for " << s->resolution << std::endl;
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }

            if (s->sentPackets >= s->packetsToSend) {
                auto endTime = std::chrono::high_resolution_clock::now();
                double duration = std::chrono::duration<double>(endTime - s->startTime).count();
                std::cout << "[TCP] Finished stream for " << s->resolution << std::endl;
                close(s->sock);
                delete s;
            } else {
                std::lock_guard<std::mutex> lk(qmx);
                queue.push_back(s);
                cv.notify_one();
            }
        } else { // UDP
            int limit = (schedulingPolicy == "FCFS") ?
                        s->packetsToSend :
                        std::min(s->sentPackets + quantum, s->packetsToSend);

            int sentNow = 0;
            for (; s->sentPackets < limit; s->sentPackets++) {
                Packet pkt{};
                pkt.seq = s->sentPackets + 1;
                strcpy(pkt.payload, "VIDEO_PACKET_UDP");
                if (rand() % 100 < 90) {
                    sendto(udpSock, (char*)&pkt, sizeof(pkt), 0,
                           (sockaddr*)&s->addr, s->addrlen);
                    sentNow++;
                }
                std::cout << "[" << schedulingPolicy << "] Sent UDP packet #" << pkt.seq
                          << " for " << s->resolution << std::endl;
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }

            if (s->sentPackets >= s->packetsToSend) {
                auto endTime = std::chrono::high_resolution_clock::now();
                double duration = std::chrono::duration<double>(endTime - s->startTime).count();
                std::cout << "[UDP] Finished stream for " << s->resolution << std::endl;

                Packet endPkt{};
                endPkt.seq = -1;
                strcpy(endPkt.payload, "END");
                sendto(udpSock, (char*)&endPkt, sizeof(endPkt), 0,
                       (sockaddr*)&s->addr, s->addrlen);

                delete s;
            } else {
                std::lock_guard<std::mutex> lk(qmx);
                queue.push_back(s);
                cv.notify_one();
            }
        }
    }
}

void negotiationHandler(int cli, int tcpPort, int udpPort) {
    CtrlMsg req{};
    recv(cli, (char*)&req, sizeof(req), 0);

    std::string res(req.data);
    if (res != "480p" && res != "720p" && res != "1080p") res = "720p";

    CtrlMsg resp{};
    resp.type = 2;
    std::string msg = "OK RES=" + res + " TCP=" + std::to_string(tcpPort) +
                      " UDP=" + std::to_string(udpPort);
    resp.length = msg.size();
    strcpy(resp.data, msg.c_str());
    send(cli, (char*)&resp, sizeof(resp), 0);
    close(cli);
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "Usage: server <port> <FCFS|RR>\n";
        return 1;
    }
    int basePort = atoi(argv[1]);
    schedulingPolicy = argv[2];

    int negSock = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(basePort);
    bind(negSock, (sockaddr*)&addr, sizeof(addr));
    listen(negSock, 5);

    int tcpSock = socket(AF_INET, SOCK_STREAM, 0);
    addr.sin_port = htons(basePort+1);
    bind(tcpSock, (sockaddr*)&addr, sizeof(addr));
    listen(tcpSock, 5);

    int udpSock = socket(AF_INET, SOCK_DGRAM, 0);
    addr.sin_port = htons(basePort+2);
    bind(udpSock, (sockaddr*)&addr, sizeof(addr));

    std::thread(scheduler, udpSock).detach();

    std::thread([&]{
        while (running) {
            int cli = accept(negSock, nullptr, nullptr);
            if (cli >= 0)
                std::thread(negotiationHandler, cli, basePort+1, basePort+2).detach();
        }
    }).detach();

    std::thread([&]{
        while (running) {
            int cli = accept(tcpSock, nullptr, nullptr);
            if (cli < 0) continue;
            char buf[128];
            int n = recv(cli, buf, sizeof(buf)-1, 0);
            if (n > 0) {
                buf[n] = '\0';
                std::string resolution(buf);
                Session* s = new Session{cli, {}, 0, TCP, resolution, packetsForResolution(resolution)};
                {
                    std::lock_guard<std::mutex> lk(qmx);
                    queue.push_back(s);
                    cv.notify_one();
                }
            }
        }
    }).detach();

    std::thread([&]{
        while (running) {
            sockaddr_in caddr{}; socklen_t clen = sizeof(caddr);
            char buf[128];
            int n = recvfrom(udpSock, buf, sizeof(buf)-1, 0, (sockaddr*)&caddr, &clen);
            if (n > 0) {
                buf[n] = '\0';
                std::string resolution(buf);
                Session* s = new Session{-1, caddr, clen, UDP, resolution, packetsForResolution(resolution)};
                {
                    std::lock_guard<std::mutex> lk(qmx);
                    queue.push_back(s);
                    cv.notify_one();
                }
            }
        }
    }).detach();

    std::cout << "Server running. Negotiation port " << basePort
              << " TCP stream " << basePort+1
              << " UDP stream " << basePort+2
              << " Policy=" << schedulingPolicy << std::endl;

    while (running) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    close(negSock);
    close(tcpSock);
    close(udpSock);
    return 0;
}
