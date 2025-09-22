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

    // Game constants
    const float W = 800.f, H = 450.f; // world size
    const float PADDLE_H = 80.f, PADDLE_W = 10.f;
    const float BALL_R = 6.f;
    const float PADDLE_SPEED = 260.f; // px/s
    const float BALL_SPEED = 260.f;

    // Authoritative state
    SState st{};
    st.tick = 0;
    st.ballX = W * 0.5f;
    st.ballY = H * 0.5f;
    st.ballVX = BALL_SPEED;
    st.ballVY = BALL_SPEED * 0.6f;
    st.paddleY[0] = H * 0.5f; // center
    st.paddleY[1] = H * 0.5f;

    // Per-client input latch (0 or BTN_UP/DOWN or both)
    uint8_t inputs[2] = {0, 0};

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

    auto nextTick = std::chrono::steady_clock::now();
    const auto dt = std::chrono::milliseconds(16); // ~60Hz
    auto nextBroadcast = nextTick; // broadcast at 20Hz
    constexpr int broadcastEvery = 3; // every 3 ticks (~20Hz)

    for (;;) {
        // ---- 1) Poll inputs often (≤ 2 ms) ----
        fd_set rfds;
        FD_ZERO(&rfds);
        int maxfd = -1;
        for (int c: clients) {
            FD_SET(c, &rfds);
            if (c > maxfd) maxfd = c;
        }

        auto now_for_poll = std::chrono::steady_clock::now();
        auto remain = nextTick - now_for_poll;

        if (remain > std::chrono::milliseconds(2)) remain = std::chrono::milliseconds(2);
        if (remain < std::chrono::milliseconds(0)) remain = std::chrono::milliseconds(0);

        long usec = std::chrono::duration_cast<std::chrono::microseconds>(remain).count();
        timeval tv{(int) (usec / 1000000), (int) (usec % 1000000)};

        int ready = select(maxfd + 1, &rfds, nullptr, nullptr, &tv);
        if (ready > 0) {
            for (size_t i = 0; i < clients.size();) {
                int c = clients[i];
                if (!FD_ISSET(c, &rfds)) {
                    ++i;
                    continue;
                }

                MsgHeader h{};
                if (!recv_header(c, h)) {
                    printf("[srv] client[%zu] disconnected\n", i);
                    closesocket(c);
                    clients.erase(clients.begin() + (long) i);
                    continue;
                }

                if (h.type == C_PING && h.size == sizeof(CPing)) {
                    CPing p{};
                    if (recv_payload(c, p)) {
                        SPong q{p.clientSendMs, now_unix_ms()};
                        send_msg(c, S_PONG, q);
                    } else {
                        closesocket(c);
                        clients.erase(clients.begin() + (long) i);
                        continue;
                    }
                } else if (h.type == C_INPUT && h.size == sizeof(CInput)) {
                    CInput ci{};
                    if (!recv_payload(c, ci)) {
                        closesocket(c);
                        clients.erase(clients.begin() + (long) i);
                        continue;
                    }
                    size_t playerIndex = i;
                    if (playerIndex > 1) playerIndex = 1;
                    inputs[playerIndex] = ci.buttons;
                } else {
                    std::vector<char> junk(h.size);
                    if (!recv_all(c, junk.data(), (int) junk.size())) {
                        closesocket(c);
                        clients.erase(clients.begin() + (long) i);
                        continue;
                    }
                }
                ++i;
            }
        }

        // ---- 2) Fixed tick ----
        auto now = std::chrono::steady_clock::now();
        if (now < nextTick) continue;
        nextTick += dt;
        st.tick++;

        // Apply inputs to paddles
        const float PADDLE_SPEED = 260.f, H = 450.f, PADDLE_H = 80.f;
        for (int p = 0; p < 2 && p < (int) clients.size(); ++p) {
            float dir = 0.f;
            if (inputs[p] & BTN_UP) dir -= 1.f;
            if (inputs[p] & BTN_DOWN) dir += 1.f;
            st.paddleY[p] += dir * PADDLE_SPEED * (16.0f / 1000.f);
            if (st.paddleY[p] < PADDLE_H * 0.5f) st.paddleY[p] = PADDLE_H * 0.5f;
            if (st.paddleY[p] > H - PADDLE_H * 0.5f) st.paddleY[p] = H - PADDLE_H * 0.5f;
        }

        // Move ball + simple collisions
        const float W = 800.f, BALL_R = 6.f;
        st.ballX += st.ballVX * (16.0f / 1000.f);
        st.ballY += st.ballVY * (16.0f / 1000.f);

        if (st.ballY < BALL_R) {
            st.ballY = BALL_R;
            st.ballVY = -st.ballVY;
        }
        if (st.ballY > H - BALL_R) {
            st.ballY = H - BALL_R;
            st.ballVY = -st.ballVY;
        }

        auto collidePaddle = [&](float px, float pyCenter, int side) {
            const float PADDLE_W = 10.f;
            const float halfH = PADDLE_H * 0.5f;
            float left = px - PADDLE_W * 0.5f, right = px + PADDLE_W * 0.5f;
            float top = pyCenter - halfH, bot = pyCenter + halfH;
            if (st.ballX + BALL_R < left || st.ballX - BALL_R > right) return false;
            if (st.ballY + BALL_R < top || st.ballY - BALL_R > bot) return false;
            st.ballX = (side == 0) ? right + BALL_R : left - BALL_R;
            st.ballVX = (side == 0) ? std::abs(st.ballVX) : -std::abs(st.ballVX);
            float t = (st.ballY - pyCenter) / halfH;
            st.ballVY += t * 60.f;
            return true;
        };
        collidePaddle(20.f, st.paddleY[0], 0);
        collidePaddle(W - 20.f, st.paddleY[1], 1);

        if (st.ballX < -20.f || st.ballX > W + 20.f) {
            const float BALL_SPEED = 260.f;
            st.ballX = W * 0.5f;
            st.ballY = H * 0.5f;
            st.ballVX = (st.ballVX < 0 ? 1.f : -1.f) * BALL_SPEED;
            st.ballVY = BALL_SPEED * 0.6f;
        }

        // ---- 3) Broadcast ~20 Hz ----
        if ((st.tick % broadcastEvery) == 0) {
            for (size_t i = 0; i < clients.size();) {
                int c = clients[i];
                if (!send_msg(c, S_STATE, st)) {
                    closesocket(c);
                    clients.erase(clients.begin() + (long) i);
                    continue;
                }
                ++i;
            }
        }
    }


#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}
