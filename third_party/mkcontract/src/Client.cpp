#include "mkcontract/Client.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace mkcontract::live {
namespace {

#if defined(_WIN32)
using SocketHandle = SOCKET;
constexpr SocketHandle kNoSocket = INVALID_SOCKET;
void closeSocket(SocketHandle s) { closesocket(s); }
#else
using SocketHandle = int;
constexpr SocketHandle kNoSocket = -1;
void closeSocket(SocketHandle s) { close(s); }
#endif

bool startup() {
#if defined(_WIN32)
    static const bool ok = [] {
        WSADATA data;
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    return ok;
#else
    return true;
#endif
}

void setTimeout(SocketHandle s, int ms) {
#if defined(_WIN32)
    const DWORD t = static_cast<DWORD>(ms);
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&t), sizeof(t));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&t), sizeof(t));
#else
    timeval t{ms / 1000, (ms % 1000) * 1000};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &t, sizeof(t));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &t, sizeof(t));
#endif
}

} // namespace

HttpResult request(const Instance& to, const char* method, const std::string& path, const std::string& body, int timeoutMs) {
    HttpResult result;
    if (!startup() || to.port == 0) return result;
    const SocketHandle s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == kNoSocket) return result;
    setTimeout(s, timeoutMs);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(to.port);
    addr.sin_addr.s_addr = htonl(0x7F000001u); // 127.0.0.1
    if (connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        closeSocket(s);
        return result;
    }
    std::string req = std::string(method) + " /" + to.token + "/" + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n";
    if (!body.empty() || std::strcmp(method, "POST") == 0)
        req += "Content-Type: application/x-www-form-urlencoded\r\nContent-Length: " + std::to_string(body.size()) + "\r\n";
    req += "\r\n" + body;
    size_t sent = 0;
    while (sent < req.size()) {
        const int n = send(s, req.data() + sent, static_cast<int>(req.size() - sent), 0);
        if (n <= 0) {
            closeSocket(s);
            return result;
        }
        sent += static_cast<size_t>(n);
    }
    std::string response;
    char buffer[16384];
    for (;;) {
        const int n = static_cast<int>(recv(s, buffer, sizeof(buffer), 0));
        if (n <= 0) break; // end (the server closes) or timeout
        response.append(buffer, static_cast<size_t>(n));
    }
    closeSocket(s);
    const size_t headerEnd = response.find("\r\n\r\n");
    if (response.rfind("HTTP/1.1 ", 0) != 0 || headerEnd == std::string::npos) return result;
    result.status = std::atoi(response.c_str() + 9);
    result.body = response.substr(headerEnd + 4);
    return result;
}

std::string urlEncode(const std::string& text) {
    std::string out;
    for (const unsigned char c : text) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            char buf[4];
            std::snprintf(buf, sizeof(buf), "%%%02X", c);
            out += buf;
        }
    }
    return out;
}

} // namespace mkcontract::live
