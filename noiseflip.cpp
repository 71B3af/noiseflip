// noiseflip.cpp  (Noiseflip)
//
// Reads the local skater's physics state from skate. (run through the ORIGINAL ReSkate) and the
// gamepad (XInput), and sends MIDI notes for: jump, land, bail, grind/slide, manual, grab, push,
// footplant and no-foot grab. All tunable values live in noiseflip.ini.
//
// Read only: it opens Skate.exe with PROCESS_VM_READ and never writes to it, never injects, and
// needs no changes to ReSkate. It works for game build 25414733 only (see skate_chain.h).
//
// Build: see README.md (MinGW or MSVC, 64-bit).

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#include <mmsystem.h>
#include <xinput.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <deque>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "cc.h"
#include "config.h"
#include "http_lite.h"
#include "mapper.h"
#include "midi_wire.h"
#include "skate_chain.h"
#include "trigger_engine.h"
#include "ui_server.h"

#ifdef _MSC_VER
#pragma comment(lib, "winmm.lib")
#endif

static_assert(sizeof(void*) == 8, "build this as a 64-bit program");

using namespace skate;

// ---------------------------------------------------------------------------------------------
// small helpers

static std::atomic<bool> g_stop{false};
static BOOL WINAPI on_ctrl(DWORD) { g_stop = true; return TRUE; }

static std::int64_t now_us() {
    static const LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (c.QuadPart / freq.QuadPart) * 1000000 + (c.QuadPart % freq.QuadPart) * 1000000 / freq.QuadPart;
}
static const std::int64_t g_start_us = now_us();

// One lock for everything the UI's thread and the main loop share: the config, the engine, the CC mixer, the live values.
static CRITICAL_SECTION g_lock, g_log_lock;
static struct LockInit { LockInit() { InitializeCriticalSection(&g_lock); InitializeCriticalSection(&g_log_lock); } } g_lock_init;
struct Lock {
    Lock() { EnterCriticalSection(&g_lock); }
    ~Lock() { LeaveCriticalSection(&g_lock); }
};

// The last lines the program said, for the UI's log (the console shows them too).
struct LogLine { unsigned long long seq; double t; std::string text; };
static std::deque<LogLine> g_log;
static unsigned long long g_log_seq = 0;

static void say(const char* fmt, ...) {
    char text[768];
    va_list args;
    va_start(args, fmt);
    vsnprintf(text, sizeof text, fmt, args);
    va_end(args);
    const double t = static_cast<double>(now_us() - g_start_us) / 1e6;
    std::printf("[%8.3f] %s\n", t, text);
    std::fflush(stdout);
    EnterCriticalSection(&g_log_lock);
    g_log.push_back({++g_log_seq, t, text});
    while (g_log.size() > 300) g_log.pop_front();
    LeaveCriticalSection(&g_log_lock);
}

static std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

static bool read_file(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    if (out.size() >= 3 && static_cast<unsigned char>(out[0]) == 0xEF && static_cast<unsigned char>(out[1]) == 0xBB &&
        static_cast<unsigned char>(out[2]) == 0xBF)
        out.erase(0, 3);  // UTF-8 byte order mark added by some editors
    return true;
}
static bool write_file(const std::string& path, const std::string& text) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f << text;
    return static_cast<bool>(f);
}
static ULONGLONG file_stamp(const std::string& path) {
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &d)) return 0;
    return (static_cast<ULONGLONG>(d.ftLastWriteTime.dwHighDateTime) << 32) | d.ftLastWriteTime.dwLowDateTime;
}
static std::string exe_dir() {
    char path[MAX_PATH];
    const DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
    std::string s(path, n);
    const auto slash = s.find_last_of("\\/");
    return slash == std::string::npos ? std::string(".") : s.substr(0, slash);
}

// ---------------------------------------------------------------------------------------------
// command line

struct Cli {
    std::string config_path;
    bool write_config = false, force = false;
    DWORD pid = 0;
    int midi_index = -1;
    bool list = false, dry_run = false, test_midi = false, verbose = false, pad_test = false, watch = false, help = false;
    bool midi_trace = false, no_midi_check = false, allow_multiple = false, test_cc = false, no_cc = false, map_mode = false;
    bool no_ui = false, no_window = false;
    int ui_port = 8431;
    std::string test_cc_target, test_note_target;   // --test-cc20 / --test-note37 (or a name)
    std::optional<bool> note_off_zero;
    std::string log_path;
    std::optional<std::string> midi, process;
    std::optional<int> channel, velocity, hold, poll, pad;
    std::optional<bool> detect_angle;   // --detect rate|angle
    bool has_notes = false;
    int notes[3] = {36, 38, 40};
};

static void usage() {
    std::puts(
        "noiseflip: MIDI notes from skate.'s physics state and your gamepad\n"
        "\n"
        "Everything tunable is in noiseflip.ini (created next to the exe on first run, reloaded while it runs).\n"
        "Options below override the file.\n"
        "\n"
        "  --config FILE       use this ini file (default: noiseflip.ini next to the exe)\n"
        "  --write-config      write a fresh default ini and exit (add --force to overwrite)\n"
        "  --list              list MIDI output ports and exit\n"
        "  --midi NAME         MIDI port whose name contains NAME\n"
        "  --midi-index N      use MIDI port number N (see --list)\n"
        "  --channel N         MIDI channel 1-16\n"
        "  --notes J,L,B       notes for jump, land, bail\n"
        "  --velocity V        1-127\n"
        "  --hold MS           length of the pulse triggers\n"
        "  --poll-ms N         polling interval, 0 = spin (uses a full CPU core)\n"
        "  --pad N             use only XInput slot N (0-3); default merges every connected pad\n"
        "  --test-cc           sweep every MIDI controller from 0 to 127 and back, one after another, and exit\n"
        "  --test-cc20         send ONLY controller 20, sweeping slowly, until you press Ctrl+C (also: --test-cc 20, --test-cc intensity)\n"
        "  --test-note37       send ONLY note 37, again and again, until you press Ctrl+C (also: --test-note 37, --test-note land)\n"
        "  --map               interactive mapper: type  n land  or  c intensity  to choose what is sent, Enter stops, q quits\n"
        "  --no-cc             do not send any MIDI controllers (CC), only notes\n"
        "  --no-ui             do not start the UI (console only, as before v5)\n"
        "  --no-window         start the UI's server but do not open its window (open the address it prints yourself)\n"
        "  --ui-port N         the UI's port on 127.0.0.1 (default 8431; the next ten are tried when it is busy)\n"
        "  --midi-trace        print the exact MIDI bytes sent (to see what really goes to the port)\n"
        "  --note-off STYLE    noteoff (a real note-off message) or velocity0 (note-on with velocity 0)\n"
        "  --no-midi-check     do not listen to the MIDI port for notes that Noiseflip did not send\n"
        "  --allow-multiple    allow a second copy of Noiseflip to run (normally refused: two copies double the notes)\n"
        "  --detect MODE       rotation triggers: rate (one note per spin) or angle (a note every 180 degrees)\n"
        "  --process NAME      game process name        --pid N   attach to a process id\n"
        "  --pad-test          show live gamepad values (to find your manual stick range) and exit with Ctrl+C\n"
        "  --watch             show live speed, height and board spin while you skate (to tune the rotation thresholds)\n"
        "  --test-midi         send every trigger's note once and exit (no game needed)\n"
        "  --dry-run           do not open a MIDI port, only print\n"
        "  --log FILE          write a CSV log of state changes and triggers (to find out what happened)\n"
        "  -v, --verbose       print every physics state change\n"
        "  -h, --help          this text\n");
}

static bool parse_args(int argc, char** argv, Cli& o) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](const char* what) -> const char* {
            if (i + 1 >= argc) { std::fprintf(stderr, "%s needs a value (%s)\n", a.c_str(), what); return nullptr; }
            return argv[++i];
        };
        auto int_in = [&](const char* what, int lo, int hi, std::optional<int>& dst) {
            auto v = next(what);
            if (!v) return false;
            const int x = std::atoi(v);
            if (x < lo || x > hi) { std::fprintf(stderr, "%s must be %d-%d\n", a.c_str(), lo, hi); return false; }
            dst = x;
            return true;
        };
        if (a == "-h" || a == "--help") o.help = true;
        else if (a == "--list") o.list = true;
        else if (a == "--dry-run") o.dry_run = true;
        else if (a == "--test-midi") o.test_midi = true;
        else if (a == "--pad-test") o.pad_test = true;
        else if (a == "--watch") o.watch = true;
        else if (a == "--midi-trace") o.midi_trace = true;
        else if (a == "--map") o.map_mode = true;
        else if (glued_value(a, "--test-cc", o.test_cc_target)) {}                 // --test-cc20, --test-cc=intensity
        else if (glued_value(a, "--test-note", o.test_note_target)) {}             // --test-note37, --test-note=land
        else if (a == "--test-cc") {
            if (i + 1 < argc && argv[i + 1][0] != '-') o.test_cc_target = argv[++i];   // --test-cc 20
            else o.test_cc = true;                                                      // no value: all of them, one after another
        }
        else if (a == "--test-note") {
            auto v = next("a note number 0-127 or a trigger name");
            if (!v) return false;
            o.test_note_target = v;
        }
        else if (a == "--no-cc") o.no_cc = true;
        else if (a == "--no-ui") o.no_ui = true;
        else if (a == "--no-window") o.no_window = true;
        else if (a == "--ui-port") { std::optional<int> v; if (!int_in("1024-65000", 1024, 65000, v)) return false; o.ui_port = *v; }
        else if (a == "--no-midi-check") o.no_midi_check = true;
        else if (a == "--allow-multiple") o.allow_multiple = true;
        else if (a == "--note-off") {
            auto v = next("noteoff|velocity0");
            if (!v) return false;
            const std::string m = lower(v);
            if (m != "noteoff" && m != "velocity0") { std::fprintf(stderr, "--note-off must be noteoff or velocity0\n"); return false; }
            o.note_off_zero = (m == "velocity0");
        }
        else if (a == "--write-config") o.write_config = true;
        else if (a == "--force") o.force = true;
        else if (a == "-v" || a == "--verbose") o.verbose = true;
        else if (a == "--detect") {
            auto v = next("rate|angle");
            if (!v) return false;
            const std::string m = lower(v);
            if (m != "rate" && m != "angle") { std::fprintf(stderr, "--detect must be rate or angle\n"); return false; }
            o.detect_angle = (m == "angle");
        }
        else if (a == "--config") { auto v = next("file"); if (!v) return false; o.config_path = v; }
        else if (a == "--log") { auto v = next("file"); if (!v) return false; o.log_path = v; }
        else if (a == "--midi") { auto v = next("name"); if (!v) return false; o.midi = v; }
        else if (a == "--midi-index") { auto v = next("number"); if (!v) return false; o.midi_index = std::atoi(v); }
        else if (a == "--process") { auto v = next("name"); if (!v) return false; o.process = v; }
        else if (a == "--pid") { auto v = next("id"); if (!v) return false; o.pid = static_cast<DWORD>(std::strtoul(v, nullptr, 10)); }
        else if (a == "--channel") { if (!int_in("1-16", 1, 16, o.channel)) return false; }
        else if (a == "--velocity") { if (!int_in("1-127", 1, 127, o.velocity)) return false; }
        else if (a == "--hold") { if (!int_in("ms", 1, 60000, o.hold)) return false; }
        else if (a == "--poll-ms") { if (!int_in("ms", 0, 100, o.poll)) return false; }
        else if (a == "--pad") { if (!int_in("0-3", 0, 3, o.pad)) return false; }
        else if (a == "--notes") {
            auto v = next("J,L,B");
            if (!v) return false;
            int x, y, z;
            if (std::sscanf(v, "%d,%d,%d", &x, &y, &z) != 3 || x < 0 || x > 127 || y < 0 || y > 127 || z < 0 || z > 127) {
                std::fprintf(stderr, "notes must be three numbers 0-127, e.g. 36,38,40\n");
                return false;
            }
            o.has_notes = true; o.notes[0] = x; o.notes[1] = y; o.notes[2] = z;
        } else {
            std::fprintf(stderr, "unknown option: %s (try --help)\n", a.c_str());
            return false;
        }
    }
    return true;
}

