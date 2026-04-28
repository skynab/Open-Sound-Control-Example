// osc_server.cpp - OSC server with a small ANSI-terminal dashboard UI.
//
// The dashboard shows: bind address, last sender, total received, echo and
// pause toggles, mirrored state from incoming /synth/freq, /mixer/.../gain,
// /lfo/value, /transport/* messages, three "push" sliders for sending values
// back to the last seen client, and a scrolling log of sent (->) and
// received (<-) messages.
//
// Controls (type a letter and press Enter):
//   e        toggle echo (reply with /ack<address>)
//   p        toggle pause (drop incoming)
//   c        clear log
//   1 / !    selected slider = freq  / push it now
//   2 / @    selected slider = gain  / push it now
//   3 / #    selected slider = lfo   / push it now
//   + / -    nudge the selected slider up / down
//   b        send /server/ping to last sender
//   a        send /server/announce "hello from server"
//   y / n    send /server/transport/play  /  /server/transport/stop
//   h        show command help in the log
//   q        quit
//
// Build: see Makefile (Linux/macOS: just `make`, Windows MinGW: -lws2_32).
// Run:   ./osc_server [host] [port]   (defaults 127.0.0.1 9000)

#include "osc.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace ansi {
constexpr const char* CLEAR  = "\033[2J";
constexpr const char* HOME   = "\033[H";
constexpr const char* HIDE   = "\033[?25l";
constexpr const char* SHOW   = "\033[?25h";
constexpr const char* RESET  = "\033[0m";
constexpr const char* BOLD   = "\033[1m";
constexpr const char* DIM    = "\033[2m";
constexpr const char* GREEN  = "\033[32m";
constexpr const char* CYAN   = "\033[36m";
constexpr const char* YELLOW = "\033[33m";
constexpr const char* RED    = "\033[31m";
constexpr const char* CLR_EOL = "\033[K";
}

static std::atomic<bool> g_running{true};
static void on_sigint(int) { g_running = false; }

struct State {
    std::mutex mtx;

    // Bound socket and the address we're bound to. The input thread can
    // request a rebind which closes this and replaces it with a new one.
    socket_t    sock = OSC_INVALID_SOCKET;
    std::string bind_host = "127.0.0.1";
    uint16_t    bind_port = 9000;

    bool echo   = true;
    bool paused = false;

    bool        has_sender = false;
    sockaddr_in last_sender{};
    std::string last_sender_str = "(none)";
    uint64_t    total_received  = 0;

    // Theremin on/off is mirrored from the client because it's
    // displayed inside the Theremin push frame next to the sliders.
    std::string therm_on_s     = "-";

    // "Push" sliders that the user adjusts to control the client
    float push_freq        = 440.0f;
    float push_gain        = 0.50f;
    float push_lfo         = 0.00f;
    float push_therm_pitch = 440.0f;
    float push_therm_vol   = 0.50f;
    int   selected         = 0;   // 0=freq 1=gain 2=lfo 3=therm_pitch 4=therm_vol

    // Editable display names for the three core sliders. Renamed via the
    // 'L <slot> <name>' command, which also pushes /server/rename/<slot>
    // to the last-seen client.
    std::string name_freq = "Frequency";
    std::string name_gain = "Gain";
    std::string name_lfo  = "LFO";

    std::deque<std::string> log;

    void add_log(std::string s) {
        log.push_back(std::move(s));
        while (log.size() > 12) log.pop_front();
    }
};

struct Snap {
    std::string host_str; uint16_t port;
    bool        bound;
    bool        echo, paused;
    std::string last_sender_str;
    uint64_t    total_received;
    std::string therm_on_s;
    float       push_freq, push_gain, push_lfo;
    float       push_therm_pitch, push_therm_vol;
    int         selected;
    std::string name_freq, name_gain, name_lfo;
    std::deque<std::string> log;
};

