#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <thread>
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

  int port = 7777;
  if (argc >= 3 && std::string(argv[1]) == "--port") port = std::atoi(argv[2]);

  int ls = socket(AF_INET, SOCK_STREAM, 0);
  if (ls < 0) { perror("socket"); return 1; }

  int opt = 1;
  setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));

  sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_ANY); a.sin_port = htons(port);
  if (bind(ls,(sockaddr*)&a,sizeof(a))<0){ perror("bind"); return 1; }
  if (listen(ls, 2) < 0){ perror("listen"); return 1; }

  printf("[srv] listening on 0.0.0.0:%d\n", port);

  std::vector<int> clients; clients.reserve(2);
  while ((int)clients.size() < 2) {
    sockaddr_in cli{}; socklen_t cl = sizeof(cli);
    int s = accept(ls, (sockaddr*)&cli, &cl);
    if (s < 0) { perror("accept"); return 1; }
    set_tcp_nodelay(s);

    char ip[64];
    inet_ntop(AF_INET, &cli.sin_addr, ip, sizeof(ip));
    printf("[srv] client %zu connected: %s:%d (TCP_NODELAY=on)\n",
           clients.size(), ip, ntohs(cli.sin_port));

    // greet with S_HELLO
    uint32_t nowMs = (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
    send_msg(s, S_HELLO, SHello{ nowMs });

    // read client's CHello (optional)
    MsgHeader h{};
    if (recv_header(s, h) && h.type == C_HELLO && h.size == sizeof(CHello)) {
      CHello ch{};
      if (recv_payload(s, ch)) {
        printf("[srv]   name='%.*s'\n", (int)sizeof(ch.name), ch.name);
      }
    } else {
      printf("[srv]   (no CHello)\n");
    }

    clients.push_back(s);
  }

  printf("[srv] two clients connected, starting 1 Hz broadcast\n");
  uint32_t tick = 0;
  while (true) {
    tick++;
    uint64_t unixMs = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
    SBroadcast b{ tick, unixMs };

    for (int c : clients) {
      if (!send_msg(c, S_BROADCAST, b)) {
        printf("[srv] send failed; closing\n");
        closesocket(c);
        return 0;
      }
    }

    printf("[srv] broadcast tick=%u unixMs=%llu\n", tick, (unsigned long long)unixMs);
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }

#ifdef _WIN32
  WSACleanup();
#endif
  return 0;
}