static void apply_cli(const Cli& o, Config& c) {
    if (o.midi) c.midi_port = *o.midi;
    if (o.process) c.process = *o.process;
    if (o.channel) c.channel = *o.channel;
    if (o.velocity) c.velocity = *o.velocity;
    if (o.hold) c.hold_ms = *o.hold;
    if (o.poll) c.poll_ms = *o.poll;
    if (o.pad) c.pad = *o.pad;
    if (o.detect_angle) c.rotation.angle = *o.detect_angle;
    if (o.note_off_zero) c.note_off_zero = *o.note_off_zero;
    if (o.no_midi_check) c.midi_check = false;
    if (o.no_cc) for (int i = 0; i < CcCount; ++i) c.cc[i].enabled = false;
    if (o.has_notes) { c.t[Jump].note = o.notes[0]; c.t[Land].note = o.notes[1]; c.t[Bail].note = o.notes[2]; }
}

// ---------------------------------------------------------------------------------------------
// MIDI output (WinMM, which loopMIDI ports appear in)

static std::string midi_port_name(UINT id) {
    MIDIOUTCAPSA caps;
    if (midiOutGetDevCapsA(id, &caps, sizeof caps) != MMSYSERR_NOERROR) return "?";
    return caps.szPname;
}
static void list_midi_ports() {
    const UINT n = midiOutGetNumDevs();
    if (!n) { std::puts("No MIDI output ports found. Is loopMIDI running?"); return; }
    std::puts("MIDI output ports:");
    for (UINT i = 0; i < n; ++i) std::printf("  %u: %s\n", i, midi_port_name(i).c_str());
}
static int find_midi_port(const std::string& needle) {
    const UINT n = midiOutGetNumDevs();
    const std::string want = lower(needle);
    for (UINT i = 0; i < n; ++i)
        if (lower(midi_port_name(i)).find(want) != std::string::npos) return static_cast<int>(i);
    return -1;
}

struct MidiOut {
    HMIDIOUT handle = nullptr;
    bool open(UINT id) { return midiOutOpen(&handle, id, 0, 0, CALLBACK_NULL) == MMSYSERR_NOERROR; }
    void send_raw(const MidiMsg& m) {
        if (handle) midiOutShortMsg(handle, static_cast<DWORD>(pack(m)));
    }
    void close() {
        if (!handle) return;
        midiOutReset(handle);  // all notes off on every channel
        midiOutClose(handle);
        handle = nullptr;
    }
};

// Listens to the same loopMIDI port from the input side. loopMIDI hands back whatever any program sends to the
// port, so this sees Noiseflip's own notes AND anything else that is sending to it.
struct MidiMonitor {
    HMIDIIN handle = nullptr;
    CRITICAL_SECTION lock;
    std::vector<std::pair<std::uint32_t, std::int64_t>> queue;
    bool lock_ready = false;

    static void CALLBACK proc(HMIDIIN, UINT msg, DWORD_PTR instance, DWORD_PTR p1, DWORD_PTR) {
        if (msg != MIM_DATA) return;
        auto* self = reinterpret_cast<MidiMonitor*>(instance);
        EnterCriticalSection(&self->lock);
        if (self->queue.size() < 4096) self->queue.emplace_back(static_cast<std::uint32_t>(p1), now_us());
        LeaveCriticalSection(&self->lock);
    }
    bool open(const std::string& port_name) {
        if (!lock_ready) { InitializeCriticalSection(&lock); lock_ready = true; }
        const std::string want = lower(port_name);
        const UINT n = midiInGetNumDevs();
        for (UINT i = 0; i < n; ++i) {
            MIDIINCAPSA caps;
            if (midiInGetDevCapsA(i, &caps, sizeof caps) != MMSYSERR_NOERROR) continue;
            if (lower(caps.szPname).find(want) == std::string::npos) continue;
            if (midiInOpen(&handle, i, reinterpret_cast<DWORD_PTR>(&MidiMonitor::proc), reinterpret_cast<DWORD_PTR>(this),
                           CALLBACK_FUNCTION) != MMSYSERR_NOERROR) { handle = nullptr; return false; }
            midiInStart(handle);
            return true;
        }
        return false;
    }
    std::vector<std::pair<std::uint32_t, std::int64_t>> drain() {
        std::vector<std::pair<std::uint32_t, std::int64_t>> out;
        if (!lock_ready) return out;
        EnterCriticalSection(&lock);
        out.swap(queue);
        LeaveCriticalSection(&lock);
        return out;
    }
    void close() {
        if (handle) { midiInStop(handle); midiInReset(handle); midiInClose(handle); handle = nullptr; }
        if (lock_ready) { DeleteCriticalSection(&lock); lock_ready = false; }
    }
};

// ---------------------------------------------------------------------------------------------
// gamepad (XInput, loaded dynamically like ReSkate does)

class PadReader {
public:
    bool load() {
        for (const auto name : {L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll"}) {
            HMODULE m = LoadLibraryW(name);
            if (!m) continue;
            get_ = reinterpret_cast<GetState>(reinterpret_cast<void*>(GetProcAddress(m, "XInputGetState")));
            if (get_) return true;
        }
        return false;
    }
    bool available() const { return get_ != nullptr; }
    void set_slot(int slot) { slot_ = slot; connected_ = 0; }  // -1 = merge every connected pad

    void start_prober() {
        if (!get_ || thread_) return;
        thread_ = CreateThread(nullptr, 0, &PadReader::probe_thread, this, 0, nullptr);
    }
    void stop_prober() {
        stop_probe_ = true;
        if (thread_) { WaitForSingleObject(thread_, 2000); CloseHandle(thread_); thread_ = nullptr; }
    }

    // Reads the pad(s). When several pads are connected (virtual pads such as DS4Windows or Steam can
    // take any slot) they are merged: buttons ORed, triggers and sticks take the strongest.
    Pad read() {
        Pad out;
        if (!get_) return out;
        const unsigned mask = connected_.load();
        for (DWORD slot = 0; slot < XUSER_MAX_COUNT; ++slot) {
            const unsigned bit = 1u << slot;
            if (!(mask & bit)) continue;
            XINPUT_STATE s{};
            if (get_(slot, &s) != ERROR_SUCCESS) { connected_.fetch_and(~bit); continue; }
            const XINPUT_GAMEPAD& g = s.Gamepad;
            out.connected = true;
            out.buttons |= g.wButtons;
            out.lt = std::max<int>(out.lt, g.bLeftTrigger);
            out.rt = std::max<int>(out.rt, g.bRightTrigger);
            auto strength = [](int x, int y) { return std::abs(x) + std::abs(y); };
            if (strength(g.sThumbLX, g.sThumbLY) > strength(out.lx, out.ly)) { out.lx = g.sThumbLX; out.ly = g.sThumbLY; }
            if (strength(g.sThumbRX, g.sThumbRY) > strength(out.rx, out.ry)) { out.rx = g.sThumbRX; out.ry = g.sThumbRY; }
        }
        return out;
    }
    unsigned connected_mask() const { return connected_.load(); }

private:
    using GetState = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
    // Asking an empty slot is slow (Windows looks for a device), so empty slots are tried here, once a second.
    static DWORD WINAPI probe_thread(void* self) {
        auto* p = static_cast<PadReader*>(self);
        while (!p->stop_probe_) {
            for (DWORD slot = 0; slot < XUSER_MAX_COUNT && !p->stop_probe_; ++slot) {
                if (p->slot_ >= 0 && static_cast<int>(slot) != p->slot_) continue;
                const unsigned bit = 1u << slot;
                if (p->connected_.load() & bit) continue;
                XINPUT_STATE s{};
                if (p->get_(slot, &s) == ERROR_SUCCESS) p->connected_.fetch_or(bit);
            }
            for (int i = 0; i < 10 && !p->stop_probe_; ++i) Sleep(100);
        }
        return 0;
    }
    GetState get_ = nullptr;
    std::atomic<unsigned> connected_{0};
    std::atomic<int> slot_{-1};
    std::atomic<bool> stop_probe_{false};
    HANDLE thread_ = nullptr;
};

static std::string buttons_text(unsigned b) {
    std::string s;
    for (const auto& n : button_names()) if (b & n.mask) { if (!s.empty()) s += ' '; s += n.name; }
    return s.empty() ? "-" : s;
}

// ---------------------------------------------------------------------------------------------
// the whole app state

// What the UI shows live. Written by the main loop, read when the snapshot is built (always under g_lock).
struct Live {
    bool attached = false;
    std::string status = "Waiting for the game";
    Input in;                       // the last sample: state, pad, motion
    bool have_state = false;
};

struct TestState {                  // the repeating note or controller of the Mapping tab
    int kind = 0, id = -1;          // kind: 0 nothing, 1 a trigger's note, 2 a controller
    MapTarget target;
    bool note_on = false;
    int last_cc = -1;
    std::int64_t start = 0, last_send = 0, last_refresh = 0;
};

struct App {
    Cli cli;
    std::string config_path;
    ULONGLONG config_stamp = 0;
    std::int64_t next_config_check = 0;
    Config cfg;
    TriggerEngine engine;
    PadReader pad;
    MidiOut midi;
    MidiMonitor monitor;
    MidiAudit audit;
    CcMixer cc;
    bool monitoring = false;
    unsigned seen_switches = 0;
    unsigned foreign_shown = 0;
    FILE* log = nullptr;