// Close the current socket (if any) and try to bind a new one to host:port.
// Returns true on success. On failure the State is left with sock =
// INVALID and the dashboard shows the bound row as "(unbound)".
static bool do_rebind(State& st, const std::string& host, uint16_t port) {
    socket_t old_sock;
    {
        std::lock_guard<std::mutex> lk(st.mtx);
        old_sock = st.sock;
        st.sock = OSC_INVALID_SOCKET;
    }
    if (old_sock != OSC_INVALID_SOCKET) osc_close_socket(old_sock);

    socket_t new_sock = OSC_INVALID_SOCKET;
    std::string err;
    try {
        new_sock = osc::make_udp_socket();
        osc::bind_udp(new_sock, host, port);
        osc::set_recv_timeout_ms(new_sock, 200);
    } catch (const std::exception& e) {
        if (new_sock != OSC_INVALID_SOCKET) { osc_close_socket(new_sock); new_sock = OSC_INVALID_SOCKET; }
        err = e.what();
    }

    std::lock_guard<std::mutex> lk(st.mtx);
    st.sock = new_sock;
    if (new_sock != OSC_INVALID_SOCKET) {
        st.bind_host = host;
        st.bind_port = port;
        st.add_log("[server] bound to " + host + ":" + std::to_string(port));
        return true;
    }
    st.add_log("!  bind " + host + ":" + std::to_string(port) + " failed: " + err);
    return false;
}

static std::string fmt_args(const std::vector<osc::Argument>& args) {
    std::ostringstream s;
    for (size_t i = 0; i < args.size(); ++i) {
        if (i) s << ", ";
        const auto& a = args[i];
        if      (auto p = std::get_if<int32_t>(&a))     s << *p << "i";
        else if (auto p = std::get_if<float>(&a))       s << *p << "f";
        else if (auto p = std::get_if<std::string>(&a)) s << '"' << *p << '"';
        else if (auto p = std::get_if<bool>(&a))        s << (*p ? "true" : "false");
        else if (auto p = std::get_if<osc::Blob>(&a))   s << "<blob " << p->size() << "B>";
    }
    return s.str();
}

static std::string bar(double v, int width) {
    if (v < 0.0) v = 0.0;
    if (v > 1.0) v = 1.0;
    int filled = static_cast<int>(v * width + 0.5);
    std::string s = "[";
    for (int i = 0; i < width; ++i) s += (i < filled ? "#" : "-");
    s += "]";
    return s;
}

// ----- Receive loop --------------------------------------------------------

static void recv_loop(State& st) {
    std::vector<uint8_t> buf(65535);
    while (g_running) {
        // Read the current socket (it can be swapped by a rebind).
        socket_t sock;
        bool paused_now, echo_now;
        {
            std::lock_guard<std::mutex> lk(st.mtx);
            sock        = st.sock;
            paused_now  = st.paused;
            echo_now    = st.echo;
        }
        if (sock == OSC_INVALID_SOCKET) {
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            continue;
        }

        sockaddr_in src{};
        socklen_t_compat slen = sizeof(src);
        int n = ::recvfrom(sock, reinterpret_cast<char*>(buf.data()),
                           static_cast<int>(buf.size()), 0,
                           reinterpret_cast<sockaddr*>(&src), &slen);
        if (n <= 0) continue;  // timeout or socket got closed under us
        if (paused_now) continue;

        try {
            osc::Message m = osc::decode(buf.data(), static_cast<size_t>(n));
            char ip[64] = {0};
            inet_ntop(AF_INET, &src.sin_addr, ip, sizeof(ip));
            std::ostringstream sender; sender << ip << ":" << ntohs(src.sin_port);

            {
                std::lock_guard<std::mutex> lk(st.mtx);
                st.last_sender = src;
                st.has_sender = true;
                st.last_sender_str = sender.str();
                st.total_received++;

                if (m.address == "/theremin/on" && !m.args.empty()) {
                    if (auto p = std::get_if<bool>(&m.args[0])) {
                        st.therm_on_s = *p ? "ON" : "OFF";
                    }
                }

                st.add_log("<- " + sender.str() + "  " + m.address +
                           "  " + fmt_args(m.args));
            }

            if (echo_now && m.address.rfind("/ack", 0) != 0) {
                osc::MessageBuilder b("/ack" + m.address);
                for (const auto& a : m.args) {
                    if      (auto p = std::get_if<int32_t>(&a))     b.add(*p);
                    else if (auto p = std::get_if<float>(&a))       b.add(*p);
                    else if (auto p = std::get_if<std::string>(&a)) b.add(*p);
                    else if (auto p = std::get_if<bool>(&a))        b.add(*p);
                    else if (auto p = std::get_if<osc::Blob>(&a))   b.add(*p);
                }
                auto reply = b.build();
                ::sendto(sock, reinterpret_cast<const char*>(reply.data()),
                         static_cast<int>(reply.size()), 0,
                         reinterpret_cast<sockaddr*>(&src), sizeof(src));
            }
        } catch (const std::exception& e) {
            std::lock_guard<std::mutex> lk(st.mtx);
            st.add_log(std::string("!  decode error: ") + e.what());
        }
    }
}

