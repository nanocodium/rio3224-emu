// Small portability layer: the emulator core is plain C++20; only the optional TCP bridges for the Dante module
// UARTs, the interactive console and the TAP network need OS calls.  Linux/WSL and Windows (MSVC or MinGW) are supported.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cerrno>

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <windows.h>
  #include <io.h>
  #include <conio.h>
  typedef intptr_t ssize_t;
  #ifndef MSG_NOSIGNAL
    #define MSG_NOSIGNAL 0
  #endif
  inline void platformInit() { WSADATA w; WSAStartup(MAKEWORD(2, 2), &w); }
  inline void sockClose(int s) { closesocket((SOCKET)s); }
  inline void sockNonBlocking(int s) { u_long one = 1; ioctlsocket((SOCKET)s, FIONBIO, &one); }
  inline bool sockWouldBlock() { int e = WSAGetLastError(); return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS; }
  inline const char* sockError() { static char b[64]; snprintf(b, sizeof b, "winsock error %d", WSAGetLastError()); return b; }
  inline bool stdinIsTty() { return _isatty(_fileno(stdin)) != 0; }
#else
  #include <unistd.h>
  #include <fcntl.h>
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <arpa/inet.h>
  inline void platformInit() {}
  inline void sockClose(int s) { close(s); }
  inline void sockNonBlocking(int s) { fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK); }
  inline bool sockWouldBlock() { return errno == EAGAIN || errno == EWOULDBLOCK; }
  inline const char* sockError() { return strerror(errno); }
  inline bool stdinIsTty() { return isatty(0) != 0; }
#endif

#ifndef M_PI
  #define M_PI 3.14159265358979323846
#endif