    // v5: the UI
    Live live;
    TestState test;
    int test_kind_req = 0, test_id_req = -1;      // asked for by the UI, served by the main loop
    int trig_note[TriggerCount] = {}, trig_vel[TriggerCount] = {};
    int cc_sent[CcCount];
    std::string midi_name;
    std::string ini_text, defaults_text;          // the config as saved, and the built-in defaults
    std::string file_midi_port, file_process;     // what the file says (a change needs a restart; cfg keeps the running ones)
    std::string snapshot = "{}";
    std::int64_t next_snapshot = 0;
    std::string saved_note;                       // "saved 12:03:11"
    std::string preset_name, preset_ini;          // the preset last loaded or saved, and the config it stands for
    App() { for (int& v : cc_sent) v = -1; }
};

static void log_line(App& app, const char* fmt, ...) {
    if (!app.log) return;
    char text[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(text, sizeof text, fmt, args);
    va_end(args);
    std::fprintf(app.log, "%.3f,%s\n", static_cast<double>(now_us() - g_start_us) / 1e6, text);
    std::fflush(app.log);
}

static void print_summary(const Config& c, bool angle_mode) {
    std::string s;
    for (int t = 0; t < TriggerCount; ++t) {
        if (!c.t[t].enabled) continue;
        s += std::string(section_name(t)) + "=" + std::to_string(c.t[t].note) + " ";
    }
    say("MIDI channel %d, velocity %d, pulse length %d ms.  Notes: %s", c.channel, c.velocity, c.hold_ms, s.c_str());
    const std::string button = c.rotation.switch_button ? detail::button_str(c.rotation.switch_button) : "";
    if (angle_mode) say("Rotation mode: ANGLE (a note every %.0f degrees, per axis; a revert on the ground plays the shuvit note)", c.rotation.step_deg);
    else say("Rotation mode: RATE (one note when the spin passes its threshold)");
    if (c.rotation.switch_button) say("Press %s on the gamepad to switch between angle and rate mode.", button.c_str());
    std::string cc_list;
    for (int i = 0; i < CcCount; ++i)
        if (c.cc[i].enabled) cc_list += std::string(cc_name(i)) + "=CC" + std::to_string(c.cc[i].cc) + " ";
    if (cc_list.empty()) say("MIDI controllers: off");
    else say("MIDI controllers (channel %d): %s  (--test-cc sweeps them so you can map them)", c.channel, cc_list.c_str());
}

static void report(const ParseResult& r) {
    for (const auto& e : r.errors) say("CONFIG ERROR  %s", e.c_str());
    for (const auto& w : r.warnings) say("config note   %s", w.c_str());
}

// Reads the ini. Returns false (and leaves `out` alone) on errors.
static bool load_config(const App& app, Config& out, ParseResult& res) {
    std::string text;
    if (!read_file(app.config_path, text)) { res.errors.push_back("cannot read " + app.config_path); return false; }
    Config c;
    parse_ini(text, c, res);
    if (!res.ok()) return false;
    apply_cli(app.cli, c);
    out = c;
    return true;
}

static std::string clock_text() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    char b[16];
    std::snprintf(b, sizeof b, "%02d:%02d:%02d", t.wHour, t.wMinute, t.wSecond);
    return b;
}

// Takes a new config. The caller holds g_lock. The MIDI port and the process name only change at the next start,
// so the running config keeps the old ones (the file gets the new ones).
static void install_config(App& app, Config c, bool say_it) {
    app.file_midi_port = c.midi_port;
    app.file_process = c.process;
    if (c.midi_port != app.cfg.midi_port || c.process != app.cfg.process)
        if (say_it) say("midi_port / process changed: restart the program for that to take effect.");
    if (c.pad != app.cfg.pad) app.pad.set_slot(c.pad);
    app.ini_text = write_ini(c);
    c.midi_port = app.cfg.midi_port;
    c.process = app.cfg.process;
    app.cfg = c;
    app.engine.set_config(c);
}

static void check_reload(App& app) {
    const std::int64_t now = now_us();
    if (now < app.next_config_check) return;
    app.next_config_check = now + 500000;
    const ULONGLONG stamp = file_stamp(app.config_path);
    if (!stamp || stamp == app.config_stamp) return;
    app.config_stamp = stamp;
    Config c;
    ParseResult r;
    if (!load_config(app, c, r)) {
        report(r);
        say("Config NOT reloaded: fix the error above; the previous values stay active.");
        return;
    }
    report(r);
    {
        Lock l;
        install_config(app, c, true);
        app.saved_note = "reloaded from file " + clock_text();
    }
    say("Config reloaded.");
    print_summary(app.cfg, app.engine.angle_mode());
}

// The UI changed some settings: check them on top of the running config, save the file, and use them at once.
static bool apply_settings(App& app, const std::vector<http::Setting>& v, std::string& error) {
    Lock l;
    Config c = app.cfg;
    c.midi_port = app.file_midi_port;
    c.process = app.file_process;
    ParseResult r;
    parse_ini(http::settings_to_ini(v), c, r);
    if (!r.ok()) {
        for (const auto& e : r.errors) {
            const auto colon = e.find(": ");
            error += (error.empty() ? "" : "; ") + (colon == std::string::npos ? e : e.substr(colon + 2));
        }
        return false;
    }
    install_config(app, c, true);
    if (!write_file(app.config_path, app.ini_text)) { error = "could not write " + app.config_path; return false; }
    app.config_stamp = file_stamp(app.config_path);       // our own save is not a change to reload
    app.saved_note = "saved " + clock_text();
    return true;
}

// Everything that goes to the MIDI port goes through here.
static void send_midi(App& app, const Action& a) {
    const MidiMsg m = encode(a, app.cfg.note_off_zero ? NoteOffStyle::Velocity0 : NoteOffStyle::NoteOff);
    app.audit.sent(m, now_us());          // remembered BEFORE sending so the echo can never arrive first
    if (app.cli.midi_trace) say("MIDI out  %s", describe(m).c_str());
    log_line(app, "MIDI,%02X,%02X,%02X", m.status, m.d1, m.d2);
    app.midi.send_raw(m);
}

// Controller messages (CC). They are not notes, so the foreign-note audit does not look at them.
static void send_cc(App& app, const CcMessage& m) {
    const MidiMsg msg = encode_cc(m.channel, m.cc, m.value);
    if (m.index >= 0 && m.index < CcCount) app.cc_sent[m.index] = m.value;
    if (app.cli.midi_trace) {
        if (m.index >= 0) say("MIDI out  %s   [%s]", describe(msg).c_str(), cc_name(m.index));
        else say("MIDI out  %s", describe(msg).c_str());
    }
    app.midi.send_raw(msg);
}

// Looks at what comes back from the port. Notes that Noiseflip did not send are reported.
static void service_midi(App& app) {
    if (!app.monitoring) return;
    for (const auto& item : app.monitor.drain()) {
        const MidiMsg m = unpack(item.first);
        if (app.audit.received(m, item.second)) continue;
        if (app.foreign_shown < 40) {
            ++app.foreign_shown;
            std::string hint;
            for (int t = 0; t < TriggerCount; ++t)
                if (m.d1 == app.cfg.t[t].note) { hint = std::string(", the note of ") + trigger_name(t); break; }
            say("FOREIGN MIDI on the port: %s%s. Noiseflip did not send this: another program (or a second Noiseflip) is sending to the same port.",
                describe(m).c_str(), hint.c_str());
            if (app.foreign_shown == 40) say("(further foreign MIDI messages are counted but not shown)");
        }
        log_line(app, "FOREIGN,%02X,%02X,%02X", m.status, m.d1, m.d2);
    }
}

static void emit(App& app, const std::vector<Action>& actions, const Pad& pad) {
    for (const auto& a : actions) {
        send_midi(app, a);
        if (a.on && a.trigger >= 0 && a.trigger < TriggerCount) { app.trig_note[a.trigger] = a.note; app.trig_vel[a.trigger] = a.velocity; }
        if (a.on) {
            char extra[80] = "";
            if (a.trigger == Land && a.measure >= 0) std::snprintf(extra, sizeof extra, "  drop %.2f m", a.measure);
            else if (is_rotation(a.trigger) && a.angle >= 0)
                std::snprintf(extra, sizeof extra, "  %.0f deg about %c", a.angle, "XYZ"[a.axis >= 0 && a.axis < 3 ? a.axis : 0]);
            else if (is_rotation(a.trigger) && a.measure >= 0) std::snprintf(extra, sizeof extra, "  spin %.0f deg/s", a.measure);
            if (a.from_state != a.to_state)
                say("%-10s ON   note %-3d vel %-3d  (state %u -> %u)%s", trigger_name(a.trigger), a.note, a.velocity, a.from_state, a.to_state, extra);
            else
                say("%-10s ON   note %-3d vel %-3d%s", trigger_name(a.trigger), a.note, a.velocity, extra);
            log_line(app, "ON,%s,%d,%u,%u,vel=%d,measure=%.3f,lt=%d,rt=%d,ry=%d,buttons=%s", trigger_name(a.trigger), a.note, a.from_state,
                     a.to_state, a.velocity, a.measure, pad.lt, pad.rt, pad.ry, buttons_text(pad.buttons).c_str());
        } else {
            say("%-10s off  note %d", trigger_name(a.trigger), a.note);
            log_line(app, "OFF,%s,%d", trigger_name(a.trigger), a.note);
        }
    }
}

// ---------------------------------------------------------------------------------------------
// the game process

struct WinMemory : Memory {
    HANDLE process = nullptr;
    bool read(std::uintptr_t address, void* out, std::size_t size) override {
        SIZE_T got = 0;
        return ReadProcessMemory(process, reinterpret_cast<LPCVOID>(address), out, size, &got) && got == size;
    }
};

static DWORD find_process(const std::string& name) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32 pe;
    pe.dwSize = sizeof pe;
    DWORD pid = 0;
    const std::string want = lower(name);
    for (BOOL ok = Process32First(snap, &pe); ok; ok = Process32Next(snap, &pe)) {
        if (lower(pe.szExeFile) == want) { pid = pe.th32ProcessID; break; }
    }
    CloseHandle(snap);
    return pid;
}