// ----- Push helper ---------------------------------------------------------

template <class Arg>
static void push_to_sender(State& st, const std::string& addr, const Arg& arg) {
    socket_t sock;
    sockaddr_in dst;
    bool has;
    std::string sstr;
    {
        std::lock_guard<std::mutex> lk(st.mtx);
        sock = st.sock;
        has = st.has_sender;
        if (has) { dst = st.last_sender; sstr = st.last_sender_str; }
    }
    if (sock == OSC_INVALID_SOCKET) {
        std::lock_guard<std::mutex> lk(st.mtx);
        st.add_log("!  no socket bound (use 'r <host> <port>')");
        return;
    }
    if (!has) {
        std::lock_guard<std::mutex> lk(st.mtx);
        st.add_log("!  no client yet to push '" + addr + "' to");
        return;
    }
    osc::MessageBuilder b(addr); b.add(arg);
    auto pkt = b.build();
    ::sendto(sock, reinterpret_cast<const char*>(pkt.data()),
             static_cast<int>(pkt.size()), 0,
             reinterpret_cast<sockaddr*>(&dst), sizeof(dst));
    std::vector<osc::Argument> args; args.emplace_back(arg);
    std::lock_guard<std::mutex> lk(st.mtx);
    st.add_log("-> " + sstr + "  " + addr + "  " + fmt_args(args));
}

// ----- Stdin command loop --------------------------------------------------

