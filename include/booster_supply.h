#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// BoosterSupply – one TDK-Lambda GEN power supply accessed via SCPI/TCP
// ─────────────────────────────────────────────────────────────────────────────
//
//  Pure C++ rewrite — uses POSIX sockets instead of QTcpSocket.
//
//  Protocol:  plain-text SCPI over TCP port 8003 (TDK-Lambda default).
//  Each supply exposes exactly one channel.
//
//  Commands used:
//    OUTP:STAT?          → "ON" or "OFF"
//    OUTP:STAT 1/0      → turn output on/off
//    MEAS:VOLT?          → measured output voltage (float string)
//    MEAS:CURR?          → measured output current (float string)
//    SOUR:VOLT <val>     → set voltage
//    SOUR:CURR <val>     → set current limit
//    SOUR:MOD?           → operating mode: "CV" or "CC"
//
//  Threading:
//    BoosterSupply objects are owned by the booster poll thread.
//    All I/O is synchronous (blocking with short timeouts).
// ─────────────────────────────────────────────────────────────────────────────

#include <string>
#include <cmath>
#include <limits>
#include <cstdio>
#include <cstring>
#include <algorithm>

#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <errno.h>

struct BoosterSupply {
    std::string name;       // human label, e.g. "Booster 1"
    std::string ip;
    uint16_t    port = 8003;

    // Readback (updated by poll)
    double      vmon      = std::numeric_limits<double>::quiet_NaN();
    double      vset      = std::numeric_limits<double>::quiet_NaN();
    double      imon      = std::numeric_limits<double>::quiet_NaN();
    double      iset      = std::numeric_limits<double>::quiet_NaN();
    bool        on        = false;
    std::string mode;       // "CV", "CC", or ""
    std::string error;      // last error string, empty if OK
    bool        connected = false;

    int         fd_ = -1;   // POSIX socket file descriptor

    // ── Socket helpers ───────────────────────────────────────────────────

    void closeSocket()
    {
        if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
        connected = false;
    }