static bool find_module_base(DWORD pid, const std::string& name, std::uintptr_t& base) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return false;
    MODULEENTRY32 me;
    me.dwSize = sizeof me;
    bool found = false;
    const std::string want = lower(name);
    for (BOOL ok = Module32First(snap, &me); ok; ok = Module32Next(snap, &me)) {
        if (lower(me.szModule) == want) { base = reinterpret_cast<std::uintptr_t>(me.modBaseAddr); found = true; break; }
    }
    CloseHandle(snap);
    return found;
}

static bool is_writable(DWORD protect) {
    if (protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    return protect & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY);
}

// Looks through the game's writable memory for the client object: an 8-byte-aligned value equal
// to the client vtable. The game's own image is searched first (cheap), then everything else.
// A candidate is accepted only if it really is the client (see skate::resolve). Takes a few
// seconds, so it is done rarely.
static std::uintptr_t scan_for_client(WinMemory& mem, std::uintptr_t base) {
    const std::uintptr_t target = base + rva::client_vtable;
    std::vector<std::uint8_t> buffer(4u << 20);
    std::uintptr_t fallback = 0;  // a client object that is not in a level right now

    for (int pass = 0; pass < 2; ++pass) {
        std::uintptr_t address = 0x10000;
        while (address < highest_user_address && !g_stop) {
            MEMORY_BASIC_INFORMATION mbi;
            if (!VirtualQueryEx(mem.process, reinterpret_cast<LPCVOID>(address), &mbi, sizeof mbi)) break;
            const auto region = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
            const auto end = region + mbi.RegionSize;
            const bool image = mbi.Type == MEM_IMAGE;
            if (mbi.State == MEM_COMMIT && is_writable(mbi.Protect) && ((pass == 0) == image)) {
                for (std::uintptr_t p = region; p < end && !g_stop; p += buffer.size()) {
                    const std::size_t n = static_cast<std::size_t>(std::min<std::uintptr_t>(buffer.size(), end - p));
                    if (!mem.read(p, buffer.data(), n)) continue;
                    for (std::size_t i = 0; i + 8 <= n; i += 8) {
                        std::uintptr_t v;
                        std::memcpy(&v, buffer.data() + i, 8);
                        if (v != target) continue;
                        const auto candidate = p + i;
                        const auto r = resolve(mem, base, candidate);
                        if (r.result == Result::Ok) return candidate;
                        if (r.result != Result::NoClient && !fallback) fallback = candidate;
                    }
                }
            }
            address = end > address ? end : address + 0x1000;
        }
    }
    return fallback;
}

// ---------------------------------------------------------------------------------------------
// the UI: live snapshot, the repeating test signal of the Mapping tab, and the API the page talks to

static std::string jnum(double v) {
    char b[48];
    if (!std::isfinite(v)) v = 0;
    std::snprintf(b, sizeof b, "%.3f", v);
    return b;
}
static const char* jbool(bool v) { return v ? "true" : "false"; }

// Called with g_lock held.
static std::string build_snapshot(App& app) {
    const Live& L = app.live;
    const Input& in = L.in;
    std::string o = "{";
    o += "\"attached\":" + std::string(jbool(L.attached)) + ",\"status\":" + http::json_escape(L.status);
    o += ",\"hasState\":" + std::string(jbool(L.attached && in.game_ok)) + ",\"state\":" + std::to_string(in.state);
    o += ",\"motion\":" + std::string(jbool(in.motion.ok)) + ",\"speed\":" + jnum(in.motion.speed) + ",\"y\":" + jnum(in.motion.y);
    o += ",\"spin\":[" + jnum(in.motion.w_dps[0]) + "," + jnum(in.motion.w_dps[1]) + "," + jnum(in.motion.w_dps[2]) + "]";
    const Pad& p = in.pad;
    o += ",\"pad\":{\"c\":" + std::string(jbool(p.connected)) + ",\"lt\":" + std::to_string(p.lt) + ",\"rt\":" + std::to_string(p.rt) +
         ",\"lx\":" + std::to_string(p.lx) + ",\"ly\":" + std::to_string(p.ly) + ",\"rx\":" + std::to_string(p.rx) +
         ",\"ry\":" + std::to_string(p.ry) + ",\"b\":" + std::to_string(p.buttons) + "}";
    o += ",\"mode\":" + http::json_escape(app.engine.angle_mode() ? "angle" : "rate");
    o += ",\"switches\":" + std::to_string(app.engine.mode_switches());
    o += ",\"midi\":{\"open\":" + std::string(jbool(app.midi.handle != nullptr)) + ",\"port\":" + http::json_escape(app.midi_name) +
         ",\"sent\":" + std::to_string(app.audit.sent_count()) + ",\"foreign\":" + std::to_string(app.audit.foreign()) + "}";
    o += ",\"test\":{\"kind\":" + std::to_string(app.test.kind) + ",\"id\":" + std::to_string(app.test.id) + "}";
    o += ",\"saved\":" + http::json_escape(app.saved_note);
    o += ",\"preset\":{\"name\":" + http::json_escape(app.preset_name) + ",\"dirty\":" +
         jbool(!app.preset_name.empty() && app.ini_text != app.preset_ini) + "}";
    o += ",\"energy\":" + jnum(app.engine.energy());
    o += ",\"trig\":[";
    for (int t = 0; t < TriggerCount; ++t) {
        if (t) o += ",";
        o += "{\"on\":" + std::string(jbool(app.engine.active(t))) + ",\"note\":" + std::to_string(app.trig_note[t]) +
             ",\"vel\":" + std::to_string(app.trig_vel[t]) + ",\"n\":" + std::to_string(app.engine.fired(t)) + "}";
    }
    o += "],\"cc\":[";
    for (int i = 0; i < CcCount; ++i) {
        if (i) o += ",";
        o += "{\"v\":" + jnum(app.engine.cc_targets()[i]) + ",\"s\":" + std::to_string(app.cc_sent[i]) + "}";
    }
    o += "]}";
    return o;
}

// The Mapping tab's TEST buttons: one note or one controller on repeat until the same button is pressed again
// (or another one is). Runs in the main loop so it uses the same MIDI port as everything else.
static void service_test(App& app) {
    int kind, id;
    static Config cfg;
    bool refresh = false;
    {
        Lock l;
        kind = app.test_kind_req;
        id = app.test_id_req;
        const std::int64_t now = now_us();
        if (kind != 0 && now - app.test.last_refresh > 100000) { cfg = app.cfg; refresh = true; app.test.last_refresh = now; }
        if (kind != app.test.kind || id != app.test.id) { cfg = app.cfg; refresh = true; }
    }
    TestState& t = app.test;
    auto note_action = [&](bool on) {
        Action a;
        a.on = on; a.trigger = 0; a.note = t.target.number; a.velocity = on ? t.target.velocity : 0; a.channel = t.target.channel;
        return a;
    };
    if (kind != t.kind || id != t.id) {
        if (t.kind == 1 && t.note_on) { send_midi(app, note_action(false)); t.note_on = false; }
        Lock l;
        t.kind = kind; t.id = id; t.start = now_us(); t.last_cc = -1; t.last_send = 0;
        t.target = MapTarget();
        if (kind) {
            std::string err;
            if (kind == 1 && id >= 0 && id < TriggerCount) resolve_note(cfg, section_name(id), t.target, err);
            else if (kind == 2 && id >= 0 && id < CcCount) resolve_cc(cfg, cc_section(id), t.target, err);
            else t.kind = 0;
        }
        if (t.kind) say("TEST sending %s on channel %d (press the same TEST again to stop).", t.target.label.c_str(), t.target.channel + 1);
        else say("TEST stopped.");
        return;
    }
    if (!t.kind) return;
    if (refresh) {                                   // a number or channel edited while testing: follow it
        MapTarget n;
        std::string err;
        if (t.kind == 1) resolve_note(cfg, section_name(t.id), n, err); else resolve_cc(cfg, cc_section(t.id), n, err);
        if (n.number != t.target.number || n.channel != t.target.channel) {
            if (t.kind == 1 && t.note_on) { send_midi(app, note_action(false)); t.note_on = false; }
            t.target = n;
        } else {
            t.target.velocity = n.velocity;
        }
    }
    const std::int64_t now = now_us(), el = now - t.start;
    if (t.kind == 1) {
        const bool on = note_pulse_on(el);
        if (on && !t.note_on) { send_midi(app, note_action(true)); t.note_on = true; }
        else if (!on && t.note_on) { send_midi(app, note_action(false)); t.note_on = false; }
    } else if (now - t.last_send >= 25000) {
        t.last_send = now;
        const int v = cc_sweep_value(el);
        if (v != t.last_cc) {
            CcMessage m;
            m.index = -1; m.channel = t.target.channel; m.cc = t.target.number; m.value = v;
            send_cc(app, m);
            t.last_cc = v;
        }
    }
}

