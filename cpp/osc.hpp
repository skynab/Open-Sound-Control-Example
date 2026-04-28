// osc.hpp - Header-only, dependency-free OSC 1.0 encoder/decoder + UDP socket
// helpers that work on Windows, macOS, and Linux.
//
// Usage: include this file from your client/server .cpp files. On Windows you
// must link against Ws2_32 (the #pragma below handles MSVC; for MinGW pass
// -lws2_32 on the link line).

#pragma once

#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
    #endif
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #ifdef _MSC_VER
        #pragma comment(lib, "Ws2_32.lib")
    #endif
    using socket_t = SOCKET;
    using socklen_t_compat = int;
    #define OSC_INVALID_SOCKET INVALID_SOCKET
    #define osc_close_socket closesocket
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <errno.h>
    using socket_t = int;
    using socklen_t_compat = socklen_t;
    #define OSC_INVALID_SOCKET (-1)
    #define osc_close_socket ::close
#endif

#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace osc {

// ---- One-time platform startup/teardown (Winsock needs WSAStartup) --------

inline void platform_init() {
#ifdef _WIN32
    static bool initialized = false;
    if (!initialized) {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            throw std::runtime_error("WSAStartup failed");
        }
        initialized = true;
    }
#endif
}

inline void platform_shutdown() {
#ifdef _WIN32
    WSACleanup();
#endif
}

// ---- Byte-order helpers ---------------------------------------------------

inline uint32_t to_be32(uint32_t v)   { return htonl(v); }
inline uint32_t from_be32(uint32_t v) { return ntohl(v); }

// Convert between a host-order uint32_t and the IEEE-754 bit pattern of a float.
// Byte-order conversion is done by the be32 helpers, NOT here, to avoid
// double-swapping when these are composed.
inline uint32_t float_to_host_bits(float f) {
    uint32_t bits;
    std::memcpy(&bits, &f, sizeof(bits));
    return bits;
}

inline float host_bits_to_float(uint32_t host_bits) {
    float f;
    std::memcpy(&f, &host_bits, sizeof(f));
    return f;
}

// ---- Argument variant -----------------------------------------------------

using Blob = std::vector<uint8_t>;
using Argument = std::variant<int32_t, float, std::string, Blob, bool>;

struct Message {
    std::string address;
    std::string typetag;        // includes leading ','
    std::vector<Argument> args;
};

// ---- Encoding -------------------------------------------------------------

inline void append_padded_string(std::vector<uint8_t>& out, const std::string& s) {
    out.insert(out.end(), s.begin(), s.end());
    out.push_back(0);
    while (out.size() % 4 != 0) out.push_back(0);
}

inline void append_be32(std::vector<uint8_t>& out, uint32_t v) {
    uint32_t be = to_be32(v);
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&be);
    out.insert(out.end(), p, p + 4);
}

class MessageBuilder {
public:
    explicit MessageBuilder(std::string address)
        : address_(std::move(address)), typetag_(",") {}

    MessageBuilder& add(int32_t v)              { typetag_ += 'i'; ints_.push_back(v); order_.push_back('i'); return *this; }
    MessageBuilder& add(float v)                { typetag_ += 'f'; floats_.push_back(v); order_.push_back('f'); return *this; }
    MessageBuilder& add(const std::string& v)   { typetag_ += 's'; strings_.push_back(v); order_.push_back('s'); return *this; }
    MessageBuilder& add(const char* v)          { return add(std::string(v)); }
    MessageBuilder& add(bool v)                 { typetag_ += (v ? 'T' : 'F'); return *this; }
    MessageBuilder& add(const Blob& v)          { typetag_ += 'b'; blobs_.push_back(v); order_.push_back('b'); return *this; }

    std::vector<uint8_t> build() const {
        std::vector<uint8_t> out;
        append_padded_string(out, address_);
        append_padded_string(out, typetag_);

        size_t i_idx = 0, f_idx = 0, s_idx = 0, b_idx = 0;
        for (char c : order_) {
            switch (c) {
                case 'i': append_be32(out, static_cast<uint32_t>(ints_[i_idx++])); break;
                case 'f': append_be32(out, float_to_host_bits(floats_[f_idx++])); break;
                case 's': append_padded_string(out, strings_[s_idx++]); break;
                case 'b': {
                    const Blob& b = blobs_[b_idx++];
                    append_be32(out, static_cast<uint32_t>(b.size()));
                    out.insert(out.end(), b.begin(), b.end());
                    while (out.size() % 4 != 0) out.push_back(0);
                    break;
                }
            }
        }
        return out;
    }

private:
    std::string address_;
    std::string typetag_;
    std::string order_;                 // only data-bearing tags (i,f,s,b)
    std::vector<int32_t> ints_;
    std::vector<float> floats_;
    std::vector<std::string> strings_;
    std::vector<Blob> blobs_;
};

