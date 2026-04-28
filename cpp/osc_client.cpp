// osc_client.cpp - OSC client with a small ANSI-terminal dashboard UI.
//
// Auto-cycles through a demo (frequency sweep, gain breath, LFO ramp,
// transport toggle, periodic /hello) while a background thread reads
// /ack replies from the server. The dashboard redraws in place, showing
// the current state with progress bars and a scrolling log of sent (->)
// and received (<-) messages.
//
// Build: see Makefile (Linux/macOS: just `make`, Windows MinGW: -lws2_32).
// Run:   ./osc_client                         (defaults 127.0.0.1 9000)
//        ./osc_client 192.168.1.20 9000
// Quit:  Ctrl+C

#include "osc.hpp"
#include "theremin.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <mutex>
#include <optional>
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
constexpr const char* CLR_EOL = "\033[K";  // clear to end of line
}

static std::atomic<bool> g_running{true};
static void on_sigint(int) { g_running = false; }

// Shared state: log + currently-targeted destination. Both the input
// thread and the main demo loop need to read/write the destination, so
// it's mutex-protected.
struct State {
    mutable std::mutex mtx;
    sockaddr_in dest{};
    std::string dest_host = "127.0.0.1";
    uint16_t    dest_port = 9000;
    bool        paused = false;     // pause the demo loop
    // Display names for the three core sliders (the server can rename
    // them via /server/rename/{freq,gain,lfo}).
    std::string name_freq = "Frequency";
    std::string name_gain = "Gain";
    std::string name_lfo  = "LFO";

    // Latest server-pushed values for the three core controls. Once set,
    // the demo loop adopts them: the dashboard bar shows the server's
    // value and the next /synth/* message echoes it back.
    std::optional<float> server_freq;
    std::optional<float> server_gain;
    std::optional<float> server_lfo;

    std::deque<std::string> log;

    void add_log(std::string line) {
        std::lock_guard<std::mutex> lk(mtx);
        log.push_back(std::move(line));
        while (log.size() > 12) log.pop_front();
    }
    std::vector<std::string> log_snapshot() const {
        std::lock_guard<std::mutex> lk(mtx);
        return {log.begin(), log.end()};
    }
    sockaddr_in get_dest() const {
        std::lock_guard<std::mutex> lk(mtx);
        return dest;
    }
};

static std::string bar(double v, int width) {
    if (v < 0.0) v = 0.0;
    if (v > 1.0) v = 1.0;
    int filled = static_cast<int>(v * width + 0.5);
    std::string s = "[";
    for (int i = 0; i < width; ++i) s += (i < filled ? "#" : "-");
    s += "]";
    return s;
}

static std::string fmt_args(const osc::Message& m) {
    std::ostringstream s;
    for (size_t i = 0; i < m.args.size(); ++i) {
        if (i) s << ", ";
        const auto& a = m.args[i];
        if      (auto p = std::get_if<int32_t>(&a))     s << *p << "i";
        else if (auto p = std::get_if<float>(&a))       s << *p << "f";
        else if (auto p = std::get_if<std::string>(&a)) s << '"' << *p << '"';
        else if (auto p = std::get_if<bool>(&a))        s << (*p ? "true" : "false");
        else if (auto p = std::get_if<osc::Blob>(&a))   s << "<blob " << p->size() << "B>";
    }
    return s.str();
}

// Theremin + outgoing socket are global so the recv loop and input loop
// can both reach them without threading them through every signature.
static osc::Theremin* g_therm     = nullptr;        // set in main()
static socket_t       g_send_sock = OSC_INVALID_SOCKET;

static void recv_loop(socket_t sock, State& st) {
    std::vector<uint8_t> buf(65535);
    while (g_running) {
        sockaddr_in src{};
        socklen_t_compat slen = sizeof(src);
        int n = ::recvfrom(sock, reinterpret_cast<char*>(buf.data()),
                           static_cast<int>(buf.size()), 0,
                           reinterpret_cast<sockaddr*>(&src), &slen);
        if (n <= 0) continue;  // timeout or transient error
        try {
            osc::Message m = osc::decode(buf.data(), static_cast<size_t>(n));
            st.add_log("<- " + m.address + "  " + fmt_args(m));
            // Server-pushed theremin updates: drive the local synth.
            if (g_therm && !m.args.empty()) {
                if (m.address == "/server/theremin/pitch") {
                    if (auto p = std::get_if<float>(&m.args[0])) g_therm->set_pitch(*p);
                } else if (m.address == "/server/theremin/volume") {
                    if (auto p = std::get_if<float>(&m.args[0])) g_therm->set_volume(*p);
                }
            }
            // Server-pushed rename: relabel our core sliders.
            if (!m.args.empty()) {
                if (auto p = std::get_if<std::string>(&m.args[0])) {
                    std::lock_guard<std::mutex> lk(st.mtx);
                    if      (m.address == "/server/rename/freq") st.name_freq = *p;
                    else if (m.address == "/server/rename/gain") st.name_gain = *p;
                    else if (m.address == "/server/rename/lfo")  st.name_lfo  = *p;
                }
            }
            // Server-pushed values for the three core sliders.
            if (!m.args.empty()) {
                if (auto p = std::get_if<float>(&m.args[0])) {
                    std::lock_guard<std::mutex> lk(st.mtx);
                    if      (m.address == "/server/freq") st.server_freq = *p;
                    else if (m.address == "/server/gain") st.server_gain = *p;
                    else if (m.address == "/server/lfo")  st.server_lfo  = *p;
                }
            }
        } catch (const std::exception& e) {
            st.add_log(std::string("!  decode: ") + e.what());
        }
    }
}