// Every loop of the program, with or without the game.
static void service_ui(App& app) {
    service_test(app);
    const std::int64_t now = now_us();
    if (now >= app.next_snapshot) {
        app.next_snapshot = now + 40000;
        Lock l;
        app.snapshot = build_snapshot(app);
    }
}

// ---- the UI window: size and position are remembered in noiseflip.window ("x y w h", sent by the page)
struct WinRect { int x = 60, y = 30, w = 1120, h = 800; };
static std::string window_file() { return exe_dir() + "\\noiseflip.window"; }
static bool window_valid(int x, int y, int w, int h) {
    return w >= 400 && w <= 16000 && h >= 300 && h <= 16000 && x > -16000 && x < 32000 && y > -16000 && y < 32000;
}
static WinRect load_window() {
    WinRect r;
    std::string t;
    if (!read_file(window_file(), t)) return r;
    WinRect s;
    if (std::sscanf(t.c_str(), "%d %d %d %d", &s.x, &s.y, &s.w, &s.h) != 4 || !window_valid(s.x, s.y, s.w, s.h)) return r;
    POINT pt{s.x + 60, s.y + 20};                                      // the title bar must still be on a monitor
    if (!MonitorFromPoint(pt, MONITOR_DEFAULTTONULL)) return r;
    return s;
}
// the program's icon (resource 1) for the console window as well
static void set_console_icon() {
    HWND w = GetConsoleWindow();
    HMODULE m = GetModuleHandleA(nullptr);
    if (!w) return;
    HANDLE big = LoadImageA(m, MAKEINTRESOURCEA(1), IMAGE_ICON, 32, 32, LR_SHARED);
    HANDLE small = LoadImageA(m, MAKEINTRESOURCEA(1), IMAGE_ICON, 16, 16, LR_SHARED);
    if (big) SendMessageA(w, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(big));
    if (small) SendMessageA(w, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(small));
}

static void open_ui_window(const std::string& url) {
    // an app window (no tabs, no address bar) from Edge or Chrome; the default browser when neither is found
    const WinRect r = load_window();
    const std::string args = "--app=" + url + " --window-size=" + std::to_string(r.w) + "," + std::to_string(r.h) +
                             " --window-position=" + std::to_string(r.x) + "," + std::to_string(r.y);
    for (const char* browser : {"msedge", "chrome"}) {
        const HINSTANCE h = ShellExecuteA(nullptr, "open", browser, args.c_str(), nullptr, SW_SHOWNORMAL);
        if (reinterpret_cast<INT_PTR>(h) > 32) return;
    }
    ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// ---------------------------------------------------------------------------------------------
// presets: the whole config under a name, as presets/<name>.ini next to the exe

static std::string presets_dir() { return exe_dir() + "\\presets"; }
static std::string preset_file(const std::string& name) { return presets_dir() + "\\" + name + ".ini"; }

static std::string date_text() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    char b[40];
    std::snprintf(b, sizeof b, "%04d-%02d-%02d %02d:%02d:%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    return b;
}

struct PresetInfo { std::string name, desc, saved; };

static void preset_header(const std::string& text, std::string& desc, std::string& saved) {
    std::stringstream in(text);
    std::string line;
    for (int n = 0; n < 8 && std::getline(in, line); ++n) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.compare(0, 15, "; description: ") == 0) desc = line.substr(15);
        else if (line.compare(0, 9, "; saved: ") == 0) saved = line.substr(9);
    }
}

