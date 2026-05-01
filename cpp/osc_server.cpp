// osc_server.cpp - FLTK GUI OSC server.
//
// Mirrors python/osc_server.py: green banner, editable bind address,
// editable Freq/Gain/LFO push sliders that rename the matching client
// sliders, theremin push (with the client's on/off shown read-only as
// a green/red badge), action buttons, behaviour toggles, and a log.
//
// Build: needs FLTK (libfltk1.3-dev / brew install fltk / vcpkg fltk).
// The Makefile auto-detects fltk-config and reports a helpful error if
// it is not installed.

#include "osc.hpp"

#include <FL/Fl.H>
#include <FL/Fl_Window.H>
#include <FL/Fl_Group.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Value_Slider.H>
#include <FL/Fl_Text_Buffer.H>
#include <FL/Fl_Text_Display.H>
#include <FL/fl_draw.H>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

// ---- Application state (shared across threads) ----------------------------
namespace {

struct AppState {
    std::mutex mtx;

    socket_t    sock      = OSC_INVALID_SOCKET;
    std::string bind_host = "127.0.0.1";
    uint16_t    bind_port = 9000;

    bool echo   = true;
    bool paused = false;

    bool        has_sender = false;
    sockaddr_in last_sender{};
    std::string last_sender_str = "(none)";
    uint64_t    total_received  = 0;

    // Theremin on/off mirrored from the client.
    std::string therm_on_str = "-";

    // Editable display names for the three core sliders.
    std::string name_freq = "Frequency";
    std::string name_gain = "Gain";
    std::string name_lfo  = "LFO";

