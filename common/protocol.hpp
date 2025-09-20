#pragma once
#include <cstdint>
#include <vector>

#pragma pack(push,1)
struct MsgHeader {
    uint8_t type;
    uint16_t size;
};

enum : uint8_t {
    S_HELLO = 1, // server -> client greeting
    S_BROADCAST = 2, // server -> client tick
    C_HELLO = 10, // client -> server greeting
    C_PING = 4,
    S_PONG = 5,
    C_INPUT = 20, // client -> server paddle input
    S_STATE = 21 // server -> client authoritative state
};

#pragma pack(push,1)
struct CInput {
    uint8_t  buttons;   // bit0=UP, bit1=DOWN
    uint32_t seq;       // client-side counter for debugging
};

struct SState {
    uint32_t tick;
    float    ballX, ballY;
    float    ballVX, ballVY;
    float    paddleY[2];  // [0]=left, [1]=right
};
#pragma pack(pop)

#pragma pack(push,1)
struct CPing {
    uint64_t clientSendMs;
};

struct SPong {
    uint64_t clientSendMs;
    uint64_t serverRecvMs;
};
#pragma pack(pop)


struct SHello {
    uint32_t serverStartMs;
};

struct SBroadcast {
    uint32_t tick;
    uint64_t serverUnixMs;
};

struct CHello {
    char name[16];
};
#pragma pack(pop)

static constexpr uint8_t BTN_UP   = 1 << 0;
static constexpr uint8_t BTN_DOWN = 1 << 1;

// ----- exact-write helpers over TCP -----
#ifdef _WIN32
#include <winsock2.h>
#pragma comment(lib, "Ws2_32.lib")
#else
#include <sys/socket.h>
#include <unistd.h>
#endif

inline bool send_all(int s, const void *d, int len) {
    const char *p = (const char *) d;
    int left = len;
    while (left > 0) {
        int n = send(s, p, left, 0);
        if (n <= 0) return false;
        p += n;
        left -= n;
    }
    return true;
}

inline bool recv_all(int s, void *d, int len) {
    char *p = (char *) d;
    int left = len;
    while (left > 0) {
        int n = recv(s, p, left, 0);
        if (n <= 0) return false;
        p += n;
        left -= n;
    }
    return true;
}

inline bool send_header(int s, const MsgHeader &h) { return send_all(s, &h, sizeof(h)); }

template<class T>
inline bool send_msg(int s, uint8_t type, const T &payload) {
    MsgHeader h{type, static_cast<uint16_t>(sizeof(T))};
    return send_header(s, h) && send_all(s, &payload, sizeof(T));
}

inline bool recv_header(int s, MsgHeader &h) { return recv_all(s, &h, sizeof(h)); }

template<class T>
inline bool recv_payload(int s, T &out) { return recv_all(s, &out, sizeof(T)); }