// ---- Decoding -------------------------------------------------------------

inline std::string read_padded_string(const uint8_t* data, size_t size, size_t& offset) {
    size_t end = offset;
    while (end < size && data[end] != 0) ++end;
    if (end >= size) throw std::runtime_error("OSC: unterminated string");
    std::string s(reinterpret_cast<const char*>(data + offset), end - offset);
    // advance past NUL and any padding to the next 4-byte boundary
    offset = (end + 4) & ~static_cast<size_t>(3);
    if (offset > size) throw std::runtime_error("OSC: padding overruns packet");
    return s;
}

inline uint32_t read_be32(const uint8_t* data, size_t size, size_t& offset) {
    if (offset + 4 > size) throw std::runtime_error("OSC: truncated 32-bit value");
    uint32_t be;
    std::memcpy(&be, data + offset, 4);
    offset += 4;
    return from_be32(be);
}

inline Message decode(const uint8_t* data, size_t size) {
    Message msg;
    size_t offset = 0;
    msg.address = read_padded_string(data, size, offset);
    msg.typetag = read_padded_string(data, size, offset);
    if (msg.typetag.empty() || msg.typetag[0] != ',') {
        throw std::runtime_error("OSC: type tag must start with ','");
    }

    for (size_t i = 1; i < msg.typetag.size(); ++i) {
        char tag = msg.typetag[i];
        switch (tag) {
            case 'i': {
                uint32_t v = read_be32(data, size, offset);
                msg.args.emplace_back(static_cast<int32_t>(v));
                break;
            }
            case 'f': {
                uint32_t host_bits = read_be32(data, size, offset);
                msg.args.emplace_back(host_bits_to_float(host_bits));
                break;
            }
            case 's': {
                msg.args.emplace_back(read_padded_string(data, size, offset));
                break;
            }
            case 'b': {
                uint32_t blen = read_be32(data, size, offset);
                if (offset + blen > size) throw std::runtime_error("OSC: truncated blob");
                Blob b(data + offset, data + offset + blen);
                offset += blen;
                offset = (offset + 3) & ~static_cast<size_t>(3);
                msg.args.emplace_back(std::move(b));
                break;
            }
            case 'T': msg.args.emplace_back(true);  break;
            case 'F': msg.args.emplace_back(false); break;
            case 'N': /* nil */                     break;  // no value pushed
            case 'I': msg.args.emplace_back(std::numeric_limits<float>::infinity()); break;
            default:
                throw std::runtime_error(std::string("OSC: unsupported type tag '") + tag + "'");
        }
    }
    return msg;
}

// ---- UDP socket helpers ---------------------------------------------------

inline socket_t make_udp_socket() {
    platform_init();
    socket_t s = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (s == OSC_INVALID_SOCKET) {
        throw std::runtime_error("socket() failed");
    }
    return s;
}

inline void bind_udp(socket_t s, const std::string& host, uint16_t port) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (host == "0.0.0.0" || host.empty()) {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    } else if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        throw std::runtime_error("invalid bind address: " + host);
    }
    int yes = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&yes), sizeof(yes));
    if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        throw std::runtime_error("bind() failed");
    }
}

inline sockaddr_in make_dest(const std::string& host, uint16_t port) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        throw std::runtime_error("invalid dest address: " + host);
    }
    return addr;
}

inline void send_to(socket_t s, const sockaddr_in& dest, const std::vector<uint8_t>& packet) {
    int sent = ::sendto(s, reinterpret_cast<const char*>(packet.data()),
                        static_cast<int>(packet.size()), 0,
                        reinterpret_cast<const sockaddr*>(&dest), sizeof(dest));
    if (sent < 0) throw std::runtime_error("sendto() failed");
}

// Set a receive timeout so a recvfrom() loop can periodically check a quit flag.
inline void set_recv_timeout_ms(socket_t s, int ms) {
#ifdef _WIN32
    DWORD tv = static_cast<DWORD>(ms);
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&tv), sizeof(tv));
#else
    timeval tv;
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
}

// Enable ANSI escape sequences on Windows 10+ consoles. No-op elsewhere.
inline void enable_ansi() {
#ifdef _WIN32
    #ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
    #define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
    #endif
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &mode)) {
        SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }
#endif
}

// Read this socket's bound local port (useful for showing where replies arrive).
inline uint16_t local_port_of(socket_t s) {
    sockaddr_in addr{};
    socklen_t_compat len = sizeof(addr);
    if (::getsockname(s, reinterpret_cast<sockaddr*>(&addr), &len) != 0) return 0;
    return ntohs(addr.sin_port);
}

} // namespace osc
