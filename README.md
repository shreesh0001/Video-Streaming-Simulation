# Video-Streaming-Simulation

# Video Streaming Simulation using TCP, UDP, and Multithreading

This project simulates a video streaming service implemented in **C++** using **POSIX sockets**.  
The simulation supports both **TCP** and **UDP** streaming modes and includes two scheduling policies:  
- **FCFS (First-Come-First-Serve)**  
- **RR (Round-Robin)**  

It measures key performance metrics like throughput, latency, CPU usage, memory usage, and (for UDP) packet loss.

---

## 📌 Features
- Two-phase protocol:
  1. **Negotiation Phase (TCP)** – client requests resolution, server responds with port assignments.
  2. **Streaming Phase (TCP/UDP)** – mock video packets transmitted using chosen protocol.
- Multi-threaded server to handle multiple concurrent clients.
- Scheduling policies: **FCFS** and **Round-Robin**.
- UDP packet loss simulation (~10%).
- Client logs results (throughput, latency, CPU, memory, packet loss) into `results.csv`.

---

## 📂 Project Structure
├── client.cpp # Client program
├── server.cpp # Server program
├── results.csv # Metrics are appended here
└── README.md # This file

---

## ⚙️ Build Instructions

### Compile Server
```bash
g++ -O2 -std=c++17 -pthread -o server server.cpp
g++ -O2 -std=c++17 -pthread -o client client.cpp
Run Instructions
1. Start the Server
./server <BasePort> <FCFS|RR>
./server 5000 FCFS
./client <ServerIP> <ServerPort> <TCP|UDP> <Resolution>
./client 127.0.0.1 5000 TCP 720p
./client 127.0.0.1 5000 UDP 1080p


=== Performance Metrics ===
Mode: TCP  Resolution: 720p
Packets Received: 100
Throughput: 0.163 Mbps
Latency: 0.42 ms
CPU Usage: 0.0 %
Memory Usage: 5 MB
Packet Loss: 0 %


Mode,Resolution,PacketsReceived,Throughput(Mbps),Latency(ms),CPU(%),Memory(MB),PacketLoss(%)
TCP,720p,100,0.1639,0.42,0.0,5,0
UDP,720p,89,0.1455,0.47,0.0,5,11

