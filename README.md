# multiplayer-pong

A personal project to implement multiplayer Pong in C++ over TCP.
- Uses raw TCP sockets (socket, bind, listen, accept, connect).
- Explores how client/server protocols are structured with headers and payloads.
- Includes connection handshakes, periodic state updates, latency measurement, and an authoritative game loop.
- Experiments with rendering and input sync using SDL2.

## Build
```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug
cmake --build .