static std::vector<PresetInfo> list_presets() {
    std::vector<PresetInfo> v;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((presets_dir() + "\\*.ini").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return v;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const std::string f = fd.cFileName;
        if (f.size() <= 4 || lower(f.substr(f.size() - 4)) != ".ini") continue;
        PresetInfo p;
        p.name = f.substr(0, f.size() - 4);
        if (!http::preset_name_ok(p.name)) continue;          // also leaves out the hidden _previous
        std::string text;
        if (read_file(preset_file(p.name), text)) preset_header(text, p.desc, p.saved);
        v.push_back(p);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    std::sort(v.begin(), v.end(), [](const PresetInfo& a, const PresetInfo& b) { return lower(a.name) < lower(b.name); });
    return v;
}

// The config a preset's text would give. The settings of this PC (MIDI port, game process, polling, pad slot,
// note-off style, port check) stay as they are. Called with g_lock held.
static bool preset_result(App& app, const std::string& text, Config& out, std::string& error) {
    Config c = app.cfg;
    c.midi_port = app.file_midi_port;
    c.process = app.file_process;
    ParseResult r;
    parse_ini(text, c, r);
    if (!r.ok()) {
        for (const auto& e : r.errors) {
            const auto colon = e.find(": ");
            error += (error.empty() ? "" : "; ") + (colon == std::string::npos ? e : e.substr(colon + 2));
        }
        return false;
    }
    c.midi_port = app.file_midi_port;
    c.process = app.file_process;
    c.poll_ms = app.cfg.poll_ms;
    c.pad = app.cfg.pad;
    c.note_off_zero = app.cfg.note_off_zero;
    c.midi_check = app.cfg.midi_check;
    out = c;
    return true;
}

// Remembers which preset the running config belongs to (g_lock held). A preset that no longer exists is forgotten.
static void sync_preset_state(App& app, const std::string& name) {
    std::string text, why;
    Config c;
    if (name.empty() || !read_file(preset_file(name), text) || !preset_result(app, text, c, why)) { app.preset_name.clear(); app.preset_ini.clear(); return; }
    app.preset_name = name;
    app.preset_ini = write_ini(c);
}

static void remember_last_preset(const std::string& name) {
    if (name.empty()) DeleteFileA((presets_dir() + "\\_last.txt").c_str());
    else write_file(presets_dir() + "\\_last.txt", name);
}

static std::string preset_text(const std::string& name, const std::string& desc, const std::string& ini) {
    return "; Noiseflip preset\n; name: " + name + "\n; description: " + desc + "\n; saved: " + date_text() + "\n\n" + ini;
}

static bool preset_save(App& app, const std::string& name, std::string desc, std::string& why) {
    if (!http::preset_name_ok(name)) { why = "a preset name is 1 to 48 letters, digits, spaces and _ - . ( )"; return false; }
    for (auto& ch : desc) if (ch == '\n' || ch == '\r' || static_cast<unsigned char>(ch) < 0x20) ch = ' ';
    desc = http::trim(desc);
    if (desc.size() > 200) desc.resize(200);
    Lock l;
    CreateDirectoryA(presets_dir().c_str(), nullptr);
    if (!write_file(preset_file(name), preset_text(name, desc, app.ini_text))) { why = "could not write " + preset_file(name); return false; }
    app.preset_name = name;
    app.preset_ini = app.ini_text;
    remember_last_preset(name);
    say("Preset saved: %s", name.c_str());
    return true;
}

static bool preset_load(App& app, const std::string& name, std::string& why) {
    const bool undo = name == "_previous";
    if (!undo && !http::preset_name_ok(name)) { why = "that is not a preset name"; return false; }
    std::string text;
    if (!read_file(preset_file(name), text)) { why = undo ? "there is nothing to undo" : "there is no preset called " + name; return false; }
    Lock l;
    Config c;
    if (!preset_result(app, text, c, why)) { why = "the preset has an error: " + why; return false; }
    // the way back: what was running just before (loading again from _previous swaps the two)
    std::string before = app.preset_name;
    CreateDirectoryA(presets_dir().c_str(), nullptr);
    write_file(preset_file("_previous"), "; Noiseflip: the settings that ran before the last preset load\n; active: " + before + "\n\n" + app.ini_text);
    install_config(app, c, false);
    if (!write_file(app.config_path, app.ini_text)) { why = "could not write " + app.config_path; return false; }
    app.config_stamp = file_stamp(app.config_path);
    app.saved_note = "preset " + (undo ? std::string("restored") : name) + " " + clock_text();
    if (undo) {
        std::string active;
        std::stringstream in(text);
        std::string line;
        while (std::getline(in, line) && line.compare(0, 1, ";") == 0)
            if (line.compare(0, 10, "; active: ") == 0) active = http::trim(line.substr(10));
        sync_preset_state(app, active);
    } else {
        sync_preset_state(app, name);
    }
    remember_last_preset(app.preset_name);
    say(undo ? "Preset load undone." : "Preset loaded: %s", name.c_str());
    return true;
}

static std::string presets_json(App& app) {
    std::string cur;
    { Lock l; cur = app.preset_name; }
    std::string o = std::string("{\"undo\":") + jbool(file_stamp(preset_file("_previous")) != 0) + ",\"current\":" + http::json_escape(cur) + ",\"list\":[";
    bool first = true;
    for (const auto& p : list_presets()) {
        if (!first) o += ",";
        first = false;
        o += "{\"name\":" + http::json_escape(p.name) + ",\"desc\":" + http::json_escape(p.desc) + ",\"saved\":" + http::json_escape(p.saved) + "}";
    }
    return o + "]}";
}

static std::string json_reply(bool ok, const std::string& error = "") {
    return http::response(ok ? 200 : 400, "application/json; charset=utf-8",
                          std::string("{\"ok\":") + jbool(ok) + (ok ? "" : ",\"error\":" + http::json_escape(error)) + "}");
}

static std::string handle_api(App& app, const http::Request& r) {
    if (r.method == "GET") {
        if (r.path == "/api/config") { Lock l; return http::response(200, "text/plain; charset=utf-8", app.ini_text); }
        if (r.path == "/api/defaults") return http::response(200, "text/plain; charset=utf-8", app.defaults_text);
        if (r.path == "/api/presets") return http::response(200, "application/json; charset=utf-8", presets_json(app));
        if (r.path == "/api/ports") {
            std::string out;
            const UINT n = midiOutGetNumDevs();
            for (UINT i = 0; i < n; ++i) out += midi_port_name(i) + "\n";
            return http::response(200, "text/plain; charset=utf-8", out);
        }
        if (r.path == "/api/live") {
            const unsigned long long since = std::strtoull(http::query_param(r.query, "since").c_str(), nullptr, 10);
            std::string snap;
            { Lock l; snap = app.snapshot; }
            std::string lines;
            unsigned long long top = since;
            EnterCriticalSection(&g_log_lock);
            for (const auto& ln : g_log) {
                if (ln.seq <= since) continue;
                if (!lines.empty()) lines += ",";
                lines += "{\"s\":" + std::to_string(ln.seq) + ",\"t\":" + jnum(ln.t) + ",\"m\":" + http::json_escape(ln.text) + "}";
                top = ln.seq;
            }
            if (since > g_log_seq) top = g_log_seq;       // the program restarted under an old page
            LeaveCriticalSection(&g_log_lock);
            return http::response(200, "application/json; charset=utf-8",
                                  "{\"snap\":" + snap + ",\"seq\":" + std::to_string(top) + ",\"log\":[" + lines + "]}");
        }
        return http::response(404, "text/plain", "not found");
    }
    if (r.path == "/api/set") {
        std::vector<http::Setting> v;
        std::string why;
        if (!http::parse_settings(r.body, v, why)) return json_reply(false, why);
        if (!apply_settings(app, v, why)) return json_reply(false, why);
        return json_reply(true);
    }
    if (r.path == "/api/test") {
        const std::string b = http::trim(r.body);
        int kind = 0, id = -1;
        if (b.compare(0, 5, "note|") == 0) { kind = 1; id = std::atoi(b.c_str() + 5); }
        else if (b.compare(0, 3, "cc|") == 0) { kind = 2; id = std::atoi(b.c_str() + 3); }
        else if (b != "stop") return json_reply(false, "note|<n>, cc|<n> or stop");
        Lock l;
        app.test_kind_req = kind;
        app.test_id_req = id;
        return json_reply(true);
    }
    if (r.path == "/api/preset/save") {
        const auto nl = r.body.find('\n');
        std::string why;
        if (!preset_save(app, http::trim(r.body.substr(0, nl)), nl == std::string::npos ? "" : r.body.substr(nl + 1), why)) return json_reply(false, why);
        return json_reply(true);
    }
    if (r.path == "/api/preset/load" || r.path == "/api/preset/undo") {
        std::string why;
        if (!preset_load(app, r.path == "/api/preset/undo" ? "_previous" : http::trim(r.body), why)) return json_reply(false, why);
        return json_reply(true);
    }
    if (r.path == "/api/preset/delete") {
        const std::string name = http::trim(r.body);
        if (!http::preset_name_ok(name)) return json_reply(false, "that is not a preset name");
        if (!DeleteFileA(preset_file(name).c_str())) return json_reply(false, "there is no preset called " + name);
        Lock l;
        if (app.preset_name == name) { app.preset_name.clear(); app.preset_ini.clear(); remember_last_preset(""); }
        say("Preset deleted: %s", name.c_str());
        return json_reply(true);
    }
    if (r.path == "/api/preset/folder") {
        CreateDirectoryA(presets_dir().c_str(), nullptr);
        ShellExecuteA(nullptr, "open", presets_dir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return json_reply(true);
    }
    if (r.path == "/api/window") {
        int x, y, w, h;
        if (std::sscanf(r.body.c_str(), "%d %d %d %d", &x, &y, &w, &h) != 4 || !window_valid(x, y, w, h)) return json_reply(false, "bad window");
        char b[96];
        std::snprintf(b, sizeof b, "%d %d %d %d", x, y, w, h);
        write_file(window_file(), b);
        return json_reply(true);
    }
    if (r.path == "/api/quit") { g_stop = true; return json_reply(true); }
    return http::response(404, "text/plain", "not found");
}

// ---------------------------------------------------------------------------------------------
// one attached game session

static std::string process_name(App& app) { Lock l; return app.cfg.process; }

static void run_session(App& app, DWORD pid) {
    WinMemory mem;
    mem.process = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION | SYNCHRONIZE, FALSE, pid);
    if (!mem.process) mem.process = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid);
    if (!mem.process) {
        say("Cannot open process %lu (error %lu). If the game runs as administrator, run this as administrator too.", pid, GetLastError());
        Sleep(2000);
        return;
    }
    std::uintptr_t base = 0;
    for (int i = 0; i < 50 && !find_module_base(pid, process_name(app), base) && !g_stop; ++i) Sleep(100);
    if (!base) {
        say("Could not find the module %s in process %lu.", process_name(app).c_str(), pid);
        CloseHandle(mem.process);
        Sleep(2000);
        return;
    }
    app.cc.reset();   // every controller is sent once at the start of a session
    { Lock l; app.live.attached = true; app.live.status = "Attached: looking for the skater"; }
    say("Attached to %s (pid %lu), image base 0x%llx", process_name(app).c_str(), pid, static_cast<unsigned long long>(base));

    std::uintptr_t client = 0;
    std::int64_t last_scan = 0, scan_gap = 3000000;  // 3 s, doubles up to 30 s while nothing is found
    std::int64_t not_ok_since = 0;
    Result printed = Result::NoClient;
    std::string printed_step;
    bool printed_once = false;
    std::uint32_t last_state = 0;
    bool have_state = false;
    bool pad_reported = false, pad_was_connected = false;
    bool motion_reported = false, motion_was_ok = false;
    std::int64_t last_watch = 0;

    while (!g_stop) {
        if (WaitForSingleObject(mem.process, 0) == WAIT_OBJECT_0) { say("The game has closed."); break; }
        const std::int64_t now = now_us();
        check_reload(app);
        service_midi(app);

        // is the remembered client object still a client object?
        if (client) {
            std::uintptr_t vt = 0;
            if (!rd_ptr(mem, client, vt) || vt != base + rva::client_vtable) client = 0;
        }

        Resolved r;
        if (client) r = resolve(mem, base, client);
        else r.result = Result::NoClient;

        if (r.result == Result::Ok) {
            not_ok_since = 0;
            scan_gap = 3000000;
        } else {
            if (!not_ok_since) not_ok_since = now;
            // nothing usable for a while: look again for the client object, with backoff
            const bool first_look = last_scan == 0;
            if (first_look || (now - not_ok_since >= 3000000 && now - last_scan >= scan_gap)) {
                say("Looking for the game's client object (the game's memory is big: this can take a while)...");
                const std::int64_t began = now_us();
                const std::uintptr_t found = scan_for_client(mem, base);
                last_scan = now_us();
                scan_gap = std::min<std::int64_t>(scan_gap * 2, 30000000);
                if (found) {
                    if (found != client)
                        say("Client object at 0x%llx (scan took %.1f s)", static_cast<unsigned long long>(found),
                            static_cast<double>(last_scan - began) / 1e6);
                    client = found;
                    r = resolve(mem, base, client);
                } else {
                    say("Client object not found yet. Start a level; will retry.");
                }
            }
        }

        // report status changes (not every sample)
        if (r.result != Result::Teleporting &&
            (!printed_once || r.result != printed || printed_step != r.step)) {
            printed_once = true;
            printed = r.result;
            printed_step = r.step;
            switch (r.result) {
                case Result::Ok: say("Skater found. Reading the physics state."); log_line(app, "STATUS,skater found"); { Lock l; app.live.status = "Skater found: reading the physics state"; } break;
                case Result::NotInLevel: say("Waiting: the game is not in a level."); log_line(app, "STATUS,not in a level"); { Lock l; app.live.status = "Attached: the game is not in a level"; } break;
                case Result::ChainBroken: say("Skater not available (%s).", r.step); log_line(app, "STATUS,skater not available (%s)", r.step); { Lock l; app.live.status = std::string("Skater not available (") + r.step + ")"; } break;
                case Result::NoClient: break;
                case Result::Teleporting: break;
            }
        }

        const Pad pad = app.pad.read();
        if (pad.connected != pad_was_connected || !pad_reported) {
            pad_reported = true;
            pad_was_connected = pad.connected;
            if (pad.connected) say("Gamepad connected (XInput slots mask %u).", app.pad.connected_mask());
            else say("No gamepad found (XInput). Grab, manual, push, footplant and nofootgrab need one; the rest still work.");
        }

        Input in;
        in.pad = pad;
        if (r.result == Result::Ok) {
            RawMotion raw;
            const char* why = "";
            const bool got = read_motion(mem, base, r, raw, why);
            if (got != motion_was_ok || !motion_reported) {
                motion_reported = true;
                motion_was_ok = got;
                if (got) say("Board motion found (speed, height and board spin).");
                else say("Board motion unavailable (%s): landing height, speed bonus and rotation triggers are off.", why);
                log_line(app, "STATUS,board motion %s", got ? "ok" : why);
            }
            if (got) {
                in.motion.ok = true;
                in.motion.y = raw.position[1];
                in.motion.speed = std::sqrt(static_cast<double>(raw.velocity[0]) * raw.velocity[0] +
                                            static_cast<double>(raw.velocity[2]) * raw.velocity[2]);
                for (int a = 0; a < 3; ++a) in.motion.w_dps[a] = static_cast<double>(raw.spin[a]) * 57.29577951308232;
                if (app.cli.watch && now - last_watch >= 50000) {
                    last_watch = now;
                    std::printf("state %-4u speed %5.2f m/s  height %8.3f m  spin deg/s: X %7.1f  Y %7.1f  Z %7.1f\n", r.state, in.motion.speed,
                                in.motion.y, in.motion.w_dps[0], in.motion.w_dps[1], in.motion.w_dps[2]);
                    std::fflush(stdout);
                }
            }
            if (!have_state || r.state != last_state) {
                if (app.cli.verbose) say("state %u%s", r.state, have_state ? "" : "  (first sample)");
                log_line(app, "STATE,%u", r.state);
            }
            last_state = r.state;
            have_state = true;
            in.game_ok = true;
            in.state = r.state;
        } else if (r.result != Result::Teleporting) {
            have_state = false;
        }
        std::vector<Action> actions;
        std::vector<CcMessage> ccs;
        {
            Lock l;
            app.live.in = in;
            actions = app.engine.update(now_us(), in);
            ccs = app.cc.update(now_us(), app.engine.cc_targets(), app.cfg);
        }
        emit(app, actions, pad);
        for (const auto& m : ccs) send_cc(app, m);
        service_ui(app);
        if (app.engine.mode_switches() != app.seen_switches) {
            app.seen_switches = app.engine.mode_switches();
            say("Rotation mode switched to %s (%s pressed).", app.engine.angle_mode() ? "ANGLE: a note every 180 degrees" : "RATE: one note per spin",
                detail::button_str(app.cfg.rotation.switch_button).c_str());
            log_line(app, "MODE,%s", app.engine.angle_mode() ? "angle" : "rate");
        }

        if (app.cfg.poll_ms > 0) Sleep(static_cast<DWORD>(app.cfg.poll_ms));
        else YieldProcessor();
    }

    { Lock l; app.live.attached = false; app.live.in.game_ok = false; app.live.status = "Waiting for the game"; }
    emit(app, app.engine.release_all(), Pad{});
    CloseHandle(mem.process);
}