    // Drain any stale bytes sitting in the receive buffer.
    void drain()
    {
        if (fd_ < 0) return;
        char buf[256];
        // Non-blocking drain
        struct pollfd pfd = { fd_, POLLIN, 0 };
        while (::poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN)) {
            ssize_t n = ::recv(fd_, buf, sizeof(buf), 0);
            if (n <= 0) break;
        }
    }

    // Send a query command and return the trimmed response line.
    // Returns "" on any I/O error (and marks disconnected).
    std::string sendCmd(const std::string &cmd, int timeoutMs = 2000)
    {
        if (fd_ < 0) return {};

        drain();

        std::string tx = cmd + "\n";
        ssize_t sent = ::send(fd_, tx.data(), tx.size(), MSG_NOSIGNAL);
        if (sent != static_cast<ssize_t>(tx.size())) {
            closeSocket();
            return {};
        }

        // Accumulate until we see '\n'
        std::string resp;
        char buf[256];
        while (resp.find('\n') == std::string::npos) {
            struct pollfd pfd = { fd_, POLLIN, 0 };
            int pr = ::poll(&pfd, 1, timeoutMs);
            if (pr <= 0 || !(pfd.revents & POLLIN)) {
                closeSocket();
                return {};
            }
            ssize_t n = ::recv(fd_, buf, sizeof(buf), 0);
            if (n <= 0) {
                closeSocket();
                return {};
            }
            resp.append(buf, static_cast<size_t>(n));
        }

        // Trim whitespace
        auto start = resp.find_first_not_of(" \t\r\n");
        auto end   = resp.find_last_not_of(" \t\r\n");
        if (start == std::string::npos) return {};
        return resp.substr(start, end - start + 1);
    }

    // Send a set command (no response expected from TDK-Lambda writes).
    void sendSet(const std::string &cmd, int timeoutMs = 2000)
    {
        if (fd_ < 0) return;
        drain();

        std::string tx = cmd + "\n";
        ssize_t sent = ::send(fd_, tx.data(), tx.size(), MSG_NOSIGNAL);
        if (sent != static_cast<ssize_t>(tx.size())) {
            closeSocket();
        }
        // Give the supply a moment to process (avoid pipelining issues)
        (void)timeoutMs;
    }

    // Ensure the socket is connected; reconnect if needed.
    bool ensureConnected(int timeoutMs = 3000)
    {
        if (fd_ >= 0 && connected)
            return true;

        closeSocket();

        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0) {
            error = "socket() failed: " + std::string(strerror(errno));
            connected = false;
            return false;
        }

        // Set non-blocking for connect with timeout
        int flags = ::fcntl(fd_, F_GETFL, 0);
        ::fcntl(fd_, F_SETFL, flags | O_NONBLOCK);

        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port   = htons(port);
        if (::inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1) {
            // Try DNS resolution
            struct addrinfo hints{}, *res = nullptr;
            hints.ai_family   = AF_INET;
            hints.ai_socktype = SOCK_STREAM;
            int gai = ::getaddrinfo(ip.c_str(), nullptr, &hints, &res);
            if (gai != 0 || !res) {
                error = "getaddrinfo failed: " + std::string(gai_strerror(gai));
                closeSocket();
                return false;
            }
            addr.sin_addr = reinterpret_cast<struct sockaddr_in*>(res->ai_addr)->sin_addr;
            ::freeaddrinfo(res);
        }

        int cr = ::connect(fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
        if (cr < 0 && errno != EINPROGRESS) {
            error = "connect failed: " + std::string(strerror(errno));
            closeSocket();
            return false;
        }

        if (cr < 0) {
            // Wait for connection with timeout
            struct pollfd pfd = { fd_, POLLOUT, 0 };
            int pr = ::poll(&pfd, 1, timeoutMs);
            if (pr <= 0) {
                error = (pr == 0) ? "connect timeout" : "poll failed: " + std::string(strerror(errno));
                closeSocket();
                return false;
            }
            // Check for connection error
            int sockerr = 0;
            socklen_t len = sizeof(sockerr);
            ::getsockopt(fd_, SOL_SOCKET, SO_ERROR, &sockerr, &len);
            if (sockerr != 0) {
                error = "connect failed: " + std::string(strerror(sockerr));
                closeSocket();
                return false;
            }
        }

        // Restore blocking mode
        ::fcntl(fd_, F_SETFL, flags);

        // Enable TCP keepalive
        int optval = 1;
        ::setsockopt(fd_, SOL_SOCKET, SO_KEEPALIVE, &optval, sizeof(optval));

        connected = true;
        error.clear();
        return true;
    }

    // ── One full poll cycle ──────────────────────────────────────────────

    void poll()
    {
        if (!ensureConnected()) return;

        auto failWith = [this](const char *msg) {
            connected = false;
            error     = msg;
        };

        // Output state
        std::string s = sendCmd("OUTP:STAT?");
        if (s.empty()) { failWith("no response (OUTP:STAT?)"); return; }
        on = (strcasecmp(s.c_str(), "ON") == 0);

        // Measured voltage
        s = sendCmd("MEAS:VOLT?");
        if (s.empty()) { failWith("no response (MEAS:VOLT?)"); return; }
        try { vmon = std::stod(s); } catch (...) { vmon = std::numeric_limits<double>::quiet_NaN(); }

        // Measured current
        s = sendCmd("MEAS:CURR?");
        if (s.empty()) { failWith("no response (MEAS:CURR?)"); return; }
        try { imon = std::stod(s); } catch (...) { imon = std::numeric_limits<double>::quiet_NaN(); }

        // Operating mode
        s = sendCmd("SOUR:MOD?");
        if (s.empty()) { failWith("no response (SOUR:MOD?)"); return; }
        mode = s;

        // VSet
        s = sendCmd("SOUR:VOLT:LEV:IMM:AMPL?");
        if (s.empty()) { failWith("no response (SOUR:VOLT:LEV:IMM:AMPL?)"); return; }
        try { vset = std::stod(s); } catch (...) {}

        // ISet
        s = sendCmd("SOUR:CURR:LEV:IMM:AMPL?");
        if (s.empty()) { failWith("no response (SOUR:CURR:LEV:IMM:AMPL?)"); return; }
        try { iset = std::stod(s); } catch (...) {}

        error.clear();
    }

    void setOutput(bool enable)
    {
        if (!ensureConnected()) return;
        sendSet(enable ? "OUTP:STAT 1" : "OUTP:STAT 0");
        on = enable;
    }

    void setVoltage(double volts)
    {
        if (!ensureConnected()) return;
        char buf[64];
        std::snprintf(buf, sizeof(buf), "SOUR:VOLT:LEV:IMM:AMPL %.2f", volts);
        sendSet(buf);
        vset = volts;
    }

    void setCurrent(double amps)
    {
        if (!ensureConnected()) return;
        char buf[64];
        std::snprintf(buf, sizeof(buf), "SOUR:CURR:LEV:IMM:AMPL %.3f", amps);
        sendSet(buf);
        iset = amps;
    }

    ~BoosterSupply() { closeSocket(); }
};