// Stdin command loop. Commands are line-buffered: type and press Enter.
//   t <host> <port>   change target address
//   T                 toggle theremin on/off (sends /theremin/on)
//   P <hz>            set theremin pitch
//   V <0..1>          set theremin volume
//   p                 toggle pause (stop sending demo messages)
//   c                 clear log
//   h                 show help
//   q                 quit
template <class Arg>
static void send_to_dest(State& st, const std::string& addr, const Arg& a) {
    if (g_send_sock == OSC_INVALID_SOCKET) return;
    osc::MessageBuilder b(addr); b.add(a);
    sockaddr_in d;
    { std::lock_guard<std::mutex> lk(st.mtx); d = st.dest; }
    osc::send_to(g_send_sock, d, b.build());
}

static void input_loop(State& st) {
    std::string line;
    while (g_running && std::getline(std::cin, line)) {
        if (line.empty()) continue;
        std::istringstream iss(line);
        std::string cmd; iss >> cmd;

        if (cmd == "q" || cmd == "Q") { g_running = false; return; }

        if (cmd == "t" || cmd == "target") {
            std::string h; int p = 0;
            if ((iss >> h >> p) && p >= 0 && p <= 65535) {
                try {
                    sockaddr_in d = osc::make_dest(h, static_cast<uint16_t>(p));
                    {
                        std::lock_guard<std::mutex> lk(st.mtx);
                        st.dest = d;
                        st.dest_host = h;
                        st.dest_port = static_cast<uint16_t>(p);
                    }
                    st.add_log("[client] target = " + h + ":" + std::to_string(p));
                } catch (const std::exception& e) {
                    st.add_log(std::string("!  target error: ") + e.what());
                }
            } else {
                st.add_log("!  usage: t <host> <port>");
            }
            continue;
        }

        if (cmd == "p") {
            std::lock_guard<std::mutex> lk(st.mtx);
            st.paused = !st.paused;
            st.log.push_back(std::string("[client] demo ") +
                             (st.paused ? "PAUSED" : "RUNNING"));
            while (st.log.size() > 12) st.log.pop_front();
            continue;
        }
        if (cmd == "c") {
            std::lock_guard<std::mutex> lk(st.mtx);
            st.log.clear();
            continue;
        }
        if (cmd == "h" || cmd == "?") {
            st.add_log("[help] t <host> <port>=retarget  p=pause demo  c=clear  q=quit");
            st.add_log("[help] T=theremin on/off  P <hz>=pitch  V <0..1>=volume");
            continue;
        }

        if (cmd == "T") {
            if (g_therm) {
                bool now = !g_therm->on();
                g_therm->set_on(now);
                st.add_log(std::string("[client] theremin ") + (now ? "ON" : "OFF"));
                send_to_dest(st, "/theremin/on", now);
            }
            continue;
        }
        if (cmd == "P") {
            float hz = 0.0f;
            if (iss >> hz && hz >= 20.0f && hz <= 20000.0f && g_therm) {
                g_therm->set_pitch(hz);
                st.add_log("[client] theremin pitch = " + std::to_string(hz) + " Hz");
                send_to_dest(st, "/theremin/pitch", hz);
            } else {
                st.add_log("!  usage: P <hz>");
            }
            continue;
        }
        if (cmd == "V") {
            float v = 0.0f;
            if (iss >> v && v >= 0.0f && v <= 1.0f && g_therm) {
                g_therm->set_volume(v);
                st.add_log("[client] theremin volume = " + std::to_string(v));
                send_to_dest(st, "/theremin/volume", v);
            } else {
                st.add_log("!  usage: V <0..1>");
            }
            continue;
        }
    }
}

