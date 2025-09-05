// client.cpp - Linux / POSIX conversion of your Windows client
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h>

#include <cstring>
#include <iostream>
#include <fstream>
#include <string>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <sstream>

struct CtrlMsg { int type; int length; char data[256]; };
struct Packet { int seq; char payload[1024]; };

using hrclock = std::chrono::high_resolution_clock;
using time_point = std::chrono::high_resolution_clock::time_point;

// Read total jiffies from /proc/stat (sum of fields after "cpu")
unsigned long long readTotalJiffies() {
    std::ifstream f("/proc/stat");
    if (!f) return 0;
    std::string line;
    std::getline(f, line);
    std::istringstream iss(line);
    std::string cpu;
    iss >> cpu; // "cpu"
    unsigned long long val, total = 0;
    while (iss >> val) total += val;
    return total;
}

// Read process utime + stime from /proc/self/stat (fields 14 and 15)
unsigned long long readProcessJiffies() {
    std::ifstream f("/proc/self/stat");
    if (!f) return 0;
    std::string content;
    std::getline(f, content);
    std::istringstream iss(content);
    // fields: pid (1) comm (2) state (3) ... utime(14) stime(15)
    std::string tmp;
    for (int i = 0; i < 13; ++i) iss >> tmp; // skip first 13 fields
    unsigned long long utime = 0, stime = 0;
    iss >> utime >> stime;
    return utime + stime;
}

// Convert pages to MB using page size
size_t getMemoryUsageMB() {
    std::ifstream f("/proc/self/statm");
    if (!f) return 0;
    unsigned long long sizePages = 0;
    f >> sizePages;
    long pageSize = sysconf(_SC_PAGESIZE);
    unsigned long long bytes = sizePages * (unsigned long long)pageSize;
    return static_cast<size_t>(bytes / 1024 / 1024);
}

// Compute overall CPU % between two snapshots (proc and total jiffies)
double computeCPUPercent(unsigned long long startProc, unsigned long long startTotal,
                         unsigned long long endProc, unsigned long long endTotal) {
    unsigned long long procDelta = (endProc >= startProc) ? (endProc - startProc) : 0;
    unsigned long long totalDelta = (endTotal >= startTotal) ? (endTotal - startTotal) : 0;
    if (totalDelta == 0) return 0.0;
    // procDelta and totalDelta are in jiffies; percentage:
    return (100.0 * (double)procDelta) / (double)totalDelta;
}