static void input_loop(State& st) {
    std::string line;
    while (g_running && std::getline(std::cin, line)) {
        if (line.empty()) continue;

        // Multi-token command: "r <host> <port>" rebinds.
        std::istringstream iss(line);
        std::string cmd; iss >> cmd;
        if (cmd == "r" || cmd == "rebind") {
            std::string h; int p = 0;
            if ((iss >> h >> p) && p >= 0 && p <= 65535) {
                do_rebind(st, h, static_cast<uint16_t>(p));
            } else {
                std::lock_guard<std::mutex> lk(st.mtx);
                st.add_log("!  usage: r <host> <port>");
            }
            continue;
        }

        // L <slot 1..3> <new name...>  -- rename Frequency/Gain/LFO and
        // tell the client to rename its matching slider.
        if (cmd == "L" || cmd == "rename") {
            int slot = 0;
            if (!(iss >> slot)) {
                std::lock_guard<std::mutex> lk(st.mtx);
                st.add_log("!  usage: L <1|2|3> <new name>");
                continue;
            }
            std::string rest;
            std::getline(iss, rest);
            size_t p = rest.find_first_not_of(" \t");
            if (p == std::string::npos) {
                std::lock_guard<std::mutex> lk(st.mtx);
                st.add_log("!  usage: L <1|2|3> <new name>");
                continue;
            }
            std::string name = rest.substr(p);

            const char* address = nullptr;
            {
                std::lock_guard<std::mutex> lk(st.mtx);
                if      (slot == 1) { st.name_freq = name; address = "/server/rename/freq"; }
                else if (slot == 2) { st.name_gain = name; address = "/server/rename/gain"; }
                else if (slot == 3) { st.name_lfo  = name; address = "/server/rename/lfo";  }
                else {
                    st.add_log("!  slot must be 1 (freq), 2 (gain), or 3 (lfo)");
                    continue;
                }
                st.add_log(std::string("[server] renamed slot ") +
                           std::to_string(slot) + " -> \"" + name + "\"");
            }
            push_to_sender(st, address, name);
            continue;
        }

        char c = line[0];
        switch (c) {
            case 'q': case 'Q': g_running = false; return;
            case 'e': {
                std::lock_guard<std::mutex> lk(st.mtx);
                st.echo = !st.echo;
                st.add_log(std::string("[server] echo ") +
                           (st.echo ? "ON" : "OFF"));
            } break;
            case 'p': {
                std::lock_guard<std::mutex> lk(st.mtx);
                st.paused = !st.paused;
                st.add_log(std::string("[server] paused ") +
                           (st.paused ? "ON" : "OFF"));
            } break;
            case 'c': {
                std::lock_guard<std::mutex> lk(st.mtx);
                st.log.clear();
            } break;

            case '1': { std::lock_guard<std::mutex> lk(st.mtx); st.selected = 0; } break;
            case '2': { std::lock_guard<std::mutex> lk(st.mtx); st.selected = 1; } break;
            case '3': { std::lock_guard<std::mutex> lk(st.mtx); st.selected = 2; } break;
            case '4': { std::lock_guard<std::mutex> lk(st.mtx); st.selected = 3; } break;
            case '5': { std::lock_guard<std::mutex> lk(st.mtx); st.selected = 4; } break;

            case '+': case '=':
            case '-': case '_': {
                bool up = (c == '+' || c == '=');
                int sel; float v; const char* addr;
                {
                    std::lock_guard<std::mutex> lk(st.mtx);
                    sel = st.selected;
                    auto bump = [&](float& fld, float lo, float hi, float step) -> float {
                        fld = up ? std::min(hi, fld + step) : std::max(lo, fld - step);
                        return fld;
                    };
                    if      (sel == 0) { v = bump(st.push_freq,         20.0f, 2000.0f, 20.0f);  addr = "/server/freq"; }
                    else if (sel == 1) { v = bump(st.push_gain,          0.0f,    1.0f, 0.05f); addr = "/server/gain"; }
                    else if (sel == 2) { v = bump(st.push_lfo,           0.0f,    1.0f, 0.05f); addr = "/server/lfo";  }
                    else if (sel == 3) { v = bump(st.push_therm_pitch,  80.0f, 2000.0f, 20.0f); addr = "/server/theremin/pitch";  }
                    else               { v = bump(st.push_therm_vol,     0.0f,    1.0f, 0.05f); addr = "/server/theremin/volume"; }
                }
                push_to_sender(st, addr, v);
            } break;

            // Push the currently-selected slider's value
            case '!': push_to_sender(st, "/server/freq",
                                     [&]{ std::lock_guard<std::mutex> lk(st.mtx); return st.push_freq; }()); break;
            case '@': push_to_sender(st, "/server/gain",
                                     [&]{ std::lock_guard<std::mutex> lk(st.mtx); return st.push_gain; }()); break;
            case '#': push_to_sender(st, "/server/lfo",
                                     [&]{ std::lock_guard<std::mutex> lk(st.mtx); return st.push_lfo;  }()); break;
            case '$': push_to_sender(st, "/server/theremin/pitch",
                                     [&]{ std::lock_guard<std::mutex> lk(st.mtx); return st.push_therm_pitch; }()); break;
            case '%': push_to_sender(st, "/server/theremin/volume",
                                     [&]{ std::lock_guard<std::mutex> lk(st.mtx); return st.push_therm_vol;   }()); break;

            case 'b': push_to_sender(st, "/server/ping",     true);                           break;
            case 'a': push_to_sender(st, "/server/announce", std::string("hello from server")); break;
            case 'y': push_to_sender(st, "/server/transport/play", true);                     break;
            case 'n': push_to_sender(st, "/server/transport/stop", true);                     break;

            case 'h': case '?': {
                std::lock_guard<std::mutex> lk(st.mtx);
                st.add_log("[help] e=echo p=pause c=clear  1/2/3/4/5=select slider  +/- nudge");
                st.add_log("[help] !@#$% push freq/gain/lfo/therm-pitch/therm-volume");
                st.add_log("[help] L <1|2|3> <name> rename freq/gain/lfo on server+client");
                st.add_log("[help] b=ping a=announce y=play n=stop  r <host> <port>=rebind  q=quit");
            } break;

            default: break;  // ignore
        }
    }
}

