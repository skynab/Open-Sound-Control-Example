// osc_client.cpp - FLTK GUI OSC client.
//
// Mirrors python/osc_client.py: blue banner, editable target address,
// Frequency/Gain/LFO sliders (server can rename them), Theremin frame
// with On/Off button + Test Sound + Pitch + Volume sliders driving a
// real digital synth, action buttons, note trigger, and a log.
//
// Build: needs FLTK (libfltk1.3-dev / brew install fltk / vcpkg fltk).
// Audio uses the theremin.hpp backends (ALSA / CoreAudio / WinMM /
// silent fallback). The Makefile auto-detects FLTK and the audio libs.

#include "osc.hpp"
#include "theremin.hpp"

#include <FL/Fl.H>
#include <FL/Fl_Window.H>
#include <FL/Fl_Group.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Toggle_Button.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Int_Input.H>
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
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

namespace {

// ---- Application state ----------------------------------------------------
struct AppState {
    std::mutex mtx;

    socket_t    sock      = OSC_INVALID_SOCKET;
    sockaddr_in dest{};
    std::string dest_host = "127.0.0.1";
    uint16_t    dest_port = 9000;
    uint16_t    local_port = 0;

    // Display names for the three core sliders. The server can rename
    // them via /server/rename/{freq,gain,lfo} messages.
    std::string name_freq = "Frequency";
    std::string name_gain = "Gain";
    std::string name_lfo  = "LFO";

    // Server-pushed values waiting to be applied to widgets on the main
    // thread (we can't touch widgets from the recv thread directly).
    std::optional<float> pending_freq;
    std::optional<float> pending_gain;
    std::optional<float> pending_lfo;
    std::optional<float> pending_therm_pitch;
    std::optional<float> pending_therm_volume;
    std::optional<bool>  pending_play;

    bool names_dirty = false;

