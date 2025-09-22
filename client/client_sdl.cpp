#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <atomic>
#include <thread>
#include <mutex>
#include <chrono>
#include <vector>

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

#include <SDL.h>
#include "../common/protocol.hpp"

static bool set_tcp_nodelay(int s){
#ifdef _WIN32
  char yes = 1;
#else
  int yes = 1;
#endif
  return setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes)) == 0;
}
static uint64_t now_ms(){
  return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::system_clock::now().time_since_epoch()).count();
}

int main(int argc,char** argv){
#ifdef _WIN32
  WSADATA wsa; WSAStartup(MAKEWORD(2,2), &wsa);
#endif
  const char* host = (argc>=2)? argv[1] : "127.0.0.1";
  int port = (argc>=4 && std::string(argv[2])=="--port")? std::atoi(argv[3]) : 7777;
  std::string name = (argc>=6 && std::string(argv[4])=="--name")? argv[5] : "Player";

  // ---- connect ----
  int s = socket(AF_INET, SOCK_STREAM, 0);
  if (s<0){ perror("[cli] socket"); return 1; }
  sockaddr_in a{}; a.sin_family=AF_INET; a.sin_port=htons(port);
  if (inet_pton(AF_INET, host, &a.sin_addr) <= 0){ perror("[cli] inet_pton"); return 1; }
  if (connect(s,(sockaddr*)&a,sizeof(a))<0){ perror("[cli] connect"); return 1; }
  set_tcp_nodelay(s);
  printf("[cli] connected to %s:%d\n", host, port);

  // ---- handshake ----
  MsgHeader h{};
  if (!recv_header(s,h) || h.type!=S_HELLO || h.size!=sizeof(SHello)){ printf("[cli] expected S_HELLO\n"); return 1; }
  SHello sh{}; if (!recv_payload(s,sh)){ printf("[cli] read SHello fail\n"); return 1; }
  CHello ch{}; std::memset(ch.name,0,sizeof(ch.name));
  std::snprintf(ch.name,sizeof(ch.name),"%s", name.c_str());
  if (!send_msg(s, C_HELLO, ch)){ printf("[cli] send CHello fail\n"); return 1; }

  // ---- SDL init ----
  if (SDL_Init(SDL_INIT_VIDEO|SDL_INIT_EVENTS)!=0){ printf("SDL_Init: %s\n", SDL_GetError()); return 1; }
  const int WIN_W=800, WIN_H=450;
  SDL_Window* win = SDL_CreateWindow("Pong (SDL client)",
    SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, WIN_W, WIN_H, SDL_WINDOW_SHOWN);
  SDL_Renderer* ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  if (!win || !ren){ printf("SDL window/renderer error: %s\n", SDL_GetError()); return 1; }

  // shared state from network
  std::mutex mtx;
  SState latest{};             // last authoritative state
  std::atomic<bool> running{true};

  // ---- RX thread ----
  std::thread rx([&](){
    while (running.load()){
      MsgHeader hh{};
      if (!recv_header(s, hh)){ printf("[cli] server closed\n"); running.store(false); break; }
      if (hh.type==S_STATE && hh.size==sizeof(SState)){
        SState st{}; if (!recv_payload(s,st)){ running.store(false); break; }
        std::lock_guard<std::mutex> lk(mtx); latest = st;
      }else if (hh.type==S_PONG && hh.size==sizeof(SPong)){
        SPong p{}; if (!recv_payload(s,p)){ running.store(false); break; }
        (void)p; // (optional) compute/print RTT
      }else{
        std::vector<char> junk(hh.size);
        if (!recv_all(s, junk.data(), (int)junk.size())){ running.store(false); break; }
      }
    }
  });

  // ---- input + render loop ----
  uint8_t buttons=0; uint32_t inputSeq=0;
  auto lastPing = std::chrono::steady_clock::now();
  auto lastInput= std::chrono::steady_clock::now();

  while (running.load()){
    // input
    SDL_Event e;
    while (SDL_PollEvent(&e)){
      if (e.type==SDL_QUIT) running.store(false);
      if (e.type==SDL_KEYDOWN || e.type==SDL_KEYUP){
        bool down = (e.type==SDL_KEYDOWN);
        if (e.key.keysym.sym==SDLK_UP)    { if (down) buttons |= BTN_UP;   else buttons &= ~BTN_UP; }
        if (e.key.keysym.sym==SDLK_DOWN)  { if (down) buttons |= BTN_DOWN; else buttons &= ~BTN_DOWN; }
      }
    }

    // periodic ping (~3s)
    auto now = std::chrono::steady_clock::now();
    if (now - lastPing > std::chrono::seconds(3)){
      CPing ping{ now_ms() }; send_msg(s, C_PING, ping); lastPing = now;
    }
    // send inputs ~60Hz
    if (now - lastInput > std::chrono::milliseconds(16)){
      CInput in{ buttons, ++inputSeq }; send_msg(s, C_INPUT, in); lastInput = now;
    }

    // snapshot for render
    SState st{}; { std::lock_guard<std::mutex> lk(mtx); st = latest; }

    // render
    SDL_SetRenderDrawColor(ren, 18,18,20,255); SDL_RenderClear(ren);

    // ball (12x12)
    SDL_SetRenderDrawColor(ren, 240,240,240,255);
    SDL_Rect ball{ (int)(st.ballX - 6), (int)(st.ballY - 6), 12, 12 };
    SDL_RenderFillRect(ren, &ball);

    // paddles (10x80)
    const int PW=10, PH=80;
    SDL_Rect lp{ 10, (int)(st.paddleY[0] - PH/2), PW, PH };
    SDL_Rect rp{ WIN_W-10-PW, (int)(st.paddleY[1] - PH/2), PW, PH };
    SDL_RenderFillRect(ren, &lp);
    SDL_RenderFillRect(ren, &rp);

    // center dashed line
    SDL_SetRenderDrawColor(ren, 80,80,80,255);
    for (int y=0; y<WIN_H; y+=20){ SDL_Rect d{ WIN_W/2-1, y, 2, 10 }; SDL_RenderFillRect(ren, &d); }

    SDL_RenderPresent(ren);
  }

  running.store(false);
  if (rx.joinable()) rx.join();
  closesocket(s);
  SDL_DestroyRenderer(ren); SDL_DestroyWindow(win); SDL_Quit();
#ifdef _WIN32
  WSACleanup();
#endif
  return 0;
}