// ----- Drawing -------------------------------------------------------------

static void draw(const Snap& s) {
    std::ostringstream o;
    o << ansi::HOME;

    // ---- Banner -----------------------------------------------------
    o << "\033[42;1;97m"
      << "                            OSC  SERVER                            "
      << ansi::RESET << ansi::CLR_EOL << "\n"
      << ansi::CLR_EOL << "\n";

    // ---- Address ----------------------------------------------------
    o << "Bind address:    "
      << (s.bound ? std::string(ansi::CYAN) + s.host_str + ":" + std::to_string(s.port)
                  : std::string(ansi::RED)  + "(unbound)")
      << ansi::RESET
      << "    Last sender: " << s.last_sender_str
      << "    Received: " << s.total_received
      << ansi::CLR_EOL << "\n";
    o << ansi::DIM << "(type  r <host> <port>  to rebind)"
      << ansi::RESET << ansi::CLR_EOL << "\n";
    o << ansi::CLR_EOL << "\n";

    // Helper: render one selectable push slider. `label` is left-padded
    // to a fixed width so editing the name doesn't mis-align the bars.
    auto draw_slider = [&](int idx, const std::string& label, double v01,
                           const char* fmt_val, double raw) {
        char vbuf[32]; std::snprintf(vbuf, sizeof(vbuf), fmt_val, raw);
        char lbuf[32]; std::snprintf(lbuf, sizeof(lbuf), "%-12s",
                                     label.c_str());
        o << "  " << (s.selected == idx ? ansi::GREEN : ansi::DIM)
          << (s.selected == idx ? ">" : " ") << " "
          << lbuf << " " << bar(v01, 30) << " " << vbuf
          << ansi::RESET << ansi::CLR_EOL << "\n";
    };

    // ---- Sliders (Freq / Gain / LFO) -- mirrors the client's order --
    o << ansi::BOLD << "Sliders" << ansi::RESET
      << ansi::DIM
      << "  (push to last-seen sender; 1..3 select, +/- nudge, !@# push, "
         "L <slot> <name> rename)"
      << ansi::RESET << ansi::CLR_EOL << "\n";
    draw_slider(0, s.name_freq, (s.push_freq - 20.0) / (2000.0 - 20.0),
                "%6.1f Hz", static_cast<double>(s.push_freq));
    draw_slider(1, s.name_gain, s.push_gain, "%5.2f",
                static_cast<double>(s.push_gain));
    draw_slider(2, s.name_lfo,  s.push_lfo,  "%5.2f",
                static_cast<double>(s.push_lfo));
    o << ansi::CLR_EOL << "\n";

    // ---- Theremin ---------------------------------------------------
    o << ansi::BOLD << "Theremin" << ansi::RESET
      << ansi::DIM
      << "  (push to last-seen sender; 4/5 select, $/% push -- "
      << "On/Off lives on the client)"
      << ansi::RESET << ansi::CLR_EOL << "\n";
    o << "    On/Off:     "
      << (s.therm_on_s == "ON"  ? std::string(ansi::GREEN) + "ON " :
          s.therm_on_s == "OFF" ? std::string(ansi::DIM)   + "OFF" :
                                  std::string(ansi::DIM)   + "-  ")
      << ansi::RESET << ansi::CLR_EOL << "\n";
    draw_slider(3, "Pitch      ",
                (s.push_therm_pitch - 80.0) / (2000.0 - 80.0),
                "%6.1f Hz", static_cast<double>(s.push_therm_pitch));
    draw_slider(4, "Volume     ", s.push_therm_vol, "%5.2f",
                static_cast<double>(s.push_therm_vol));
    o << ansi::CLR_EOL << "\n";

    // ---- Action buttons (info row; the actual triggers are stdin) ---
    o << ansi::BOLD << "Buttons" << ansi::RESET
      << ansi::DIM
      << "  (y=tell-client play  n=tell-client stop  b=ping  a=announce)"
      << ansi::RESET << ansi::CLR_EOL << "\n";
    o << ansi::CLR_EOL << "\n";

    // ---- Behaviour --------------------------------------------------
    o << ansi::BOLD << "Behaviour" << ansi::RESET
      << ansi::DIM
      << "  (e=toggle echo  p=toggle pause  c=clear log)"
      << ansi::RESET << ansi::CLR_EOL << "\n";
    o << "    Echo:   " << (s.echo  ? std::string(ansi::GREEN) + "ON " :
                                      std::string(ansi::DIM)   + "OFF")
      << ansi::RESET
      << "    Paused: " << (s.paused ? std::string(ansi::YELLOW) + "YES" :
                                       std::string(ansi::DIM)    + "no ")
      << ansi::RESET << ansi::CLR_EOL << "\n";
    o << ansi::CLR_EOL << "\n";

    // ---- Log --------------------------------------------------------
    o << ansi::BOLD << "Log" << ansi::RESET
      << ansi::DIM   << "  (-> sent / <- received)" << ansi::RESET
      << ansi::CLR_EOL << "\n";
    for (size_t i = 0; i < 12; ++i) {
        o << "  ";
        if (i < s.log.size()) {
            const std::string& line = s.log[i];
            if      (!line.empty() && line[0] == '-') o << ansi::GREEN;
            else if (!line.empty() && line[0] == '<') o << ansi::CYAN;
            else if (!line.empty() && line[0] == '!') o << ansi::RED;
            else                                       o << ansi::YELLOW;
            o << line << ansi::RESET;
        }
        o << ansi::CLR_EOL << "\n";
    }
    o << ansi::CLR_EOL << "\n";
    o << ansi::DIM
      << "Commands: e=echo p=pause c=clear | 1..5=select +/-=nudge !@#$%=push\n"
      << "          b=ping a=announce y=play n=stop | r <host> <port>=rebind | h=help q=quit  (then Enter)"
      << ansi::RESET << ansi::CLR_EOL << "\n";

    std::cout << o.str();
    std::cout.flush();
}