static void draw(const std::string& host, uint16_t port, uint16_t local_port,
                 const std::string& name_freq, const std::string& name_gain,
                 const std::string& name_lfo,
                 double freq, double gain, double lfo, bool playing,
                 bool paused, const std::vector<std::string>& lines) {
    std::ostringstream o;
    o << ansi::HOME;

    // ---- Banner -----------------------------------------------------
    // Bright blue background, white bold text, full-width.
    o << "\033[44;1;97m"
      << "                            OSC  CLIENT                            "
      << ansi::RESET << ansi::CLR_EOL << "\n"
      << ansi::CLR_EOL << "\n";

    // ---- Address ----------------------------------------------------
    o << "Target address:  " << ansi::CYAN << host << ":" << port << ansi::RESET
      << "    Local recv port: " << local_port << ansi::CLR_EOL << "\n";
    o << ansi::DIM << "(type  t <host> <port>  to retarget)"
      << ansi::RESET << ansi::CLR_EOL << "\n";
    o << ansi::CLR_EOL << "\n";

    // ---- State (Frequency / Gain / LFO bars from the demo) ----------
    char fbuf[64];
    o << ansi::BOLD << "Sliders" << ansi::RESET
      << ansi::DIM   << "  (driven by the auto-demo)" << ansi::RESET
      << ansi::CLR_EOL << "\n";
    auto pad = [](const std::string& name) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%-12s", (name + ":").c_str());
        return std::string(buf);
    };
    std::snprintf(fbuf, sizeof(fbuf), "%6.1f Hz", freq);
    o << "  " << pad(name_freq) << bar((freq - 220.0) / 660.0, 30) << " " << fbuf
      << ansi::CLR_EOL << "\n";
    std::snprintf(fbuf, sizeof(fbuf), "%5.2f", gain);
    o << "  " << pad(name_gain) << bar(gain, 30) << " " << fbuf
      << ansi::CLR_EOL << "\n";
    std::snprintf(fbuf, sizeof(fbuf), "%5.2f", lfo);
    o << "  " << pad(name_lfo)  << bar(lfo, 30) << " " << fbuf
      << ansi::CLR_EOL << "\n";
    o << ansi::CLR_EOL << "\n";

    // ---- Theremin ---------------------------------------------------
    if (g_therm) {
        o << ansi::BOLD << "Theremin" << ansi::RESET
          << ansi::DIM   << "  (T=on/off  P <hz>  V <0..1>)" << ansi::RESET
          << ansi::CLR_EOL << "\n";
        o << "  On/Off:     "
          << (g_therm->on() ? std::string(ansi::GREEN) + "ON "
                            : std::string(ansi::DIM)   + "OFF")
          << ansi::RESET << ansi::CLR_EOL << "\n";
        std::snprintf(fbuf, sizeof(fbuf), "%6.1f Hz", g_therm->pitch());
        o << "  Pitch:      "
          << bar((g_therm->pitch() - 80.0) / (2000.0 - 80.0), 30) << " " << fbuf
          << ansi::CLR_EOL << "\n";
        std::snprintf(fbuf, sizeof(fbuf), "%5.2f", g_therm->volume());
        o << "  Volume:     " << bar(g_therm->volume(), 30) << " " << fbuf
          << ansi::CLR_EOL << "\n";
        o << ansi::DIM << "  backend: " << g_therm->backend() << ansi::RESET
          << ansi::CLR_EOL << "\n";
        o << ansi::CLR_EOL << "\n";
    }

    // ---- Action / Behaviour line ------------------------------------
    o << ansi::BOLD << "Behaviour" << ansi::RESET << ansi::CLR_EOL << "\n";
    o << "  Demo:       " << (paused ? std::string(ansi::YELLOW) + "PAUSED"
                                     : std::string(ansi::GREEN)  + "RUNNING")
      << ansi::RESET
      << "    Transport: "
      << (playing ? std::string(ansi::GREEN) + ">> PLAYING"
                  : std::string(ansi::DIM)   + "[] STOPPED")
      << ansi::RESET << ansi::CLR_EOL << "\n";
    o << ansi::CLR_EOL << "\n";

    // ---- Log --------------------------------------------------------
    o << ansi::BOLD << "Log" << ansi::RESET
      << ansi::DIM   << "  (-> sent / <- received)" << ansi::RESET
      << ansi::CLR_EOL << "\n";
    for (size_t i = 0; i < 12; ++i) {
        o << "  ";
        if (i < lines.size()) {
            const std::string& s = lines[i];
            if (!s.empty() && s[0] == '-')      o << ansi::GREEN;
            else if (!s.empty() && s[0] == '<') o << ansi::CYAN;
            else                                 o << ansi::YELLOW;
            o << s << ansi::RESET;
        }
        o << ansi::CLR_EOL << "\n";
    }
    o << ansi::CLR_EOL << "\n";

    // ---- Command bar ------------------------------------------------
    o << ansi::DIM
      << "Commands: t <host> <port>=retarget  p=pause demo  c=clear  q=quit\n"
      << "          T=theremin on/off  P <hz>=pitch  V <0..1>=volume   (then Enter)"
      << ansi::RESET << ansi::CLR_EOL << "\n";

    std::cout << o.str();
    std::cout.flush();
}