// ---------------------------------------------------------------------------------------------
// test modes

static void run_midi_test(App& app) {
    say("MIDI test: every trigger's note once (pulse length %d ms).", app.cfg.hold_ms);
    for (int t = 0; t < TriggerCount && !g_stop; ++t) {
        Action on;
        on.on = true; on.trigger = t; on.note = app.cfg.t[t].note;
        on.velocity = app.cfg.t[t].velocity > 0 ? app.cfg.t[t].velocity : app.cfg.velocity;
        on.channel = (app.cfg.t[t].channel > 0 ? app.cfg.t[t].channel : app.cfg.channel) - 1;
        emit(app, {on}, Pad{});
        Sleep(static_cast<DWORD>(app.cfg.t[t].hold_ms > 0 ? app.cfg.t[t].hold_ms : app.cfg.hold_ms));
        Action off = on;
        off.on = false; off.velocity = 0;
        emit(app, {off}, Pad{});
        for (int i = 0; i < 30; ++i) { service_midi(app); Sleep(10); }
    }
}

// ---------------------------------------------------------------------------------------------
// the mapper: ONE note or ONE controller on repeat, so it can be MIDI-mapped in Ableton without playing

static CRITICAL_SECTION g_line_lock;
static std::vector<std::string> g_lines;
static bool g_stdin_closed = false;

static DWORD WINAPI line_reader(void*) {
    std::string line;
    while (std::getline(std::cin, line)) {
        EnterCriticalSection(&g_line_lock);
        g_lines.push_back(line);
        LeaveCriticalSection(&g_line_lock);
    }
    EnterCriticalSection(&g_line_lock);
    g_stdin_closed = true;
    LeaveCriticalSection(&g_line_lock);
    return 0;
}

static void print_map_list(const Config& c) {
    std::printf("\nNOTES    (type  n <number or name>,  for example  n 37  or  n land)\n");
    for (int t = 0; t < TriggerCount; ++t)
        std::printf("  n %-11s note %3d  %-4s%s\n", section_name(t), c.t[t].note, note_name(c.t[t].note).c_str(), c.t[t].enabled ? "" : "   (disabled)");
    std::printf("CONTROLLERS    (type  c <number or name>,  for example  c 20  or  c intensity)\n");
    for (int i = 0; i < CcCount; ++i) {
        std::string name = cc_name(i);
        for (auto& ch : name) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        std::printf("  c %-11s CC %3d%s\n", name.c_str(), c.cc[i].cc, c.cc[i].enabled ? "" : "   (disabled)");
    }
    std::printf("Enter = stop sending    list = show this again    q = quit    (any other number works too, e.g. n 99 or c 74)\n\n");
    std::fflush(stdout);
}

static void run_mapper(App& app, MapTarget cur, bool interactive) {
    bool note_on = false;
    int last_cc = -1;
    std::int64_t start = now_us(), last_send = 0, last_cycle = -1;
    unsigned pulses = 0;

    auto note_action = [&](bool on) {
        Action a;
        a.on = on; a.trigger = 0; a.note = cur.number; a.velocity = on ? cur.velocity : 0; a.channel = cur.channel;
        return a;
    };
    auto silence = [&]() { if (cur.kind == MapTarget::Note && note_on) { send_midi(app, note_action(false)); note_on = false; } };
    auto announce = [&]() {
        if (cur.kind == MapTarget::Note)
            say("Sending %s, channel %d, velocity %d: on for 0.4 s, off for 0.6 s, again and again.", cur.label.c_str(), cur.channel + 1, cur.velocity);
        else if (cur.kind == MapTarget::Cc)
            say("Sending %s, channel %d: sweeping 0 -> 127 -> 0 every 3 seconds, again and again.", cur.label.c_str(), cur.channel + 1);
        else
            say("Nothing is being sent.");
        if (cur.kind != MapTarget::None)
            say("In Ableton: press Ctrl+M, click the knob / pad / button you want, and it learns this. Press Ctrl+M again when done.");
    };

    if (interactive) {
        InitializeCriticalSection(&g_line_lock);
        CreateThread(nullptr, 0, line_reader, nullptr, 0, nullptr);   // typing must not stop the sending
        say("MIDI mapper. Choose what to send; it repeats until you choose something else.");
        print_map_list(app.cfg);
    }
    if (cur.kind != MapTarget::None) announce();
    else if (interactive) say("Type what to send, for example  n land  or  c intensity.");

    while (!g_stop) {
        service_midi(app);
        if (interactive) {
            std::vector<std::string> typed;
            bool closed = false;
            EnterCriticalSection(&g_line_lock);
            typed.swap(g_lines);
            closed = g_stdin_closed;
            LeaveCriticalSection(&g_line_lock);
            for (const auto& line : typed) {
                const MapCommand cmd = parse_map_command(app.cfg, line);
                if (cmd.type == MapCommand::Quit) { g_stop = true; break; }
                if (cmd.type == MapCommand::List) { print_map_list(app.cfg); continue; }
                if (cmd.type == MapCommand::Error) { say("%s", cmd.message.c_str()); continue; }
                silence();
                last_cc = -1;
                last_cycle = -1;
                pulses = 0;
                start = now_us();
                if (cmd.type == MapCommand::Stop) cur = MapTarget();
                else cur = cmd.target;
                announce();
            }
            if (closed && typed.empty()) g_stop = true;          // the input was closed (a pipe ended): stop
        }
        if (g_stop) break;

        const std::int64_t now = now_us(), t = now - start;
        if (cur.kind == MapTarget::Note) {
            const bool on = note_pulse_on(t);
            if (on && !note_on) { send_midi(app, note_action(true)); note_on = true; say("note %d ON  (%u)", cur.number, ++pulses); }
            else if (!on && note_on) { send_midi(app, note_action(false)); note_on = false; }
        } else if (cur.kind == MapTarget::Cc) {
            if (now - last_send >= 25000) {                       // 40 updates per second at most
                last_send = now;
                const int v = cc_sweep_value(t);
                if (v != last_cc) {
                    CcMessage m;
                    m.index = -1; m.channel = cur.channel; m.cc = cur.number; m.value = v;
                    send_cc(app, m);
                    last_cc = v;
                }
                const std::int64_t cycle = t / 3000000;
                if (cycle != last_cycle) { last_cycle = cycle; say("CC %d sweep %lld", cur.number, static_cast<long long>(cycle + 1)); }
            }
        }
        Sleep(5);
    }
    silence();
    say("Mapper stopped.");
}

static void run_cc_test(App& app) {
    say("Controller test: each enabled controller goes from 0 to 127 and back to 0 (about 3 seconds each).");
    say("In Ableton: Ctrl+M (MIDI map), click the knob you want, then start this; the knob follows the sweep.");
    for (int i = 0; i < CcCount && !g_stop; ++i) {
        const CcCfg& k = app.cfg.cc[i];
        if (!k.enabled) continue;
        const int channel = (k.channel > 0 ? k.channel : app.cfg.channel) - 1;
        say("%s: CC %d on channel %d", cc_name(i), k.cc, channel + 1);
        for (int step = 0; step <= 100 && !g_stop; ++step) {              // 0 -> 127 -> 0 in 100 steps of 30 ms
            const double x = step <= 50 ? step / 50.0 : (100 - step) / 50.0;
            CcMessage m;
            m.index = i; m.channel = channel; m.cc = k.cc; m.value = static_cast<int>(std::lround(127.0 * x));
            send_cc(app, m);
            Sleep(30);
        }
    }
    say("Controller test done.");
}

static void run_pad_test(App& app) {
    if (!app.pad.available()) { say("XInput is not available on this system."); return; }
    say("Gamepad test. Move the sticks and press triggers and buttons. Ctrl+C to quit.");
    say("'manual stick' says yes while the stick is inside your [manual_stick] range (%d-%d on %s).",
        app.cfg.manual_min, app.cfg.manual_max, detail::axis_str(app.cfg.manual_axis));
    Pad last;
    bool first = true;
    std::int64_t last_print = 0;
    while (!g_stop) {
        const Pad p = app.pad.read();
        const bool changed = first || p.connected != last.connected || p.lt != last.lt || p.rt != last.rt ||
                             p.buttons != last.buttons || std::abs(p.rx - last.rx) > 300 || std::abs(p.ry - last.ry) > 300 ||
                             std::abs(p.lx - last.lx) > 300 || std::abs(p.ly - last.ly) > 300;
        if (changed && now_us() - last_print > 50000) {
            last_print = now_us();
            last = p;
            first = false;
            if (!p.connected) std::printf("no gamepad found (is it connected? Steam Input may hide it)\n");
            else std::printf("LT %3d  RT %3d | L(%6d,%6d)  R(%6d,%6d) | %-16s | manual stick: %s\n", p.lt, p.rt, p.lx, p.ly, p.rx, p.ry,
                             buttons_text(p.buttons).c_str(), app.engine.in_manual_range(p) ? "yes" : "no");
            std::fflush(stdout);
        }
        Sleep(5);
    }
}

// ---------------------------------------------------------------------------------------------