    std::deque<std::string> pending_log;
};

std::atomic<bool> g_running{true};
AppState g_st;
osc::Theremin* g_therm = nullptr;

// ---- FLTK widgets ---------------------------------------------------------
Fl_Window*       w_window         = nullptr;
Fl_Input*        w_host_input     = nullptr;
Fl_Input*        w_port_input     = nullptr;
Fl_Box*          w_dest_status    = nullptr;
Fl_Box*          w_name_freq_lbl  = nullptr;
Fl_Box*          w_name_gain_lbl  = nullptr;
Fl_Box*          w_name_lfo_lbl   = nullptr;
Fl_Value_Slider* w_freq_slider    = nullptr;
Fl_Value_Slider* w_gain_slider    = nullptr;
Fl_Value_Slider* w_lfo_slider     = nullptr;
Fl_Toggle_Button* w_therm_btn     = nullptr;
Fl_Box*          w_therm_status   = nullptr;
Fl_Value_Slider* w_therm_pitch    = nullptr;
Fl_Value_Slider* w_therm_volume   = nullptr;
Fl_Box*          w_audio_warning  = nullptr;
Fl_Int_Input*    w_note_input     = nullptr;
Fl_Int_Input*    w_vel_input      = nullptr;
Fl_Text_Buffer*  w_log_buffer     = nullptr;
Fl_Text_Display* w_log_display    = nullptr;

// ---- Dark theme -----------------------------------------------------------
// Same palette as the server. Repaints FLTK's defaults so widgets pick
// it up automatically; widgets with hardcoded colours (banner, warning
// panel) are set explicitly later.
void apply_dark_theme() {
    Fl::scheme("gtk+");
    Fl::background (40,  40,  40);            // #282828 window / frame bg
    Fl::background2(30,  30,  30);            // #1E1E1E input / log bg
    Fl::foreground (220, 220, 220);           // #DCDCDC text

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

template <class Arg>
void send_msg(const std::string& addr, const Arg& arg) {
    socket_t sock; sockaddr_in dest;
    {
        std::lock_guard<std::mutex> lk(g_st.mtx);
        sock = g_st.sock;
        dest = g_st.dest;
    }
    if (sock == OSC_INVALID_SOCKET) return;
    osc::MessageBuilder b(addr); b.add(arg);
    auto pkt = b.build();
    osc::send_to(sock, dest, pkt);
    std::ostringstream s;
    s << "-> " << addr << "  ";
    if constexpr (std::is_same_v<Arg, std::string>) s << "\"" << arg << "\"";
    else if constexpr (std::is_same_v<Arg, bool>)   s << (arg ? "true" : "false");
    else                                            s << arg;
    log_line(s.str());
}

void send_two_ints(const std::string& addr, int32_t a, int32_t b) {
    socket_t sock; sockaddr_in dest;
    {
        std::lock_guard<std::mutex> lk(g_st.mtx);
        sock = g_st.sock;
        dest = g_st.dest;
    }
    if (sock == OSC_INVALID_SOCKET) return;
    osc::MessageBuilder mb(addr); mb.add(a); mb.add(b);
    osc::send_to(sock, dest, mb.build());
    std::ostringstream s;
    s << "-> " << addr << "  " << a << ", " << b;
    log_line(s.str());
}

// ---- Receive thread -------------------------------------------------------
void recv_loop() {
    std::vector<uint8_t> buf(65535);
    while (g_running) {
        socket_t sock;
        { std::lock_guard<std::mutex> lk(g_st.mtx); sock = g_st.sock; }
        if (sock == OSC_INVALID_SOCKET) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        sockaddr_in src{};
        socklen_t_compat slen = sizeof(src);
        int n = ::recvfrom(sock, reinterpret_cast<char*>(buf.data()),
                           static_cast<int>(buf.size()), 0,
                           reinterpret_cast<sockaddr*>(&src), &slen);
        if (n <= 0) continue;

        try {
            osc::Message m = osc::decode(buf.data(), static_cast<size_t>(n));
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
            log_line("<- " + m.address + "  " + argstr.str());

            if (!m.args.empty()) {
                std::lock_guard<std::mutex> lk(g_st.mtx);
                if (auto p = std::get_if<float>(&m.args[0])) {
                    if      (m.address == "/server/freq")             g_st.pending_freq = *p;
                    else if (m.address == "/server/gain")             g_st.pending_gain = *p;
                    else if (m.address == "/server/lfo")              g_st.pending_lfo = *p;
                    else if (m.address == "/server/theremin/pitch")   g_st.pending_therm_pitch = *p;
                    else if (m.address == "/server/theremin/volume")  g_st.pending_therm_volume = *p;
                }
                if (std::holds_alternative<bool>(m.args[0])) {
                    if      (m.address == "/server/transport/play") g_st.pending_play = true;
                    else if (m.address == "/server/transport/stop") g_st.pending_play = false;
                }
                if (auto p = std::get_if<std::string>(&m.args[0])) {
                    if      (m.address == "/server/rename/freq") { g_st.name_freq = *p; g_st.names_dirty = true; }
                    else if (m.address == "/server/rename/gain") { g_st.name_gain = *p; g_st.names_dirty = true; }
                    else if (m.address == "/server/rename/lfo")  { g_st.name_lfo  = *p; g_st.names_dirty = true; }
                }
            }
        } catch (const std::exception& e) {
            log_line(std::string("!  decode error: ") + e.what());
        }
    }
}

// ---- Helpers used by callbacks --------------------------------------------
void apply_dest(const char* host_c, const char* port_c) {
    std::string h = host_c ? host_c : "127.0.0.1";
    if (h.empty()) h = "127.0.0.1";
    int p = std::atoi(port_c ? port_c : "9000");
    if (p < 0 || p > 65535) {
        log_line("!  invalid port");
        return;
    }
    try {
        sockaddr_in d = osc::make_dest(h, static_cast<uint16_t>(p));
        std::lock_guard<std::mutex> lk(g_st.mtx);
        g_st.dest      = d;
        g_st.dest_host = h;
        g_st.dest_port = static_cast<uint16_t>(p);
    } catch (const std::exception& e) {
        log_line(std::string("!  apply target failed: ") + e.what());
        return;
    }
    char buf[160];
    std::snprintf(buf, sizeof(buf),
                  "sending to %s:%d   listening on 127.0.0.1:%u",
                  h.c_str(), p, static_cast<unsigned>(g_st.local_port));
    w_dest_status->copy_label(buf);
}

void update_therm_status() {
    if (!g_therm) return;
    char buf[160];
    std::snprintf(buf, sizeof(buf), "%s | %.1f Hz, %.2f  | %s",
                  g_therm->on() ? "ON " : "OFF",
                  g_therm->pitch(), g_therm->volume(),
                  g_therm->backend().c_str());
    w_therm_status->copy_label(buf);
}

// ---- Callbacks ------------------------------------------------------------
void apply_dest_cb(Fl_Widget*, void*) {
    apply_dest(w_host_input->value(), w_port_input->value());
}
void freq_cb(Fl_Widget*, void*)  { send_msg<float>("/synth/freq",          static_cast<float>(w_freq_slider->value())); }
void gain_cb(Fl_Widget*, void*)  { send_msg<float>("/mixer/channel/1/gain",static_cast<float>(w_gain_slider->value())); }
void lfo_cb(Fl_Widget*, void*)   { send_msg<float>("/lfo/value",           static_cast<float>(w_lfo_slider->value())); }

void therm_btn_cb(Fl_Widget*, void*) {
    bool on = w_therm_btn->value() != 0;
    if (g_therm) g_therm->set_on(on);
    w_therm_btn->copy_label(on ? "Theremin: ON " : "Theremin: OFF");
    update_therm_status();
    send_msg<bool>("/theremin/on", on);
    log_line(on ? std::string("[theremin] ON  (") + (g_therm ? g_therm->backend() : "") + ")"
                : std::string("[theremin] OFF"));
}
void therm_pitch_cb(Fl_Widget*, void*) {
    float v = static_cast<float>(w_therm_pitch->value());
    if (g_therm) g_therm->set_pitch(v);
    update_therm_status();
    send_msg<float>("/theremin/pitch", v);
}
void therm_volume_cb(Fl_Widget*, void*) {
    float v = static_cast<float>(w_therm_volume->value());
    if (g_therm) g_therm->set_volume(v);
    update_therm_status();
    send_msg<float>("/theremin/volume", v);
}

// 1-second test tone state (restored after the timeout).
struct TestSoundState { float pitch; float vol; bool on; };
TestSoundState g_test_prev{};
void test_sound_restore_cb(void*) {
    if (!g_therm) return;
    g_therm->set_pitch(g_test_prev.pitch);
    g_therm->set_volume(g_test_prev.vol);
    g_therm->set_on(g_test_prev.on);
    update_therm_status();
}
void test_sound_cb(Fl_Widget*, void*) {
    if (!g_therm || !g_therm->available()) {
        log_line("!  no audio backend; install sounddevice / libasound2-dev");
        return;
    }
    g_test_prev.pitch = g_therm->pitch();
    g_test_prev.vol   = g_therm->volume();
    g_test_prev.on    = g_therm->on();
    g_therm->set_pitch(440.0f);
    g_therm->set_volume(0.7f);
    g_therm->set_on(true);
    update_therm_status();
    log_line("[theremin] Test Sound: 440 Hz for 1 second");
    Fl::add_timeout(1.0, test_sound_restore_cb);
}

void play_cb(Fl_Widget*, void*)  { send_msg<bool>("/transport/play", true); }
void stop_cb(Fl_Widget*, void*)  { send_msg<bool>("/transport/stop", true); }
void hello_cb(Fl_Widget*, void*) { send_msg<std::string>("/hello", "world"); }
void note_trigger_cb(Fl_Widget*, void*) {
    int note = std::atoi(w_note_input->value());
    int vel  = std::atoi(w_vel_input->value());
    send_two_ints("/synth/note", note, vel);
}
void clear_log_cb(Fl_Widget*, void*) { w_log_buffer->text(""); }

// Periodic main-thread tick: drain queues and apply server-pushed
// updates to the widgets.
void tick_cb(void*) {
    std::vector<std::string> lines;
    std::optional<float> freq, gain, lfo, tp, tv;
    std::optional<bool> play;
    bool names_dirty = false;
    std::string nf, ng, nl;

    {
        std::lock_guard<std::mutex> lk(g_st.mtx);
        if (!g_st.pending_log.empty()) {
            lines.assign(g_st.pending_log.begin(), g_st.pending_log.end());
            g_st.pending_log.clear();
        }
        freq = g_st.pending_freq;          g_st.pending_freq.reset();
        gain = g_st.pending_gain;          g_st.pending_gain.reset();
        lfo  = g_st.pending_lfo;           g_st.pending_lfo.reset();
        tp   = g_st.pending_therm_pitch;   g_st.pending_therm_pitch.reset();
        tv   = g_st.pending_therm_volume;  g_st.pending_therm_volume.reset();
        play = g_st.pending_play;          g_st.pending_play.reset();
        names_dirty = g_st.names_dirty;    g_st.names_dirty = false;
        nf = g_st.name_freq; ng = g_st.name_gain; nl = g_st.name_lfo;
    }

    for (const auto& l : lines) {
        w_log_buffer->append(l.c_str());
        w_log_buffer->append("\n");
    }
    if (!lines.empty()) w_log_display->scroll(w_log_buffer->length(), 0);

    // Setting the slider value programmatically and then firing the
    // callback so the new value gets sent on as /synth/* (mirrors the
    // tkinter Scale behavior on the Python side).
    if (freq) { w_freq_slider->value(*freq); freq_cb(w_freq_slider, nullptr); }
    if (gain) { w_gain_slider->value(*gain); gain_cb(w_gain_slider, nullptr); }
    if (lfo)  { w_lfo_slider->value(*lfo);   lfo_cb(w_lfo_slider, nullptr); }
    if (tp)   { w_therm_pitch->value(*tp);   therm_pitch_cb(w_therm_pitch, nullptr); }
    if (tv)   { w_therm_volume->value(*tv);  therm_volume_cb(w_therm_volume, nullptr); }
    if (play) {
        // Just log; the client doesn't have a transport to drive locally.
        log_line(*play ? "[client] server told us: play"
                       : "[client] server told us: stop");
    }
    if (names_dirty) {
        w_name_freq_lbl->copy_label(nf.c_str());
        w_name_gain_lbl->copy_label(ng.c_str());
        w_name_lfo_lbl ->copy_label(nl.c_str());
        w_name_freq_lbl->redraw();
        w_name_gain_lbl->redraw();
        w_name_lfo_lbl ->redraw();
    }

    Fl::repeat_timeout(0.05, tick_cb);
}

} // namespace

// ---- main -----------------------------------------------------------------
int main(int argc, char** argv) {
    std::string host = (argc > 1) ? argv[1] : "127.0.0.1";
    uint16_t    port = (argc > 2) ? static_cast<uint16_t>(std::atoi(argv[2])) : 9000;

    Fl::lock();
    apply_dark_theme();

    // Create theremin synth (audio thread starts immediately; output is
    // gated by on/off).
    osc::Theremin therm;
    g_therm = &therm;

    // Open the UDP socket once; ephemeral local port so the server's
    // /ack replies route back.
    socket_t sock = osc::make_udp_socket();
    osc::bind_udp(sock, "0.0.0.0", 0);
    osc::set_recv_timeout_ms(sock, 200);
    {
        std::lock_guard<std::mutex> lk(g_st.mtx);
        g_st.sock       = sock;
        g_st.dest       = osc::make_dest(host, port);
        g_st.dest_host  = host;
        g_st.dest_port  = port;
        g_st.local_port = osc::local_port_of(sock);
    }

    const int W = 600, H = 760;
    w_window = new Fl_Window(W, H, "OSC Client");

    int y = 0;

    // ---- Banner ----
    auto* banner = new Fl_Box(0, y, W, 44, "OSC  CLIENT");
    banner->box(FL_FLAT_BOX);
    banner->color(fl_rgb_color(21, 101, 192));   // #1565C0
    banner->labelcolor(FL_WHITE);
    banner->labelsize(18);
    banner->labelfont(FL_HELVETICA_BOLD);
    y += 54;

    // ---- Target address ----
    auto* addr_grp = new Fl_Group(10, y, W-20, 70, "Target address");
    addr_grp->box(FL_ENGRAVED_FRAME);
    addr_grp->align(FL_ALIGN_TOP_LEFT);
    {
        new Fl_Box(20, y+18, 36, 24, "Host:");
        w_host_input = new Fl_Input(60, y+18, 140, 24);
        w_host_input->value(host.c_str());
        new Fl_Box(210, y+18, 36, 24, "Port:");
        w_port_input = new Fl_Input(250, y+18, 64, 24);
        w_port_input->value(std::to_string(port).c_str());
        auto* apply = new Fl_Button(324, y+18, 70, 24, "Apply");
        apply->callback(apply_dest_cb);
    }
    addr_grp->end();
    y += 80;

    char dest_buf[160];
    std::snprintf(dest_buf, sizeof(dest_buf),
                  "sending to %s:%u   listening on 127.0.0.1:%u",
                  host.c_str(), port,
                  static_cast<unsigned>(g_st.local_port));
    w_dest_status = new Fl_Box(20, y, W-40, 22, "");
    w_dest_status->copy_label(dest_buf);
    w_dest_status->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    w_dest_status->labelfont(FL_HELVETICA_BOLD);
    y += 28;

    // ---- Sliders (Frequency / Gain / LFO) ----
    auto* sliders_grp = new Fl_Group(10, y, W-20, 130,
                                     "Sliders (send on change)");
    sliders_grp->box(FL_ENGRAVED_FRAME);
    sliders_grp->align(FL_ALIGN_TOP_LEFT);
    {
        const int LX = 20, LW = 110, SX = 140, SW = W - 160;
        w_name_freq_lbl = new Fl_Box(LX, y+12, LW, 24, "Frequency");
        w_name_freq_lbl->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
        w_freq_slider = new Fl_Value_Slider(SX, y+12, SW, 24);
        w_freq_slider->type(FL_HOR_NICE_SLIDER);
        w_freq_slider->bounds(20, 2000);
        w_freq_slider->value(440.0);
        w_freq_slider->callback(freq_cb);

        w_name_gain_lbl = new Fl_Box(LX, y+44, LW, 24, "Gain");
        w_name_gain_lbl->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
        w_gain_slider = new Fl_Value_Slider(SX, y+44, SW, 24);
        w_gain_slider->type(FL_HOR_NICE_SLIDER);
        w_gain_slider->bounds(0, 1);
        w_gain_slider->value(0.75);
        w_gain_slider->callback(gain_cb);

        w_name_lfo_lbl = new Fl_Box(LX, y+76, LW, 24, "LFO");
        w_name_lfo_lbl->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
        w_lfo_slider = new Fl_Value_Slider(SX, y+76, SW, 24);
        w_lfo_slider->type(FL_HOR_NICE_SLIDER);
        w_lfo_slider->bounds(0, 1);
        w_lfo_slider->value(0.5);
        w_lfo_slider->callback(lfo_cb);
    }
    sliders_grp->end();
    y += 140;

    // ---- Theremin (real audio synth on this machine) ----
    auto* therm_grp = new Fl_Group(10, y, W-20, 160,
                                   "Theremin (digital synth on this client)");
    therm_grp->box(FL_ENGRAVED_FRAME);
    therm_grp->align(FL_ALIGN_TOP_LEFT);
    {
        // NO SOUND warning row, only visible if audio is unavailable.
        if (!g_therm->available()) {
            w_audio_warning = new Fl_Box(20, y+12, W-40, 22,
                                         "NO SOUND - audio backend not available");
            w_audio_warning->box(FL_FLAT_BOX);
            w_audio_warning->color(fl_rgb_color(80, 30, 30));      // dark red
            w_audio_warning->labelcolor(fl_rgb_color(255, 205, 210));// light pink
            w_audio_warning->labelfont(FL_HELVETICA_BOLD);
            w_audio_warning->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
        }
        int row_y = g_therm->available() ? y+12 : y+38;

        w_therm_btn = new Fl_Toggle_Button(20, row_y, 130, 28, "Theremin: OFF");
        w_therm_btn->callback(therm_btn_cb);
        auto* test = new Fl_Button(160, row_y, 120, 28, "Test Sound (1s)");
        test->callback(test_sound_cb);
        if (!g_therm->available()) test->deactivate();
        char st_buf[160];
        std::snprintf(st_buf, sizeof(st_buf), "OFF | 440.0 Hz, 0.70 | %s",
                      g_therm->backend().c_str());
        w_therm_status = new Fl_Box(290, row_y, W-310, 28, "");
        w_therm_status->copy_label(st_buf);
        w_therm_status->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);

        const int LX = 20, LW = 110, SX = 140, SW = W - 160;
        new Fl_Box(LX, row_y+38, LW, 24, "Pitch (Hz)");
        w_therm_pitch = new Fl_Value_Slider(SX, row_y+38, SW, 24);
        w_therm_pitch->type(FL_HOR_NICE_SLIDER);
        w_therm_pitch->bounds(80, 2000);
        w_therm_pitch->value(440.0);
        w_therm_pitch->callback(therm_pitch_cb);

        new Fl_Box(LX, row_y+70, LW, 24, "Volume");
        w_therm_volume = new Fl_Value_Slider(SX, row_y+70, SW, 24);
        w_therm_volume->type(FL_HOR_NICE_SLIDER);
        w_therm_volume->bounds(0, 1);
        w_therm_volume->value(0.7);
        w_therm_volume->callback(therm_volume_cb);
    }
    therm_grp->end();
    y += 170;

