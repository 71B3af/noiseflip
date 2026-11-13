// ui_server.h  (Windows)
//
// A tiny web server for Noiseflip's own UI: it listens on 127.0.0.1 only, serves the files of the `ui` folder next to
// the exe, and hands every /api/ request to a function of the program. One thread accepts, and each connection
// gets a thread of its own that lives for one request ("Connection: close"), so a browser's idle speculative
// connection can never block the others.
//
// Safety: only loopback is bound; the Host header must be 127.0.0.1 or localhost on our port (no DNS rebinding);
// every POST must carry the header "X-Noiseflip" (a web page of another site cannot send it without a preflight,
// which this server never answers).
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <fstream>
#include <algorithm>
#include <functional>
#include <memory>
#include <sstream>
#include <string>

#include "http_lite.h"

namespace skate {

class UiServer {
public:
    // Returns the response for an /api/ request (a whole HTTP response from http::response).
    using ApiHandler = std::function<std::string(const http::Request&)>;

    ~UiServer() { stop(); }

    // Tries `first_port` and the next ten. False when none could be bound.
    bool start(int first_port, const std::string& ui_dir, ApiHandler api) {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
        dir_ = ui_dir;
        api_ = std::move(api);
        for (int p = first_port; p <= first_port + 10; ++p) {
            SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (s == INVALID_SOCKET) continue;
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(static_cast<u_short>(p));
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0 && listen(s, 16) == 0) {
                listen_ = s;
                port_ = p;
                running_ = true;
                thread_ = CreateThread(nullptr, 0, &UiServer::accept_thread, this, 0, nullptr);
                return thread_ != nullptr;
            }
            closesocket(s);
        }
        return false;
    }

    void stop() {
        if (!running_) return;
        running_ = false;
        if (thread_) { WaitForSingleObject(thread_, 2000); CloseHandle(thread_); thread_ = nullptr; }
        if (listen_ != INVALID_SOCKET) { closesocket(listen_); listen_ = INVALID_SOCKET; }
    }

    int port() const { return port_; }
    std::string url() const { return "http://127.0.0.1:" + std::to_string(port_) + "/"; }

private:
    struct Conn { UiServer* self; SOCKET s; };

    static DWORD WINAPI accept_thread(void* p) {
        auto* self = static_cast<UiServer*>(p);
        while (self->running_) {
            fd_set set;
            FD_ZERO(&set);
            FD_SET(self->listen_, &set);
            timeval tv{0, 200000};
            if (select(0, &set, nullptr, nullptr, &tv) <= 0) continue;
            SOCKET c = accept(self->listen_, nullptr, nullptr);
            if (c == INVALID_SOCKET) continue;
            DWORD timeout = 3000;
            setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof timeout);
            setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof timeout);
            auto* conn = new Conn{self, c};
            HANDLE h = CreateThread(nullptr, 0, &UiServer::conn_thread, conn, 0, nullptr);
            if (h) CloseHandle(h);
            else { closesocket(c); delete conn; }
        }
        return 0;
    }

    static DWORD WINAPI conn_thread(void* p) {
        std::unique_ptr<Conn> conn(static_cast<Conn*>(p));
        conn->self->serve(conn->s);
        shutdown(conn->s, SD_SEND);
        closesocket(conn->s);
        return 0;
    }

    static void send_all(SOCKET s, const std::string& data) {
        std::size_t off = 0;
        while (off < data.size()) {
            const int n = send(s, data.data() + off, static_cast<int>(std::min<std::size_t>(data.size() - off, 1 << 16)), 0);
            if (n <= 0) return;
            off += static_cast<std::size_t>(n);
        }
    }

    void serve(SOCKET s) {
        std::string buf;
        char tmp[4096];
        std::size_t head_end = std::string::npos;
        while (head_end == std::string::npos && buf.size() < 16384) {
            const int n = recv(s, tmp, sizeof tmp, 0);
            if (n <= 0) return;
            buf.append(tmp, static_cast<std::size_t>(n));
            head_end = buf.find("\r\n\r\n");
        }
        if (head_end == std::string::npos) return;
        http::Request req;
        if (!http::parse_head(buf.substr(0, head_end), req)) { send_all(s, http::response(405, "text/plain", "method not allowed")); return; }
        if (!http::host_ok(req.header("host"), port_)) { send_all(s, http::response(403, "text/plain", "forbidden host")); return; }

        std::size_t length = static_cast<std::size_t>(std::atoi(req.header("content-length").c_str()));
        if (length > 65536) { send_all(s, http::response(413, "text/plain", "too large")); return; }
        req.body = buf.substr(head_end + 4);
        while (req.body.size() < length) {
            const int n = recv(s, tmp, sizeof tmp, 0);
            if (n <= 0) return;
            req.body.append(tmp, static_cast<std::size_t>(n));
        }
        req.body.resize(length);

        if (req.path.compare(0, 5, "/api/") == 0) {
            if (req.method == "POST" && req.header("x-noiseflip").empty()) { send_all(s, http::response(403, "text/plain", "missing header")); return; }
            send_all(s, api_(req));
            return;
        }
        if (req.method != "GET") { send_all(s, http::response(405, "text/plain", "method not allowed")); return; }
        send_all(s, file(req.path == "/" ? "/index.html" : req.path));
    }

    std::string file(const std::string& path) const {
        if (!http::safe_path(path)) return http::response(404, "text/plain", "not found");
        std::string full = dir_ + path;
        for (auto& c : full) if (c == '/') c = '\\';
        std::ifstream f(full, std::ios::binary);
        if (!f) {
            if (path == "/index.html")
                return http::response(404, "text/plain; charset=utf-8", "Noiseflip's ui folder was not found next to the exe: " + dir_);
            return http::response(404, "text/plain", "not found");
        }
        std::ostringstream ss;
        ss << f.rdbuf();
        const std::string mime = http::mime_for(path);
        const bool big = mime.compare(0, 4, "font") == 0 || mime == "image/png";
        return http::response(200, mime, ss.str(), big);
    }

    std::string dir_;
    ApiHandler api_;
    SOCKET listen_ = INVALID_SOCKET;
    int port_ = 0;
    volatile bool running_ = false;
    HANDLE thread_ = nullptr;
};

}  // namespace skate
