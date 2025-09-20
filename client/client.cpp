#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <chrono>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
// (keep winsock2.h included BEFORE windows.h if you ever add it)
using socklen_t = int;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>   // <-- add this
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

int main(int argc, char **argv) {
#ifdef _WIN32
    WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

    static constexpr uint8_t BTN_UP = 1 << 0;
    static constexpr uint8_t BTN_DOWN = 1 << 1;

    const char *host = (argc >= 2) ? argv[1] : "127.0.0.1";
    int port = (argc >= 4 && std::string(argv[2]) == "--port") ? std::atoi(argv[3]) : 7777;
    std::string name = (argc >= 6 && std::string(argv[4]) == "--name") ? argv[5] : "Player";

    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) {
        perror("[cli] socket");
        return 1;
    }

    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &a.sin_addr) <= 0) {
        perror("[cli] inet_pton");
        return 1;
    }
    if (connect(s, (sockaddr *) &a, sizeof(a)) < 0) {
        perror("[cli] connect");
        return 1;
    }
    set_tcp_nodelay(s);

    printf("[cli] connected to %s:%d (TCP_NODELAY=on)\n", host, port);

    // Expect S_HELLO
    MsgHeader h{};
    if (!recv_header(s, h) || h.type != S_HELLO || h.size != sizeof(SHello)) {
        printf("[cli] expected S_HELLO, got something else\n");
        return 1;
    }
    SHello sh{};
    if (!recv_payload(s, sh)) {
        printf("[cli] failed to read SHello\n");
        return 1;
    }
    printf("[cli] S_HELLO serverStartMs=%u\n", sh.serverStartMs);

    // Send CHello with our name
    CHello ch{};
    std::memset(ch.name, 0, sizeof(ch.name));
    std::snprintf(ch.name, sizeof(ch.name), "%s", name.c_str());
    if (!send_msg(s, C_HELLO, ch)) {
        printf("[cli] failed to send CHello\n");
        return 1;
    }
    printf("[cli] sent CHello name='%s'\n", name.c_str());

    auto lastPing = std::chrono::steady_clock::now();
    auto lastInput = std::chrono::steady_clock::now();
    uint32_t inputSeq = 0;
    uint8_t buttons = 0;

    // Receive loop
    while (true) {
        MsgHeader h{};
        if (!recv_header(s, h)) {
            printf("[cli] server closed\n");
            break;
        }

        if (h.type == S_BROADCAST && h.size == sizeof(SBroadcast)) {
            SBroadcast b{};
            if (!recv_payload(s, b)) {
                printf("[cli] payload read error\n");
                break;
            }
            printf("[cli] S_BROADCAST: tick=%u serverUnixMs=%llu\n",
                   b.tick, (unsigned long long) b.serverUnixMs);
        } else if (h.type == S_PONG && h.size == sizeof(SPong)) {
            SPong p{};
            if (!recv_payload(s, p)) {
                printf("[cli] payload read error\n");
                break;
            }
            uint64_t nowMs = (uint64_t) std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            uint64_t rtt = nowMs - p.clientSendMs;
            printf("[cli] S_PONG: RTT=%llums (serverRecvMs=%llu)\n",
                   (unsigned long long) rtt,
                   (unsigned long long) p.serverRecvMs);
        }
        else if (h.type == S_STATE && h.size == sizeof(SState)) {
            SState st{};
            if (!recv_payload(s, st)) { printf("[cli] state payload error\n"); break; }
            printf("[cli] tick=%u ball=(%.1f,%.1f) paddles=(L %.1f | R %.1f)\n",
                   st.tick, st.ballX, st.ballY, st.paddleY[0], st.paddleY[1]);
        }
        else {
            std::vector<char> junk(h.size);
            if (!recv_all(s, junk.data(), (int) junk.size())) break;
        }

        // Periodic ping (every ~3s). This relies on receiving something periodically.
        auto now = std::chrono::steady_clock::now();
        if (now - lastPing > std::chrono::seconds(3)) {
            uint64_t nowMs = (uint64_t) std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            CPing ping{nowMs};
            send_msg(s, C_PING, ping);
            lastPing = now;
            printf("[cli] sent C_PING\n");
        }

        // Fake input every 100ms: alternate up/down every 2 seconds
        if (now - lastInput > std::chrono::milliseconds(100)) {
            auto t = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
            if ((t / 2) % 2 == 0) buttons = BTN_UP;
            else buttons = BTN_DOWN;

            CInput ci{buttons, ++inputSeq};
            send_msg(s, C_INPUT, ci);
            lastInput = now;
        }
    }

    closesocket(s);
#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}
