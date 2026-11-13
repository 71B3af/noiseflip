// config.h
//
// Every tunable value in one place, plus a small INI reader/writer (no dependencies).
// The program writes a fully commented noiseflip.ini on first run; edit it while the program
// runs and the values are reloaded within about a second.
#pragma once
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace skate {

enum Trigger {
    Jump = 0, Land, Bail,          // from the physics state
    Grind, Manual, Grab,           // held while a condition is true ("gates")
    Push, Footplant, NoFootGrab,   // short pulses
    RotX, RotY, RotZ,              // board rotation about each axis (pulse or held, your choice)
    Fingerflip,                    // grab + board rotation, in the air
    TriggerCount
};

inline const char* trigger_name(int t) {
    static const char* names[] = {"JUMP", "LAND", "BAIL", "GRIND", "MANUAL", "GRAB", "PUSH", "FOOTPLANT", "NOFOOTGRAB",
                                  "ROT_X", "ROT_Y", "ROT_Z", "FINGERFLIP"};
    return t >= 0 && t < TriggerCount ? names[t] : "?";
}
inline const char* section_name(int t) {
    static const char* names[] = {"jump", "land", "bail", "grind", "manual", "grab", "push", "footplant", "nofootgrab",
                                  "rot_x", "rot_y", "rot_z", "fingerflip"};
    return t >= 0 && t < TriggerCount ? names[t] : "?";
}

enum class Kind { Pulse, Gate, Flexible };   // Flexible: `mode = pulse` or `mode = gate` in the ini
inline Kind kind_of(int t) {
    if (t == Grind || t == Manual || t == Grab) return Kind::Gate;
    if (t >= RotX) return Kind::Flexible;
    return Kind::Pulse;
}
inline bool is_rotation(int t) { return t >= RotX && t <= Fingerflip; }

enum class Context { Air, GroundOrAir, Any };

// A list of state numbers and ranges, e.g. "100-199, 504". May be empty.
struct Ranges {
    std::vector<std::pair<std::uint32_t, std::uint32_t>> r;
    bool contains(std::uint32_t s) const {
        for (const auto& p : r) if (s >= p.first && s <= p.second) return true;
        return false;
    }
    std::string str() const {
        std::string out;
        for (const auto& p : r) {
            if (!out.empty()) out += ", ";
            out += std::to_string(p.first);
            if (p.second != p.first) out += "-" + std::to_string(p.second);
        }
        return out;
    }
};

enum class Axis { LX, LY, RX, RY };
enum class Direction { Both, Up, Down };

// XInput button masks.
struct ButtonName { const char* name; unsigned mask; };
inline const std::vector<ButtonName>& button_names() {
    static const std::vector<ButtonName> v = {
        {"A", 0x1000}, {"B", 0x2000}, {"X", 0x4000}, {"Y", 0x8000}, {"LB", 0x0100}, {"RB", 0x0200},
        {"LS", 0x0040}, {"RS", 0x0080}, {"START", 0x0010}, {"BACK", 0x0020},
        {"DPAD_UP", 0x0001}, {"DPAD_DOWN", 0x0002}, {"DPAD_LEFT", 0x0004}, {"DPAD_RIGHT", 0x0008}};
    return v;
}

constexpr int kNoteStepDefault = -1000;   // "use [rotation] note_step"

struct TriggerCfg {
    bool enabled = true;
    int note = 0;
    int velocity = -1;      // -1 = use [general] velocity
    int channel = -1;       // -1 = use [general] channel (1-16)
    int delay_ms = 0;       // pulse: wait this long after the event;  gate: condition must hold this long
    int hold_ms = -1;       // pulse: note length (-1 = [general] hold_ms)
    int off_delay_ms = 0;   // gate: condition must be false this long before the note ends.
                            // rotation triggers call it tail_ms; in pulse mode it is the re-arm time
    int min_hold_ms = 0;    // gate: shortest note length
    int speed_bonus = -1;   // extra velocity at top speed (-1 = use [general] speed_bonus)

    // rotation triggers and fingerflip only
    bool gate = false;      // false: one fixed-length note when the rotation starts; true: held while it lasts
    int threshold = 300;    // degrees per second that starts it
    int release = 200;      // it keeps going until the rate is below this
    Context context = Context::Air;
    int axes = 7;           // fingerflip: which axes count (bit 1 = x, 2 = y, 4 = z)
    int detect = -1;        // -1 = [rotation] detect, 0 = rate, 1 = angle
    double step_deg = -1;   // angle mode: degrees per note (-1 = [rotation] step_deg)
    int note_step = kNoteStepDefault;   // angle mode: semitones per step (default = [rotation] note_step)
};

// How the rotation triggers (rot_x, rot_y, rot_z, fingerflip) work, unless a trigger sets its own `detect`.
//   rate : one note when the spin RATE passes `threshold` degrees per second (the first mode)
//   angle: one note for every `step_deg` degrees the board has turned (a kickflip = 2 notes of 180)
struct Rotation {
    bool angle = true;          // true = angle mode (the default), false = rate mode
    double step_deg = 180;      // angle mode: a note for every this many degrees, separately for each axis
    int min_rate = 150;         // angle mode: spin slower than this (deg/s) does not count as rotating
    int ground_min_rate = 300;  // the same, but while the skater is on the ground: only QUICK turns (a revert) count there,
                                // not carving
    int idle_reset_ms = 150;    // angle mode: after this long below min_rate the turned angle starts again from 0
    int hold_ms = 120;          // angle mode: length of each note (a new step cuts the previous note)
    int note_step = 0;          // angle mode: semitones added per step within one trick (0 = the same note every time)
    Ranges extra_states;        // physics states that count as "on the ground" for the rotation triggers only
                                // (for example the state of a manual, if it has its own number)
    unsigned switch_button = 0x0040;   // pressing this button flips between rate and angle mode while playing (0 = off)
};

struct LandVelocity {
    bool enabled = true;
    int min_velocity = 8;       // the quietest landing
    int max_velocity = 110;     // the hardest landing
    double min_height = 0.1;    // drop (metres) at or below which the landing is the quietest
    double max_height = 3.0;    // drop at or above which it is the hardest
    double curve = 1.0;         // 1 = straight line; below 1 small drops get louder; above 1 only big drops are loud
};

// ---- MIDI continuous controllers (CC) ----
enum CcId { CcIntensity = 0, CcRotX, CcRotY, CcRotZ, CcManual, CcSpeed, CcCount };
inline const char* cc_section(int i) {
    static const char* names[] = {"cc_intensity", "cc_rot_x", "cc_rot_y", "cc_rot_z", "cc_manual", "cc_speed"};
    return i >= 0 && i < CcCount ? names[i] : "?";
}
inline const char* cc_name(int i) {
    static const char* names[] = {"INTENSITY", "ROT_X", "ROT_Y", "ROT_Z", "MANUAL", "SPEED"};
    return i >= 0 && i < CcCount ? names[i] : "?";
}

struct CcCfg {                      // what every controller has
    bool enabled = true;
    int cc = 20;                    // controller number 0-127
    int channel = -1;               // -1 = [general] channel
    double smooth_ms = 0;           // smoothing time constant; 0 = none
    int min_step = 1;               // ignore moves smaller than this (unless the value has stopped for 100 ms)
    int max_rate_hz = 60;           // never more than this many messages per second
};

// The energy meter: every action adds energy, it falls back over time. More intense playing = a higher value.
struct IntensityCfg {
    double decay_s = 6;             // the energy falls to 37% in this time; 0 = it never falls
    int min_value = 0, max_value = 127;
    double curve = 1;               // 1 = straight; below 1 low energy is already loud; above 1 only high energy is
    bool land_scaled = true;        // a landing adds energy in proportion to how hard it was
    double event[TriggerCount] = {};        // energy (0-1) added by each note of that trigger
    double grind_per_s = 0.20, manual_per_s = 0.15, grab_per_s = 0.10;   // added every second while held
    double speed_per_s = 0;         // added every second at top speed (0 = speed does not count)
};

// A board spin kept as a fader: spinning one way raises it, the other way lowers it, and it stays where it is.
struct RotationCc {
    double range_deg = 3600;        // this many degrees of net spin = the full 0-127
    int min_rate = 150;             // spin slower than this (deg/s) is not counted (ollie pitch, drift)
    int ground_min_rate = 300;      // the same on the ground (only quick turns, like a revert)
    Context context = Context::Air; // where the spin counts
    bool invert = false;            // true: the direction that raises it is the other one
    int start = 0;                  // value at the start
    double decay_per_s = 0;         // 127-scale units per second back to `start`; 0 = it stays
};

struct ManualCc {                   // the manual stick position, scaled from the manual range to 0-127
    bool only_while_manual = true;  // true: only while the manual note is sounding; false: whenever the stick is in range
    bool signed_mode = false;       // false: how far the stick is pushed (0-127); true: up = 64-127, down = 0-63
    int idle = -1;                  // the value when no manual: -1 = automatic (0, or 64 in signed mode)
};

struct SpeedCc {                   // the board's horizontal speed, scaled to 0-127
    double min_speed = 0;           // m/s: at or below this the value is 0
    double max_speed = 10;          // m/s: at or above this the value is 127
    double curve = 1;               // 1 = straight; below 1 slow speeds are already high; above 1 only fast ones are
    bool invert = false;            // true: faster = lower
};

struct Config {
    // [general]
    int velocity = 60;
    int channel = 1;          // 1-16
    int hold_ms = 300;        // default length of the pulse triggers
    std::string midi_port = "loopMIDI";
    std::string process = "Skate.exe";
    int poll_ms = 1;          // 0 = spin
    int pad = -1;             // -1 = merge every connected pad, 0-3 = one XInput slot
    bool note_off_zero = false;   // false: a real note-off (0x80). true: note-on with velocity 0 (0x90) instead
    bool midi_check = true;       // watch the MIDI port for notes Noiseflip did not send, and warn
    double speed_min = 1.0;   // m/s: at or below this the speed bonus is 0
    double speed_max = 8.0;   // m/s: at or above this the speed bonus is full
    int speed_bonus = 40;     // extra velocity at top speed, for every trigger that does not set its own

    // [states]
    Ranges ground{{{100, 199}}};
    Ranges air{{{200, 299}}};
    Ranges grind{{{400, 499}}};
    Ranges bail{{{504, 504}}};
    int bail_rearm_ms = 500;  // after a bail, the skater must ride this long before another bail counts

    // [input]
    int trigger_press = 100;    // LT/RT value (0-255) that counts as pressed
    int trigger_release = 60;   // ... and the value below which it counts as released
    bool grab_lt = true, grab_rt = true;
    unsigned push_button = 0x1000 | 0x4000;        // A or X (the two feet); a list: A, X
    unsigned footplant_button = 0x1000 | 0x4000;   // grab + A or X
    unsigned nofootgrab_button = 0x2000;           // grab + B
    bool push_block_with_grab = true;

    // [manual_stick]
    Axis manual_axis = Axis::RY;
    Direction manual_direction = Direction::Both;
    int manual_min = 13000, manual_max = 30000;
    int manual_hysteresis = 1500;
    int manual_cross_max = 32767;   // the other axis of the same stick must stay within +-this
    Ranges manual_ignore;           // physics states in which a manual never counts (e.g. the crouch before a pop)

    // [land_velocity]
    LandVelocity land;

    // [rotation]
    Rotation rotation;

    // [cc_*]
    CcCfg cc[CcCount];
    IntensityCfg intensity;
    RotationCc rot_cc[3];
    ManualCc manual_cc;
    SpeedCc speed_cc;

    TriggerCfg t[TriggerCount];

    Config() {
        //                              jump land bail grind manual grab push footplant nofootgrab rot_x rot_y rot_z fingerflip
        const int notes[TriggerCount] = {36,  37,  39,  40,   41,    44,  38,  45,       46,        48,   49,   50,   51};
        for (int i = 0; i < TriggerCount; ++i) t[i].note = notes[i];
        t[Push].delay_ms = 500;
        t[Footplant].delay_ms = 200;       // the skater grabs the board a moment after the button
        t[NoFootGrab].delay_ms = 200;
        for (int i = 0; i < TriggerCount; ++i) t[i].gate = kind_of(i) == Kind::Gate;   // the default mode; every trigger can be switched
        t[Grind].off_delay_ms = 60;
        t[Manual].delay_ms = 200;       // so the crouch before an ollie (stick pulled back) is not a manual
        t[Manual].off_delay_ms = 100;
        t[Manual].min_hold_ms = 50;
        t[Grab].delay_ms = 200;
        t[Grab].off_delay_ms = 50;
        for (int i = RotX; i <= Fingerflip; ++i) t[i].off_delay_ms = 200;   // tail
        t[RotY].context = Context::GroundOrAir;   // a revert is a quick 180 about the vertical axis on the ground:
                                                  // it plays the same note as a shuvit

        // MIDI CC numbers 20-24 are free ones (no standard meaning)
        for (int i = 0; i < CcCount; ++i) cc[i].cc = 20 + i;
        cc[CcIntensity].smooth_ms = 150;
        cc[CcManual].smooth_ms = 30;
        rot_cc[1].context = Context::GroundOrAir;   // y also counts a revert
        const double ev[TriggerCount] = {0.10, 0.10, 0.0, 0.10, 0.05, 0.05, 0.04, 0.10, 0.10, 0.12, 0.12, 0.12, 0.12};
        for (int i = 0; i < TriggerCount; ++i) intensity.event[i] = ev[i];
    }
};

struct ParseResult {
    std::vector<std::string> errors, warnings;
    bool ok() const { return errors.empty(); }
};

// ---------------------------------------------------------------------------------------------
// parsing helpers

namespace detail {

inline std::string trim(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}
inline std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
inline bool to_int(const std::string& s, int& out) {
    if (s.empty()) return false;
    char* end = nullptr;
    const long v = std::strtol(s.c_str(), &end, 10);
    if (*end != 0 || v < -2000000000L || v > 2000000000L) return false;
    out = static_cast<int>(v);
    return true;
}
inline bool to_double(const std::string& s, double& out) {
    if (s.empty()) return false;
    char* end = nullptr;
    const double v = std::strtod(s.c_str(), &end);
    if (*end != 0 || !std::isfinite(v)) return false;
    out = v;
    return true;
}
inline bool to_bool(const std::string& s, bool& out) {
    const std::string v = lower(s);
    if (v == "true" || v == "yes" || v == "on" || v == "1") { out = true; return true; }
    if (v == "false" || v == "no" || v == "off" || v == "0") { out = false; return true; }
    return false;
}
// "100-199, 504". An empty value or "none" gives an empty list.
inline bool to_ranges(const std::string& s, Ranges& out, bool allow_empty) {
    Ranges result;
    const std::string v = lower(trim(s));
    if (v.empty() || v == "none") {
        if (!allow_empty) return false;
        out = result;
        return true;
    }
    std::stringstream ss(s);
    std::string part;
    while (std::getline(ss, part, ',')) {
        part = trim(part);
        if (part.empty()) continue;
        const auto dash = part.find('-');
        int a = 0, b = 0;
        if (dash == std::string::npos) {
            if (!to_int(part, a) || a < 0) return false;
            b = a;
        } else {
            if (!to_int(trim(part.substr(0, dash)), a) || !to_int(trim(part.substr(dash + 1)), b) || a < 0 || b < a) return false;
        }
        result.r.emplace_back(static_cast<std::uint32_t>(a), static_cast<std::uint32_t>(b));
    }
    if (result.r.empty() && !allow_empty) return false;
    out = result;
    return true;
}
inline bool to_button(const std::string& s, unsigned& out) {
    const std::string v = lower(s);
    for (const auto& b : button_names()) if (lower(b.name) == v) { out = b.mask; return true; }
    return false;
}
inline std::string button_str(unsigned mask) {
    for (const auto& b : button_names()) if (b.mask == mask) return b.name;
    return "A";
}
// A list of buttons: "A, X" (separators: comma, space, + or |). At least one.
inline bool to_buttons(const std::string& s, unsigned& out) {
    unsigned mask = 0;
    std::string token;
    auto flush = [&]() -> bool {
        if (token.empty()) return true;
        unsigned m = 0;
        if (!to_button(token, m)) return false;
        mask |= m;
        token.clear();
        return true;
    };
    for (char c : s) {
        if (c == ',' || c == ' ' || c == '+' || c == '|' || c == '\t') { if (!flush()) return false; }
        else token += c;
    }
    if (!flush() || !mask) return false;
    out = mask;
    return true;
}
inline std::string buttons_str(unsigned mask) {
    std::string out;
    for (const auto& b : button_names())
        if (mask & b.mask) { if (!out.empty()) out += ", "; out += b.name; }
    return out.empty() ? "A" : out;
}
inline const char* axis_str(Axis a) {
    switch (a) { case Axis::LX: return "LX"; case Axis::LY: return "LY"; case Axis::RX: return "RX"; default: return "RY"; }
}
inline const char* direction_str(Direction d) {
    switch (d) { case Direction::Up: return "up"; case Direction::Down: return "down"; default: return "both"; }
}
inline const char* context_str(Context c) {
    switch (c) { case Context::GroundOrAir: return "ground_or_air"; case Context::Any: return "any"; default: return "air"; }
}
inline bool to_axes(const std::string& s, int& mask) {
    int m = 0;
    for (char c : lower(s)) {
        if (c == 'x') m |= 1;
        else if (c == 'y') m |= 2;
        else if (c == 'z') m |= 4;
        else if (c == ',' || c == ' ' || c == '+') continue;
        else return false;
    }
    if (!m) return false;
    mask = m;
    return true;
}
inline std::string axes_str(int mask) {
    std::string s;
    if (mask & 1) s += "x";
    if (mask & 2) s += (s.empty() ? "y" : ", y");
    if (mask & 4) s += (s.empty() ? "z" : ", z");
    return s;
}
inline std::string num(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%g", v);
    return buf;
}

}  // namespace detail

// Parses INI text on top of `cfg` (start from a default Config so removed keys revert).
inline void parse_ini(const std::string& text, Config& cfg, ParseResult& res) {
    using namespace detail;
    std::string section;
    int section_trigger = -1, section_cc = -1;
    std::set<std::string> seen;          // "section|key": a key written twice is almost always a mistake
    std::stringstream in(text);
    std::string raw;
    int line_no = 0;

    auto err = [&](const std::string& m) { res.errors.push_back("line " + std::to_string(line_no) + ": " + m); };
    auto warn = [&](const std::string& m) { res.warnings.push_back("line " + std::to_string(line_no) + ": " + m); };

    while (std::getline(in, raw)) {
        ++line_no;
        // strip comments: ';' or '#' at the start of the line or after whitespace
        std::string line;
        for (std::size_t i = 0; i < raw.size(); ++i) {
            if ((raw[i] == ';' || raw[i] == '#') && (i == 0 || std::isspace(static_cast<unsigned char>(raw[i - 1])))) break;
            line += raw[i];
        }
        line = trim(line);
        if (line.empty()) continue;

        if (line.front() == '[') {
            if (line.back() != ']') { err("section header must end with ]"); continue; }
            section = lower(trim(line.substr(1, line.size() - 2)));
            section_trigger = -1;
            section_cc = -1;
            for (int t = 0; t < TriggerCount; ++t) if (section == section_name(t)) section_trigger = t;
            for (int i = 0; i < CcCount; ++i) if (section == cc_section(i)) section_cc = i;
            if (section_trigger < 0 && section_cc < 0 && section != "general" && section != "states" && section != "input" &&
                section != "manual_stick" && section != "land_velocity" && section != "rotation")
                warn("unknown section [" + section + "]");
            continue;
        }
        const auto eq = line.find('=');
        if (eq == std::string::npos) { err("expected key = value"); continue; }
        const std::string key = lower(trim(line.substr(0, eq)));
        const std::string val = trim(line.substr(eq + 1));
        if (section.empty()) { err("key outside of a section"); continue; }
        if (!seen.insert(section + "|" + key).second) {
            warn("'" + key + "' is written twice in [" + section + "]; the LAST one is used." +
                 (key.find("button") != std::string::npos ? " To allow several buttons put them on one line: " + key + " = A, X" : std::string()));
        }

        auto get_int = [&](int& dst, int lo, int hi) {
            int v;
            if (!to_int(val, v) || v < lo || v > hi) { err("'" + key + "' must be a number from " + std::to_string(lo) + " to " + std::to_string(hi)); return; }
            dst = v;
        };
        auto get_double = [&](double& dst, double lo, double hi) {
            double v;
            if (!to_double(val, v) || v < lo || v > hi) { err("'" + key + "' must be a number from " + num(lo) + " to " + num(hi)); return; }
            dst = v;
        };
        auto get_bool = [&](bool& dst) {
            bool v;
            if (!to_bool(val, v)) { err("'" + key + "' must be true or false"); return; }
            dst = v;
        };
        auto get_ranges = [&](Ranges& dst, bool allow_empty) {
            if (!to_ranges(val, dst, allow_empty)) err("'" + key + "' must be numbers or ranges like 100-199, 504");
        };
        auto get_buttons = [&](unsigned& dst) {
            if (!to_buttons(val, dst)) err("'" + key + "' must be one or more buttons separated by commas, for example A, X. Names: A B X Y LB RB LS RS START BACK DPAD_UP DPAD_DOWN DPAD_LEFT DPAD_RIGHT");
        };
        auto get_button = [&](unsigned& dst) {
            if (!to_button(val, dst)) err("'" + key + "' must be a button: A B X Y LB RB LS RS START BACK DPAD_UP DPAD_DOWN DPAD_LEFT DPAD_RIGHT");
        };

        bool known = true;
        if (section == "general") {
            if (key == "velocity") get_int(cfg.velocity, 1, 127);
            else if (key == "channel") get_int(cfg.channel, 1, 16);
            else if (key == "hold_ms") get_int(cfg.hold_ms, 1, 60000);
            else if (key == "midi_port") cfg.midi_port = val;
            else if (key == "process") cfg.process = val;
            else if (key == "poll_ms") get_int(cfg.poll_ms, 0, 100);
            else if (key == "pad") {
                if (lower(val) == "auto") cfg.pad = -1; else get_int(cfg.pad, 0, 3);
            } else if (key == "note_off") {
                const std::string v = lower(val);
                if (v == "noteoff") cfg.note_off_zero = false;
                else if (v == "velocity0") cfg.note_off_zero = true;
                else err("'note_off' must be noteoff or velocity0");
            } else if (key == "midi_check") get_bool(cfg.midi_check);
            else if (key == "speed_min") get_double(cfg.speed_min, 0, 1000);
            else if (key == "speed_max") get_double(cfg.speed_max, 0, 1000);
            else if (key == "speed_bonus") get_int(cfg.speed_bonus, 0, 126);
            else known = false;
        } else if (section == "states") {
            if (key == "ground") get_ranges(cfg.ground, false);
            else if (key == "air") get_ranges(cfg.air, false);
            else if (key == "grind") get_ranges(cfg.grind, false);
            else if (key == "bail") get_ranges(cfg.bail, false);
            else if (key == "bail_rearm_ms") get_int(cfg.bail_rearm_ms, 0, 600000);
            else known = false;
        } else if (section == "input") {
            if (key == "trigger_press") get_int(cfg.trigger_press, 1, 255);
            else if (key == "trigger_release") get_int(cfg.trigger_release, 0, 254);
            else if (key == "grab_lt") get_bool(cfg.grab_lt);
            else if (key == "grab_rt") get_bool(cfg.grab_rt);
            else if (key == "push_button") get_buttons(cfg.push_button);
            else if (key == "footplant_button") get_buttons(cfg.footplant_button);
            else if (key == "nofootgrab_button") get_buttons(cfg.nofootgrab_button);
            else if (key == "push_block_with_grab") get_bool(cfg.push_block_with_grab);
            else known = false;
        } else if (section == "manual_stick") {
            if (key == "axis") {
                const std::string v = lower(val);
                if (v == "lx") cfg.manual_axis = Axis::LX;
                else if (v == "ly") cfg.manual_axis = Axis::LY;
                else if (v == "rx") cfg.manual_axis = Axis::RX;
                else if (v == "ry") cfg.manual_axis = Axis::RY;
                else err("'axis' must be LX, LY, RX or RY");
            } else if (key == "direction") {
                const std::string v = lower(val);
                if (v == "both") cfg.manual_direction = Direction::Both;
                else if (v == "up") cfg.manual_direction = Direction::Up;
                else if (v == "down") cfg.manual_direction = Direction::Down;
                else err("'direction' must be both, up or down");
            } else if (key == "min") get_int(cfg.manual_min, 0, 32767);
            else if (key == "max") get_int(cfg.manual_max, 0, 32767);
            else if (key == "hysteresis") get_int(cfg.manual_hysteresis, 0, 32767);
            else if (key == "cross_axis_max") get_int(cfg.manual_cross_max, 0, 32767);
            else if (key == "ignore_states") get_ranges(cfg.manual_ignore, true);
            else known = false;
        } else if (section == "land_velocity") {
            LandVelocity& l = cfg.land;
            if (key == "enabled") get_bool(l.enabled);
            else if (key == "min_velocity") get_int(l.min_velocity, 1, 127);
            else if (key == "max_velocity") get_int(l.max_velocity, 1, 127);
            else if (key == "min_height") get_double(l.min_height, 0, 1000);
            else if (key == "max_height") get_double(l.max_height, 0, 1000);
            else if (key == "curve") get_double(l.curve, 0.05, 20);
            else known = false;
        } else if (section == "rotation") {
            Rotation& r = cfg.rotation;
            if (key == "detect") {
                const std::string v = lower(val);
                if (v == "rate") r.angle = false;
                else if (v == "angle") r.angle = true;
                else err("'detect' must be rate or angle");
            } else if (key == "step_deg") get_double(r.step_deg, 10, 3600);
            else if (key == "min_rate") get_int(r.min_rate, 1, 100000);
            else if (key == "ground_min_rate") get_int(r.ground_min_rate, 1, 100000);
            else if (key == "extra_states") get_ranges(r.extra_states, true);
            else if (key == "switch_button") {
                if (lower(val) == "none") r.switch_button = 0; else get_button(r.switch_button);
            }
            else if (key == "idle_reset_ms") get_int(r.idle_reset_ms, 0, 60000);
            else if (key == "hold_ms") get_int(r.hold_ms, 1, 60000);
            else if (key == "note_step") get_int(r.note_step, -48, 48);
            else known = false;
        } else if (section_cc >= 0) {
            CcCfg& k = cfg.cc[section_cc];
            if (key == "enabled") get_bool(k.enabled);
            else if (key == "cc") get_int(k.cc, 0, 127);
            else if (key == "channel") { if (lower(val) == "default") k.channel = -1; else get_int(k.channel, 1, 16); }
            else if (key == "smooth_ms") get_double(k.smooth_ms, 0, 10000);
            else if (key == "min_step") get_int(k.min_step, 1, 127);
            else if (key == "max_rate_hz") get_int(k.max_rate_hz, 1, 1000);
            else if (section_cc == CcIntensity) {
                IntensityCfg& n = cfg.intensity;
                bool event_key = false;
                for (int t = 0; t < TriggerCount; ++t)
                    if (key == section_name(t)) { get_double(n.event[t], -1, 1); event_key = true; }
                if (event_key) {}
                else if (key == "decay_s") get_double(n.decay_s, 0, 600);
                else if (key == "min_value") get_int(n.min_value, 0, 127);
                else if (key == "max_value") get_int(n.max_value, 0, 127);
                else if (key == "curve") get_double(n.curve, 0.05, 20);
                else if (key == "land_scaled") get_bool(n.land_scaled);
                else if (key == "grind_per_s") get_double(n.grind_per_s, -5, 5);
                else if (key == "manual_per_s") get_double(n.manual_per_s, -5, 5);
                else if (key == "grab_per_s") get_double(n.grab_per_s, -5, 5);
                else if (key == "speed_per_s") get_double(n.speed_per_s, -5, 5);
                else known = false;
            } else if (section_cc == CcManual) {
                ManualCc& m = cfg.manual_cc;
                if (key == "only_while_manual") get_bool(m.only_while_manual);
                else if (key == "mode") {
                    const std::string v = lower(val);
                    if (v == "magnitude") m.signed_mode = false;
                    else if (v == "signed") m.signed_mode = true;
                    else err("'mode' must be magnitude or signed");
                } else if (key == "idle") { if (lower(val) == "auto") m.idle = -1; else get_int(m.idle, 0, 127); }
                else known = false;
            } else if (section_cc == CcSpeed) {
                SpeedCc& sp = cfg.speed_cc;
                if (key == "min_speed") get_double(sp.min_speed, 0, 1000);
                else if (key == "max_speed") get_double(sp.max_speed, 0, 1000);
                else if (key == "curve") get_double(sp.curve, 0.05, 20);
                else if (key == "invert") get_bool(sp.invert);
                else known = false;
            } else {
                RotationCc& r = cfg.rot_cc[section_cc - CcRotX];
                if (key == "range_deg") get_double(r.range_deg, 10, 100000);
                else if (key == "min_rate") get_int(r.min_rate, 0, 100000);
                else if (key == "ground_min_rate") get_int(r.ground_min_rate, 0, 100000);
                else if (key == "context") {
                    const std::string v = lower(val);
                    if (v == "air") r.context = Context::Air;
                    else if (v == "ground_or_air") r.context = Context::GroundOrAir;
                    else if (v == "any") r.context = Context::Any;
                    else err("'context' must be air, ground_or_air or any");
                } else if (key == "invert") get_bool(r.invert);
                else if (key == "start") get_int(r.start, 0, 127);
                else if (key == "decay_per_s") get_double(r.decay_per_s, 0, 1000);
                else known = false;
            }
        } else if (section_trigger >= 0) {
            TriggerCfg& c = cfg.t[section_trigger];
            const bool rotation = is_rotation(section_trigger);
            if (key == "enabled") get_bool(c.enabled);
            else if (key == "note") get_int(c.note, 0, 127);
            else if (key == "velocity") { if (lower(val) == "default") c.velocity = -1; else get_int(c.velocity, 1, 127); }
            else if (key == "channel") { if (lower(val) == "default") c.channel = -1; else get_int(c.channel, 1, 16); }
            else if (key == "speed_bonus") { if (lower(val) == "default") c.speed_bonus = -1; else get_int(c.speed_bonus, 0, 126); }
            else if (key == "delay_ms") get_int(c.delay_ms, 0, 60000);
            else if (key == "hold_ms") {
                if (lower(val) == "default") c.hold_ms = -1; else get_int(c.hold_ms, 1, 60000);
            } else if (key == "off_delay_ms" || key == "tail_ms") {
                get_int(c.off_delay_ms, 0, 60000);
            } else if (key == "min_hold_ms") {
                get_int(c.min_hold_ms, 0, 60000);
            } else if (key == "mode") {
                const std::string v = lower(val);
                if (v == "pulse") c.gate = false;
                else if (v == "gate" || v == "held") c.gate = true;
                else err("'mode' must be pulse or gate");
            } else if (key == "threshold" || key == "release") {
                if (!rotation) warn("'" + key + "' is ignored for " + section);
                get_int(key == "threshold" ? c.threshold : c.release, 1, 100000);
            } else if (key == "context") {
                if (!rotation) warn("'context' is ignored for " + section);
                const std::string v = lower(val);
                if (v == "air") c.context = Context::Air;
                else if (v == "ground_or_air") c.context = Context::GroundOrAir;
                else if (v == "any") c.context = Context::Any;
                else err("'context' must be air, ground_or_air or any");
            } else if (key == "detect") {
                if (!rotation) warn("'detect' is ignored for " + section);
                const std::string v = lower(val);
                if (v == "default") c.detect = -1;
                else if (v == "rate") c.detect = 0;
                else if (v == "angle") c.detect = 1;
                else err("'detect' must be default, rate or angle");
            } else if (key == "step_deg") {
                if (!rotation) warn("'step_deg' is ignored for " + section);
                if (lower(val) == "default") c.step_deg = -1; else get_double(c.step_deg, 10, 3600);
            } else if (key == "note_step") {
                if (!rotation) warn("'note_step' is ignored for " + section);
                if (lower(val) == "default") c.note_step = kNoteStepDefault; else get_int(c.note_step, -48, 48);
            } else if (key == "axes") {
                if (section_trigger != Fingerflip) warn("'axes' is ignored for " + section + ": its axis is fixed");
                if (!to_axes(val, c.axes)) err("'axes' must be some of x, y, z (for example: x, y, z)");
            } else known = false;
        } else {
            continue;  // unknown section already warned about
        }
        if (!known) warn("unknown key '" + key + "' in [" + section + "] (typo?)");
    }

    // cross checks
    if (cfg.manual_min > cfg.manual_max) res.errors.push_back("[manual_stick] min must not be larger than max");
    if (cfg.trigger_release >= cfg.trigger_press) res.errors.push_back("[input] trigger_release must be below trigger_press");
    if (cfg.land.min_height >= cfg.land.max_height) res.errors.push_back("[land_velocity] min_height must be below max_height");
    if (cfg.land.min_velocity > cfg.land.max_velocity) res.errors.push_back("[land_velocity] min_velocity must not be above max_velocity");
    if (cfg.speed_min >= cfg.speed_max) res.errors.push_back("[general] speed_min must be below speed_max");
    for (int t = RotX; t <= Fingerflip; ++t)
        if (cfg.t[t].release >= cfg.t[t].threshold)
            res.errors.push_back(std::string("[") + section_name(t) + "] release must be below threshold");
    if (cfg.speed_cc.min_speed >= cfg.speed_cc.max_speed) res.errors.push_back("[cc_speed] min_speed must be below max_speed");
    if (cfg.intensity.min_value > cfg.intensity.max_value) res.errors.push_back("[cc_intensity] min_value must not be above max_value");
    for (int i = 0; i < CcCount; ++i)
        for (int j = i + 1; j < CcCount; ++j)
            if (cfg.cc[i].enabled && cfg.cc[j].enabled && cfg.cc[i].cc == cfg.cc[j].cc && cfg.cc[i].channel == cfg.cc[j].channel)
                res.warnings.push_back(std::string("[") + cc_section(i) + "] and [" + cc_section(j) + "] both send CC " +
                                       std::to_string(cfg.cc[i].cc) + " on the same channel: they would fight over it");
    if (!cfg.grab_lt && !cfg.grab_rt) res.warnings.push_back("[input] grab_lt and grab_rt are both off: the grab, footplant, nofootgrab and fingerflip triggers can never fire");
}

// ---------------------------------------------------------------------------------------------
// writing a commented default file

inline std::string write_ini(const Config& c) {
    using namespace detail;
    std::ostringstream o;
    auto b = [](bool v) { return v ? "true" : "false"; };
    auto opt = [](int v) { return v < 0 ? std::string("default") : std::to_string(v); };

    o << "; noiseflip.ini: every tunable value of noiseflip.exe.\n"
         "; Edit and save while the program runs: it reloads within about a second.\n"
         "; (midi_port, process, poll_ms and pad need a restart.) Lines starting with ; or # are comments.\n"
         "; A mistake in this file is reported in the console and the previous values stay active.\n\n";

    o << "[general]\n"
         "velocity = " << c.velocity << "        ; 1-127, used by every trigger unless it sets its own\n"
         "channel = " << c.channel << "          ; MIDI channel 1-16\n"
         "hold_ms = " << c.hold_ms << "       ; length of the pulse triggers\n"
         "midi_port = " << c.midi_port << "   ; the MIDI output port whose name contains this text\n"
         "process = " << c.process << "     ; game process name\n"
         "poll_ms = " << c.poll_ms << "          ; how often to read the game and the pad; 0 = as fast as possible (uses a full CPU core)\n"
         "pad = " << (c.pad < 0 ? std::string("auto") : std::to_string(c.pad)) << "           ; auto = merge every connected XInput pad, or 0-3 for one slot\n"
         "note_off = " << (c.note_off_zero ? "velocity0" : "noteoff") << "        ; noteoff = a real MIDI note-off message; velocity0 = a note-on with velocity 0 (some devices prefer it)\n"
         "midi_check = " << b(c.midi_check) << "      ; also listen to the MIDI port and warn when notes arrive that Noiseflip did not send\n"
         "speed_min = " << num(c.speed_min) << "        ; m/s. At or below this speed the speed bonus is 0 ...\n"
         "speed_max = " << num(c.speed_max) << "        ; ... at or above this speed it is the full bonus\n"
         "speed_bonus = " << c.speed_bonus << "         ; extra velocity (0-126) at top speed, for every trigger that does not set its own.\n"
         "                      ; Example: velocity = 80 and speed_bonus = 30 gives 80 when slow and 110 when fast. 0 = off\n\n";

    o << "[states]   ; physics_state numbers. Lists and ranges: 100-199, 504. Use  -v  to see the real numbers.\n"
         "ground = " << c.ground.str() << "       ; riding on the ground (103 = crouch before a pop)\n"
         "air = " << c.air.str() << "          ; in the air (200 = pop, 201 = known air)\n"
         "grind = " << c.grind.str() << "       ; grind and slide (the whole family counts as one trigger)\n"
         "bail = " << c.bail.str() << "           ; off the board / bail\n"
         "bail_rearm_ms = " << c.bail_rearm_ms << "   ; after a bail the skater must ride (ground or air) this long before another\n"
         "                      ; bail can fire. This stops a ragdoll that flickers in and out of the bail state\n"
         "                      ; from firing the bail note again.\n\n";

    o << "[input]    ; gamepad (XInput). Names: A B X Y LB RB LS RS START BACK DPAD_UP DPAD_DOWN DPAD_LEFT DPAD_RIGHT\n"
         "          ; A is the bottom button: Cross on a PlayStation pad.\n"
         "trigger_press = " << c.trigger_press << "     ; LT/RT value 0-255 that counts as pressed\n"
         "trigger_release = " << c.trigger_release << "   ; it counts as released again below this (stops flicker near the threshold)\n"
         "grab_lt = " << b(c.grab_lt) << "\n"
         "grab_rt = " << b(c.grab_rt) << "\n"
         "push_button = " << buttons_str(c.push_button) << "     ; one or more buttons, separated by commas: A, X = either foot pushes\n"
         "footplant_button = " << buttons_str(c.footplant_button) << "   ; grab + any of these buttons (a list, like push_button)\n"
         "nofootgrab_button = " << buttons_str(c.nofootgrab_button) << "  ; grab + any of these buttons (a list)\n"
         "push_block_with_grab = " << b(c.push_block_with_grab) << "   ; a button press that is part of grab + button is not a push\n\n";

    o << "[manual_stick]   ; the stick range for the manual trigger (manual and nose manual are ONE trigger)\n"
         "axis = " << axis_str(c.manual_axis) << "           ; LX, LY, RX or RY (raw XInput units, -32768 to 32767)\n"
         "direction = " << direction_str(c.manual_direction) << "    ; both = up (manual) and down (nose manual); or up / down only\n"
         "min = " << c.manual_min << "\n"
         "max = " << c.manual_max << "\n"
         "hysteresis = " << c.manual_hysteresis << "   ; once inside, it stays on until the value is this far outside the range\n"
         "cross_axis_max = " << c.manual_cross_max << "   ; the other axis of the stick must stay within +-this (32767 = no limit)\n"
         "ignore_states = " << c.manual_ignore.str() << "   ; physics states in which a manual never counts, e.g. 103 (the crouch before a pop). Empty = none\n"
         "                      ; (the manual trigger also waits [manual] delay_ms, 200 by default, which is what\n"
         "                      ; keeps the crouch before an ollie from counting)\n\n";

    o << "[land_velocity]   ; the landing note's velocity follows how far the skater fell (apex height - landing height)\n"
         "enabled = " << b(c.land.enabled) << "\n"
         "min_velocity = " << c.land.min_velocity << "     ; the quietest landing (for example off a curb)\n"
         "max_velocity = " << c.land.max_velocity << "    ; the hardest landing (a big drop)\n"
         "min_height = " << num(c.land.min_height) << "      ; metres: a drop this small or less gives min_velocity\n"
         "max_height = " << num(c.land.max_height) << "      ; metres: a drop this big or more gives max_velocity\n"
         "curve = " << num(c.land.curve) << "           ; 1 = straight line; below 1 small drops are louder, above 1 only big drops are loud\n"
         "                  ; (speed_bonus below is added on top of this)\n\n";

    o << "[rotation]   ; the two ways the rotation triggers (rot_x, rot_y, rot_z, fingerflip) can work. THE SWITCH IS  detect\n"
         "detect = " << (c.rotation.angle ? "angle" : "rate") << "         ; angle = one note for every step_deg degrees the board turns, separately for each axis\n"
         "                      ; rate  = one note when the spin rate passes the trigger's threshold (deg/s)\n"
         "switch_button = " << (c.rotation.switch_button ? button_str(c.rotation.switch_button) : std::string("none")) << "       ; press this button while playing to flip between angle and rate mode (LS = click the left stick). none = off\n"
         "step_deg = " << num(c.rotation.step_deg) << "         ; angle mode: degrees per note (180: a kickflip = 2 notes, a double flip = 4, a revert = 1)\n"
         "min_rate = " << c.rotation.min_rate << "         ; angle mode: slower than this (deg/s) is not a rotation (slow ollie pitch, drift)\n"
         "ground_min_rate = " << c.rotation.ground_min_rate << "  ; the same, but on the ground: only quick turns count there (a revert), not carving\n"
         "idle_reset_ms = " << c.rotation.idle_reset_ms << "    ; angle mode: after this long below min_rate the turned angle starts again from 0\n"
         "hold_ms = " << c.rotation.hold_ms << "         ; angle mode: length of each note; the next step cuts it short\n"
         "note_step = " << c.rotation.note_step << "          ; angle mode: semitones added per step inside one trick, so a double flip climbs: 0 = same note\n"
         "extra_states = " << c.rotation.extra_states.str() << "   ; physics states that count as on the ground for the rotation triggers only (e.g. a manual's own state). Empty = none\n"
         "                      ; (a trigger can override detect, step_deg and note_step in its own section)\n"
         "                      ; REVERTS: rot_y listens on the ground as well as in the air (context = ground_or_air), so a revert\n"
         "                      ; (a quick 180 about the vertical axis) plays the shuvit note. rot_x and rot_z stay air-only.\n\n";

    o << "; Per-trigger sections. Keys:\n"
         ";   enabled      true / false\n"
         ";   note         MIDI note 0-127  (Ableton names: 36 = C1, 38 = D1, 40 = E1, 42 = F#1 ...)\n"
         ";   velocity     1-127 or default\n"
         ";   speed_bonus  extra velocity at top speed, 0-126, or default ([general] speed_bonus)\n"
         ";   channel      1-16 or default\n"
         ";   delay_ms     pulse: wait this long after the event.  gate: the condition must hold this long first\n"
         ";   hold_ms      pulses: note length (default = [general] hold_ms)\n"
         ";   off_delay_ms gates: the condition must stay false this long before the note ends (debounce)\n"
         ";   min_hold_ms  gates: shortest note length\n"
         "; Rotation triggers (rot_x, rot_y, rot_z, fingerflip) also have:\n"
         ";   mode         pulse = one fixed note when the rotation starts; gate = held while it lasts\n"
         ";   threshold    degrees per second that starts it   (a trick is about 900-1200, an ollie much less)\n"
         ";   release      it counts as over when the rate falls below this\n"
         ";   tail_ms      it must stay below release this long before it can start again (pulse) or the note ends (gate)\n"
         ";   context      air / ground_or_air / any: where the rotation counts\n"
         ";   axes         fingerflip only: which axes count, for example x, y, z\n"
         ";   detect       default / rate / angle: this trigger's own choice (default = [rotation] detect)\n"
         ";   step_deg     angle mode: degrees per note, or default\n"
         ";   note_step    angle mode: semitones per step, or default\n\n";

    const char* what[TriggerCount] = {
        "ground -> air",
        "air -> ground (velocity from the drop, see [land_velocity])",
        "any state -> bail state (re-armed by riding, see bail_rearm_ms)",
        "held while the physics state is in the grind list",
        "held while on the ground and the stick is in the manual range, after delay_ms",
        "held while LT/RT is pressed (after delay_ms) on the ground or in the air, not in a manual or grind",
        "A pressed on the ground (not together with a grab); fires delay_ms later if still on the ground",
        "grab + footplant button on the ground or in the air, not in a manual or grind",
        "grab + nofootgrab button on the ground or in the air, not in a manual or grind",
        "board rotation about the world X axis (rate mode: deg/s; angle mode: a note per step_deg degrees)",
        "board rotation about the world Y axis, the vertical one: a shuvit in the air and a revert (or manual revert) on the ground",
        "board rotation about the world Z axis (a flip is mostly this)",
        "LT/RT held + the board rotating (on its own, not the skater) in the air"};
    for (int t = 0; t < TriggerCount; ++t) {
        const TriggerCfg& k = c.t[t];
        const Kind kind = kind_of(t);
        o << "[" << section_name(t) << "]   ; " << what[t] << "\n"
          << "enabled = " << b(k.enabled) << "\n"
          << "note = " << k.note << "\n"
          << "velocity = " << opt(k.velocity) << "\n"
          << "speed_bonus = " << opt(k.speed_bonus) << "\n"
          << "channel = " << opt(k.channel) << "\n"
          << "delay_ms = " << k.delay_ms << "\n";
        o << "mode = " << (k.gate ? "gate" : "pulse") << "   ; pulse = a fixed-length note when it starts; gate = held while it lasts\n";
        if (kind == Kind::Flexible) {
            o << "threshold = " << k.threshold << "\n"
              << "release = " << k.release << "\n"
              << "tail_ms = " << k.off_delay_ms << "\n"
              << "context = " << context_str(k.context) << "\n";
            if (t == Fingerflip) o << "axes = " << axes_str(k.axes) << "\n";
            o << "detect = " << (k.detect < 0 ? "default" : (k.detect == 0 ? "rate" : "angle")) << "\n"
              << "step_deg = " << (k.step_deg <= 0 ? std::string("default") : num(k.step_deg)) << "\n"
              << "note_step = " << (k.note_step == kNoteStepDefault ? std::string("default") : std::to_string(k.note_step)) << "\n";
        } else {
            o << "off_delay_ms = " << k.off_delay_ms << "\n";
        }
        o << "hold_ms = " << opt(k.hold_ms) << "\n"
          << "min_hold_ms = " << k.min_hold_ms << "\n";
        o << "\n";
    }

    // ---- the MIDI controllers ----
    auto cc_common = [&](int i) {
        const CcCfg& k = c.cc[i];
        o << "enabled = " << b(k.enabled) << "\n"
          << "cc = " << k.cc << "              ; controller number 0-127 (20-31 and 102-119 have no standard meaning)\n"
          << "channel = " << opt(k.channel) << "          ; 1-16 or default ([general] channel)\n"
          << "smooth_ms = " << num(k.smooth_ms) << "         ; smoothing (time constant); 0 = none. Higher = softer, slower\n"
          << "min_step = " << k.min_step << "           ; ignore moves smaller than this (a value that has stopped for 100 ms is still sent)\n"
          << "max_rate_hz = " << k.max_rate_hz << "       ; never more than this many messages per second\n";
    };
    o << "; ===== MIDI controllers (CC). Map them in Ableton with MIDI map. Each is a value 0-127. =====\n"
         "; --test-cc sweeps every controller from 0 to 127 and back, so you can MIDI-map them without playing.\n\n";

    o << "[cc_intensity]   ; ENERGY METER: every action adds energy, it falls back over time. More intense playing = a higher value\n";
    cc_common(CcIntensity);
    o << "decay_s = " << num(c.intensity.decay_s) << "           ; the energy falls to 37% in this many seconds; 0 = it never falls\n"
         "min_value = " << c.intensity.min_value << "         ; the value when there is no energy\n"
         "max_value = " << c.intensity.max_value << "       ; the value at full energy\n"
         "curve = " << num(c.intensity.curve) << "             ; 1 = straight; below 1 even a little energy gives a high value; above 1 only a lot does\n"
         "land_scaled = " << b(c.intensity.land_scaled) << "     ; a landing adds energy in proportion to how hard it was\n"
         "; energy (0 to 1) added by every note of a trigger. It adds a share of what is missing, so it never overflows.\n"
         "; A negative number takes energy away (for example bail = -0.3).\n";
    for (int t = 0; t < TriggerCount; ++t) o << section_name(t) << " = " << num(c.intensity.event[t]) << "\n";
    o << "; energy added every second while held, and while skating fast\n"
         "grind_per_s = " << num(c.intensity.grind_per_s) << "\n"
         "manual_per_s = " << num(c.intensity.manual_per_s) << "\n"
         "grab_per_s = " << num(c.intensity.grab_per_s) << "\n"
         "speed_per_s = " << num(c.intensity.speed_per_s) << "        ; at top speed ([general] speed_min .. speed_max); 0 = speed does not count\n\n";

    const char* rot_what[3] = {"x", "y (the vertical axis: shuvits and reverts)", "z (flips)"};
    for (int a = 0; a < 3; ++a) {
        const RotationCc& r = c.rot_cc[a];
        o << "[" << cc_section(CcRotX + a) << "]   ; board spin about " << rot_what[a]
          << ", kept as a fader: one way raises it, the other way lowers it, and it STAYS after landing\n";
        cc_common(CcRotX + a);
        o << "range_deg = " << num(r.range_deg) << "      ; this many degrees of net spin = the full 0 to 127 (3600 = ten full turns)\n"
             "min_rate = " << r.min_rate << "          ; spin slower than this (deg/s) is not counted (ollie pitch, drift)\n"
             "ground_min_rate = " << r.ground_min_rate << "   ; the same on the ground (only quick turns, like a revert)\n"
             "context = " << context_str(r.context) << "          ; air / ground_or_air / any: where the spin counts\n"
             "invert = " << b(r.invert) << "           ; true: the other direction raises it\n"
             "start = " << r.start << "              ; the value at the start\n"
             "decay_per_s = " << num(r.decay_per_s) << "        ; units per second drifting back to start; 0 = it stays where it is\n\n";
    }

    o << "[cc_manual]   ; the manual stick, scaled from [manual_stick] min..max to 0-127\n";
    cc_common(CcManual);
    o << "only_while_manual = " << b(c.manual_cc.only_while_manual) << "   ; true: only while the manual note sounds (so the crouch before an ollie does not move it)\n"
         "mode = " << (c.manual_cc.signed_mode ? "signed" : "magnitude") << "          ; magnitude = how far the stick is pushed (0-127); signed = manual up 64-127, nose manual down 0-63\n"
         "idle = " << (c.manual_cc.idle < 0 ? std::string("auto") : std::to_string(c.manual_cc.idle)) << "              ; the value when there is no manual: auto = 0 (64 in signed mode), or a number\n\n";

    o << "[cc_speed]   ; the board's horizontal speed, scaled from min_speed..max_speed (m/s) to 0-127\n";
    cc_common(CcSpeed);
    o << "min_speed = " << num(c.speed_cc.min_speed) << "         ; m/s: at or below this the value is 0\n"
         "max_speed = " << num(c.speed_cc.max_speed) << "         ; m/s: at or above this the value is 127\n"
         "curve = " << num(c.speed_cc.curve) << "             ; 1 = straight; below 1 slow speeds are already high; above 1 only fast ones are\n"
         "invert = " << b(c.speed_cc.invert) << "           ; true: faster = lower\n";
    return o.str();
}

}  // namespace skate