int main(int argc, char** argv) {
    std::string host = (argc > 1) ? argv[1] : "127.0.0.1";
    uint16_t    port = (argc > 2) ? static_cast<uint16_t>(std::atoi(argv[2])) : 9000;

    std::signal(SIGINT, on_sigint);
#ifndef _WIN32
    std::signal(SIGTERM, on_sigint);
#endif

    try {
        osc::enable_ansi();

        socket_t sock = osc::make_udp_socket();
        osc::bind_udp(sock, "0.0.0.0", 0);          // ephemeral local port
        osc::set_recv_timeout_ms(sock, 200);
        uint16_t local_port = osc::local_port_of(sock);

        State st;
        st.dest      = osc::make_dest(host, port);
        st.dest_host = host;
        st.dest_port = port;

        osc::Theremin therm;
        g_therm     = &therm;
        g_send_sock = sock;

        std::thread rcv(recv_loop, sock, std::ref(st));
        std::thread inp(input_loop, std::ref(st));

        std::cout << ansi::CLEAR << ansi::HIDE; std::cout.flush();

        double t = 0.0;
        bool playing = false;
        int  hello_n = 0;
        int  tick = 0;

        while (g_running) {
            bool paused;
            sockaddr_in dest;
            std::string cur_host, cur_name_freq, cur_name_gain, cur_name_lfo;
            uint16_t cur_port;
            std::optional<float> sf, sg, sl;
            {
                std::lock_guard<std::mutex> lk(st.mtx);
                paused        = st.paused;
                dest          = st.dest;
                cur_host      = st.dest_host;
                cur_port      = st.dest_port;
                cur_name_freq = st.name_freq;
                cur_name_gain = st.name_gain;
                cur_name_lfo  = st.name_lfo;
                sf = st.server_freq;
                sg = st.server_gain;
                sl = st.server_lfo;
            }

            // Animated values; once the server has pushed a value, that
            // sticks and the demo follows the server's set point.
            double freq = sf ? static_cast<double>(*sf)
                             : 220.0 + 330.0 * (1.0 + std::sin(t * 0.6));   // 220..880
            double gain = sg ? static_cast<double>(*sg)
                             : 0.5  + 0.45 * std::sin(t * 0.9);             // 0.05..0.95
            double lfo  = sl ? static_cast<double>(*sl)
                             : std::fmod(t * 0.25, 1.0);                    // 0..1 sawtooth

            if (!paused) {
                if (tick % 4 == 0) {
                    osc::MessageBuilder b("/synth/freq"); b.add(static_cast<float>(freq));
                    osc::send_to(sock, dest, b.build());
                    st.add_log("-> /synth/freq " + std::to_string(static_cast<int>(freq)) + ".0");
                }
                if (tick % 4 == 2) {
                    osc::MessageBuilder b("/mixer/channel/1/gain"); b.add(static_cast<float>(gain));
                    osc::send_to(sock, dest, b.build());
                    char buf[32]; std::snprintf(buf, sizeof(buf), "%.2f", gain);
                    st.add_log(std::string("-> /mixer/channel/1/gain ") + buf);
                }
                if (tick % 8 == 4) {
                    osc::MessageBuilder b("/lfo/value"); b.add(static_cast<float>(lfo));
                    osc::send_to(sock, dest, b.build());
                    char buf[32]; std::snprintf(buf, sizeof(buf), "%.2f", lfo);
                    st.add_log(std::string("-> /lfo/value ") + buf);
                }
                if (tick % 80 == 0) {
                    playing = !playing;
                    osc::MessageBuilder b(playing ? "/transport/play" : "/transport/stop");
                    b.add(true);
                    osc::send_to(sock, dest, b.build());
                    st.add_log(playing ? "-> /transport/play T" : "-> /transport/stop T");
                }
                if (tick % 60 == 30) {
                    std::string greet = "world " + std::to_string(hello_n++);
                    osc::MessageBuilder b("/hello"); b.add(greet);
                    osc::send_to(sock, dest, b.build());
                    st.add_log("-> /hello \"" + greet + "\"");
                }
            }

            draw(cur_host, cur_port, local_port,
                 cur_name_freq, cur_name_gain, cur_name_lfo,
                 freq, gain, lfo, playing, paused, st.log_snapshot());

            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            t += 0.05;
            ++tick;
        }

        // Clean up: unblock the recv thread, then exit.
        std::cout << ansi::SHOW << ansi::RESET << "\n";
        std::cout.flush();
        therm.stop();
        g_therm = nullptr;
        osc_close_socket(sock);
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
