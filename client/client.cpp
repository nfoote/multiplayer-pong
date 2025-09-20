#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
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

int main(int argc, char** argv) {
#ifdef _WIN32
  WSADATA wsa; WSAStartup(MAKEWORD(2,2), &wsa);
#endif

  const char* host = (argc >= 2) ? argv[1] : "127.0.0.1";
  int port = (argc >= 4 && std::string(argv[2]) == "--port") ? std::atoi(argv[3]) : 7777;
  std::string name = (argc >= 6 && std::string(argv[4]) == "--name") ? argv[5] : "Player";

  int s = socket(AF_INET, SOCK_STREAM, 0);
  if (s < 0) { perror("[cli] socket"); return 1; }

  sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(port);
  if (inet_pton(AF_INET, host, &a.sin_addr) <= 0) { perror("[cli] inet_pton"); return 1; }
  if (connect(s, (sockaddr*)&a, sizeof(a)) < 0) { perror("[cli] connect"); return 1; }
  set_tcp_nodelay(s);

  printf("[cli] connected to %s:%d (TCP_NODELAY=on)\n", host, port);

  // Expect S_HELLO
  MsgHeader h{};
  if (!recv_header(s, h) || h.type != S_HELLO || h.size != sizeof(SHello)) {
    printf("[cli] expected S_HELLO, got something else\n");
    return 1;
  }
  SHello sh{};
  if (!recv_payload(s, sh)) { printf("[cli] failed to read SHello\n"); return 1; }
  printf("[cli] S_HELLO serverStartMs=%u\n", sh.serverStartMs);

  // Send CHello with our name
  CHello ch{}; std::memset(ch.name, 0, sizeof(ch.name));
  std::snprintf(ch.name, sizeof(ch.name), "%s", name.c_str());
  if (!send_msg(s, C_HELLO, ch)) { printf("[cli] failed to send CHello\n"); return 1; }
  printf("[cli] sent CHello name='%s'\n", name.c_str());

  // Receive loop
  while (true) {
    if (!recv_header(s, h)) { printf("[cli] server closed\n"); break; }

    if (h.type == S_BROADCAST && h.size == sizeof(SBroadcast)) {
      SBroadcast b{};
      if (!recv_payload(s, b)) { printf("[cli] payload read error\n"); break; }
      printf("[cli] S_BROADCAST: tick=%u serverUnixMs=%llu\n",
             b.tick, (unsigned long long)b.serverUnixMs);
    } else {
      // skip unknown message types
      std::vector<char> junk(h.size);
      if (!recv_all(s, junk.data(), (int)junk.size())) break;
    }
  }

  closesocket(s);
#ifdef _WIN32
  WSACleanup();
#endif
  return 0;
}
