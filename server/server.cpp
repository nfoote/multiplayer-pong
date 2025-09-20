// server/server.cpp
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <chrono>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using socklen_t = int;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#define closesocket close
#endif

#include "../common/protocol.hpp"

static bool set_tcp_nodelay(int s) {
#ifdef _WIN32
    char yes = 1;
#else
    int yes = 1;
#endif
    return setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes)) == 0;
}

static uint64_t now_unix_ms() {
    return (uint64_t) std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

static uint32_t now_steady_ms() {
    return (uint32_t) std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

int main(int argc, char **argv) {
#ifdef _WIN32
    WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

    int port = 7777;
    if (argc >= 3 && std::string(argv[1]) == "--port") port = std::atoi(argv[2]);

    // listening socket
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls < 0) {
        perror("[srv] socket");
        return 1;
    }

    int opt = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (char *) &opt, sizeof(opt));

    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(port);

    if (bind(ls, (sockaddr *) &a, sizeof(a)) < 0) {
        perror("[srv] bind");
        return 1;
    }
    if (listen(ls, 2) < 0) {
        perror("[srv] listen");
        return 1;
    }

    printf("[srv] listening on 0.0.0.0:%d\n", port);

    // accept exactly two clients
    std::vector<int> clients;
    clients.reserve(2);
    while ((int) clients.size() < 2) {
        sockaddr_in cli{};
        socklen_t cl = sizeof(cli);
        int s = accept(ls, (sockaddr *) &cli, &cl);
        if (s < 0) {
            perror("[srv] accept");
            return 1;
        }
        set_tcp_nodelay(s);

        char ip[64];
        inet_ntop(AF_INET, &cli.sin_addr, ip, sizeof(ip));
        printf("[srv] client %zu connected: %s:%d (TCP_NODELAY=on)\n",
               clients.size(), ip, ntohs(cli.sin_port));

        // greet
        send_msg(s, S_HELLO, SHello{now_steady_ms()});

        // optional CHello
        MsgHeader h{};
        if (recv_header(s, h) && h.type == C_HELLO && h.size == sizeof(CHello)) {
            CHello ch{};
            if (recv_payload(s, ch)) {
                printf("[srv]   name='%.*s'\n", (int) sizeof(ch.name), ch.name);
            }
        } else {
            printf("[srv]   (no CHello)\n");
        }

        clients.push_back(s);
    }

    printf("[srv] two clients connected, starting 1 Hz broadcast + ping handler\n");

    uint32_t tick = 0;
    while (true) {
        // --- 1) Handle any incoming messages (non-blocking select) -------------
        fd_set readfds;
        FD_ZERO(&readfds);
        int maxfd = -1;
        for (int c: clients) {
            FD_SET(c, &readfds);
            if (c > maxfd) maxfd = c;
        }
        timeval tv{0, 0}; // poll, don't block
        int ready = select(maxfd + 1, &readfds, nullptr, nullptr, &tv);

        if (ready > 0) {
            for (int c: clients) {
                if (!FD_ISSET(c, &readfds)) continue;

                MsgHeader h{};
                if (!recv_header(c, h)) {
                    printf("[srv] client disconnected; shutting down demo\n");
                    closesocket(c);
                    return 0; // (tutorial: exit; production: remove this client and continue)
                }

                if (h.type == C_PING && h.size == sizeof(CPing)) {
                    CPing ping{};
                    if (!recv_payload(c, ping)) continue;
                    SPong pong{ping.clientSendMs, now_unix_ms()};
                    send_msg(c, S_PONG, pong);
                    printf("[srv] handled C_PING -> S_PONG\n");
                } else {
                    // drain unknown payload
                    std::vector<char> junk(h.size);
                    if (!recv_all(c, junk.data(), (int) junk.size())) {
                        printf("[srv] payload read error; closing client\n");
                        closesocket(c);
                        return 0;
                    }
                }
            }
        }

        // --- 2) Broadcast server tick once per second ---------------------------
        tick++;
        SBroadcast b{tick, now_unix_ms()};
        for (int c: clients) {
            if (!send_msg(c, S_BROADCAST, b)) {
                printf("[srv] send failed; closing\n");
                closesocket(c);
                return 0;
            }
        }
        printf("[srv] broadcast tick=%u unixMs=%llu\n",
               tick, (unsigned long long) b.serverUnixMs);

        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}