    // Pending UI updates that the FLTK main thread should drain.
    std::deque<std::string> pending_log;     // lines to append to the log
    bool stats_dirty = true;                 // last sender / received count
    bool therm_dirty = true;                 // therm_on_str changed
};

std::atomic<bool> g_running{true};
AppState g_st;

// ---- FLTK widgets (created in main, accessed from main thread) ------------
Fl_Window*       w_window           = nullptr;
Fl_Input*        w_host_input       = nullptr;
Fl_Input*        w_port_input       = nullptr;
Fl_Box*          w_bind_status      = nullptr;
Fl_Box*          w_last_sender      = nullptr;
Fl_Box*          w_total_received   = nullptr;
Fl_Input*        w_name_freq        = nullptr;
Fl_Input*        w_name_gain        = nullptr;
Fl_Input*        w_name_lfo         = nullptr;
Fl_Value_Slider* w_freq_slider      = nullptr;
Fl_Value_Slider* w_gain_slider      = nullptr;
Fl_Value_Slider* w_lfo_slider       = nullptr;
Fl_Box*          w_therm_badge      = nullptr;
Fl_Value_Slider* w_therm_pitch      = nullptr;
Fl_Value_Slider* w_therm_volume     = nullptr;
Fl_Check_Button* w_echo_check       = nullptr;
Fl_Check_Button* w_paused_check     = nullptr;
Fl_Text_Buffer*  w_log_buffer       = nullptr;
Fl_Text_Display* w_log_display      = nullptr;

// ---- Dark theme -----------------------------------------------------------
// Repaints FLTK's default palette to a dark scheme. Widgets that use the
// default colours pick this up automatically; the few widgets with
// hardcoded colours (badges, banner) are set explicitly later.
void apply_dark_theme() {
    Fl::scheme("gtk+");
    Fl::background (40,  40,  40);            // #282828 window / frame bg
    Fl::background2(30,  30,  30);            // #1E1E1E input / log bg
    Fl::foreground (220, 220, 220);           // #DCDCDC text

    // Auto-computed shading is poor on a dark base, so set the LIGHT/DARK
    // ladder explicitly. Buttons, sliders, and frames use these.
    Fl::set_color(FL_LIGHT3,  72,  72,  72);
    Fl::set_color(FL_LIGHT2,  60,  60,  60);
    Fl::set_color(FL_LIGHT1,  50,  50,  50);
    Fl::set_color(FL_DARK1,   28,  28,  28);
    Fl::set_color(FL_DARK2,   18,  18,  18);
    Fl::set_color(FL_DARK3,   10,  10,  10);
    Fl::set_color(FL_INACTIVE_COLOR, 130, 130, 130);
    Fl::set_color(FL_SELECTION_COLOR, 38, 79, 120);   // #264F78
}

// ---- Helpers --------------------------------------------------------------
void log_line(const std::string& s) {
    std::lock_guard<std::mutex> lk(g_st.mtx);
    g_st.pending_log.push_back(s);
    while (g_st.pending_log.size() > 500) g_st.pending_log.pop_front();
}

void log_line_locked_already(const std::string& s) {
    g_st.pending_log.push_back(s);
    while (g_st.pending_log.size() > 500) g_st.pending_log.pop_front();
}

template <class Arg>
void push_to_sender(const std::string& addr, const Arg& arg) {
    socket_t sock; sockaddr_in dst; bool has; std::string sstr;
    {
        std::lock_guard<std::mutex> lk(g_st.mtx);
        sock = g_st.sock;
        has  = g_st.has_sender;
        if (has) { dst = g_st.last_sender; sstr = g_st.last_sender_str; }
    }
    if (sock == OSC_INVALID_SOCKET) {
        log_line("!  no socket bound");
        return;
    }
    if (!has) {
        log_line("!  no client yet to push '" + addr + "' to");
        return;
    }
    osc::MessageBuilder b(addr); b.add(arg);
    auto pkt = b.build();
    ::sendto(sock, reinterpret_cast<const char*>(pkt.data()),
             static_cast<int>(pkt.size()), 0,
             reinterpret_cast<sockaddr*>(&dst), sizeof(dst));
    std::ostringstream s;
    s << "-> " << sstr << "  " << addr << "  ";
    if constexpr (std::is_same_v<Arg, std::string>) s << "\"" << arg << "\"";
    else if constexpr (std::is_same_v<Arg, bool>)   s << (arg ? "true" : "false");
    else                                            s << arg;
    log_line(s.str());
}

bool do_rebind(const std::string& host, uint16_t port) {
    socket_t old_sock;
    {
        std::lock_guard<std::mutex> lk(g_st.mtx);
        old_sock = g_st.sock;
        g_st.sock = OSC_INVALID_SOCKET;
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

    {
        std::lock_guard<std::mutex> lk(g_st.mtx);
        g_st.sock = new_sock;
        if (new_sock != OSC_INVALID_SOCKET) {
            g_st.bind_host = host;
            g_st.bind_port = port;
            log_line_locked_already("[server] bound to " + host + ":" +
                                    std::to_string(port));
            return true;
        }
        log_line_locked_already("!  bind " + host + ":" +
                                std::to_string(port) + " failed: " + err);
        return false;
    }
}

// ---- Receive thread -------------------------------------------------------
void recv_loop() {
    std::vector<uint8_t> buf(65535);
    while (g_running) {
        socket_t sock; bool paused_now, echo_now;
        {
            std::lock_guard<std::mutex> lk(g_st.mtx);
            sock        = g_st.sock;
            paused_now  = g_st.paused;
            echo_now    = g_st.echo;
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
        if (n <= 0) continue;
        if (paused_now) continue;

        try {
            osc::Message m = osc::decode(buf.data(), static_cast<size_t>(n));
            char ip[64] = {0};
            inet_ntop(AF_INET, &src.sin_addr, ip, sizeof(ip));
            std::ostringstream sender;
            sender << ip << ":" << ntohs(src.sin_port);

            // Format args for the log
            std::ostringstream argstr;
            for (size_t i = 0; i < m.args.size(); ++i) {
                if (i) argstr << ", ";
                const auto& a = m.args[i];
                if      (auto p = std::get_if<int32_t>(&a))     argstr << *p << "i";
                else if (auto p = std::get_if<float>(&a))       argstr << *p << "f";
                else if (auto p = std::get_if<std::string>(&a)) argstr << '"' << *p << '"';
                else if (auto p = std::get_if<bool>(&a))        argstr << (*p ? "true" : "false");
                else if (auto p = std::get_if<osc::Blob>(&a))   argstr << "<blob " << p->size() << "B>";
            }

            {
                std::lock_guard<std::mutex> lk(g_st.mtx);
                g_st.last_sender     = src;
                g_st.has_sender      = true;
                g_st.last_sender_str = sender.str();
                g_st.total_received++;
                g_st.stats_dirty = true;
                if (m.address == "/theremin/on" && !m.args.empty()) {
                    if (auto p = std::get_if<bool>(&m.args[0])) {
                        g_st.therm_on_str = *p ? "ON" : "OFF";
                        g_st.therm_dirty = true;
                    }
                }
                log_line_locked_already("<- " + sender.str() + "  " +
                                        m.address + "  " + argstr.str());
            }

            // Echo back
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
            log_line(std::string("!  decode error: ") + e.what());
        }
    }
}

// ---- FLTK callbacks -------------------------------------------------------
void rebind_cb(Fl_Widget*, void*) {
    const char* host = w_host_input->value();
    int port = std::atoi(w_port_input->value());
    if (port < 0 || port > 65535) {
        w_bind_status->copy_label("invalid port");
        return;
    }
    bool ok = do_rebind(host ? host : "127.0.0.1",
                        static_cast<uint16_t>(port));
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%s",
                  ok ? "bound" : "BIND FAILED");
    w_bind_status->copy_label(buf);
}

void name_freq_cb(Fl_Widget*, void*) {
    std::string s = w_name_freq->value();
    {
        std::lock_guard<std::mutex> lk(g_st.mtx);
        if (g_st.name_freq == s) return;
        g_st.name_freq = s;
    }
    push_to_sender<std::string>("/server/rename/freq", s);
}
void name_gain_cb(Fl_Widget*, void*) {
    std::string s = w_name_gain->value();
    {
        std::lock_guard<std::mutex> lk(g_st.mtx);
        if (g_st.name_gain == s) return;
        g_st.name_gain = s;
    }
    push_to_sender<std::string>("/server/rename/gain", s);
}
void name_lfo_cb(Fl_Widget*, void*) {
    std::string s = w_name_lfo->value();
    {
        std::lock_guard<std::mutex> lk(g_st.mtx);
        if (g_st.name_lfo == s) return;
        g_st.name_lfo = s;
    }
    push_to_sender<std::string>("/server/rename/lfo", s);
}

void freq_cb(Fl_Widget*, void*)   { push_to_sender<float>("/server/freq",
                                       static_cast<float>(w_freq_slider->value())); }
void gain_cb(Fl_Widget*, void*)   { push_to_sender<float>("/server/gain",
                                       static_cast<float>(w_gain_slider->value())); }
void lfo_cb(Fl_Widget*, void*)    { push_to_sender<float>("/server/lfo",
                                       static_cast<float>(w_lfo_slider->value())); }
void therm_pitch_cb(Fl_Widget*, void*) {
    push_to_sender<float>("/server/theremin/pitch",
                          static_cast<float>(w_therm_pitch->value()));
}
void therm_volume_cb(Fl_Widget*, void*) {
    push_to_sender<float>("/server/theremin/volume",
                          static_cast<float>(w_therm_volume->value()));
}

void btn_play_cb (Fl_Widget*, void*) { push_to_sender<bool>("/server/transport/play", true); }
void btn_stop_cb (Fl_Widget*, void*) { push_to_sender<bool>("/server/transport/stop", true); }
void btn_ping_cb (Fl_Widget*, void*) { push_to_sender<bool>("/server/ping", true); }
void btn_announce_cb(Fl_Widget*, void*) {
    push_to_sender<std::string>("/server/announce", "hello from server");
}

void echo_cb(Fl_Widget*, void*) {
    std::lock_guard<std::mutex> lk(g_st.mtx);
    g_st.echo = (w_echo_check->value() != 0);
    log_line_locked_already(std::string("[server] echo ") +
                            (g_st.echo ? "ON" : "OFF"));
}
void paused_cb(Fl_Widget*, void*) {
    std::lock_guard<std::mutex> lk(g_st.mtx);
    g_st.paused = (w_paused_check->value() != 0);
    log_line_locked_already(std::string("[server] paused ") +
                            (g_st.paused ? "ON" : "OFF"));
}
void clear_log_cb(Fl_Widget*, void*) {
    w_log_buffer->text("");
}

// Periodic main-thread tick: drain the pending log and refresh status.
void tick_cb(void*) {
    std::vector<std::string> lines;
    bool stats_dirty = false, therm_dirty = false;
    std::string sender_str, therm_str;
    uint64_t total = 0;

    {
        std::lock_guard<std::mutex> lk(g_st.mtx);
        if (!g_st.pending_log.empty()) {
            lines.assign(g_st.pending_log.begin(), g_st.pending_log.end());
            g_st.pending_log.clear();
        }
        stats_dirty = g_st.stats_dirty; g_st.stats_dirty = false;
        therm_dirty = g_st.therm_dirty; g_st.therm_dirty = false;
        sender_str = g_st.last_sender_str;
        total = g_st.total_received;
        therm_str = g_st.therm_on_str;
    }

    for (const auto& l : lines) {
        w_log_buffer->append(l.c_str());
        w_log_buffer->append("\n");
    }
    if (!lines.empty()) {
        w_log_display->scroll(w_log_buffer->length(), 0);
    }
    if (stats_dirty) {
        w_last_sender->copy_label(sender_str.c_str());
        char tbuf[32]; std::snprintf(tbuf, sizeof(tbuf), "%llu",
                                     static_cast<unsigned long long>(total));
        w_total_received->copy_label(tbuf);
    }
    if (therm_dirty) {
        w_therm_badge->copy_label(therm_str.c_str());
        if (therm_str == "ON") {
            w_therm_badge->color(fl_rgb_color(46, 125, 50));   // green
            w_therm_badge->labelcolor(FL_WHITE);
        } else if (therm_str == "OFF") {
            w_therm_badge->color(fl_rgb_color(198, 40, 40));   // red
            w_therm_badge->labelcolor(FL_WHITE);
        } else {
            w_therm_badge->color(fl_rgb_color(70, 70, 70));    // dark-neutral
            w_therm_badge->labelcolor(FL_WHITE);
        }
        w_therm_badge->redraw();
    }

    Fl::repeat_timeout(0.05, tick_cb);
}
} // namespace

// ---- main -----------------------------------------------------------------
int main(int argc, char** argv) {
    std::string host = (argc > 1) ? argv[1] : "127.0.0.1";
    uint16_t    port = (argc > 2) ? static_cast<uint16_t>(std::atoi(argv[2])) : 9000;
    g_st.bind_host = host;
    g_st.bind_port = port;

    Fl::lock();   // enable thread-safe widget access (we drain via timer)
    apply_dark_theme();

    const int W = 640, H = 760;
    w_window = new Fl_Window(W, H, "OSC Server");

    int y = 0;

    // ---- Banner ----
    auto* banner = new Fl_Box(0, y, W, 44, "OSC  SERVER");
    banner->box(FL_FLAT_BOX);
    banner->color(fl_rgb_color(46, 125, 50));   // #2E7D32
    banner->labelcolor(FL_WHITE);
    banner->labelsize(18);
    banner->labelfont(FL_HELVETICA_BOLD);
    y += 54;

    // ---- Bind address ----
    auto* bind_grp = new Fl_Group(10, y, W-20, 70, "Bind address");
    bind_grp->box(FL_ENGRAVED_FRAME);
    bind_grp->align(FL_ALIGN_TOP_LEFT);
    {
        new Fl_Box(20, y+18, 36, 24, "Host:");
        w_host_input = new Fl_Input(60, y+18, 140, 24);
        w_host_input->value(host.c_str());
        new Fl_Box(210, y+18, 36, 24, "Port:");
        w_port_input = new Fl_Input(250, y+18, 64, 24);
        w_port_input->value(std::to_string(port).c_str());
        auto* rb = new Fl_Button(324, y+18, 70, 24, "Rebind");
        rb->callback(rebind_cb);
        w_bind_status = new Fl_Box(404, y+18, W-424, 24, "(starting)");
        w_bind_status->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    }
    bind_grp->end();
    y += 80;

    // ---- Stats line ----
    new Fl_Box(20, y, 90, 22, "Last sender:");
    w_last_sender = new Fl_Box(110, y, 200, 22, "(none)");
    w_last_sender->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    new Fl_Box(330, y, 80, 22, "Received:");
    w_total_received = new Fl_Box(410, y, 100, 22, "0");
    w_total_received->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    y += 30;

    // ---- Sliders frame (Frequency / Gain / LFO with editable names) ----
    auto* sliders_grp = new Fl_Group(10, y, W-20, 130,
                                     "Sliders (push to last-seen sender)");
    sliders_grp->box(FL_ENGRAVED_FRAME);
    sliders_grp->align(FL_ALIGN_TOP_LEFT);
    {
        const int LX = 20, LW = 110, SX = 140, SW = W - 160;
        // Frequency
        w_name_freq = new Fl_Input(LX, y+12, LW, 24);
        w_name_freq->value("Frequency");
        w_name_freq->callback(name_freq_cb);
        w_name_freq->when(FL_WHEN_ENTER_KEY | FL_WHEN_RELEASE);
        w_freq_slider = new Fl_Value_Slider(SX, y+12, SW, 24);
        w_freq_slider->type(FL_HOR_NICE_SLIDER);
        w_freq_slider->bounds(20, 2000);
        w_freq_slider->value(440.0);
        w_freq_slider->callback(freq_cb);
        // Gain
        w_name_gain = new Fl_Input(LX, y+44, LW, 24);
        w_name_gain->value("Gain");
        w_name_gain->callback(name_gain_cb);
        w_name_gain->when(FL_WHEN_ENTER_KEY | FL_WHEN_RELEASE);
        w_gain_slider = new Fl_Value_Slider(SX, y+44, SW, 24);
        w_gain_slider->type(FL_HOR_NICE_SLIDER);
        w_gain_slider->bounds(0, 1);
        w_gain_slider->value(0.5);
        w_gain_slider->callback(gain_cb);
        // LFO
        w_name_lfo = new Fl_Input(LX, y+76, LW, 24);
        w_name_lfo->value("LFO");
        w_name_lfo->callback(name_lfo_cb);
        w_name_lfo->when(FL_WHEN_ENTER_KEY | FL_WHEN_RELEASE);
        w_lfo_slider = new Fl_Value_Slider(SX, y+76, SW, 24);
        w_lfo_slider->type(FL_HOR_NICE_SLIDER);
        w_lfo_slider->bounds(0, 1);
        w_lfo_slider->value(0.0);
        w_lfo_slider->callback(lfo_cb);
    }
    sliders_grp->end();
    y += 140;

    // ---- Theremin frame ----
    auto* therm_grp = new Fl_Group(10, y, W-20, 120,
                                   "Theremin (push to last-seen sender)");
    therm_grp->box(FL_ENGRAVED_FRAME);
    therm_grp->align(FL_ALIGN_TOP_LEFT);
    {
        // On/Off badge (read-only mirror of the client's switch).
        new Fl_Box(20, y+12, 130, 24, "On/Off (client-side):");
        w_therm_badge = new Fl_Box(150, y+12, 50, 24, "-");
        w_therm_badge->box(FL_FLAT_BOX);
        w_therm_badge->color(fl_rgb_color(70, 70, 70));   // dark-neutral
        w_therm_badge->labelcolor(FL_WHITE);
        w_therm_badge->labelfont(FL_HELVETICA_BOLD);
        // Pitch / Volume push sliders
        const int LX = 20, LW = 110, SX = 140, SW = W - 160;
        new Fl_Box(LX, y+44, LW, 24, "Pitch");
        w_therm_pitch = new Fl_Value_Slider(SX, y+44, SW, 24);
        w_therm_pitch->type(FL_HOR_NICE_SLIDER);
        w_therm_pitch->bounds(80, 2000);
        w_therm_pitch->value(440.0);
        w_therm_pitch->callback(therm_pitch_cb);
        new Fl_Box(LX, y+76, LW, 24, "Volume");
        w_therm_volume = new Fl_Value_Slider(SX, y+76, SW, 24);
        w_therm_volume->type(FL_HOR_NICE_SLIDER);
        w_therm_volume->bounds(0, 1);
        w_therm_volume->value(0.5);
        w_therm_volume->callback(therm_volume_cb);
    }
    therm_grp->end();
    y += 130;

    // ---- Buttons ----
    auto* btn_grp = new Fl_Group(10, y, W-20, 50, "Buttons");
    btn_grp->box(FL_ENGRAVED_FRAME);
    btn_grp->align(FL_ALIGN_TOP_LEFT);
    {
        int bx = 20;
        auto* b1 = new Fl_Button(bx, y+10, 130, 28, "Tell client: Play"); bx += 140;
        b1->callback(btn_play_cb);
        auto* b2 = new Fl_Button(bx, y+10, 130, 28, "Tell client: Stop"); bx += 140;
        b2->callback(btn_stop_cb);
        auto* b3 = new Fl_Button(bx, y+10, 70, 28, "Ping");               bx += 80;
        b3->callback(btn_ping_cb);
        auto* b4 = new Fl_Button(bx, y+10, 90, 28, "Announce");
        b4->callback(btn_announce_cb);
    }
    btn_grp->end();
    y += 60;

    // ---- Behaviour ----
    auto* beh_grp = new Fl_Group(10, y, W-20, 50, "Behaviour");
    beh_grp->box(FL_ENGRAVED_FRAME);
    beh_grp->align(FL_ALIGN_TOP_LEFT);
    {
        w_echo_check = new Fl_Check_Button(20, y+12, 220, 24,
                                           "Echo  (reply with /ack<address>)");
        w_echo_check->value(1);
        w_echo_check->callback(echo_cb);
        w_paused_check = new Fl_Check_Button(260, y+12, 200, 24,
                                             "Pause  (drop incoming)");
        w_paused_check->callback(paused_cb);
        auto* clr = new Fl_Button(W-110, y+10, 80, 28, "Clear log");
        clr->callback(clear_log_cb);
    }
    beh_grp->end();
    y += 60;

    // ---- Log ----
    auto* log_grp = new Fl_Group(10, y, W-20, H-y-10,
                                 "Log  (-> sent  /  <- received)");
    log_grp->box(FL_ENGRAVED_FRAME);
    log_grp->align(FL_ALIGN_TOP_LEFT);
    {
        w_log_buffer = new Fl_Text_Buffer();
        w_log_display = new Fl_Text_Display(20, y+12, W-40, H-y-30);
        w_log_display->buffer(w_log_buffer);
        w_log_display->textfont(FL_COURIER);
        w_log_display->textsize(12);
    }
    log_grp->end();

    w_window->end();
    w_window->resizable(log_grp);
    w_window->show(argc, argv);

    // Initial bind + start network thread + start UI tick
    do_rebind(host, port);
    std::thread rcv(recv_loop);
    Fl::add_timeout(0.05, tick_cb);

    int rc = Fl::run();

    // Shutdown
    g_running = false;
    {
        std::lock_guard<std::mutex> lk(g_st.mtx);
        if (g_st.sock != OSC_INVALID_SOCKET) {
            osc_close_socket(g_st.sock);
            g_st.sock = OSC_INVALID_SOCKET;
        }
    }
    if (rcv.joinable()) rcv.join();
    osc::platform_shutdown();
    return rc;
}