    // ---- Buttons ----
    auto* btn_grp = new Fl_Group(10, y, W-20, 50, "Buttons");
    btn_grp->box(FL_ENGRAVED_FRAME);
    btn_grp->align(FL_ALIGN_TOP_LEFT);
    {
        int bx = 20;
        auto* p = new Fl_Button(bx, y+10, 70, 28, "Play");          bx += 80;
        p->callback(play_cb);
        auto* s = new Fl_Button(bx, y+10, 70, 28, "Stop");          bx += 80;
        s->callback(stop_cb);
        auto* h = new Fl_Button(bx, y+10, 130, 28, "/hello \"world\"");
        h->callback(hello_cb);
    }
    btn_grp->end();
    y += 60;

    // ---- Note trigger ----
    auto* note_grp = new Fl_Group(10, y, W-20, 50, "Note trigger");
    note_grp->box(FL_ENGRAVED_FRAME);
    note_grp->align(FL_ALIGN_TOP_LEFT);
    {
        new Fl_Box(20, y+12, 50, 24, "Note:");
        w_note_input = new Fl_Int_Input(70, y+12, 60, 24);
        w_note_input->value("60");
        new Fl_Box(140, y+12, 60, 24, "Velocity:");
        w_vel_input = new Fl_Int_Input(200, y+12, 60, 24);
        w_vel_input->value("100");
        auto* trig = new Fl_Button(280, y+12, 90, 24, "Trigger");
        trig->callback(note_trigger_cb);
    }
    note_grp->end();
    y += 60;

    // ---- Behaviour ----
    auto* beh_grp = new Fl_Group(10, y, W-20, 50, "Behaviour");
    beh_grp->box(FL_ENGRAVED_FRAME);
    beh_grp->align(FL_ALIGN_TOP_LEFT);
    {
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

    std::thread rcv(recv_loop);
    Fl::add_timeout(0.05, tick_cb);

    int rc = Fl::run();

    g_running = false;
    {
        std::lock_guard<std::mutex> lk(g_st.mtx);
        if (g_st.sock != OSC_INVALID_SOCKET) {
            osc_close_socket(g_st.sock);
            g_st.sock = OSC_INVALID_SOCKET;
        }
    }
    if (rcv.joinable()) rcv.join();
    therm.stop();
    osc::platform_shutdown();
    return rc;
}
