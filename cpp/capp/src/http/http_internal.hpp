// HTTP 层内部共享（只有 capp/src/http/*.cpp 用；对外接口在 capp/http_server.hpp）
//
// 这四个是连接与 TLS 的底层零件：ClientConn 的读写和 HttpServer 的 TLS 建立/accept
// 都要用，所以从 http_server.cpp 里提出来共享，而不是各自 static 一份。

#pragma once

#include <string>

namespace capp {

int tsl_bio_send(void* ctx, const unsigned char* buf, size_t len);
int tsl_bio_recv(void* ctx, unsigned char* buf, size_t len);
std::string tls_strerror(int code);
bool poll_for(int fd, short events, int timeout_ms);

}  // namespace capp