int main(int argc, char** argv) {
    App app;
    if (!parse_args(argc, argv, app.cli)) return 1;
    if (app.cli.help) { usage(); return 0; }
    if (app.cli.list) { list_midi_ports(); return 0; }

    app.config_path = app.cli.config_path.empty() ? exe_dir() + "\\noiseflip.ini" : app.cli.config_path;
    if (app.cli.config_path.empty() && !file_stamp(app.config_path) && !app.cli.write_config) {
        const std::string old_name = exe_dir() + "\\skate_midi.ini";   // the file this program used before it was called Noiseflip
        if (file_stamp(old_name)) {
            app.config_path = old_name;
            say("Using your existing skate_midi.ini. Rename it to noiseflip.ini when you like; new options keep their defaults until you add them.");
        }
    }

    if (app.cli.write_config) {
        if (file_stamp(app.config_path) && !app.cli.force) {
            std::fprintf(stderr, "%s already exists. Use --force to overwrite it.\n", app.config_path.c_str());
            return 1;
        }
        if (!write_file(app.config_path, write_ini(Config()))) { std::fprintf(stderr, "cannot write %s\n", app.config_path.c_str()); return 1; }
        std::printf("Wrote %s\n", app.config_path.c_str());
        return 0;
    }

    if (!file_stamp(app.config_path)) {
        if (write_file(app.config_path, write_ini(Config()))) say("Created %s with the default values. Edit it any time.", app.config_path.c_str());
        else say("Could not create %s; using the built-in defaults.", app.config_path.c_str());
    }
    {
        ParseResult r;
        Config c;
        if (file_stamp(app.config_path) && !load_config(app, c, r)) {
            report(r);
            std::fprintf(stderr, "Fix the errors in %s (or delete it to get a fresh one) and start again.\n", app.config_path.c_str());
            return 3;
        }
        report(r);
        if (file_stamp(app.config_path)) app.cfg = c;
        else { app.cfg = Config(); apply_cli(app.cli, app.cfg); }
    }
    app.config_stamp = file_stamp(app.config_path);
    app.engine.set_config(app.cfg);
    app.file_midi_port = app.cfg.midi_port;
    app.file_process = app.cfg.process;
    app.ini_text = write_ini(app.cfg);
    app.defaults_text = write_ini(Config());
    say("Config: %s", app.config_path.c_str());
    {
        std::string last;
        read_file(presets_dir() + "\\_last.txt", last);
        Lock l;
        sync_preset_state(app, http::trim(last));
        if (!app.preset_name.empty()) say("Preset: %s", app.preset_name.c_str());
    }

    const bool mapping_mode = app.cli.map_mode || !app.cli.test_cc_target.empty() || !app.cli.test_note_target.empty();
    const bool ui_mode = !app.cli.no_ui && !mapping_mode && !app.cli.test_midi && !app.cli.test_cc && !app.cli.pad_test && !app.cli.watch;
    if (ui_mode && !app.cli.allow_multiple) {
        // a second launch only opens the window of the one that runs
        HANDLE mutex = CreateMutexA(nullptr, FALSE, "Local\\Noiseflip.SingleInstance");
        if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
            say("Noiseflip is already running: opening its window.");
            if (!app.cli.no_window) open_ui_window("http://127.0.0.1:" + std::to_string(app.cli.ui_port) + "/");
            return 0;
        }
    }

    SetConsoleCtrlHandler(on_ctrl, TRUE);
    SetConsoleTitleA("Noiseflip");
    set_console_icon();
    timeBeginPeriod(1);  // 1 ms timer resolution so the polling loop really runs every ~1 ms

    if (!app.cli.log_path.empty()) {
        app.log = std::fopen(app.cli.log_path.c_str(), "w");
        if (!app.log) say("Cannot write the log file %s", app.cli.log_path.c_str());
        else { std::fputs("seconds,event,detail...\n", app.log); say("Logging to %s", app.cli.log_path.c_str()); }
    }

    if (app.pad.load()) {
        app.pad.set_slot(app.cfg.pad);
        app.pad.start_prober();
        Sleep(150);  // let the first probe find a pad before the first sample
    } else {
        say("XInput could not be loaded: gamepad triggers are disabled.");
    }

    if (app.cli.pad_test) {
        run_pad_test(app);
        app.pad.stop_prober();
        timeEndPeriod(1);
        return 0;
    }

    if (!app.cli.dry_run) {
        const int id = app.cli.midi_index >= 0 ? app.cli.midi_index : find_midi_port(app.cfg.midi_port);
        const bool opened = id >= 0 && app.midi.open(static_cast<UINT>(id));
        if (!opened) {
            std::fprintf(stderr, "Could not open a MIDI port matching \"%s\".\n", app.cfg.midi_port.c_str());
            list_midi_ports();
            std::fprintf(stderr, "Start loopMIDI and create a port, or pick one with midi_port in the ini / --midi NAME / --midi-index N,\n"
                                 "or use --dry-run to run without MIDI.\n");
            if (!ui_mode) {
                app.pad.stop_prober();
                timeEndPeriod(1);
                return 2;
            }
            say("No MIDI port: running without MIDI. Start loopMIDI, then restart Noiseflip.");
        } else {
        const std::string port = midi_port_name(static_cast<UINT>(id));
        app.midi_name = port;
        say("MIDI output: %s", port.c_str());

        // A second copy sending to the same port doubles every note (and an older program does the same).
        const bool mapping = app.cli.map_mode || !app.cli.test_cc_target.empty() || !app.cli.test_note_target.empty();
        if (!app.cli.allow_multiple && !mapping && !ui_mode) {
            HANDLE mutex = CreateMutexA(nullptr, FALSE, "Local\\Noiseflip.SingleInstance");
            if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
                std::fprintf(stderr, "Another Noiseflip is already running and sending MIDI. Two copies double every note.\n"
                                     "Close the other one (or use --allow-multiple if you really want both).\n");
                app.midi.close();
                app.pad.stop_prober();
                timeEndPeriod(1);
                return 4;
            }
        }
        for (const char* other : {"skate_physics_midi.exe", "skate_telemetry.exe"}) {
            const DWORD other_pid = find_process(other);
            if (other_pid) say("WARNING: %s (pid %lu) is running. If it also sends MIDI to this port you will hear extra notes. Close it.", other, other_pid);
        }
        if (app.cfg.midi_check) {
            app.monitoring = app.monitor.open(port);
            if (app.monitoring) say("MIDI port check on: notes on the port that Noiseflip did not send will be reported as FOREIGN.");
            else say("MIDI port check unavailable (could not open the port's input side); continuing without it.");
        }
        }
    } else {
        say("Dry run: no MIDI port is opened.");
    }
    print_summary(app.cfg, app.engine.angle_mode());

    bool mapper_failed = false;
    UiServer ui;
    bool ui_started = false;
    const bool mapper_used = mapping_mode;
    if (app.cli.map_mode || !app.cli.test_cc_target.empty() || !app.cli.test_note_target.empty()) {
        mapper_failed = false;
        if (!app.cli.test_cc_target.empty() && !app.cli.test_note_target.empty()) {
            std::fprintf(stderr, "Choose one: --test-cc or --test-note (or use --map to switch between them).\n");
            mapper_failed = true;
        } else {
            MapTarget target;
            std::string err;
            bool ok = true;
            if (!app.cli.test_note_target.empty()) ok = resolve_note(app.cfg, app.cli.test_note_target, target, err);
            else if (!app.cli.test_cc_target.empty()) ok = resolve_cc(app.cfg, app.cli.test_cc_target, target, err);
            if (!ok) { std::fprintf(stderr, "%s\n", err.c_str()); mapper_failed = true; }
            else run_mapper(app, target, app.cli.map_mode);
        }
    } else if (app.cli.test_cc) {
        run_cc_test(app);
    } else if (app.cli.test_midi) {
        run_midi_test(app);
    } else {
        if (ui_mode) {
            char exe[MAX_PATH];
            ui_started = ui.start(app.cli.ui_port, exe_dir() + "\\ui", [&app](const http::Request& req) { return handle_api(app, req); });
            if (!ui_started) say("The UI could not start (ports %d-%d are busy). Noiseflip runs without it.", app.cli.ui_port, app.cli.ui_port + 10);
            else {
                say("UI: %s   (closing its window leaves Noiseflip running; start Noiseflip again to get the window back)", ui.url().c_str());
                if (!app.cli.no_window) open_ui_window(ui.url());
            }
            (void)exe;
        }
        say("Waiting for %s ... (Ctrl+C to quit)", process_name(app).c_str());
        std::int64_t last_find = 0;
        DWORD pid = 0;
        while (!g_stop) {
            check_reload(app);
            service_midi(app);
            {   // the pad and the test signals work without the game, so the UI is useful while setting up
                const Pad p = app.pad.read();
                Lock l;
                app.live.in.pad = p;
            }
            service_ui(app);
            if (now_us() - last_find >= 500000) {
                last_find = now_us();
                pid = app.cli.pid ? app.cli.pid : find_process(process_name(app));
            }
            if (!pid) { Sleep(5); continue; }
            run_session(app, pid);
            pid = 0;
            last_find = now_us();
            if (g_stop || app.cli.pid) break;
            say("Waiting for %s ...", process_name(app).c_str());
        }
    }

    if (app.test.kind == 1 && app.test.note_on) {
        Action off;
        off.on = false; off.note = app.test.target.number; off.channel = app.test.target.channel;
        send_midi(app, off);
    }
    ui.stop();
    emit(app, app.engine.release_all(), Pad{});
    std::string stats;
    for (int t = 0; t < TriggerCount; ++t) {
        char buf[96];
        std::snprintf(buf, sizeof buf, "%s %llu (ignored %llu%s)  ", section_name(t), app.engine.fired(t), app.engine.suppressed(t),
                      app.engine.cancelled(t) ? ", cancelled" : "");
        stats += buf;
    }
    if (!mapper_used) say("Triggers: %s", stats.c_str());           // the mapper plays no triggers: nothing to report
    if (app.monitoring && !mapper_used) {
        Sleep(100);
        service_midi(app);
        say("MIDI: sent %llu note messages, %llu came back through the port, %llu FOREIGN (not sent by Noiseflip), %llu not echoed.",
            app.audit.sent_count(), app.audit.ours(), app.audit.foreign(), app.audit.unechoed(now_us() + 3000000));
    }
    app.pad.stop_prober();
    app.monitor.close();
    app.midi.close();
    if (app.log) std::fclose(app.log);
    timeEndPeriod(1);
    return mapper_failed ? 1 : 0;
}
