// mapper.h
//
// Helpers for MIDI-mapping in Ableton without playing the game: send ONE note or ONE controller over and over, so you
// can click the target in Ableton, press Ctrl+M and let it learn.
//
//   noiseflip.exe --test-note37     one note, repeating            (also: --test-note 37, --test-note land)
//   noiseflip.exe --test-cc20       one controller, sweeping       (also: --test-cc 20, --test-cc intensity)
//   noiseflip.exe --map             interactive: type what to send next, no restart in between
//
// No platform code here, so it is unit tested.
#pragma once
#include <cctype>
#include <cstdint>
#include <string>
#include <vector>

#include "config.h"

namespace skate {

struct MapTarget {
    enum Kind { None, Note, Cc } kind = None;
    int number = 0;        // the note or the controller number
    int channel = 0;       // 0-15
    int velocity = 100;    // notes only
    std::string label;     // for the console
};

// Ableton's naming: note 60 is C3, so 36 is C1 and 37 is C#1.
inline std::string note_name(int note) {
    static const char* names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    if (note < 0 || note > 127) return "?";
    return std::string(names[note % 12]) + std::to_string(note / 12 - 2);
}

namespace mapdetail {
inline std::string lower(std::string s) { for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return s; }
inline std::string trim(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}
inline bool all_digits(const std::string& s) {
    if (s.empty() || s.size() > 4) return false;
    for (char c : s) if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    return true;
}
}  // namespace mapdetail

inline std::string note_choices() {
    std::string s;
    for (int t = 0; t < TriggerCount; ++t) { if (!s.empty()) s += ", "; s += section_name(t); }
    return s;
}
inline std::string cc_choices() {
    std::string s;
    for (int i = 0; i < CcCount; ++i) { if (!s.empty()) s += ", "; s += mapdetail::lower(cc_name(i)); }
    return s;
}

// A note, by number ("37", "note37") or by trigger name ("land"). A number that is one of your triggers' notes
// uses that trigger's channel and velocity.
inline bool resolve_note(const Config& cfg, const std::string& text_in, MapTarget& out, std::string& err) {
    using namespace mapdetail;
    std::string text = lower(trim(text_in));
    if (text.rfind("note", 0) == 0 && text.size() > 4) text = trim(text.substr(4));
    out = MapTarget();
    out.kind = MapTarget::Note;
    int trigger = -1;
    if (all_digits(text)) {
        const int n = std::stoi(text);
        if (n < 0 || n > 127) { err = "a note must be 0-127"; return false; }
        out.number = n;
        for (int t = 0; t < TriggerCount && trigger < 0; ++t) if (cfg.t[t].note == n) trigger = t;
    } else {
        for (int t = 0; t < TriggerCount; ++t)
            if (text == section_name(t) || text == lower(trigger_name(t))) trigger = t;
        if (trigger < 0) { err = "unknown note '" + text_in + "'. Use a number 0-127 or a trigger name: " + note_choices(); return false; }
        out.number = cfg.t[trigger].note;
    }
    out.channel = (trigger >= 0 && cfg.t[trigger].channel > 0 ? cfg.t[trigger].channel : cfg.channel) - 1;
    out.velocity = trigger >= 0 && cfg.t[trigger].velocity > 0 ? cfg.t[trigger].velocity : cfg.velocity;
    out.label = "note " + std::to_string(out.number) + " (" + note_name(out.number) + ")";
    if (trigger >= 0) out.label += ", the " + std::string(trigger_name(trigger)) + " note";
    return true;
}

// A controller, by number ("20", "cc20") or by name ("intensity", "rot_x", "manual").
inline bool resolve_cc(const Config& cfg, const std::string& text_in, MapTarget& out, std::string& err) {
    using namespace mapdetail;
    std::string text = lower(trim(text_in));
    if (text.rfind("cc", 0) == 0 && text.size() > 2 && text[2] != '_') text = trim(text.substr(2));
    out = MapTarget();
    out.kind = MapTarget::Cc;
    int which = -1;
    if (all_digits(text)) {
        const int n = std::stoi(text);
        if (n < 0 || n > 127) { err = "a controller number must be 0-127"; return false; }
        out.number = n;
        for (int i = 0; i < CcCount && which < 0; ++i) if (cfg.cc[i].cc == n) which = i;
    } else {
        for (int i = 0; i < CcCount; ++i)
            if (text == lower(cc_name(i)) || text == cc_section(i)) which = i;
        if (which < 0) { err = "unknown controller '" + text_in + "'. Use a number 0-127 or one of: " + cc_choices(); return false; }
        out.number = cfg.cc[which].cc;
    }
    out.channel = (which >= 0 && cfg.cc[which].channel > 0 ? cfg.cc[which].channel : cfg.channel) - 1;
    out.label = "CC " + std::to_string(out.number);
    if (which >= 0) out.label += ", the " + std::string(cc_name(which)) + " controller";
    return true;
}

// What to do with a line typed in --map mode.
struct MapCommand {
    enum Type { Select, Stop, Quit, List, Error } type = Error;
    MapTarget target;
    std::string message;
};

inline MapCommand parse_map_command(const Config& cfg, const std::string& line_in) {
    using namespace mapdetail;
    MapCommand cmd;
    const std::string line = lower(trim(line_in));
    if (line.empty() || line == "stop" || line == "s") { cmd.type = MapCommand::Stop; return cmd; }
    if (line == "q" || line == "quit" || line == "exit") { cmd.type = MapCommand::Quit; return cmd; }
    if (line == "l" || line == "list" || line == "?" || line == "h" || line == "help") { cmd.type = MapCommand::List; return cmd; }

    // "n 37", "note 37", "note37", "n37", "c 20", "cc 20", "cc20", "c20"
    char kind = 0;
    std::string rest;
    const auto space = line.find_first_of(" \t");
    if (space != std::string::npos) {
        const std::string first = line.substr(0, space);
        if (first == "n" || first == "note") { kind = 'n'; rest = trim(line.substr(space)); }
        else if (first == "c" || first == "cc") { kind = 'c'; rest = trim(line.substr(space)); }
    } else if (line.rfind("note", 0) == 0 && line.size() > 4) { kind = 'n'; rest = line.substr(4); }
    else if (line.rfind("cc", 0) == 0 && line.size() > 2 && line[2] != '_') { kind = 'c'; rest = line.substr(2); }
    else if ((line[0] == 'n' || line[0] == 'c') && all_digits(line.substr(1))) { kind = line[0]; rest = line.substr(1); }

    bool ok = false;
    std::string err;
    if (kind == 'n') ok = resolve_note(cfg, rest, cmd.target, err);
    else if (kind == 'c') ok = resolve_cc(cfg, rest, cmd.target, err);
    else {
        // a bare name: fine when it can only mean one thing
        MapTarget as_note, as_cc;
        std::string e1, e2;
        const bool n = resolve_note(cfg, line, as_note, e1), c = resolve_cc(cfg, line, as_cc, e2);
        if (n && c) { cmd.message = "'" + line_in + "' is both a note and a controller: type  n " + line + "  or  c " + line; return cmd; }
        if (n) { cmd.target = as_note; ok = true; }
        else if (c) { cmd.target = as_cc; ok = true; }
        else {
            cmd.message = "I did not understand '" + line_in + "'. Type  n 37  or  n land  for a note, c 20  or  c intensity  for a controller,"
                          "  list  to see them, Enter to stop, q to quit.";
            return cmd;
        }
    }
    if (!ok) { cmd.message = err; return cmd; }
    cmd.type = MapCommand::Select;
    return cmd;
}

// The value of --test-cc20 / --test-cc=20 / --test-cc 20 given the argument text. Returns true when `arg` starts with
// `flag` and has a value glued on (--test-cc20, --test-note=37). The plain flag (nothing glued on) returns false.
inline bool glued_value(const std::string& arg, const std::string& flag, std::string& value) {
    if (arg.size() <= flag.size() || arg.compare(0, flag.size(), flag) != 0) return false;
    std::string v = arg.substr(flag.size());
    if (!v.empty() && v[0] == '=') v.erase(0, 1);
    if (v.empty()) return false;
    value = v;
    return true;
}

// ---- the repeating patterns ----

// A slow triangle 0 -> 127 -> 0 so a DAW has time to learn it.
inline int cc_sweep_value(std::int64_t t_us, std::int64_t period_us = 3000000) {
    const std::int64_t p = t_us < 0 ? 0 : t_us % period_us;
    const double phase = static_cast<double>(p) / static_cast<double>(period_us);
    const double tri = phase < 0.5 ? phase * 2.0 : (1.0 - phase) * 2.0;
    return static_cast<int>(127.0 * tri + 0.5);
}

// A note that is on for the first part of every second.
inline bool note_pulse_on(std::int64_t t_us, std::int64_t period_us = 1000000, std::int64_t on_us = 400000) {
    return t_us >= 0 && (t_us % period_us) < on_us;
}

}  // namespace skate
