#include "support/MockVenue.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <stdexcept>

namespace hltest {

struct MockVenue::Conn final : hl::IoHandler, hl::WsFrameSink {
    MockVenue* venue{nullptr};
    int fd{-1};
    bool websocket{false};
    std::string buffer;
    hl::WsFrameDecoder decoder;

    void onIoEvent(std::uint32_t /*events*/) override {
        char buf[65536];
        const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) {
            venue->closeConn(fd);  // destroys *this
            return;
        }
        if (websocket) {
            decoder.feed(buf, static_cast<std::size_t>(n), *this);
            return;
        }
        buffer.append(buf, static_cast<std::size_t>(n));
        handleHttp();
    }

    void sendAll(std::string_view data) const {
        std::size_t off = 0;
        while (off < data.size()) {
            const ssize_t n = ::send(fd, data.data() + off, data.size() - off, MSG_NOSIGNAL);
            if (n <= 0) {
                return;
            }
            off += static_cast<std::size_t>(n);
        }
    }

    static std::string header(const std::string& head, const std::string& name) {
        std::size_t pos = 0;
        while ((pos = head.find("\r\n", pos)) != std::string::npos) {
            pos += 2;
            if (head.compare(pos, name.size(), name) == 0 && head.size() > pos + name.size() &&
                head[pos + name.size()] == ':') {
                std::size_t start = pos + name.size() + 1;
                while (start < head.size() && head[start] == ' ') {
                    ++start;
                }
                return head.substr(start, head.find("\r\n", start) - start);
            }
        }
        return {};
    }

    void handleHttp() {
        for (;;) {
            const std::size_t headEnd = buffer.find("\r\n\r\n");
            if (headEnd == std::string::npos) {
                return;
            }
            const std::string head = buffer.substr(0, headEnd + 2);
            const std::size_t sp1 = head.find(' ');
            const std::string path = head.substr(sp1 + 1, head.find(' ', sp1 + 1) - sp1 - 1);
            const std::string key = header(head, "Sec-WebSocket-Key");
            if (!key.empty()) {
                buffer.erase(0, headEnd + 4);
                websocket = true;
                sendAll("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                        "Sec-WebSocket-Accept: " + hl::wsAcceptKey(key) + "\r\n\r\n");
                if (venue->onWsOpen) {
                    venue->onWsOpen(fd);
                }
                return;
            }
            const std::string lenText = header(head, "Content-Length");
            const std::size_t len = lenText.empty() ? 0 : std::stoul(lenText);
            if (buffer.size() < headEnd + 4 + len) {
                return;
            }
            const std::string body = buffer.substr(headEnd + 4, len);
            buffer.erase(0, headEnd + 4 + len);
            venue->httpLog.emplace_back(path, body);
            HttpReply reply{404, "not found"};
            if (venue->onHttp) {
                reply = venue->onHttp(path, body);
            }
            sendAll("HTTP/1.1 " + std::to_string(reply.status) + " X\r\nContent-Type: application/json\r\nContent-Length: " +
                    std::to_string(reply.body.size()) + "\r\n\r\n" + reply.body);
        }
    }

    void sendFrame(hl::WsOpcode op, std::string_view payload) const {
        std::string f;
        f.push_back(static_cast<char>(0x80 | static_cast<std::uint8_t>(op)));
        const std::size_t n = payload.size();
        if (n < 126) {
            f.push_back(static_cast<char>(n));
        } else if (n <= 0xFFFF) {
            f.push_back(126);
            f.push_back(static_cast<char>(n >> 8));
            f.push_back(static_cast<char>(n & 0xFF));
        } else {
            f.push_back(127);
            for (int i = 7; i >= 0; --i) {
                f.push_back(static_cast<char>((static_cast<std::uint64_t>(n) >> (8 * i)) & 0xFF));
            }
        }
        f.append(payload);
        sendAll(f);
    }

    void onWsMessage(hl::WsOpcode op, std::string_view payload) override {
        if (op != hl::WsOpcode::Text) {
            return;
        }
        const std::string text{payload};
        venue->wsLog.push_back(text);
        if (venue->onWsText) {
            venue->onWsText(fd, text);
        }
    }

    void onWsControl(hl::WsOpcode op, std::string_view payload) override {
        if (op == hl::WsOpcode::Ping) {
            sendFrame(hl::WsOpcode::Pong, payload);
        }
    }
};

MockVenue::MockVenue(hl::EventLoop& loop) : loop_(loop) {
    listenFd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    const int one = 1;
    ::setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (::bind(listenFd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || ::listen(listenFd_, 16) != 0) {
        throw std::runtime_error("MockVenue: bind/listen failed");
    }
    socklen_t len = sizeof(addr);
    ::getsockname(listenFd_, reinterpret_cast<sockaddr*>(&addr), &len);
    port_ = ntohs(addr.sin_port);
    loop_.add(listenFd_, EPOLLIN, this);
}

MockVenue::~MockVenue() {
    for (auto& [fd, conn] : conns_) {
        loop_.remove(fd);
        ::close(fd);
    }
    loop_.remove(listenFd_);
    ::close(listenFd_);
}

void MockVenue::onIoEvent(std::uint32_t /*events*/) {
    const int fd = ::accept4(listenFd_, nullptr, nullptr, SOCK_CLOEXEC);
    if (fd < 0) {
        return;
    }
    auto conn = std::make_unique<Conn>();
    conn->venue = this;
    conn->fd = fd;
    loop_.add(fd, EPOLLIN, conn.get());
    conns_[fd] = std::move(conn);
}

void MockVenue::closeConn(int fd) {
    loop_.remove(fd);
    ::close(fd);
    conns_.erase(fd);
}

void MockVenue::wsSend(int conn, std::string_view text) {
    auto it = conns_.find(conn);
    if (it != conns_.end() && it->second->websocket) {
        it->second->sendFrame(hl::WsOpcode::Text, text);
    }
}

void MockVenue::wsBroadcast(std::string_view text) {
    for (auto& [fd, conn] : conns_) {
        if (conn->websocket) {
            conn->sendFrame(hl::WsOpcode::Text, text);
        }
    }
}

void MockVenue::dropWebSockets() {
    std::vector<int> fds;
    for (auto& [fd, conn] : conns_) {
        if (conn->websocket) {
            fds.push_back(fd);
        }
    }
    for (int fd : fds) {
        closeConn(fd);
    }
}

int MockVenue::openWebSockets() const {
    int n = 0;
    for (const auto& [fd, conn] : conns_) {
        n += conn->websocket ? 1 : 0;
    }
    return n;
}

bool runUntil(hl::EventLoop& loop, const std::function<bool()>& pred, int timeoutMs) {
    const auto deadline = hl::EventLoop::nowMs() + timeoutMs;
    while (!pred()) {
        if (hl::EventLoop::nowMs() >= deadline) {
            return false;
        }
        loop.runOnce(10);
    }
    return true;
}

}  // namespace hltest