int tcpConnect(const char* ip, int port) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) {
        perror("socket");
        exit(1);
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) <= 0) {
        std::cerr << "Invalid address: " << ip << "\n";
        close(s);
        exit(1);
    }
    if (connect(s, (sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(s);
        exit(1);
    }
    return s;
}

int main(int argc, char* argv[]) {
    if (argc < 5) {
        std::cerr << "Usage: client <ServerIP> <ServerPort> <TCP|UDP> <Resolution>\n";
        return 1;
    }

    const char* ip = argv[1];
    int port = atoi(argv[2]);
    std::string mode = argv[3];
    std::string res = argv[4];

    // 1) Negotiation: connect to base port and send CtrlMsg
    int negSock = tcpConnect(ip, port);
    CtrlMsg req{};
    req.type = 1;
    req.length = static_cast<int>(res.size());
    memset(req.data, 0, sizeof(req.data));
    strncpy(req.data, res.c_str(), sizeof(req.data)-1);

    if (send(negSock, (char*)&req, sizeof(req), 0) <= 0) {
        perror("send negotiation");
        close(negSock);
        return 1;
    }

    CtrlMsg resp{};
    ssize_t rn = recv(negSock, (char*)&resp, sizeof(resp), 0);
    if (rn <= 0) {
        perror("recv negotiation");
        close(negSock);
        return 1;
    }
    close(negSock);

    std::cout << "Server response: " << resp.data << std::endl;

    int tcpPort = port + 1;
    int udpPort = port + 2;

    // Performance vars
    time_point overallStart = hrclock::now();
    time_point firstPktTime{};
    size_t totalBytes = 0;
    int packetsReceived = 0;
    int highestSeq = 0;

    // snapshots for CPU percent
    unsigned long long startProcJ = readProcessJiffies();
    unsigned long long startTotalJ = readTotalJiffies();

    if (mode == "TCP") {
        int ts = tcpConnect(ip, tcpPort);
        // send chosen resolution string (like Windows client)
        if (send(ts, res.c_str(), static_cast<int>(res.size()), 0) <= 0) {
            perror("send TCP resolution");
            close(ts);
            return 1;
        }

        Packet pkt{};
        while (true) {
            ssize_t n = recv(ts, (char*)&pkt, sizeof(pkt), 0);
            if (n <= 0) break;
            if (packetsReceived == 0) firstPktTime = hrclock::now();
            packetsReceived++;
            totalBytes += (size_t)n;
            std::cout << "Received TCP packet #" << pkt.seq << " [" << pkt.payload << "]\n";
        }
        close(ts);
    } else { // UDP
        int us = socket(AF_INET, SOCK_DGRAM, 0);
        if (us < 0) { perror("socket udp"); return 1; }

        sockaddr_in srv{};
        srv.sin_family = AF_INET;
        srv.sin_port = htons(udpPort);
        if (inet_pton(AF_INET, ip, &srv.sin_addr) <= 0) {
            std::cerr << "Invalid address: " << ip << "\n";
            close(us);
            return 1;
        }

        // send resolution to server
        if (sendto(us, res.c_str(), static_cast<int>(res.size()), 0, (sockaddr*)&srv, sizeof(srv)) < 0) {
            perror("sendto");
            close(us);
            return 1;
        }

        Packet pkt{};
        sockaddr_in from{};
        socklen_t flen = sizeof(from);
        // Make socket non-blocking with a timeout to avoid infinite hang if server dies
        struct timeval tv;
        tv.tv_sec = 10; tv.tv_usec = 0; // 10s recv timeout
        setsockopt(us, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv);

        while (true) {
            ssize_t n = recvfrom(us, (char*)&pkt, sizeof(pkt), 0, (sockaddr*)&from, &flen);
            if (n < 0) {
                // timeout or error
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    std::cout << "UDP recv timeout reached, assuming end.\n";
                    break;
                } else {
                    perror("recvfrom");
                    break;
                }
            }
            if (n > 0) {
                if (packetsReceived == 0) firstPktTime = hrclock::now();
                // detect END packet
                if (pkt.seq == -1 && strncmp(pkt.payload, "END", 3) == 0) {
                    std::cout << "UDP stream ended by server.\n";
                    break;
                }
                packetsReceived++;
                totalBytes += (size_t)n;
                if (pkt.seq > highestSeq) highestSeq = pkt.seq;
                std::cout << "Received UDP packet #" << pkt.seq << " [" << pkt.payload << "]\n";
            }
        }
        close(us);
    }

    time_point overallEnd = hrclock::now();
    unsigned long long endProcJ = readProcessJiffies();
    unsigned long long endTotalJ = readTotalJiffies();

    double duration = std::chrono::duration<double>(overallEnd - overallStart).count(); // seconds
    double latency = 0.0;
    if (firstPktTime.time_since_epoch().count() != 0) {
        latency = std::chrono::duration<double>(firstPktTime - overallStart).count() * 1000.0; // ms
    }
    double throughput = (duration > 0.0) ? (totalBytes * 8.0) / (duration * 1e6) : 0.0; // Mbps
    size_t memUsage = getMemoryUsageMB();
    double overallCPU = computeCPUPercent(startProcJ, startTotalJ, endProcJ, endTotalJ);

    double lossPercent = 0.0;
    if (mode == "UDP" && highestSeq > 0) {
        lossPercent = 100.0 * (1.0 - ((double)packetsReceived / (double)highestSeq));
    }

    std::cout << "\n=== Performance Metrics ===\n";
    std::cout << "Mode: " << mode << "  Resolution: " << res << "\n";
    std::cout << "Packets Received: " << packetsReceived << "\n";
    std::cout << "Throughput: " << throughput << " Mbps\n";
    std::cout << "Latency (time-to-first-packet): " << latency << " ms\n";
    std::cout << "Overall CPU Usage (proc/total): " << overallCPU << " %\n";
    std::cout << "Memory Usage: " << memUsage << " MB\n";
    if (mode == "UDP") std::cout << "Packet Loss: " << lossPercent << " %\n";

    // Save results to CSV (append)
    std::ofstream fout("results.csv", std::ios::app);
    bool writeHeader = false;
    if (!fout.good()) {
        std::cerr << "Cannot open results.csv for writing\n";
    } else {
        // determine if file is empty -> write header
        fout.seekp(0, std::ios::end);
        if (fout.tellp() == 0) {
            writeHeader = true;
        }
        if (writeHeader) {
            fout << "Mode,Resolution,PacketsReceived,Throughput(Mbps),Latency(ms),CPU(%),Memory(MB),PacketLoss(%)\n";
        }
        fout << mode << "," << res << ","
             << packetsReceived << "," << throughput << ","
             << latency << "," << overallCPU << ","
             << memUsage << "," << (mode == "UDP" ? lossPercent : 0.0) << "\n";
        fout.close();
        std::cout << "Metrics saved to results.csv\n";
    }

    return 0;
}