// ----- main ----------------------------------------------------------------

int main(int argc, char** argv) {
    std::string host = (argc > 1) ? argv[1] : "127.0.0.1";
    uint16_t    port = (argc > 2) ? static_cast<uint16_t>(std::atoi(argv[2])) : 9000;

    std::signal(SIGINT, on_sigint);
#ifndef _WIN32
    std::signal(SIGTERM, on_sigint);
#endif

    try {
        osc::enable_ansi();

        State st;
        // Initial bind. If this fails, leave the dashboard running so the
        // user can fix the address with `r <host> <port>`.
        do_rebind(st, host, port);

        std::thread rcv(recv_loop, std::ref(st));
        std::thread inp(input_loop, std::ref(st));

        std::cout << ansi::CLEAR << ansi::HIDE; std::cout.flush();

        while (g_running) {
            Snap s;
            {
                std::lock_guard<std::mutex> lk(st.mtx);
                s.bound          = (st.sock != OSC_INVALID_SOCKET);
                s.host_str       = st.bind_host;
                s.port           = st.bind_port;
                s.echo           = st.echo;
                s.paused         = st.paused;
                s.last_sender_str = st.last_sender_str;
                s.total_received = st.total_received;
                s.therm_on_s      = st.therm_on_s;
                s.push_freq       = st.push_freq;
                s.push_gain       = st.push_gain;
                s.push_lfo        = st.push_lfo;
                s.push_therm_pitch = st.push_therm_pitch;
                s.push_therm_vol   = st.push_therm_vol;
                s.selected        = st.selected;
                s.name_freq       = st.name_freq;
                s.name_gain       = st.name_gain;
                s.name_lfo        = st.name_lfo;
                s.log             = st.log;
            }
            draw(s);
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
        }

        std::cout << ansi::SHOW << ansi::RESET << "\n";
        std::cout.flush();
        {
            std::lock_guard<std::mutex> lk(st.mtx);
            if (st.sock != OSC_INVALID_SOCKET) { osc_close_socket(st.sock); st.sock = OSC_INVALID_SOCKET; }
        }
        if (rcv.joinable()) rcv.join();
        // input thread is blocked on getline; the process will reap it on exit.
        inp.detach();
        osc::platform_shutdown();
    } catch (const std::exception& e) {
        std::cout << ansi::SHOW << ansi::RESET;
        std::cerr << "fatal: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
