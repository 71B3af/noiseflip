// midi_wire.h
//
// Exactly which bytes go to the MIDI port, kept separate from Windows so they can be tested, and an audit
// that tells "Noiseflip sent this" from "somebody else sent this to the same port".
//
//   note-on  : 0x90|channel, note, velocity
//   note-off : 0x80|channel, note, 0          (or, with note_off = velocity0: 0x90|channel, note, 0)
#pragma once
#include <cstdint>
#include <cstdio>
#include <deque>
#include <map>
#include <string>
#include <vector>

#include "trigger_engine.h"

namespace skate {

enum class NoteOffStyle { NoteOff, Velocity0 };

struct MidiMsg {
    std::uint8_t status = 0, d1 = 0, d2 = 0;
};

inline MidiMsg encode(const Action& a, NoteOffStyle style) {
    MidiMsg m;
    const std::uint8_t ch = static_cast<std::uint8_t>(a.channel & 15);
    m.d1 = static_cast<std::uint8_t>(a.note & 0x7f);
    if (a.on) {
        m.status = static_cast<std::uint8_t>(0x90 | ch);
        m.d2 = static_cast<std::uint8_t>(a.velocity & 0x7f);
    } else {
        m.status = static_cast<std::uint8_t>((style == NoteOffStyle::Velocity0 ? 0x90 : 0x80) | ch);
        m.d2 = 0;
    }
    return m;
}

// A control change: 0xB0|channel, controller number, value.
inline MidiMsg encode_cc(int channel, int cc, int value) {
    MidiMsg m;
    m.status = static_cast<std::uint8_t>(0xB0 | (channel & 15));
    m.d1 = static_cast<std::uint8_t>(cc & 0x7f);
    m.d2 = static_cast<std::uint8_t>(value < 0 ? 0 : (value > 127 ? 127 : value));
    return m;
}
inline bool is_cc(const MidiMsg& m) { return (m.status & 0xf0) == 0xB0; }

// The layout midiOutShortMsg wants: status in the lowest byte, then the two data bytes.
inline std::uint32_t pack(const MidiMsg& m) {
    return static_cast<std::uint32_t>(m.status) | (static_cast<std::uint32_t>(m.d1) << 8) | (static_cast<std::uint32_t>(m.d2) << 16);
}
inline MidiMsg unpack(std::uint32_t v) {
    MidiMsg m;
    m.status = static_cast<std::uint8_t>(v & 0xff);
    m.d1 = static_cast<std::uint8_t>((v >> 8) & 0xff);
    m.d2 = static_cast<std::uint8_t>((v >> 16) & 0xff);
    return m;
}

inline bool is_note_message(const MidiMsg& m) { const auto t = m.status & 0xf0; return t == 0x80 || t == 0x90; }
inline bool is_note_on(const MidiMsg& m) { return (m.status & 0xf0) == 0x90 && m.d2 > 0; }
inline bool is_note_off(const MidiMsg& m) { return (m.status & 0xf0) == 0x80 || ((m.status & 0xf0) == 0x90 && m.d2 == 0); }

inline std::string describe(const MidiMsg& m) {
    char buf[96];
    if (is_note_message(m))
        std::snprintf(buf, sizeof buf, "%02X %02X %02X  (channel %d, note %d %s%s)", m.status, m.d1, m.d2, (m.status & 15) + 1, m.d1,
                      is_note_on(m) ? "ON, velocity " : "OFF", is_note_on(m) ? std::to_string(m.d2).c_str() : "");
    else if (is_cc(m))
        std::snprintf(buf, sizeof buf, "%02X %02X %02X  (channel %d, controller %d = %d)", m.status, m.d1, m.d2, (m.status & 15) + 1, m.d1, m.d2);
    else
        std::snprintf(buf, sizeof buf, "%02X %02X %02X", m.status, m.d1, m.d2);
    return buf;
}

// Everything Noiseflip sends is remembered here before it is sent. If the MIDI port echoes it back
// (loopMIDI does), the echo is "ours". Anything else arriving on the port was sent by someone else:
// another copy of Noiseflip, an older program, or a MIDI tool.
class MidiAudit {
public:
    // Called BEFORE the message is sent, so the echo can never arrive first.
    void sent(const MidiMsg& m, std::int64_t now_us) {
        if (!is_note_message(m)) return;
        pending_[key(m)].push_back(now_us);
        ++sent_;
    }

    // Called for every message that arrives on the port. Returns true when it was one of ours.
    bool received(const MidiMsg& m, std::int64_t now_us) {
        expire(now_us);
        if (!is_note_message(m)) return true;       // clock, sensing, controllers: not interesting
        auto it = pending_.find(key(m));
        if (it != pending_.end() && !it->second.empty()) {
            it->second.pop_front();
            ++ours_;
            return true;
        }
        ++foreign_;
        return false;
    }

    unsigned long long sent_count() const { return sent_; }
    unsigned long long ours() const { return ours_; }          // sent by us and seen coming back
    unsigned long long foreign() const { return foreign_; }    // seen on the port but not sent by us
    // messages we sent that never came back within the window (the echo may be unavailable)
    unsigned long long unechoed(std::int64_t now_us) { expire(now_us); return lost_; }

private:
    // note-on with velocity 0 and note-off are the same thing; the velocity of a note-on is not compared
    static std::uint32_t key(const MidiMsg& m) {
        const std::uint32_t kind = is_note_on(m) ? 1u : 0u;
        return (kind << 16) | (static_cast<std::uint32_t>(m.status & 15) << 8) | m.d1;
    }
    void expire(std::int64_t now_us) {
        const std::int64_t window = 2000000;   // 2 s
        for (auto& kv : pending_)
            while (!kv.second.empty() && now_us - kv.second.front() > window) { kv.second.pop_front(); ++lost_; }
    }

    std::map<std::uint32_t, std::deque<std::int64_t>> pending_;
    unsigned long long sent_ = 0, ours_ = 0, foreign_ = 0, lost_ = 0;
};

}  // namespace skate
