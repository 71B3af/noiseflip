// tests.cpp: runs on any machine (no Windows, no game).
//   g++ -std=c++17 -O1 -Wall -Wextra tests.cpp -o tests && ./tests
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "cc.h"
#include "config.h"
#include "http_lite.h"
#include "mapper.h"
#include "midi_wire.h"
#include "skate_chain.h"
#include "trigger_engine.h"

using namespace skate;

static int g_failed = 0, g_checks = 0;
#define CHECK(cond)                                                                      \
    do {                                                                                 \
        ++g_checks;                                                                      \
        if (!(cond)) { ++g_failed; std::printf("FAIL line %d: %s\n", __LINE__, #cond); } \
    } while (0)

// ---------------------------------------------------------------------------------------------
// a tiny simulator: steps the engine every millisecond with a constant input

constexpr unsigned BTN_A = 0x1000, BTN_B = 0x2000;

static Input S(std::uint32_t state) {
    Input i;
    i.game_ok = true;
    i.state = state;
    return i;
}
static Input SP(std::uint32_t state, int lt = 0, int rt = 0, int ry = 0, unsigned buttons = 0, int rx = 0) {
    Input i = S(state);
    i.pad.connected = true;
    i.pad.lt = lt; i.pad.rt = rt; i.pad.ry = ry; i.pad.rx = rx; i.pad.buttons = buttons;
    return i;
}
static Input NOGAME() { return Input{}; }

// The values these tests were written against, before the saved defaults changed (notes, velocity, delays, buttons).
static void legacy_defaults(Config& c) {
    const int notes[TriggerCount] = {36, 38, 40, 42, 44, 46, 48, 50, 52, 54, 56, 58, 60};
    for (int i = 0; i < TriggerCount; ++i) c.t[i].note = notes[i];
    c.velocity = 100;
    c.speed_bonus = 0;
    c.t[Push].delay_ms = 400;
    c.t[Footplant].delay_ms = 0;
    c.t[NoFootGrab].delay_ms = 0;
    c.push_button = 0x1000;
    c.footplant_button = 0x1000;
}

// The tests below use a config with no manual delay (the default is 200 ms; it has its own tests).
static Config test_config() {
    Config c;
    legacy_defaults(c);
    c.t[Manual].delay_ms = 0;
    c.rotation.angle = false;           // the older rotation tests are about the rate mode; angle is the real default
    return c;
}

struct Event { std::int64_t t_ms; Action a; };

struct Sim {
    TriggerEngine e;
    std::int64_t now_us = 0;
    std::vector<Event> log;
    explicit Sim(const Config& c = test_config()) : e(c) {}

    std::int64_t now_ms() const { return now_us / 1000; }
    void run(int ms, const Input& in) {
        for (int i = 0; i < ms; ++i) {
            for (const auto& a : e.update(now_us, in)) log.push_back({now_us / 1000, a});
            now_us += 1000;
        }
    }
    int ons(int t) const { int n = 0; for (const auto& x : log) if (x.a.on && x.a.trigger == t) ++n; return n; }
    int offs(int t) const { int n = 0; for (const auto& x : log) if (!x.a.on && x.a.trigger == t) ++n; return n; }
    std::int64_t on_time(int t, int nth = 0) const {
        for (const auto& x : log) if (x.a.on && x.a.trigger == t && nth-- == 0) return x.t_ms;
        return -1;
    }
    std::int64_t off_time(int t, int nth = 0) const {
        for (const auto& x : log) if (!x.a.on && x.a.trigger == t && nth-- == 0) return x.t_ms;
        return -1;
    }
    int total_ons() const { int n = 0; for (const auto& x : log) if (x.a.on) ++n; return n; }
};

// ---------------------------------------------------------------------------------------------
// the three original triggers

static void test_jump_land_bail_basics() {
    Sim s;
    s.run(1000, S(100));
    CHECK(s.total_ons() == 0);                         // the first sample only remembers
    s.run(1, S(201));                                  // 100 -> 201 at t = 1000
    CHECK(s.on_time(Jump) == 1000);
    s.run(400, S(201));
    CHECK(s.off_time(Jump) == 1300);                   // held exactly 300 ms, one off
    CHECK(s.offs(Jump) == 1);
    s.run(1, S(100));                                  // land
    CHECK(s.ons(Land) == 1);
    s.run(1000, S(100));
    s.run(1, S(504));                                  // bail
    CHECK(s.ons(Bail) == 1);
    s.run(1000, S(504));
    CHECK(s.ons(Bail) == 1 && s.offs(Bail) == 1);      // staying in 504 fires nothing more
    const auto& on = s.log.front().a;
    CHECK(on.note == 36 && on.velocity == 100 && on.channel == 0);
}

static void test_pop_states_one_note() {
    Sim s;
    s.run(5, S(100));
    s.run(5, S(103));            // crouch: still ground
    s.run(5, S(200));            // pop
    s.run(5, S(201));            // 200 -> 201: air -> air
    CHECK(s.ons(Jump) == 1);
}

static void test_flicker_is_one_signal() {
    Sim s;
    s.run(10, S(100));
    s.run(10, S(200));   // jump
    s.run(10, S(100));   // land
    s.run(10, S(200));   // jump again, 20 ms later: ignored
    s.run(10, S(100));   // land again: ignored
    s.run(10, S(201));   // ignored
    CHECK(s.ons(Jump) == 1 && s.ons(Land) == 1);
    CHECK(s.e.suppressed(Jump) == 2 && s.e.suppressed(Land) == 1);
    s.run(500, S(201));
    CHECK(s.offs(Jump) == 1 && s.offs(Land) == 1);
}

static void test_grind_is_not_jump_or_land() {
    Sim s;
    s.run(5, S(100));
    s.run(5, S(402));
    s.run(5, S(201));
    s.run(5, S(402));
    CHECK(s.ons(Jump) == 0 && s.ons(Land) == 0);
}

static void test_bail_rearm() {
    Sim s;
    s.run(10, S(201));
    s.run(1, S(504));                                   // bail #1
    CHECK(s.ons(Bail) == 1);
    s.run(400, S(504));
    s.run(100, S(100));                                 // back on the board for only 100 ms
    s.run(1, S(504));                                   // bail again: ignored (not re-armed)
    CHECK(s.ons(Bail) == 1 && s.e.suppressed(Bail) == 1);
    s.run(300, S(504));
    s.run(50, S(300));                                  // wipeout state is not riding
    s.run(10, S(504));
    CHECK(s.ons(Bail) == 1);
    s.run(100, S(100));
    s.run(700, S(201));                                 // ride (ground then air) for 800 ms: re-armed
    s.run(1, S(504));
    CHECK(s.ons(Bail) == 2);
}

static void test_reset_no_phantom() {
    Sim s;
    s.run(10, S(201));
    s.run(100, NOGAME());                               // level load
    s.run(10, S(100));                                  // first sample after a gap: not a landing
    CHECK(s.ons(Land) == 0);
    s.run(10, S(201));
    CHECK(s.ons(Jump) == 1);
}

// ---------------------------------------------------------------------------------------------
// grind

static void test_grind_gate() {
    Sim s;
    s.run(10, S(100));
    s.run(100, S(402));                                 // grind starts: note on at once
    CHECK(s.on_time(Grind) == 10);
    s.run(100, S(403));                                 // slide state inside the family: same note
    s.run(30, S(100));                                  // 30 ms hiccup, shorter than off_delay (60)
    s.run(100, S(402));
    CHECK(s.ons(Grind) == 1 && s.offs(Grind) == 0);     // one solid note, no retrigger
    const auto left = s.now_ms();
    s.run(200, S(201));                                 // leaves the grind for good
    CHECK(s.offs(Grind) == 1);
    CHECK(s.off_time(Grind) == left + 60);              // after the 60 ms off delay
    CHECK(s.ons(Jump) == 0);                            // grind -> air is not ground -> air
}

static void test_grind_off_delay_exact() {
    Sim s;
    s.run(5, S(402));
    const auto t0 = s.now_ms();
    s.run(200, S(201));
    CHECK(s.off_time(Grind) == t0 + 60);
}

// ---------------------------------------------------------------------------------------------
// manual

static void test_manual_gate_and_hysteresis() {
    Sim s;
    s.run(10, SP(100, 0, 0, 20000));                    // stick in range on the ground
    CHECK(s.ons(Manual) == 1 && s.on_time(Manual) == 0);
    s.run(200, SP(100, 0, 0, 12000));                   // below min (13000) but inside hysteresis (11500)
    s.run(200, SP(100, 0, 0, 31000));                   // above max (30000) but inside hysteresis (31500)
    CHECK(s.ons(Manual) == 1 && s.offs(Manual) == 0);
    s.run(200, SP(100, 0, 0, 32767));                   // clearly out of range
    CHECK(s.offs(Manual) == 1);
}

static void test_manual_entry_is_strict() {
    Sim s;
    s.run(200, SP(100, 0, 0, 31000));                   // never inside: no entry through the hysteresis
    s.run(200, SP(100, 0, 0, 12000));
    CHECK(s.ons(Manual) == 0);
}

static void test_manual_nose_manual_is_same_trigger() {
    Sim s;
    s.run(10, SP(100, 0, 0, 20000));                    // stick up: manual
    s.run(300, SP(100, 0, 0, 0));
    s.run(10, SP(100, 0, 0, -20000));                   // stick down: nose manual
    CHECK(s.ons(Manual) == 2);                          // two separate notes of the same trigger
    CHECK(s.offs(Manual) == 1);                         // the first ended, the second is still held
    CHECK(s.total_ons() == 2);                          // and nothing else fired
}

static void test_manual_needs_ground() {
    Sim s;
    s.run(10, SP(201, 0, 0, 20000));                    // in the air
    s.run(10, SP(504, 0, 0, 20000));                    // off the board
    CHECK(s.ons(Manual) == 0);
}

static void test_manual_off_delay_and_min_hold() {
    Sim s;
    s.run(10, SP(100, 0, 0, 20000));
    const auto t0 = s.now_ms();
    s.run(300, SP(100, 0, 0, 0));
    CHECK(s.off_time(Manual) == t0 + 100);              // off_delay 100
    Config c = test_config();
    c.t[Manual].off_delay_ms = 0;
    c.t[Manual].min_hold_ms = 80;
    Sim q(c);
    q.run(1, SP(100, 0, 0, 20000));
    q.run(200, SP(100, 0, 0, 0));
    CHECK(q.off_time(Manual) == 80);                    // shortest note length wins
}

static void test_manual_cross_axis_and_direction() {
    Config c = test_config();
    c.manual_direction = Direction::Up;
    Sim up(c);
    up.run(20, SP(100, 0, 0, -20000));
    CHECK(up.ons(Manual) == 0);
    up.run(20, SP(100, 0, 0, 20000));
    CHECK(up.ons(Manual) == 1);
    Config d = test_config();
    d.manual_cross_max = 8000;
    Sim x(d);
    x.run(20, SP(100, 0, 0, 20000, 0, 12000));          // stick pushed far sideways too
    CHECK(x.ons(Manual) == 0);
}

// ---------------------------------------------------------------------------------------------
// grab

static void test_grab_delay_and_hold() {
    Sim s;
    s.run(100, SP(201));
    s.run(500, SP(201, 255));                           // LT pressed at t = 100
    CHECK(s.on_time(Grab) == 300);                      // 200 ms later
    const auto t_release = s.now_ms();
    s.run(300, SP(201, 0));
    CHECK(s.off_time(Grab) == t_release + 50);          // off_delay 50, held until the button was released
    CHECK(s.ons(Grab) == 1);
}

static void test_grab_tap_shorter_than_delay_is_ignored() {
    Sim s;
    s.run(50, SP(201));
    s.run(100, SP(201, 255));
    s.run(300, SP(201, 0));
    CHECK(s.ons(Grab) == 0);
}

static void test_grab_works_on_ground_and_with_rt() {
    Sim s;
    s.run(10, SP(100));
    s.run(400, SP(100, 0, 255));
    CHECK(s.ons(Grab) == 1);
}

static void test_grab_trigger_hysteresis() {
    Sim s;
    s.run(10, SP(201));
    s.run(300, SP(201, 100));                           // exactly the press value
    CHECK(s.ons(Grab) == 1);
    s.run(300, SP(201, 80));                            // between release (60) and press (100): still held
    CHECK(s.offs(Grab) == 0);
    s.run(300, SP(201, 55));                            // below release: let go
    CHECK(s.offs(Grab) == 1);
    Sim q;
    q.run(10, SP(201));
    q.run(400, SP(201, 99));                            // never reaches the press value
    CHECK(q.ons(Grab) == 0);
}

static void test_no_grab_in_manual_or_grind() {
    Sim s;
    s.run(10, SP(100, 255, 0, 20000));                  // LT held while in a manual
    s.run(500, SP(100, 255, 0, 20000));
    CHECK(s.ons(Grab) == 0 && s.ons(Manual) == 1);
    Sim g;
    g.run(10, SP(402, 255));
    g.run(500, SP(402, 255));                           // LT held while grinding
    CHECK(g.ons(Grab) == 0 && g.ons(Grind) == 1);
}

static void test_grab_ends_when_manual_or_grind_starts() {
    Sim s;
    s.run(10, SP(201));
    s.run(300, SP(201, 255));
    CHECK(s.ons(Grab) == 1);
    const auto t = s.now_ms();
    s.run(300, SP(402, 255));                           // lands on a rail while still holding LT
    CHECK(s.off_time(Grab) == t + 50);
    CHECK(s.ons(Grind) == 1);
}

// ---------------------------------------------------------------------------------------------
// push, footplant, no-foot grab

static void test_push_delay_and_no_repeat() {
    Sim s;
    s.run(5, SP(100));
    const auto t0 = s.now_ms();
    s.run(1000, SP(100, 0, 0, 0, BTN_A));               // A pressed and held
    CHECK(s.on_time(Push) == t0 + 400);
    CHECK(s.ons(Push) == 1);                            // holding does not repeat
    CHECK(s.off_time(Push) == t0 + 700);
}

static void test_push_cancelled_if_not_on_ground() {
    Sim s;
    s.run(5, SP(100));
    s.run(200, SP(100, 0, 0, 0, BTN_A));
    s.run(300, SP(201, 0, 0, 0, BTN_A));                // left the ground before the delay ran out
    CHECK(s.ons(Push) == 0 && s.e.cancelled(Push) == 1);
}

static void test_push_not_in_air_or_without_pad() {
    Sim s;
    s.run(5, SP(201));
    s.run(600, SP(201, 0, 0, 0, BTN_A));
    CHECK(s.ons(Push) == 0);
    Sim q;
    Input in = S(100);
    in.pad.connected = false;
    in.pad.buttons = BTN_A;                             // stale data from a pad that is not there
    q.run(600, in);
    CHECK(q.ons(Push) == 0);
}

static void test_footplant_and_nofootgrab() {
    Sim s;
    s.run(10, SP(201));
    s.run(100, SP(201, 255));                           // grab held
    s.run(1, SP(201, 255, 0, 0, BTN_A));                // + A
    CHECK(s.ons(Footplant) == 1 && s.ons(Push) == 0);
    const auto t = s.on_time(Footplant);
    s.run(500, SP(201, 255, 0, 0, BTN_A));
    CHECK(s.off_time(Footplant) == t + 300);
    s.run(100, SP(201, 255));                           // release A
    s.run(1, SP(201, 255, 0, 0, BTN_B));                // + B
    CHECK(s.ons(NoFootGrab) == 1 && s.ons(Footplant) == 1);
}

static void test_footplant_order_does_not_matter() {
    Sim s;
    s.run(10, SP(201));
    s.run(50, SP(201, 0, 0, 0, BTN_A));                 // A first
    s.run(1, SP(201, 0, 255, 0, BTN_A));                // then RT
    CHECK(s.ons(Footplant) == 1);
}

static void test_footplant_not_in_manual_or_grind() {
    Sim s;
    s.run(10, SP(402));
    s.run(100, SP(402, 255, 0, 0, BTN_A));
    CHECK(s.ons(Footplant) == 0);
    Sim m;
    m.run(10, SP(100, 255, 0, 20000));
    m.run(100, SP(100, 255, 0, 20000, BTN_B));
    CHECK(m.ons(NoFootGrab) == 0);
}

static void test_push_then_grab_becomes_footplant_not_push() {
    Sim s;
    s.run(5, SP(100));
    s.run(100, SP(100, 0, 0, 0, BTN_A));                // A: a push is pending
    s.run(1, SP(100, 255, 0, 0, BTN_A));                // LT joins: grab + A
    CHECK(s.ons(Footplant) == 1);
    s.run(600, SP(100, 255, 0, 0, BTN_A));
    CHECK(s.ons(Push) == 0 && s.e.cancelled(Push) == 1);
}

// ---------------------------------------------------------------------------------------------
// engine behaviour

static void test_game_lost_ends_gates_and_cancels_push() {
    Sim s;
    s.run(10, SP(402));
    s.run(300, NOGAME());                               // loading: no game data, pad irrelevant
    CHECK(s.offs(Grind) == 1);
    Sim q;
    q.run(5, SP(100));
    q.run(100, SP(100, 0, 0, 0, BTN_A));
    q.run(600, NOGAME());
    CHECK(q.ons(Push) == 0);
}

static void test_disabled_trigger_and_overrides() {
    Config c = test_config();
    c.t[Jump].enabled = false;
    c.t[Land].velocity = 64;
    c.t[Land].channel = 10;
    c.t[Land].hold_ms = 120;
    Sim s(c);
    s.run(5, S(100));
    s.run(5, S(201));
    s.run(5, S(100));
    CHECK(s.ons(Jump) == 0 && s.ons(Land) == 1);
    const auto& on = [&]() -> const Action& { for (const auto& x : s.log) if (x.a.on) return x.a; return s.log[0].a; }();
    CHECK(on.velocity == 64 && on.channel == 9 && on.note == 38);
    s.run(200, S(100));
    CHECK(s.off_time(Land) - s.on_time(Land) == 120);
}

static void test_config_change_mid_note_keeps_note_off_correct() {
    Sim s;
    s.run(5, S(100));
    s.run(5, S(201));                                   // jump sounding with note 36
    Config c;
    c.t[Jump].note = 60;
    s.e.set_config(c);                                  // user edits the file now
    s.run(400, S(201));
    CHECK(s.log.size() >= 2 && !s.log[1].a.on && s.log[1].a.note == 36);
    s.run(5, S(100));
    s.run(5, S(201));
    CHECK(s.log.back().a.on && s.log.back().a.note == 60);
}

static void test_release_all() {
    Sim s;
    s.run(5, SP(402, 255));
    s.run(300, SP(402, 255));
    auto off = s.e.release_all();
    CHECK(!off.empty());
    for (const auto& a : off) CHECK(!a.on);
    CHECK(s.e.release_all().empty());
}

// ---------------------------------------------------------------------------------------------
// config file

static void test_ini_round_trip() {
    Config d;
    const std::string text = write_ini(d);
    Config parsed;
    ParseResult r;
    parse_ini(text, parsed, r);
    CHECK(r.ok());
    CHECK(r.warnings.empty());
    CHECK(write_ini(parsed) == text);
    CHECK(parsed.manual_min == 13000 && parsed.manual_max == 30000);
    CHECK(parsed.t[Grab].delay_ms == 200 && parsed.t[Push].delay_ms == 500);
    CHECK(parsed.grind.contains(450) && !parsed.grind.contains(500));
}

static void test_ini_overrides_and_errors() {
    Config c;
    ParseResult r;
    parse_ini(
        "[general]\n"
        "velocity = 90   ; louder is not always better\n"
        "[states]\n"
        "ground = 100-199, 650   # extra\n"
        "grind = 400-499, 610-612\n"
        "[manual_stick]\n"
        "min = 14000\n"
        "[grab]\n"
        "delay_ms = 250\n"
        "velocity = default\n"
        "[footplant]\n"
        "note = 61\n"
        "unknown_key = 1\n",
        c, r);
    CHECK(r.ok());
    CHECK(c.velocity == 90 && c.manual_min == 14000 && c.t[Grab].delay_ms == 250 && c.t[Footplant].note == 61);
    CHECK(c.ground.contains(650) && c.grind.contains(611) && !c.grind.contains(613));
    CHECK(r.warnings.size() == 1);

    Config d;
    ParseResult e;
    parse_ini("[general]\nvelocity = 300\n[manual_stick]\nmin = 20000\nmax = 10000\n[jump]\nnote = abc\n[push]\nbutton = A\n", d, e);
    CHECK(!e.ok());
    CHECK(e.errors.size() == 3);          // velocity, note, min > max
    CHECK(e.warnings.size() == 1);        // unknown key 'button'

    Config f;
    ParseResult g;
    parse_ini("[input]\ntrigger_press = 50\ntrigger_release = 80\n", f, g);
    CHECK(!g.ok());

    Config h;
    ParseResult k;
    parse_ini("[jump]\nhold_ms = 100\n[grind]\nhold_ms = 100\n", h, k);
    CHECK(k.ok() && k.warnings.empty());   // every trigger may be a pulse or a gate now: all keys are accepted
}

static void test_ini_buttons_and_axis() {
    Config c;
    ParseResult r;
    parse_ini("[input]\npush_button = x\nfootplant_button = RB\n[manual_stick]\naxis = LY\ndirection = down\n", c, r);
    CHECK(r.ok());
    CHECK(c.push_button == 0x4000 && c.footplant_button == 0x0200);
    CHECK(c.manual_axis == Axis::LY && c.manual_direction == Direction::Down);
    c.t[Manual].delay_ms = 0;
    Sim s(c);
    Input in = SP(100);
    in.pad.ly = -20000;                                  // down on the left stick
    s.run(20, in);
    CHECK(s.ons(Manual) == 1);
}

// ---------------------------------------------------------------------------------------------
// Memory chain, against a synthetic game built from the same offsets

struct FakeMemory : Memory {
    std::map<std::uintptr_t, std::uint8_t> bytes;
    bool read(std::uintptr_t address, void* out, std::size_t size) override {
        auto* p = static_cast<std::uint8_t*>(out);
        for (std::size_t i = 0; i < size; ++i) {
            auto it = bytes.find(address + i);
            if (it == bytes.end()) return false;
            p[i] = it->second;
        }
        return true;
    }
    template <class T> void put(std::uintptr_t address, T v) {
        std::uint8_t raw[sizeof(T)];
        std::memcpy(raw, &v, sizeof(T));
        for (std::size_t i = 0; i < sizeof(T); ++i) bytes[address + i] = raw[i];
    }
};

struct World {
    static constexpr std::uintptr_t base = 0x140000000ULL;
    static constexpr std::uintptr_t client = 0x200000000ULL, context = 0x200100000ULL, manager = 0x200200000ULL,
                                    list = 0x200300000ULL, player = 0x200400000ULL, entity = 0x200500000ULL,
                                    component = 0x200600000ULL, core = 0x200700000ULL, physics = 0x200800000ULL,
                                    selector = 0x200900000ULL, collection = 0x200a00000ULL, holder = 0x200b00000ULL,
                                    board = 0x200c00000ULL, parts = 0x200d00000ULL;
    static constexpr std::uintptr_t manager_offset = 0x1c0;
    FakeMemory m;

    World() {
        m.put<std::uint32_t>(base + rva::context_player_manager_offset, static_cast<std::uint32_t>(manager_offset));
        m.put<std::uintptr_t>(client, base + rva::client_vtable);
        m.put<std::uint32_t>(client + off::client_state, 13);
        m.put<std::uintptr_t>(client + off::client_context, context);
        m.put<std::uintptr_t>(context + manager_offset, manager);
        m.put<std::uintptr_t>(manager, base + rva::local_player_manager_vtable);
        m.put<std::uintptr_t>(manager + off::manager_begin, list);
        m.put<std::uintptr_t>(manager + off::manager_end, list + 8);
        m.put<std::uintptr_t>(list, player);
        m.put<std::uintptr_t>(player, base + rva::local_player_vtable);
        m.put<std::uintptr_t>(player + off::player_context, context);
        m.put<std::uint8_t>(player + off::player_local, 1);
        m.put<std::uint8_t>(player + off::player_remote, 0);
        m.put<std::uintptr_t>(player + off::player_entity, entity);
        m.put<std::uintptr_t>(entity, base + rva::skater_entity_vtable);
        m.put<std::uintptr_t>(entity + off::entity_player, player);
        m.put<std::uintptr_t>(entity + off::entity_context, context);
        m.put<std::uint8_t>(entity + off::entity_teleport, 0);
        m.put<std::uintptr_t>(entity + off::entity_component, component);
        m.put<std::uintptr_t>(component, base + rva::skater_component_vtable);
        m.put<std::uintptr_t>(component + off::component_core, core);
        m.put<std::uintptr_t>(core, base + rva::bail_core_vtable);
        m.put<std::uintptr_t>(core + off::core_context, physics);
        m.put<std::uintptr_t>(core + off::core_selector, selector);
        m.put<std::uintptr_t>(selector + off::selector_context, physics);
        m.put<std::uint32_t>(physics + off::context_state, 100);
        // position: collection -> entity, 4 matrices, first = 1, extra = 1 -> matrix at +0x10 + 3 * 0x20
        m.put<std::uintptr_t>(entity + off::entity_transform, collection);
        m.put<std::uintptr_t>(collection, entity);
        m.put<std::uint8_t>(collection + 8, 4);
        m.put<std::uint8_t>(collection + 9, 1);
        m.put<std::uint8_t>(collection + 10, 1);
        set_position(10.0f, 2.5f, -4.0f);
        // board: core -> holder -> board physics -> bodies
        m.put<std::uintptr_t>(core + off::core_board_holder, holder);
        m.put<std::uintptr_t>(holder + off::holder_board, board);
        m.put<std::uintptr_t>(board, base + rva::board_physics_vtable);
        m.put<std::uintptr_t>(board + off::physics_bodies, parts);
        m.put<std::uint32_t>(parts, off::board_body_count);
        m.put<std::uintptr_t>(parts + off::body_owner, board);
        set_motion(1.0f, 0.0f, 3.0f, 0.5f, 16.0f, -2.0f);
    }
    static std::uintptr_t matrix() { return collection + off::matrix_base + (1 + 2 * 1) * 0x20; }
    void set_position(float x, float y, float z) {
        m.put<float>(matrix() + off::matrix_position, x);
        m.put<float>(matrix() + off::matrix_position + 4, y);
        m.put<float>(matrix() + off::matrix_position + 8, z);
        m.put<float>(matrix() + off::matrix_position + 12, 1e30f);   // the fourth lane is not a coordinate
    }
    void set_motion(float vx, float vy, float vz, float wx, float wy, float wz) {
        const float v[3] = {vx, vy, vz}, w[3] = {wx, wy, wz};
        for (int i = 0; i < 3; ++i) {
            m.put<float>(parts + off::body_velocity + 4 * i, v[i]);
            m.put<float>(parts + off::body_spin + 4 * i, w[i]);
        }
    }
    Resolved run() { return resolve(m, base, client); }
};

static void test_chain_ok() {
    World w;
    auto r = w.run();
    CHECK(r.result == Result::Ok && r.state == 100 && r.physics_context == World::physics);
    w.m.put<std::uint32_t>(World::physics + off::context_state, 504);
    CHECK(w.run().state == 504);
    w.m.put<std::uint32_t>(World::client + off::client_state, 21);
    CHECK(w.run().result == Result::Ok);
}

static void test_chain_rejections() {
    { World w; w.m.put<std::uintptr_t>(World::client, 0x1234);                      CHECK(w.run().result == Result::NoClient); }
    { World w; w.m.put<std::uint32_t>(World::client + off::client_state, 5);        CHECK(w.run().result == Result::NotInLevel); }
    { World w; w.m.put<std::uint8_t>(World::entity + off::entity_teleport, 1);      CHECK(w.run().result == Result::Teleporting); }
    { World w; w.m.put<std::uint8_t>(World::player + off::player_remote, 1);        CHECK(w.run().result == Result::ChainBroken); }
    { World w; w.m.put<std::uint8_t>(World::player + off::player_local, 0);         CHECK(w.run().result == Result::ChainBroken); }
    { World w; w.m.put<std::uintptr_t>(World::manager + off::manager_end, World::list + 16); CHECK(w.run().result == Result::ChainBroken); }
    { World w; w.m.put<std::uintptr_t>(World::entity + off::entity_player, 0xdead); CHECK(w.run().result == Result::ChainBroken); }
    { World w; w.m.put<std::uintptr_t>(World::core, 0xdead);                        CHECK(w.run().result == Result::ChainBroken); }
    { World w; w.m.put<std::uintptr_t>(World::selector + off::selector_context, 0xdead); CHECK(w.run().result == Result::ChainBroken); }
    { World w; w.m.bytes.erase(World::physics + off::context_state);                CHECK(w.run().result == Result::ChainBroken); }
}

static void test_end_to_end_session() {
    // a whole session through the memory chain and the engine, as the program does it
    World w;
    Sim s;
    for (std::uint32_t st : {100u, 100u, 103u, 200u, 201u, 201u, 100u, 100u, 402u, 402u, 201u, 100u, 504u, 504u}) {
        w.m.put<std::uint32_t>(World::physics + off::context_state, st);
        auto r = w.run();
        CHECK(r.result == Result::Ok);
        s.run(250, S(r.state));
    }
    CHECK(s.ons(Jump) == 1 && s.ons(Land) == 2 && s.ons(Grind) == 1 && s.ons(Bail) == 1);
}


// ---------------------------------------------------------------------------------------------
// motion: input helper with the board's motion

// y in metres, speed in m/s, spin in DEGREES per second about world x, y, z
static Input M(std::uint32_t state, double y, double speed = 0, double wx = 0, double wy = 0, double wz = 0) {
    Input i = S(state);
    i.motion.ok = true;
    i.motion.y = y;
    i.motion.speed = speed;
    i.motion.w_dps[0] = wx; i.motion.w_dps[1] = wy; i.motion.w_dps[2] = wz;
    return i;
}
static Input MP(Input i, int lt = 0, int rt = 0) {
    i.pad.connected = true;
    i.pad.lt = lt; i.pad.rt = rt;
    return i;
}
static int on_velocity(const Sim& s, int trigger, int nth = 0) {
    for (const auto& x : s.log) if (x.a.on && x.a.trigger == trigger && nth-- == 0) return x.a.velocity;
    return -1;
}
static double on_measure(const Sim& s, int trigger) {
    for (const auto& x : s.log) if (x.a.on && x.a.trigger == trigger) return x.a.measure;
    return -2;
}

// a flight that peaks at `apex` above where it started and lands `landing` above the start
static void flight(Sim& s, double apex, double landing, double speed = 0) {
    s.run(20, M(100, 0, speed));
    s.run(100, M(201, apex * 0.5, speed));
    s.run(100, M(201, apex, speed));
    s.run(100, M(201, (apex + landing) / 2, speed));
    s.run(1, M(100, landing, speed));
}

// ---------------------------------------------------------------------------------------------
// manual delay and ignored states

static void test_manual_default_delay_ignores_the_crouch() {
    Config c;                                           // the real defaults: manual delay 200 ms
    CHECK(c.t[Manual].delay_ms == 200);
    Sim s(c);
    s.run(10, SP(100));
    s.run(150, SP(103, 0, 0, 20000));                   // crouch before an ollie: stick pulled back for 150 ms
    s.run(300, SP(100));
    CHECK(s.ons(Manual) == 0);
    Sim q(c);
    q.run(10, SP(100));
    const auto t0 = q.now_ms();
    q.run(600, SP(100, 0, 0, 20000));                   // a real manual: held
    CHECK(q.on_time(Manual) == t0 + 200);
}

static void test_manual_ignore_states() {
    Config c;
    c.manual_ignore.r = {{103, 103}};
    Sim s(c);
    s.run(10, SP(100));
    s.run(1000, SP(103, 0, 0, 20000));                  // however long, state 103 never counts
    CHECK(s.ons(Manual) == 0);
    s.run(400, SP(100, 0, 0, 20000));
    CHECK(s.ons(Manual) == 1);
}

// ---------------------------------------------------------------------------------------------
// landing velocity from the drop height, and the speed bonus

static void test_land_velocity_follows_the_drop() {
    Sim curb;                                           // rolls off a curb 15 cm high
    curb.run(20, M(100, 0));
    curb.run(100, M(201, 0));
    curb.run(100, M(201, -0.10));
    curb.run(1, M(100, -0.15));
    CHECK(curb.ons(Land) == 1);
    CHECK(on_velocity(curb, Land) >= 5 && on_velocity(curb, Land) <= 12);       // quiet, not silent
    CHECK(std::abs(on_measure(curb, Land) - 0.15) < 1e-9);

    Sim big;                                            // a 3 m drop from the top of the flight
    flight(big, 3.0, 0.0);
    CHECK(on_velocity(big, Land) == 110);

    Sim mid;
    flight(mid, 1.55, 0.0);                             // half way between 0.1 and 3.0
    CHECK(on_velocity(mid, Land) >= 58 && on_velocity(mid, Land) <= 60);

    Sim bigger;                                         // beyond max_height stays at max
    flight(bigger, 10.0, 0.0);
    CHECK(on_velocity(bigger, Land) == 110);
}

static void test_land_velocity_uses_apex_minus_landing_not_takeoff() {
    Sim s;                                              // jumps up a 1 m ledge: lands higher than it took off
    flight(s, 1.2, 1.0);                                // apex 1.2, lands at 1.0: the fall is only 0.2
    CHECK(std::abs(on_measure(s, Land) - 0.2) < 1e-9);
    CHECK(on_velocity(s, Land) < 20);
    Sim z;                                              // lands higher than the apex: no negative drop
    flight(z, 0.5, 0.8);
    CHECK(on_measure(z, Land) == 0.0);
    CHECK(on_velocity(z, Land) == 8);
}

static void test_land_velocity_curve_and_off_switch() {
    Config c = test_config();
    c.land.curve = 0.5;                                 // small drops get louder
    Sim s(c);
    flight(s, 1.55, 0.0);
    CHECK(on_velocity(s, Land) > 75);
    Config d = test_config();
    d.land.enabled = false;
    d.velocity = 77;
    Sim q(d);
    flight(q, 3.0, 0.0);
    CHECK(on_velocity(q, Land) == 77);                  // plain velocity again
    Config e = test_config();
    e.t[Land].enabled = true;
    Sim n(e);                                           // no motion data at all: fall back to the base velocity
    n.run(20, S(201));
    n.run(1, S(100));
    CHECK(on_velocity(n, Land) == 100);
}

static void test_speed_bonus() {
    Config c = test_config();
    c.velocity = 80;
    c.speed_bonus = 30;
    c.speed_min = 1.0;
    c.speed_max = 8.0;
    auto jump_velocity = [&](double speed) {
        Sim s(c);
        s.run(5, M(100, 0, speed));
        s.run(5, M(201, 0.2, speed));
        return on_velocity(s, Jump);
    };
    CHECK(jump_velocity(0.0) == 80);
    CHECK(jump_velocity(1.0) == 80);
    CHECK(jump_velocity(4.5) == 95);                    // half way: +15
    CHECK(jump_velocity(8.0) == 110);
    CHECK(jump_velocity(30.0) == 110);                  // never above velocity + bonus

    c.t[Jump].speed_bonus = 0;                          // a trigger can switch it off ...
    CHECK(jump_velocity(8.0) == 80);
    c.t[Jump].speed_bonus = 10;                         // ... or use its own amount
    CHECK(jump_velocity(8.0) == 90);
    c.t[Jump].speed_bonus = -1;
    c.t[Jump].velocity = 120;
    CHECK(jump_velocity(8.0) == 127);                   // clamped to 127

    Config d = test_config();                           // the bonus is added on top of the landing velocity
    d.speed_bonus = 30;
    Sim s(d);
    flight(s, 3.0, 0.0, 8.0);
    CHECK(on_velocity(s, Land) == 127);
    Sim q(d);
    flight(q, 0.1, 0.0, 8.0);                           // tiny drop at full speed: 8 + 30
    CHECK(on_velocity(q, Land) == 38);
}

// ---------------------------------------------------------------------------------------------
// board rotation

static void test_rotation_default_thresholds() {
    Sim s;
    s.run(10, M(201, 1.0, 3, 0, 0, 100));               // an ollie: slow pitch/roll, well below 300
    s.run(10, M(201, 1.0, 3, 250, 250, 250));           // still below
    CHECK(s.ons(RotX) + s.ons(RotY) + s.ons(RotZ) == 0);
    s.run(10, M(201, 1.0, 3, 0, 0, 917));               // a kickflip: 16 rad/s about z
    CHECK(s.ons(RotZ) == 1 && s.ons(RotX) == 0 && s.ons(RotY) == 0);
    CHECK(std::abs(on_measure(s, RotZ) - 917) < 1e-9);
}

static void test_rotation_each_axis_and_sign() {
    Sim s;
    s.run(5, M(201, 1.0));
    s.run(300, M(201, 1.0, 0, -400, 0, 0));             // negative rate counts (absolute value)
    s.run(300, M(201, 1.0));
    s.run(300, M(201, 1.0, 0, 0, 800, 0));
    s.run(300, M(201, 1.0));
    s.run(300, M(201, 1.0, 0, 0, 0, -1000));
    CHECK(s.ons(RotX) == 1 && s.ons(RotY) == 1 && s.ons(RotZ) == 1);
    CHECK(s.total_ons() == 3);
}

static void test_rotation_not_on_the_ground_by_default() {
    // x and z are air-only; y also listens on the ground, for reverts (see the revert tests)
    Sim s;
    s.run(10, M(100, 0));
    s.run(500, M(100, 0, 3, 700, 0, 700));              // fast spin about x and z on the ground
    CHECK(s.ons(RotX) == 0 && s.ons(RotZ) == 0);
    Config c = test_config();
    c.t[RotZ].context = Context::Any;
    Sim q(c);
    q.run(10, M(100, 0));
    q.run(500, M(100, 0, 3, 0, 0, 700));
    CHECK(q.ons(RotZ) == 1);
    Config d = test_config();
    d.t[RotZ].context = Context::GroundOrAir;
    Sim w(d);
    w.run(10, M(402, 0));
    w.run(500, M(402, 0, 3, 0, 0, 700));                // a grind is neither
    CHECK(w.ons(RotZ) == 0);
}

static void test_rotation_one_note_per_trick_with_tail_and_hysteresis() {
    Sim s;
    s.run(5, M(201, 1.0));
    s.run(100, M(201, 1.0, 0, 0, 0, 900));              // starts
    s.run(100, M(201, 1.0, 0, 0, 0, 250));              // below the threshold but above release (200): still going
    s.run(100, M(201, 1.0, 0, 0, 0, 150));              // below release, but only for 100 ms (tail is 200 ms)
    s.run(100, M(201, 1.0, 0, 0, 0, 900));              // spins again: the same trick, no second note
    CHECK(s.ons(RotZ) == 1);
    s.run(500, M(201, 1.0, 0, 0, 0, 0));                // over for longer than the tail
    s.run(100, M(201, 1.0, 0, 0, 0, 900));              // a second trick
    CHECK(s.ons(RotZ) == 2);
    s.run(400, M(201, 1.0));
    CHECK(s.offs(RotZ) == 2);                           // pulses end on their own
}

static void test_rotation_pulse_length_and_gate_mode() {
    Sim s;
    s.run(5, M(201, 1.0));
    const auto t0 = s.now_ms();
    s.run(900, M(201, 1.0, 0, 0, 0, 900));              // rotation lasts much longer than a pulse
    CHECK(s.ons(RotZ) == 1);
    CHECK(s.off_time(RotZ) == t0 + 300);                // fixed length, not the length of the spin

    Config c = test_config();
    c.t[RotZ].gate = true;
    c.t[RotZ].off_delay_ms = 80;
    Sim g(c);
    g.run(5, M(201, 1.0));
    const auto g0 = g.now_ms();
    g.run(500, M(201, 1.0, 0, 0, 0, 900));
    CHECK(g.ons(RotZ) == 1 && g.offs(RotZ) == 0);       // held while it spins
    const auto g1 = g.now_ms();
    g.run(300, M(201, 1.0));
    CHECK(g.on_time(RotZ) == g0);
    CHECK(g.off_time(RotZ) == g1 + 80);                 // ends after the tail
}

static void test_rotation_needs_motion_and_can_be_disabled() {
    Sim s;
    s.run(500, S(201));                                 // no motion data
    CHECK(s.total_ons() == 0);
    Config c = test_config();
    c.t[RotZ].enabled = false;
    Sim q(c);
    q.run(5, M(201, 1.0));
    q.run(500, M(201, 1.0, 0, 0, 0, 900));
    CHECK(q.ons(RotZ) == 0);
    Config d = test_config();
    d.t[RotX].threshold = 100;                          // everything is tunable
    d.t[RotX].release = 50;
    Sim w(d);
    w.run(5, M(201, 1.0));
    w.run(100, M(201, 1.0, 0, 120, 0, 0));
    CHECK(w.ons(RotX) == 1);
}

// ---------------------------------------------------------------------------------------------
// fingerflip

static void test_fingerflip() {
    Sim s;
    s.run(5, MP(M(201, 1.0)));
    s.run(100, MP(M(201, 1.0, 0, 0, 600, 0)));          // the board spins, nobody holds it
    CHECK(s.ons(Fingerflip) == 0 && s.ons(RotY) == 1);
    s.run(600, MP(M(201, 1.0)));
    s.run(100, MP(M(201, 1.0, 0, 0, 600, 0), 255));     // grab + spin
    CHECK(s.ons(Fingerflip) == 1);
    CHECK(s.ons(RotY) == 2);                            // the board's own rotation trigger fires too
}

static void test_fingerflip_needs_air_grab_and_enough_spin() {
    Sim s;
    s.run(5, MP(M(201, 1.0)));
    s.run(300, MP(M(201, 1.0, 0, 0, 100, 0), 255));     // grab but only a slow spin
    s.run(300, MP(M(100, 0, 0, 0, 700, 0), 255));       // grab + spin, on the ground
    CHECK(s.ons(Fingerflip) == 0);
    Config c = test_config();
    c.t[Fingerflip].axes = 2;                           // only the y axis
    Sim q(c);
    q.run(5, MP(M(201, 1.0)));
    q.run(300, MP(M(201, 1.0, 0, 700, 0, 700), 255));   // x and z, not y
    CHECK(q.ons(Fingerflip) == 0);
    q.run(300, MP(M(201, 1.0, 0, 0, 700, 0), 255));
    CHECK(q.ons(Fingerflip) == 1);
}

static void test_fingerflip_grab_started_during_the_spin() {
    Sim s;
    s.run(5, MP(M(201, 1.0)));
    s.run(200, MP(M(201, 1.0, 0, 0, 0, 900)));          // the board is spinning
    s.run(5, MP(M(201, 1.0, 0, 0, 0, 900), 0, 255));    // RT joins
    CHECK(s.ons(Fingerflip) == 1);
}

// ---------------------------------------------------------------------------------------------
// config file, new keys

static void test_ini_new_keys() {
    Config c;
    ParseResult r;
    parse_ini(
        "[general]\n"
        "speed_min = 2\nspeed_max = 12.5\nspeed_bonus = 30\n"
        "[manual_stick]\nignore_states = 103, 105-106\n"
        "[land_velocity]\nmin_velocity = 5\nmax_velocity = 105\nmin_height = 0.2\nmax_height = 4\ncurve = 0.7\n"
        "[rot_z]\nmode = gate\nthreshold = 450\nrelease = 300\ntail_ms = 90\ncontext = any\n"
        "[fingerflip]\naxes = y, z\nthreshold = 350\n"
        "[jump]\nspeed_bonus = 12\n"
        "[manual]\ndelay_ms = 250\n",
        c, r);
    CHECK(r.ok());
    CHECK(r.warnings.empty());
    CHECK(c.speed_min == 2.0 && c.speed_max == 12.5 && c.speed_bonus == 30);
    CHECK(c.manual_ignore.contains(103) && c.manual_ignore.contains(106) && !c.manual_ignore.contains(104));
    CHECK(c.land.min_velocity == 5 && c.land.max_velocity == 105 && c.land.min_height == 0.2 && c.land.curve == 0.7);
    CHECK(c.t[RotZ].gate && c.t[RotZ].threshold == 450 && c.t[RotZ].release == 300 && c.t[RotZ].off_delay_ms == 90);
    CHECK(c.t[RotZ].context == Context::Any);
    CHECK(c.t[Fingerflip].axes == 6 && c.t[Fingerflip].threshold == 350);
    CHECK(c.t[Jump].speed_bonus == 12 && c.t[Manual].delay_ms == 250);

    Config d;
    ParseResult e;
    parse_ini("[manual_stick]\nignore_states = none\n", d, e);
    CHECK(e.ok() && d.manual_ignore.r.empty());
}

static void test_ini_new_errors_and_warnings() {
    Config c;
    ParseResult r;
    parse_ini(
        "[rot_x]\nthreshold = 200\nrelease = 300\n"
        "[land_velocity]\nmin_height = 5\nmax_height = 1\n"
        "[general]\nspeed_min = 9\nspeed_max = 3\n",
        c, r);
    CHECK(!r.ok());
    CHECK(r.errors.size() == 3);
    Config d;
    ParseResult w;
    parse_ini("[jump]\nthreshold = 400\ncontext = any\nmode = gate\n[rot_x]\naxes = x\n[rot_y]\nmode = banana\n", d, w);
    CHECK(w.warnings.size() == 3);   // threshold, context on jump; axes on rot_x
    CHECK(w.errors.size() == 1);     // mode = banana
}

// ---------------------------------------------------------------------------------------------
// reading position and board motion from memory

static void test_read_motion_ok() {
    World w;
    auto r = w.run();
    CHECK(r.result == Result::Ok);
    RawMotion mo;
    const char* why = "";
    CHECK(read_motion(w.m, World::base, r, mo, why));
    CHECK(mo.position[0] == 10.0f && mo.position[1] == 2.5f && mo.position[2] == -4.0f);
    CHECK(mo.velocity[0] == 1.0f && mo.velocity[2] == 3.0f);
    CHECK(mo.spin[0] == 0.5f && mo.spin[1] == 16.0f && mo.spin[2] == -2.0f);
    w.set_position(0, 7.25f, 0);
    w.set_motion(0, 0, 0, 0, 0, 16.0f);
    CHECK(read_motion(w.m, World::base, r, mo, why));
    CHECK(mo.position[1] == 7.25f && mo.spin[2] == 16.0f);
}

static void test_read_motion_rejections() {
    const char* why = "";
    RawMotion mo;
    { World w; auto r = w.run(); w.m.put<std::uintptr_t>(World::collection, 0xdead);
      CHECK(!read_motion(w.m, World::base, r, mo, why)); }                                    // wrong back pointer
    { World w; auto r = w.run(); w.m.put<std::uint8_t>(World::collection + 8, 200);
      CHECK(!read_motion(w.m, World::base, r, mo, why)); }                                    // layout changed
    { World w; auto r = w.run(); w.set_position(0, 5e9f, 0);
      CHECK(!read_motion(w.m, World::base, r, mo, why)); }                                    // out of bounds
    { World w; auto r = w.run(); w.m.put<float>(World::matrix() + off::matrix_position + 4, std::nanf(""));
      CHECK(!read_motion(w.m, World::base, r, mo, why)); }                                    // NaN
    { World w; auto r = w.run(); w.m.put<std::uintptr_t>(World::board, 0xdead);
      CHECK(!read_motion(w.m, World::base, r, mo, why)); }                                    // not the board physics
    { World w; auto r = w.run(); w.m.put<std::uint32_t>(World::parts, 7);
      CHECK(!read_motion(w.m, World::base, r, mo, why)); }                                    // wrong body count
    { World w; auto r = w.run(); w.m.put<std::uintptr_t>(World::parts + off::body_owner, 0xdead);
      CHECK(!read_motion(w.m, World::base, r, mo, why)); }                                    // body belongs to something else
    { World w; auto r = w.run(); w.set_motion(0, 0, 0, std::nanf(""), 0, 0);
      CHECK(!read_motion(w.m, World::base, r, mo, why)); }                                    // invalid spin
}


// ---------------------------------------------------------------------------------------------
// angle mode: a note for every 180 degrees of board rotation

static Config angle_config() {
    Config c = test_config();
    c.rotation.angle = true;
    return c;
}
// the nth note-on of a trigger, or nullptr
static const Event* nth_on(const Sim& s, int trigger, int nth = 0) {
    for (const auto& x : s.log) if (x.a.on && x.a.trigger == trigger && nth-- == 0) return &x;
    return nullptr;
}

static int note_of(const Sim& s, int trigger, int nth) { const Event* e = nth_on(s, trigger, nth); return e ? e->a.note : -1; }
static double angle_of(const Sim& s, int trigger, int nth) { const Event* e = nth_on(s, trigger, nth); return e ? e->a.angle : -1; }
static int axis_of(const Sim& s, int trigger, int nth) { const Event* e = nth_on(s, trigger, nth); return e ? e->a.axis : -9; }

static void test_angle_kickflip_is_two_notes() {
    Sim s(angle_config());
    s.run(5, M(201, 1.0));
    const auto t0 = s.now_ms();
    s.run(420, M(201, 1.0, 0, 0, 0, 917));              // 16 rad/s about z for 0.42 s = 385 degrees
    CHECK(s.ons(RotZ) == 2);
    const Event* a = nth_on(s, RotZ, 0);
    const Event* b = nth_on(s, RotZ, 1);
    CHECK(a && b);
    if (a && b) {
        CHECK(a->t_ms >= t0 + 190 && a->t_ms <= t0 + 200);          // 180 degrees after 196 ms
        CHECK(b->t_ms >= t0 + 388 && b->t_ms <= t0 + 400);          // 360 degrees after 393 ms
        CHECK(a->a.angle == 180 && b->a.angle == 360 && a->a.axis == 2);
        CHECK(a->a.note == 58 && b->a.note == 58);
    }
    CHECK(s.ons(RotX) == 0 && s.ons(RotY) == 0);
    s.run(500, M(201, 1.0));
    CHECK(s.offs(RotZ) == 2);                           // each note ended
}

static void test_angle_slow_rotation_is_ignored() {
    Sim s(angle_config());
    s.run(5, M(201, 1.0));
    s.run(3000, M(201, 1.0, 0, 120, 120, 120));         // 120 deg/s is under min_rate (150): 360 degrees, no notes
    CHECK(s.total_ons() == 0);
    Sim q(angle_config());
    q.run(5, M(201, 1.0));
    q.run(900, M(201, 1.0, 0, 86, 0, 0));               // the ollie's pitch
    CHECK(q.total_ons() == 0);
}

static void test_angle_axes_are_independent() {
    Sim s(angle_config());
    s.run(5, M(201, 1.0));
    s.run(400, M(201, 1.0, 0, 600, 600, 0));            // x and y at 600 deg/s: 240 degrees each
    CHECK(s.ons(RotX) == 1 && s.ons(RotY) == 1 && s.ons(RotZ) == 0);
    CHECK(axis_of(s, RotX, 0) == 0 && axis_of(s, RotY, 0) == 1);
}

static void test_angle_double_flip_retriggers_and_cuts() {
    Sim s(angle_config());
    s.run(5, M(201, 1.0));
    s.run(410, M(201, 1.0, 0, 0, 0, 1800));             // 738 degrees in 0.41 s: a note about every 100 ms
    CHECK(s.ons(RotZ) == 4);
    // notes are 120 ms long but the steps are 100 ms apart: each new step ends the previous note first
    int on = 0, off = 0;
    bool alternating = true;
    for (const auto& x : s.log) {
        if (x.a.trigger != RotZ) continue;
        if (x.a.on) { if (on != off) alternating = false; ++on; }
        else { ++off; }
    }
    CHECK(alternating);
    s.run(500, M(201, 1.0));
    CHECK(s.offs(RotZ) == 4);
}

static void test_angle_note_step_climbs_within_a_trick() {
    Config c = angle_config();
    c.rotation.note_step = 2;
    Sim s(c);
    s.run(5, M(201, 1.0));
    s.run(410, M(201, 1.0, 0, 0, 0, 1800));
    CHECK(s.ons(RotZ) == 4);
    CHECK(note_of(s, RotZ, 0) == 58 && note_of(s, RotZ, 1) == 60 && note_of(s, RotZ, 2) == 62 && note_of(s, RotZ, 3) == 64);
    s.run(500, M(201, 1.0));                            // the trick is over; the next one starts from the base note again
    s.run(120, M(201, 1.0, 0, 0, 0, 1800));
    CHECK(s.ons(RotZ) == 5 && note_of(s, RotZ, 4) == 58);
    // a different spin direction is a different axis value but the same note ladder; a per-trigger value wins
    Config d = angle_config();
    d.rotation.note_step = 2;
    d.t[RotZ].note_step = -3;
    Sim q(d);
    q.run(5, M(201, 1.0));
    q.run(250, M(201, 1.0, 0, 0, 0, 1800));
    CHECK(note_of(q, RotZ, 0) == 58 && note_of(q, RotZ, 1) == 55);
}

static void test_angle_negative_spin_and_reversals() {
    Sim s(angle_config());
    s.run(5, M(201, 1.0));
    s.run(300, M(201, 1.0, 0, 0, 0, 500));             // +150 degrees: not yet
    s.run(600, M(201, 1.0, 0, 0, 0, -500));            // back through zero to -150: still nothing
    CHECK(s.ons(RotZ) == 0);
    s.run(300, M(201, 1.0, 0, 0, 0, -500));            // -300 degrees: one step
    CHECK(s.ons(RotZ) == 1);
    s.run(800, M(201, 1.0, 0, 0, 0, 500));             // wobbling back never replays a step
    CHECK(s.ons(RotZ) == 1);
}

static void test_angle_resets_on_landing_and_when_idle() {
    Sim s(angle_config());
    s.run(5, M(201, 1.0));
    s.run(100, M(201, 1.0, 0, 0, 0, 1000));            // 100 degrees
    s.run(10, M(100, 0));                              // lands: the angle is forgotten
    s.run(5, M(201, 1.0));
    s.run(100, M(201, 1.0, 0, 0, 0, 1000));            // another 100 degrees: still no step
    CHECK(s.ons(RotZ) == 0);

    Sim q(angle_config());
    q.run(5, M(201, 1.0));
    q.run(100, M(201, 1.0, 0, 0, 0, 1000));
    q.run(300, M(201, 1.0));                           // stops for 300 ms (idle_reset is 150): forgotten
    q.run(100, M(201, 1.0, 0, 0, 0, 1000));
    CHECK(q.ons(RotZ) == 0);

    Sim w(angle_config());
    w.run(5, M(201, 1.0));
    w.run(100, M(201, 1.0, 0, 0, 0, 1000));
    w.run(80, M(201, 1.0));                            // a short pause is part of the same trick
    w.run(100, M(201, 1.0, 0, 0, 0, 1000));
    CHECK(w.ons(RotZ) == 1);
}

static void test_angle_context_step_and_per_trigger_override() {
    Sim g(angle_config());                             // on the ground: nothing, like the rate mode
    g.run(5, M(100, 0));
    g.run(400, M(100, 0, 3, 0, 0, 1800));
    CHECK(g.total_ons() == 0);
    Config a = angle_config();
    a.t[RotZ].context = Context::Any;
    Sim any(a);
    any.run(5, M(100, 0));
    any.run(410, M(100, 0, 3, 0, 0, 1800));
    CHECK(any.ons(RotZ) == 4);

    Config c = angle_config();
    c.rotation.step_deg = 90;
    Sim s(c);
    s.run(5, M(201, 1.0));
    s.run(410, M(201, 1.0, 0, 0, 0, 900));             // 369 degrees = 4 steps of 90
    CHECK(s.ons(RotZ) == 4);
    Config d = angle_config();
    d.t[RotZ].step_deg = 360;
    Sim q(d);
    q.run(5, M(201, 1.0));
    q.run(410, M(201, 1.0, 0, 0, 0, 900));
    CHECK(q.ons(RotZ) == 1 && angle_of(q, RotZ, 0) == 360);
}

static void test_angle_switch_per_trigger_and_midflight() {
    Config c = test_config();                          // global rate, but rot_z uses angle
    c.t[RotZ].detect = 1;
    Sim s(c);
    s.run(5, M(201, 1.0));
    s.run(410, M(201, 1.0, 0, 0, 900, 900));
    CHECK(s.ons(RotZ) == 2);                           // angle: 369 degrees
    CHECK(s.ons(RotY) == 1);                           // rate: one note at the start
    Config d = angle_config();                         // global angle, but rot_y uses rate
    d.t[RotY].detect = 0;
    Sim q(d);
    q.run(5, M(201, 1.0));
    q.run(410, M(201, 1.0, 0, 0, 900, 900));
    CHECK(q.ons(RotY) == 1 && q.ons(RotZ) == 2);

    // switching while a held rate-mode note sounds must not leave the note stuck
    Config g = test_config();
    g.t[RotZ].gate = true;
    Sim m(g);
    m.run(5, M(201, 1.0));
    m.run(200, M(201, 1.0, 0, 0, 0, 900));
    CHECK(m.e.active(RotZ));
    m.e.set_config(angle_config());
    m.run(600, M(201, 1.0));
    CHECK(!m.e.active(RotZ));
    CHECK(m.ons(RotZ) == m.offs(RotZ));
}

static void test_angle_stall_cannot_become_an_angle() {
    Sim s(angle_config());
    s.run(5, M(201, 1.0));
    s.now_us += 5000000;                               // the program stalled for 5 s (a memory scan)
    s.run(1, M(201, 1.0, 0, 0, 0, 1000));              // one sample at 1000 deg/s: only 50 ms of it can count
    CHECK(s.ons(RotZ) == 0);
}

static void test_angle_fingerflip_counts_from_the_grab() {
    Sim s(angle_config());
    s.run(5, MP(M(201, 1.0)));
    s.run(150, MP(M(201, 1.0, 0, 0, 900, 0)));          // 135 degrees before any grab
    s.run(150, MP(M(201, 1.0, 0, 0, 900, 0), 255));     // grab joins: counted from here
    CHECK(s.ons(Fingerflip) == 0);
    s.run(110, MP(M(201, 1.0, 0, 0, 900, 0), 255));     // 234 degrees of grabbed turning
    CHECK(s.ons(Fingerflip) == 1);
    CHECK(angle_of(s, Fingerflip, 0) == 180 && note_of(s, Fingerflip, 0) == 60);
    CHECK(s.ons(RotY) == 2);                            // the board's own steps count the whole 369 degrees
}

static void test_angle_fingerflip_resets_when_released_and_needs_air() {
    Sim s(angle_config());
    s.run(5, MP(M(201, 1.0)));
    s.run(150, MP(M(201, 1.0, 0, 0, 900, 0), 255));     // 135 degrees held
    s.run(20, MP(M(201, 1.0, 0, 0, 900, 0)));           // let go: the count starts over
    s.run(150, MP(M(201, 1.0, 0, 0, 900, 0), 255));     // another 135
    CHECK(s.ons(Fingerflip) == 0);
    Sim g(angle_config());
    g.run(5, MP(M(100, 0)));
    g.run(400, MP(M(100, 0, 3, 0, 900, 0), 255));       // on the ground
    CHECK(g.ons(Fingerflip) == 0);
    Sim n(angle_config());
    n.run(5, MP(M(201, 1.0)));
    n.run(400, MP(M(201, 1.0, 0, 0, 900, 0), 0));       // spin but no grab
    CHECK(n.ons(Fingerflip) == 0);
}

static void test_angle_fingerflip_axes_and_independence() {
    Config c = angle_config();
    c.t[Fingerflip].axes = 2;                           // only y
    Sim s(c);
    s.run(5, MP(M(201, 1.0)));
    s.run(300, MP(M(201, 1.0, 0, 900, 0, 900), 255));   // x and z do not count
    CHECK(s.ons(Fingerflip) == 0);
    Config d = angle_config();                          // x, y, z: each axis counts its own steps
    Sim q(d);
    q.run(5, MP(M(201, 1.0)));
    q.run(250, MP(M(201, 1.0, 0, 900, 900, 0), 255));   // 225 degrees about x and about y
    CHECK(q.ons(Fingerflip) == 2);
    CHECK(axis_of(q, Fingerflip, 0) != axis_of(q, Fingerflip, 1));
}

// ---------------------------------------------------------------------------------------------
// config file: the rotation switch

static void test_ini_rotation_section() {
    Config c;
    ParseResult r;
    parse_ini(
        "[rotation]\ndetect = angle\nstep_deg = 90\nmin_rate = 200\nidle_reset_ms = 250\nhold_ms = 80\nnote_step = 3\n"
        "[rot_z]\ndetect = rate\nstep_deg = 360\nnote_step = -2\n"
        "[fingerflip]\ndetect = angle\n",
        c, r);
    CHECK(r.ok() && r.warnings.empty());
    CHECK(c.rotation.angle && c.rotation.step_deg == 90 && c.rotation.min_rate == 200 && c.rotation.idle_reset_ms == 250);
    CHECK(c.rotation.hold_ms == 80 && c.rotation.note_step == 3);
    CHECK(c.t[RotZ].detect == 0 && c.t[RotZ].step_deg == 360 && c.t[RotZ].note_step == -2);
    CHECK(c.t[Fingerflip].detect == 1 && c.t[RotX].detect == -1);

    Config d;
    ParseResult e;
    parse_ini("[rotation]\ndetect = banana\nstep_deg = 5\nhold_ms = 0\n[jump]\ndetect = angle\n", d, e);
    CHECK(!e.ok() && e.errors.size() == 3);
    CHECK(e.warnings.size() == 1);                      // detect on [jump]

    Config w;
    CHECK(w.rotation.angle);                            // angle is the default now
    const std::string text = write_ini(w);
    Config back;
    ParseResult b;
    parse_ini(text, back, b);
    CHECK(b.ok() && b.warnings.empty() && write_ini(back) == text);
    Config on;
    on.rotation.angle = true;
    on.t[RotX].detect = 0;
    on.t[Fingerflip].step_deg = 270;
    on.t[RotY].note_step = 5;
    Config back2;
    ParseResult b2;
    parse_ini(write_ini(on), back2, b2);
    CHECK(b2.ok() && back2.rotation.angle && back2.t[RotX].detect == 0 && back2.t[Fingerflip].step_deg == 270 && back2.t[RotY].note_step == 5);
}


// ---------------------------------------------------------------------------------------------
// the bytes that go to the MIDI port, and the audit that spots notes Noiseflip did not send

static Action make_action(bool on, int note, int velocity, int channel) {
    Action a;
    a.on = on; a.note = note; a.velocity = on ? velocity : 0; a.channel = channel;
    return a;
}

static void test_wire_encoding_every_note_and_channel() {
    for (int ch = 0; ch < 16; ++ch) {
        for (int note = 0; note < 128; ++note) {
            const MidiMsg on = encode(make_action(true, note, 100, ch), NoteOffStyle::NoteOff);
            const MidiMsg off = encode(make_action(false, note, 0, ch), NoteOffStyle::NoteOff);
            const MidiMsg off0 = encode(make_action(false, note, 0, ch), NoteOffStyle::Velocity0);
            CHECK(on.status == (0x90 | ch) && on.d1 == note && on.d2 == 100);
            CHECK(off.status == (0x80 | ch) && off.d1 == note && off.d2 == 0);      // the SAME note as the note-on
            CHECK(off0.status == (0x90 | ch) && off0.d1 == note && off0.d2 == 0);
            CHECK(is_note_on(on) && !is_note_off(on));
            CHECK(is_note_off(off) && !is_note_on(off) && is_note_off(off0) && !is_note_on(off0));
            CHECK(on.d1 < 0x80 && on.d2 < 0x80 && off.d1 < 0x80 && off.d2 < 0x80);   // data bytes never look like a status byte
            const MidiMsg back = unpack(pack(on));
            CHECK(back.status == on.status && back.d1 == on.d1 && back.d2 == on.d2);
        }
    }
}

static void test_wire_known_values() {
    // what midiOutShortMsg receives for note 36, velocity 100, channel 1
    CHECK(pack(encode(make_action(true, 36, 100, 0), NoteOffStyle::NoteOff)) == 0x00642490u);
    CHECK(pack(encode(make_action(false, 36, 0, 0), NoteOffStyle::NoteOff)) == 0x00002480u);
    CHECK(pack(encode(make_action(false, 36, 0, 0), NoteOffStyle::Velocity0)) == 0x00002490u);
    CHECK(pack(encode(make_action(true, 54, 127, 15), NoteOffStyle::NoteOff)) == 0x007f369fu);
    // out-of-range input cannot leak into the status byte
    const MidiMsg odd = encode(make_action(true, 200, 300, 17), NoteOffStyle::NoteOff);
    CHECK(odd.status == 0x91 && odd.d1 < 0x80 && odd.d2 < 0x80);
}

// A stand-in for a synthesizer: it only knows which notes are sounding.
struct VirtualSynth {
    bool sounding[16][128] = {};
    int errors = 0;
    std::set<int> notes_seen;
    void receive(const MidiMsg& m) {
        if (!is_note_message(m)) { ++errors; return; }
        const int ch = m.status & 15, n = m.d1;
        notes_seen.insert(n);
        if (is_note_on(m)) { if (sounding[ch][n]) ++errors; sounding[ch][n] = true; }   // a second on without an off
        else { if (!sounding[ch][n]) ++errors; sounding[ch][n] = false; }                // an off for a note that is not on
    }
    bool silent() const { for (auto& c : sounding) for (bool b : c) if (b) return false; return true; }
};

static void test_wire_random_sessions_reach_the_synth_unchanged() {
    // The whole path: random game sessions -> engine -> bytes -> a synth. Both rotation modes, both note-off styles.
    unsigned long long messages = 0;
    for (int mode = 0; mode < 2; ++mode) {
        for (int style = 0; style < 2; ++style) {
            for (unsigned seed = 1; seed <= 60; ++seed) {
                std::uint32_t rng = seed * 2654435761u + mode * 97u + style * 13u;
                auto next = [&rng]() { rng = rng * 1664525u + 1013904223u; return rng >> 8; };
                Config cfg;
                cfg.rotation.angle = mode == 1;
                TriggerEngine e(cfg);
                std::set<int> allowed;
                for (int i = 0; i < TriggerCount; ++i) allowed.insert(cfg.t[i].note);
                VirtualSynth synth;
                const NoteOffStyle ns = style ? NoteOffStyle::Velocity0 : NoteOffStyle::NoteOff;
                const unsigned states[] = {100, 103, 200, 201, 300, 402, 504, 99, 650};
                unsigned state = 100;
                Input in;
                std::int64_t now = 0;
                auto feed = [&](const std::vector<Action>& v) {
                    for (const auto& a : v) { synth.receive(unpack(pack(encode(a, ns)))); ++messages; }
                };
                for (int step = 0; step < 4000; ++step) {
                    now += 1000;
                    if (next() % 35 == 0) state = states[next() % (sizeof states / sizeof *states)];
                    in.game_ok = next() % 300 != 0;
                    in.state = state;
                    in.pad.connected = true;
                    if (next() % 30 == 0) { in.pad.lt = next() % 256; in.pad.ry = static_cast<int>(next() % 65536) - 32768;
                                            in.pad.buttons = (next() % 3 == 0 ? 0x1000u : 0u) | (next() % 4 == 0 ? 0x2000u : 0u); }
                    in.motion.ok = true;
                    if (next() % 25 == 0) { in.motion.y = (next() % 4000) / 1000.0; in.motion.speed = (next() % 9000) / 1000.0;
                                            for (int a = 0; a < 3; ++a) in.motion.w_dps[a] = next() % 3 == 0 ? static_cast<double>(next() % 3000) - 1500 : 0; }
                    feed(e.update(now, in));
                }
                feed(e.release_all());
                CHECK(synth.errors == 0);
                CHECK(synth.silent());
                for (int n : synth.notes_seen) CHECK(allowed.count(n) == 1);   // never a note that is not configured
            }
        }
    }
    std::printf("   (random sessions sent %llu MIDI messages to the virtual synth)\n", messages);
    CHECK(messages > 5000);
}

static void test_audit_recognises_its_own_echo() {
    MidiAudit a;
    const MidiMsg on = encode(make_action(true, 36, 100, 0), NoteOffStyle::NoteOff);
    const MidiMsg off = encode(make_action(false, 36, 0, 0), NoteOffStyle::NoteOff);
    a.sent(on, 1000);
    CHECK(a.received(on, 1500));
    a.sent(off, 300000);
    CHECK(a.received(off, 300400));
    CHECK(a.ours() == 2 && a.foreign() == 0 && a.sent_count() == 2);
    CHECK(a.unechoed(400000) == 0);
}

static void test_audit_ignores_velocity_and_note_off_spelling() {
    MidiAudit a;
    a.sent(encode(make_action(true, 40, 100, 3), NoteOffStyle::NoteOff), 0);
    CHECK(a.received(encode(make_action(true, 40, 77, 3), NoteOffStyle::NoteOff), 100));          // another velocity: still ours
    a.sent(encode(make_action(false, 40, 0, 3), NoteOffStyle::NoteOff), 200);
    CHECK(a.received(encode(make_action(false, 40, 0, 3), NoteOffStyle::Velocity0), 300));        // spelled as note-on velocity 0
    CHECK(a.foreign() == 0);
}

static void test_audit_catches_a_second_sender() {
    // the symptom that was reported: a jump plays 36, and a 38 shows up right after
    MidiAudit a;
    const MidiMsg ours = encode(make_action(true, 36, 100, 0), NoteOffStyle::NoteOff);
    a.sent(ours, 0);
    CHECK(a.received(ours, 100));
    const MidiMsg other = encode(make_action(true, 38, 100, 0), NoteOffStyle::NoteOff);            // someone else
    CHECK(!a.received(other, 150000));
    CHECK(a.foreign() == 1);
    // a note of ours arriving twice (two copies of Noiseflip) is also caught: the second one has no matching send
    a.sent(ours, 200000);
    CHECK(a.received(ours, 200100));
    CHECK(!a.received(ours, 200200));
    CHECK(a.foreign() == 2);
}

static void test_audit_ignores_non_note_messages_and_reports_missing_echoes() {
    MidiAudit a;
    CHECK(a.received(MidiMsg{0xF8, 0, 0}, 0));                       // clock
    CHECK(a.received(MidiMsg{0xB0, 7, 100}, 0));                     // a controller
    CHECK(a.foreign() == 0);
    a.sent(encode(make_action(true, 36, 100, 0), NoteOffStyle::NoteOff), 0);
    CHECK(a.unechoed(1000000) == 0);                                 // still inside the window
    CHECK(a.unechoed(2500000) == 1);                                 // the port never echoed it: the check is inconclusive
}

// ---------------------------------------------------------------------------------------------
// reverts: the shuvit trigger (rot_y) also plays for a quick 180 on the ground

static Config revert_config() {                          // the real defaults (angle mode), without the manual delay
    Config c;
    legacy_defaults(c);
    c.t[Manual].delay_ms = 0;
    return c;
}

static void test_defaults_angle_mode_and_rot_y_on_the_ground() {
    Config c;
    CHECK(c.rotation.angle);
    CHECK(c.t[RotY].context == Context::GroundOrAir);
    CHECK(c.t[RotX].context == Context::Air && c.t[RotZ].context == Context::Air && c.t[Fingerflip].context == Context::Air);
    CHECK(c.rotation.switch_button == 0x0040);          // the left stick click
    CHECK(c.rotation.ground_min_rate == 300 && c.rotation.extra_states.r.empty());
}

static void test_revert_plays_the_shuvit_note() {
    Sim s(revert_config());
    s.run(10, M(100, 0, 4));
    s.run(330, M(100, 0, 4, 0, 600, 0));                // a revert: 198 degrees in 0.33 s
    CHECK(s.ons(RotY) == 1);
    CHECK(note_of(s, RotY, 0) == 56 && axis_of(s, RotY, 0) == 1 && angle_of(s, RotY, 0) == 180);
    CHECK(s.ons(RotX) == 0 && s.ons(RotZ) == 0 && s.ons(Jump) == 0);
    s.run(600, M(100, 0, 4));
    CHECK(s.offs(RotY) == 1);

    Sim d(revert_config());                             // a full 360 on the ground: two notes
    d.run(10, M(100, 0, 4));
    d.run(410, M(100, 0, 4, 0, 900, 0));
    CHECK(d.ons(RotY) == 2);
}

static void test_carving_is_not_a_revert() {
    Sim s(revert_config());
    s.run(10, M(100, 0, 6));
    s.run(3000, M(100, 0, 6, 0, 120, 0));               // a wide carve: 360 degrees, slowly
    CHECK(s.ons(RotY) == 0);
    Sim h(revert_config());
    h.run(10, M(100, 0, 6));
    h.run(2000, M(100, 0, 6, 0, 250, 0));               // a hard carve, 500 degrees, but under ground_min_rate (300)
    CHECK(h.ons(RotY) == 0);
    Config c = revert_config();
    c.rotation.ground_min_rate = 200;                   // tunable: now that carve counts
    Sim t(c);
    t.run(10, M(100, 0, 6));
    t.run(2000, M(100, 0, 6, 0, 250, 0));
    CHECK(t.ons(RotY) >= 1);
}

static void test_manual_revert_counts() {
    Config c = revert_config();
    Sim s(c);
    Input in = M(100, 0, 3, 0, 700, 0);                  // spinning about the vertical axis on the ground ...
    in.pad.connected = true;
    in.pad.ry = 20000;                                   // ... while the stick holds a manual
    s.run(10, M(100, 0, 3));
    s.run(300, in);
    CHECK(s.ons(Manual) == 1 && s.ons(RotY) == 1);       // both play: the manual and the revert
}

static void test_revert_needs_the_ground_or_air_context() {
    for (unsigned state : {402u, 504u, 300u, 650u}) {   // grind, bail, wipeout, an unknown state
        Sim s(revert_config());
        s.run(10, M(state, 0, 3));
        s.run(400, M(state, 0, 3, 0, 900, 0));
        CHECK(s.ons(RotY) == 0);
    }
    Config c = revert_config();
    c.rotation.extra_states.r = {{650, 650}};            // the manual's own state, say
    Sim e(c);
    e.run(10, M(650, 0, 3));
    e.run(400, M(650, 0, 3, 0, 900, 0));
    CHECK(e.ons(RotY) == 1);
    CHECK(e.ons(Jump) == 0 && e.ons(Land) == 0);         // jump and land still use only the ground and air lists
}

static void test_revert_and_air_are_separate() {
    Sim s(revert_config());
    s.run(10, M(100, 0, 3));
    s.run(100, M(100, 0, 3, 0, 1000, 0));               // 100 degrees of revert, then the pop
    s.run(100, M(201, 0.4, 3, 0, 1000, 0));             // 100 more in the air: the revert's angle does not carry over
    CHECK(s.ons(RotY) == 0);
    Sim l(revert_config());
    l.run(10, M(201, 1.0, 3));
    l.run(100, M(201, 1.0, 3, 0, 1000, 0));             // 100 degrees in the air, then the landing
    l.run(100, M(100, 0, 3, 0, 1000, 0));               // 100 more on the ground
    CHECK(l.ons(RotY) == 0);
    Sim p(revert_config());                             // a shuvit in the air still plays, as before
    p.run(10, M(201, 1.0, 3));
    p.run(250, M(201, 1.0, 3, 0, 900, 0));
    CHECK(p.ons(RotY) == 1);
}

static void test_revert_in_rate_mode_too() {
    Config c = test_config();                            // rate mode
    Sim s(c);
    s.run(10, M(100, 0, 3));
    s.run(300, M(100, 0, 3, 0, 600, 0));
    CHECK(s.ons(RotY) == 1);
    Sim q(c);
    q.run(10, M(100, 0, 3));
    q.run(3000, M(100, 0, 3, 0, 120, 0));               // carving is below the threshold (300)
    CHECK(q.ons(RotY) == 0);
}

// ---------------------------------------------------------------------------------------------
// switching the rotation mode with the left stick

constexpr unsigned BTN_LS = 0x0040;

static Input with_buttons(Input i, unsigned buttons) {
    i.pad.connected = true;
    i.pad.buttons = buttons;
    return i;
}

static void test_left_stick_switches_the_mode() {
    Sim s(revert_config());
    CHECK(s.e.angle_mode());                             // angle at the start
    s.run(5, with_buttons(M(201, 1.0), 0));
    s.run(5, with_buttons(M(201, 1.0), BTN_LS));         // click
    CHECK(!s.e.angle_mode() && s.e.mode_switches() == 1);
    s.run(500, with_buttons(M(201, 1.0), BTN_LS));       // holding it does not switch again
    CHECK(s.e.mode_switches() == 1);
    s.run(5, with_buttons(M(201, 1.0), 0));              // let go
    s.run(5, with_buttons(M(201, 1.0), BTN_LS));         // click again
    CHECK(s.e.angle_mode() && s.e.mode_switches() == 2);
}

static void test_the_switch_changes_what_a_kickflip_plays() {
    Sim s(revert_config());
    s.run(5, with_buttons(M(201, 1.0), 0));
    s.run(410, with_buttons(M(201, 1.0, 0, 0, 0, 917), 0));
    CHECK(s.ons(RotZ) == 2);                             // angle mode: 2 notes for a kickflip
    s.run(500, with_buttons(M(201, 1.0), 0));
    s.run(5, with_buttons(M(201, 1.0), BTN_LS));         // switch to rate
    s.run(5, with_buttons(M(201, 1.0), 0));
    s.run(500, with_buttons(M(201, 1.0), 0));
    const int before = s.ons(RotZ);
    s.run(410, with_buttons(M(201, 1.0, 0, 0, 0, 917), 0));
    CHECK(s.ons(RotZ) == before + 1);                    // rate mode: one note
}

static void test_the_switch_respects_its_settings() {
    Sim s(revert_config());
    Input in = M(201, 1.0);
    in.pad.connected = false;
    in.pad.buttons = BTN_LS;                             // stale data from a pad that is not there
    s.run(20, in);
    CHECK(s.e.mode_switches() == 0);
    Config c = revert_config();
    c.rotation.switch_button = 0;                        // switch_button = none
    Sim q(c);
    q.run(5, with_buttons(M(201, 1.0), 0));
    q.run(5, with_buttons(M(201, 1.0), BTN_LS));
    CHECK(q.e.mode_switches() == 0 && q.e.angle_mode());
    Config d = revert_config();
    d.rotation.switch_button = 0x0100;                   // LB instead
    Sim w(d);
    w.run(5, with_buttons(M(201, 1.0), 0));
    w.run(5, with_buttons(M(201, 1.0), BTN_LS));
    CHECK(w.e.mode_switches() == 0);
    w.run(5, with_buttons(M(201, 1.0), 0x0100));
    CHECK(w.e.mode_switches() == 1 && !w.e.angle_mode());
}

static void test_switch_versus_the_ini_and_per_trigger_choices() {
    Sim s(revert_config());
    s.run(5, with_buttons(M(201, 1.0), 0));
    s.run(5, with_buttons(M(201, 1.0), BTN_LS));         // the player chose rate
    CHECK(!s.e.angle_mode());
    s.e.set_config(revert_config());                     // the file is saved again, `detect` unchanged
    CHECK(!s.e.angle_mode());                            // the player's choice stays
    Config rate = revert_config();
    rate.rotation.angle = false;
    s.e.set_config(rate);                                // the file now says rate: the file wins and the choice is cleared
    s.e.set_config(revert_config());                     // and when it says angle again, that is what applies
    CHECK(s.e.angle_mode());

    Config c = revert_config();
    c.t[RotY].detect = 0;                                // rot_y always uses rate
    Sim q(c);
    q.run(5, with_buttons(M(201, 1.0), 0));
    q.run(5, with_buttons(M(201, 1.0), BTN_LS));         // the switch flips only the global choice
    q.run(5, with_buttons(M(201, 1.0), 0));
    q.run(300, with_buttons(M(201, 1.0, 0, 0, 900, 0), 0));
    CHECK(q.ons(RotY) == 1);                             // still a rate-mode single note
}

static void test_ini_reverts_and_switch_keys() {
    Config c;
    ParseResult r;
    parse_ini("[rotation]\nground_min_rate = 400\nextra_states = 650, 660-662\nswitch_button = LB\n"
              "[rot_x]\ncontext = ground_or_air\n", c, r);
    CHECK(r.ok() && r.warnings.empty());
    CHECK(c.rotation.ground_min_rate == 400 && c.rotation.extra_states.contains(661) && !c.rotation.extra_states.contains(663));
    CHECK(c.rotation.switch_button == 0x0100 && c.t[RotX].context == Context::GroundOrAir);
    Config d;
    ParseResult e;
    parse_ini("[rotation]\nswitch_button = none\nextra_states = none\n", d, e);
    CHECK(e.ok() && d.rotation.switch_button == 0 && d.rotation.extra_states.r.empty());
    Config f;
    ParseResult g;
    parse_ini("[rotation]\nswitch_button = banana\nground_min_rate = 0\n", f, g);
    CHECK(!g.ok() && g.errors.size() == 2);
    Config w;
    const std::string text = write_ini(w);
    Config back;
    ParseResult b;
    parse_ini(text, back, b);
    CHECK(b.ok() && b.warnings.empty() && write_ini(back) == text);
    CHECK(back.rotation.angle && back.rotation.switch_button == 0x0040 && back.t[RotY].context == Context::GroundOrAir);
    Config off;
    off.rotation.switch_button = 0;
    off.rotation.extra_states.r = {{700, 700}};
    Config back2;
    ParseResult b2;
    parse_ini(write_ini(off), back2, b2);
    CHECK(b2.ok() && back2.rotation.switch_button == 0 && back2.rotation.extra_states.contains(700));
}

// ---------------------------------------------------------------------------------------------
// the values you saved, and buttons that come in lists (A or X)

constexpr unsigned BTN_X = 0x4000;

static bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

static Config saved_config() {                             // the real defaults, only without the manual delay
    Config c;
    c.t[Manual].delay_ms = 0;
    return c;
}

static void test_defaults_are_the_saved_ini() {
    Config c;
    const int notes[TriggerCount] = {36, 37, 39, 40, 41, 44, 38, 45, 46, 48, 49, 50, 51};   // jump land bail grind manual grab push footplant nofootgrab rot_x rot_y rot_z fingerflip
    for (int i = 0; i < TriggerCount; ++i) CHECK(c.t[i].note == notes[i]);
    CHECK(c.velocity == 60 && c.speed_bonus == 40);
    CHECK(c.t[Push].delay_ms == 500 && c.t[Footplant].delay_ms == 200 && c.t[NoFootGrab].delay_ms == 200);
    CHECK(c.push_button == (0x1000u | 0x4000u) && c.footplant_button == (0x1000u | 0x4000u) && c.nofootgrab_button == 0x2000u);
    CHECK(c.rotation.angle && c.t[Manual].delay_ms == 200);
}

static void test_the_saved_ini_loads() {
    // the lines of the ini that differ from the old defaults, as they were saved (with the repeated footplant_button)
    Config c;
    ParseResult r;
    parse_ini(
        "[general]\nvelocity = 60\nspeed_bonus = 40\n"
        "[input]\npush_button = A\nfootplant_button = A\nfootplant_button = X\nnofootgrab_button = B\n"
        "[land]\nnote = 37\n[push]\nnote = 38\ndelay_ms = 500\n[bail]\nnote = 39\n[grind]\nnote = 40\n[manual]\nnote = 41\n"
        "[grab]\nnote = 44\n[footplant]\nnote = 45\ndelay_ms = 200\n[nofootgrab]\nnote = 46\ndelay_ms = 200\n"
        "[rot_x]\nnote = 48\n[rot_y]\nnote = 49\n[rot_z]\nnote = 50\n[fingerflip]\nnote = 51\n",
        c, r);
    CHECK(r.ok());
    CHECK(r.warnings.size() == 1);                       // footplant_button is written twice
    CHECK(c.footplant_button == 0x4000u);                // ... and the last one won, which is why only X worked
}

static void test_button_lists_and_repeated_keys() {
    Config c;
    ParseResult r;
    parse_ini("[input]\npush_button = A, X\nfootplant_button = x a\nnofootgrab_button = B + Y\n", c, r);
    CHECK(r.ok() && r.warnings.empty());
    CHECK(c.push_button == (0x1000u | 0x4000u) && c.footplant_button == (0x1000u | 0x4000u) && c.nofootgrab_button == (0x2000u | 0x8000u));
    Config d;
    ParseResult e;
    parse_ini("[input]\npush_button = A, banana\nfootplant_button =\n", d, e);
    CHECK(!e.ok() && e.errors.size() == 2);
    Config f;
    ParseResult g;
    parse_ini("[input]\npush_button = A\npush_button = X\n", f, g);
    CHECK(g.ok() && g.warnings.size() == 1 && g.warnings[0].find("A, X") != std::string::npos);   // the hint names the fix
    const std::string text = write_ini(Config());
    CHECK(text.find("push_button = A, X") != std::string::npos && text.find("footplant_button = A, X") != std::string::npos);
}

static void test_push_works_with_a_or_x() {
    Config c = saved_config();
    c.t[Push].delay_ms = 0;
    c.t[Push].hold_ms = 100;
    for (unsigned button : {0x1000u, BTN_X}) {          // either foot
        Sim s(c);
        s.run(5, SP(100));
        s.run(300, SP(100, 0, 0, 0, button));
        CHECK(s.ons(Push) == 1);
    }
    Sim s(c);
    s.run(5, SP(100));
    s.run(300, SP(100, 0, 0, 0, 0x2000));               // B is not a push
    CHECK(s.ons(Push) == 0);
}

static void test_push_each_button_is_its_own_press() {
    Config c = saved_config();
    c.t[Push].delay_ms = 0;
    c.t[Push].hold_ms = 100;
    Sim s(c);
    s.run(5, SP(100));
    s.run(300, SP(100, 0, 0, 0, 0x1000));                // A pushes once and stays down
    CHECK(s.ons(Push) == 1);
    s.run(300, SP(100, 0, 0, 0, 0x1000 | BTN_X));        // X is pressed while A is still held: another push
    CHECK(s.ons(Push) == 2);
    s.run(300, SP(100, 0, 0, 0, 0x1000 | BTN_X));        // holding both does not repeat
    CHECK(s.ons(Push) == 2);
    Sim q(c);
    q.run(5, SP(100));
    q.run(300, SP(100, 0, 0, 0, 0x1000 | BTN_X));        // both in the same instant: one push
    CHECK(q.ons(Push) == 1);
}

static void test_footplant_works_with_a_or_x() {
    Config c = saved_config();
    c.t[Footplant].delay_ms = 0;
    for (unsigned button : {0x1000u, BTN_X}) {
        Sim s(c);
        s.run(5, SP(201));
        s.run(100, SP(201, 255));                        // grab held
        s.run(5, SP(201, 255, 0, 0, button));
        CHECK(s.ons(Footplant) == 1 && s.ons(Push) == 0);
    }
    Sim n(c);                                            // no-foot grab stays on B only
    n.run(5, SP(201));
    n.run(100, SP(201, 255));
    n.run(5, SP(201, 255, 0, 0, BTN_X));
    CHECK(n.ons(NoFootGrab) == 0);
    n.run(100, SP(201, 255));
    n.run(250, SP(201, 255, 0, 0, 0x2000));                // B, after the saved delay of 200 ms
    CHECK(n.ons(NoFootGrab) == 1);
}

static void test_footplant_with_x_after_the_saved_delay() {
    Sim s(saved_config());                               // footplant waits 200 ms, as saved
    s.run(5, SP(201));
    s.run(100, SP(201, 255));
    const auto t0 = s.now_ms();
    s.run(5, SP(201, 255, 0, 0, BTN_X));
    s.run(400, SP(201, 255, 0, 0, BTN_X));
    CHECK(s.on_time(Footplant) == t0 + 200);
}

// ---------------------------------------------------------------------------------------------
// MIDI controller messages

static void test_cc_wire_bytes() {
    const MidiMsg m = encode_cc(0, 20, 64);
    CHECK(m.status == 0xB0 && m.d1 == 20 && m.d2 == 64);
    CHECK(pack(m) == 0x004014B0u);
    for (int ch = 0; ch < 16; ++ch)
        for (int cc = 0; cc < 128; ++cc) {
            const MidiMsg a = encode_cc(ch, cc, 127), b = encode_cc(ch, cc, 0), hi = encode_cc(ch, cc, 999), lo = encode_cc(ch, cc, -5);
            CHECK(a.status == (0xB0 | ch) && a.d1 == cc && a.d2 == 127 && b.d2 == 0);
            CHECK(hi.d2 == 127 && lo.d2 == 0 && a.d1 < 0x80);     // the value is always a legal data byte
            CHECK(is_cc(a) && !is_note_message(a));
        }
    CHECK(describe(encode_cc(2, 21, 100)).find("controller 21 = 100") != std::string::npos);
    MidiAudit audit;                                      // controller messages are not mistaken for foreign notes
    CHECK(audit.received(encode_cc(0, 20, 5), 0) && audit.foreign() == 0);
}

static std::vector<CcMessage> mix(CcMixer& m, std::int64_t now, std::initializer_list<double> v, const Config& cfg) {
    double t[CcCount] = {};
    int i = 0;
    for (double x : v) if (i < CcCount) t[i++] = x;
    return m.update(now, t, cfg);
}

static void test_mixer_sends_first_value_then_only_changes() {
    Config cfg;
    CcMixer m;
    auto first = mix(m, 0, {10, 20, 30, 40, 50}, cfg);
    CHECK(first.size() == 6);                                     // everything is sent once at the start
    CHECK(first[0].cc == 20 && first[0].value == 10 && first[4].cc == 24 && first[4].value == 50 && first[0].channel == 0);
    CHECK(mix(m, 20000, {10, 20, 30, 40, 50}, cfg).empty());       // nothing changed
    auto moved = mix(m, 40000, {10, 25, 30, 40, 50}, cfg);
    CHECK(moved.size() == 1 && moved[0].index == CcRotX && moved[0].value == 25);
    m.reset();
    CHECK(mix(m, 60000, {10, 25, 30, 40, 50}, cfg).size() == 6);  // after a reset everything is sent again
}

static void test_mixer_rate_limit_and_min_step() {
    Config cfg;
    cfg.cc[CcRotX].max_rate_hz = 20;                              // at most one message per 50 ms
    CcMixer m;
    int sent = 0;
    for (int ms = 0; ms < 1000; ++ms) {
        double v = ms * 0.1;                                       // 0 to 100 over one second
        for (const auto& x : mix(m, ms * 1000LL, {0, v, 0, 0, 0}, cfg)) if (x.index == CcRotX) ++sent;
    }
    CHECK(sent <= 22 && sent >= 15);                               // about 20 per second
    Config cfg2;
    cfg2.cc[CcRotY].min_step = 5;
    CcMixer q;
    std::vector<int> values;
    for (int ms = 0; ms < 300; ++ms) {
        double v = 50 + ms * 0.03;                                 // a slow creep, 9 units in 300 ms
        for (const auto& x : mix(q, ms * 1000LL, {0, 0, v, 0, 0}, cfg2)) if (x.index == CcRotY) values.push_back(x.value);
    }
    CHECK(values.size() >= 2 && values.size() <= 4);               // not one message per unit
    for (std::size_t i = 1; i < values.size(); ++i) CHECK(values[i] - values[i - 1] >= 5 || i + 1 == values.size());
    double stopped = 59.0;                                         // once it stops moving, the exact value is sent
    int last = values.back();
    for (int ms = 300; ms < 600; ++ms)
        for (const auto& x : mix(q, ms * 1000LL, {0, 0, stopped, 0, 0}, cfg2)) if (x.index == CcRotY) last = x.value;
    CHECK(last == 59);
}

static void test_mixer_smoothing() {
    Config cfg;
    cfg.cc[CcManual].smooth_ms = 100;
    cfg.cc[CcManual].max_rate_hz = 1000;
    CcMixer m;
    mix(m, 0, {0, 0, 0, 0, 0}, cfg);
    std::vector<int> v;
    for (int ms = 1; ms <= 1000; ++ms)
        for (const auto& x : mix(m, ms * 1000LL, {0, 0, 0, 0, 100}, cfg)) if (x.index == CcManual) v.push_back(x.value);
    CHECK(v.size() > 20);                                          // it glides, it does not jump
    CHECK(v.front() < 20 && v.back() == 100);
    for (std::size_t i = 1; i < v.size(); ++i) CHECK(v[i] > v[i - 1]);
    int at100ms = 0;                                               // one time constant: about 63% of the way
    CcMixer q;
    mix(q, 0, {0, 0, 0, 0, 0}, cfg);
    for (int ms = 1; ms <= 100; ++ms)
        for (const auto& x : mix(q, ms * 1000LL, {0, 0, 0, 0, 100}, cfg)) if (x.index == CcManual) at100ms = x.value;
    CHECK(at100ms >= 58 && at100ms <= 68);
}

static void test_mixer_addresses_and_limits() {
    Config cfg;
    cfg.channel = 3;
    cfg.cc[CcRotZ].channel = 10;
    cfg.cc[CcRotZ].cc = 74;
    cfg.cc[CcIntensity].enabled = false;
    CcMixer m;
    auto first = mix(m, 0, {99, 0, 0, 400, -50}, cfg);
    CHECK(first.size() == 5);                                      // the disabled one sends nothing
    for (const auto& x : first) {
        CHECK(x.value >= 0 && x.value <= 127);
        if (x.index == CcRotZ) CHECK(x.channel == 9 && x.cc == 74 && x.value == 127);   // 400 is clamped
        if (x.index == CcManual) CHECK(x.channel == 2 && x.value == 0);                    // -50 is clamped
    }
    cfg.cc[CcRotZ].cc = 75;                                        // the number is edited while running
    auto again = mix(m, 20000, {99, 0, 0, 400, -50}, cfg);
    CHECK(again.size() == 1 && again[0].cc == 75);                 // the current value goes to the new number at once
}

// ---------------------------------------------------------------------------------------------
// intensity (the energy meter)

static Config cc_config() {
    Config c = test_config();
    c.intensity.decay_s = 0;                                       // tests that need the fall switch it on
    return c;
}

static void test_intensity_jump_adds_energy_and_it_falls() {
    Config c = cc_config();
    c.intensity.decay_s = 6;
    Sim s(c);
    s.run(5, S(100));
    CHECK(near(s.e.energy(), 0, 1e-9) && near(s.e.cc_targets()[CcIntensity], 0, 1e-9));
    s.run(1, S(201));                                              // a jump: +0.10
    CHECK(near(s.e.energy(), 0.10, 0.003));
    CHECK(near(s.e.cc_targets()[CcIntensity], 12.7, 0.5));
    s.run(6000, S(201));                                           // one time constant later: 37% of it
    CHECK(near(s.e.energy(), 0.10 * std::exp(-1.0), 0.004));
    Sim n(cc_config());                                            // decay_s = 0: it never falls
    n.run(5, S(100));
    n.run(1, S(201));
    n.run(60000, S(201));
    CHECK(near(n.e.energy(), 0.10, 0.003));
}

static void test_intensity_saturates_and_can_go_down() {
    Sim s(cc_config());
    s.run(5, S(100));
    for (int i = 0; i < 300; ++i) { s.run(1, S(201)); s.run(310, S(201)); s.run(1, S(100)); s.run(310, S(100)); }
    CHECK(s.e.energy() > 0.99 && s.e.energy() <= 1.0);              // it approaches the top and never goes over
    CHECK(s.e.cc_targets()[CcIntensity] <= 127.0);
    Config c = cc_config();
    c.intensity.event[Jump] = 0.5;
    c.intensity.event[Bail] = -0.3;                                 // a bail takes energy away
    Sim b(c);
    b.run(5, S(100));
    b.run(1, S(201));
    CHECK(near(b.e.energy(), 0.5, 0.003));
    b.run(5, S(504));
    CHECK(near(b.e.energy(), 0.35, 0.004));
}

static void test_intensity_land_is_scaled_by_how_hard() {
    Config c = cc_config();
    c.intensity.event[Jump] = 0;
    Sim curb(c);
    curb.run(20, M(100, 0));
    curb.run(100, M(201, 0));
    curb.run(100, M(201, -0.10));
    curb.run(1, M(100, -0.15));
    Sim big(c);
    flight(big, 3.0, 0.0);
    CHECK(curb.e.energy() > 0 && curb.e.energy() < 0.02);          // a curb: 0.10 * 10/127
    CHECK(big.e.energy() > 0.07 && big.e.energy() < 0.10);         // a big drop: 0.10 * 110/127
    Config d = c;
    d.intensity.land_scaled = false;
    Sim plain(d);
    flight(plain, 3.0, 0.0);
    CHECK(near(plain.e.energy(), 0.10, 0.003));
}

static void test_intensity_grows_while_held_and_with_speed() {
    Config c = cc_config();
    c.intensity.event[Grind] = 0;
    c.intensity.grind_per_s = 0.2;
    Sim s(c);
    s.run(5, S(100));
    s.run(1000, S(402));                                            // a grind held for one second
    CHECK(near(s.e.energy(), 1.0 - std::exp(-0.2), 0.01));
    Config d = cc_config();
    d.intensity.speed_per_s = 1.0;
    Sim fast(d);
    fast.run(1000, M(100, 0, 8.0));
    CHECK(near(fast.e.energy(), 1.0 - std::exp(-1.0), 0.02));       // at top speed
    Sim slow(d);
    slow.run(1000, M(100, 0, 1.0));
    CHECK(near(slow.e.energy(), 0, 1e-9));                          // at or below speed_min: nothing
}

static void test_intensity_mapping_and_rotation_steps() {
    Config c = cc_config();
    c.intensity.event[Jump] = 0.5;
    c.intensity.min_value = 20;
    c.intensity.max_value = 100;
    Sim s(c);
    s.run(5, S(100));
    CHECK(near(s.e.cc_targets()[CcIntensity], 20, 1e-9));           // no energy = min_value
    s.run(1, S(201));
    CHECK(near(s.e.cc_targets()[CcIntensity], 60, 0.5));            // half energy
    c.intensity.curve = 2;
    s.e.set_config(c);
    s.run(1, S(201));
    CHECK(near(s.e.cc_targets()[CcIntensity], 40, 0.5));            // the curve bends it
    Config a = cc_config();
    a.rotation.angle = true;                                        // a kickflip is two steps of 180: two bumps
    a.intensity.event[RotZ] = 0.12;
    Sim k(a);
    k.run(5, M(201, 1.0));
    k.run(410, M(201, 1.0, 0, 0, 0, 917));
    CHECK(near(k.e.energy(), 0.12 + 0.12 * (1 - 0.12), 0.01));
}

// ---------------------------------------------------------------------------------------------
// the three spin faders

static void test_spin_fader_accumulates_and_stays_after_landing() {
    Sim s(cc_config());
    s.run(5, M(201, 1.0));
    s.run(1000, M(201, 1.0, 0, 0, 0, 900));                          // 900 degrees about z in the air
    CHECK(near(s.e.cc_targets()[CcRotZ], 900.0 / 3600.0 * 127.0, 0.3));   // 31.75
    CHECK(near(s.e.cc_targets()[CcRotX], 0, 1e-9) && near(s.e.cc_targets()[CcRotY], 0, 1e-9));
    const double before = s.e.cc_targets()[CcRotZ];
    s.run(10, M(100, 0));
    s.run(5000, M(100, 0));                                          // landed and rolled for five seconds
    CHECK(near(s.e.cc_targets()[CcRotZ], before, 1e-9));              // it stays where it was
}

static void test_spin_fader_opposite_spin_lowers_and_clamps() {
    Sim s(cc_config());
    s.run(5, M(201, 1.0));
    s.run(1000, M(201, 1.0, 0, 0, 0, 900));
    s.run(500, M(201, 1.0, 0, 0, 0, -900));                          // the other way: 450 degrees back
    CHECK(near(s.e.cc_targets()[CcRotZ], 450.0 / 3600.0 * 127.0, 0.4));
    s.run(3000, M(201, 1.0, 0, 0, 0, -900));
    CHECK(near(s.e.cc_targets()[CcRotZ], 0, 1e-9));                   // it stops at 0
    s.run(1000, M(201, 1.0, 0, 0, 0, 900));
    CHECK(near(s.e.cc_targets()[CcRotZ], 900.0 / 3600.0 * 127.0, 0.4));   // and rises again at once
    Sim t(cc_config());
    t.run(5, M(201, 1.0));
    t.run(5000, M(201, 1.0, 0, 0, 0, 1000));                         // 5000 degrees: past the range
    CHECK(near(t.e.cc_targets()[CcRotZ], 127, 1e-9));
    t.run(100, M(201, 1.0, 0, 0, 0, -1000));
    CHECK(t.e.cc_targets()[CcRotZ] < 127 - 3.0);                       // and it comes down straight away
}

static void test_spin_fader_ignores_slow_spin_and_uses_each_axis() {
    Sim s(cc_config());
    s.run(5, M(201, 1.0));
    s.run(10000, M(201, 1.0, 0, 100, 100, 100));                      // below min_rate (150): drift does not count
    CHECK(near(s.e.cc_targets()[CcRotX] + s.e.cc_targets()[CcRotY] + s.e.cc_targets()[CcRotZ], 0, 1e-9));
    Sim a(cc_config());
    a.run(5, M(201, 1.0));
    a.run(1000, M(201, 1.0, 0, 360, 0, 0));                           // only x
    CHECK(near(a.e.cc_targets()[CcRotX], 10.0 / 100.0 * 127.0 * 1.0, 0.3));   // 360 degrees = 10% of 3600 = 12.7
    CHECK(near(a.e.cc_targets()[CcRotY], 0, 1e-9) && near(a.e.cc_targets()[CcRotZ], 0, 1e-9));
}

static void test_spin_fader_on_the_ground_only_for_y_and_only_quick_turns() {
    Sim s(cc_config());
    s.run(5, M(100, 0));
    s.run(500, M(100, 0, 3, 600, 600, 600));                          // quick spin on the ground
    CHECK(near(s.e.cc_targets()[CcRotX], 0, 1e-9) && near(s.e.cc_targets()[CcRotZ], 0, 1e-9));   // x and z are air-only
    CHECK(near(s.e.cc_targets()[CcRotY], 300.0 / 3600.0 * 127.0, 0.3));                          // y counts a revert: 300 degrees
    Sim c(cc_config());
    c.run(5, M(100, 0));
    c.run(3000, M(100, 0, 3, 0, 250, 0));                             // a hard carve: under ground_min_rate (300)
    CHECK(near(c.e.cc_targets()[CcRotY], 0, 1e-9));
}

static void test_spin_fader_settings() {
    Config c = cc_config();
    c.rot_cc[2].invert = true;
    c.rot_cc[2].start = 64;
    Sim s(c);
    CHECK(near(s.e.cc_targets()[CcRotZ], 64, 1e-9));                   // starts at `start`
    s.run(5, M(201, 1.0));
    s.run(1000, M(201, 1.0, 0, 0, 0, 900));
    CHECK(near(s.e.cc_targets()[CcRotZ], 64 - 31.75, 0.3));             // inverted: the same spin lowers it

    Config r = cc_config();
    r.rot_cc[2].range_deg = 360;
    Sim q(r);
    q.run(5, M(201, 1.0));
    q.run(200, M(201, 1.0, 0, 0, 0, 900));                              // 180 degrees of 360
    CHECK(near(q.e.cc_targets()[CcRotZ], 63.5, 0.8));

    Config d = cc_config();
    d.rot_cc[2].start = 10;
    d.rot_cc[2].decay_per_s = 10;
    Sim w(d);
    w.run(5, M(201, 1.0));
    w.run(1000, M(201, 1.0, 0, 0, 0, 900));                             // 10 + 31.75
    const double up = w.e.cc_targets()[CcRotZ];
    w.run(1000, M(201, 1.0));                                           // one second of calm: drifts 10 units back
    CHECK(near(w.e.cc_targets()[CcRotZ], up - 10, 0.3));
    w.run(10000, M(201, 1.0));
    CHECK(near(w.e.cc_targets()[CcRotZ], 10, 1e-6));                    // and stops at start

    Sim n(cc_config());                                                 // no game data: nothing moves
    n.run(5, M(201, 1.0));
    n.run(500, NOGAME());
    n.run(500, S(201));                                                 // no motion data either
    CHECK(near(n.e.cc_targets()[CcRotZ], 0, 1e-9));

    Config e = cc_config();                                             // an edited start value moves the fader to it
    Sim m(e);
    Config f = e;
    f.rot_cc[0].start = 100;
    m.e.set_config(f);
    CHECK(near(m.e.cc_targets()[CcRotX], 100, 1e-9) == false);          // the target is refreshed on the next sample ...
    m.run(2, S(100));
    CHECK(near(m.e.cc_targets()[CcRotX], 100, 0.5));                    // ... and then it is there
}

// ---------------------------------------------------------------------------------------------
// the manual stick

static void test_manual_cc_scales_the_stick_range() {
    Sim s(cc_config());                                                  // manual delay 0
    s.run(5, SP(100));
    s.run(20, SP(100, 0, 0, 13000));
    CHECK(near(s.e.cc_targets()[CcManual], 0, 0.01));                     // the bottom of the range = 0
    s.run(20, SP(100, 0, 0, 21500));
    CHECK(near(s.e.cc_targets()[CcManual], 63.5, 0.1));                   // the middle
    s.run(20, SP(100, 0, 0, 30000));
    CHECK(near(s.e.cc_targets()[CcManual], 127, 0.01));                   // the top
    s.run(20, SP(100, 0, 0, 31000));                                      // still held by the hysteresis, but capped
    CHECK(near(s.e.cc_targets()[CcManual], 127, 0.01));
    s.run(20, SP(100, 0, 0, -21500));                                     // nose manual: the same scale by default
    CHECK(near(s.e.cc_targets()[CcManual], 63.5, 0.1));
    s.run(400, SP(100, 0, 0, 0));                                         // the stick goes back: the value goes back
    CHECK(near(s.e.cc_targets()[CcManual], 0, 1e-9));
}

static void test_manual_cc_follows_the_manual_note() {
    Config c;                                                             // the real default: the manual waits 200 ms
    Sim s(c);
    s.run(5, SP(100));
    s.run(100, SP(100, 0, 0, 20000));
    CHECK(near(s.e.cc_targets()[CcManual], 0, 1e-9));                     // the crouch before an ollie does not move it
    s.run(150, SP(100, 0, 0, 20000));
    CHECK(near(s.e.cc_targets()[CcManual], (20000 - 13000) / 17000.0 * 127.0, 0.1));    // 52.3
    Config d = c;
    d.manual_cc.only_while_manual = false;                                // follow the stick at once
    Sim q(d);
    q.run(5, SP(100));
    q.run(50, SP(100, 0, 0, 20000));
    CHECK(near(q.e.cc_targets()[CcManual], (20000 - 13000) / 17000.0 * 127.0, 0.1));
}

static void test_manual_cc_signed_mode_idle_and_conditions() {
    Config c = cc_config();
    c.manual_cc.signed_mode = true;
    Sim s(c);
    CHECK(near(s.e.cc_targets()[CcManual], 64, 1e-9));                    // idle is the middle in signed mode
    s.run(5, SP(100));
    s.run(20, SP(100, 0, 0, 30000));
    CHECK(near(s.e.cc_targets()[CcManual], 127, 0.01));                   // manual up
    s.run(300, SP(100, 0, 0, 0));
    s.run(20, SP(100, 0, 0, -30000));
    CHECK(near(s.e.cc_targets()[CcManual], 1, 0.01));                     // nose manual down
    Config d = cc_config();
    d.manual_cc.idle = 10;
    Sim q(d);
    q.run(5, SP(100));
    CHECK(near(q.e.cc_targets()[CcManual], 10, 1e-9));
    q.run(20, SP(201, 0, 0, 25000));                                      // in the air: no manual
    CHECK(near(q.e.cc_targets()[CcManual], 10, 1e-9));
    Input in = S(100);
    in.pad.connected = false;
    in.pad.ry = 25000;                                                    // a pad that is not there
    q.run(20, in);
    CHECK(near(q.e.cc_targets()[CcManual], 10, 1e-9));
}

// ---------------------------------------------------------------------------------------------
// everything together, and the config file

static void test_cc_random_sessions_stay_legal() {
    for (unsigned seed = 1; seed <= 40; ++seed) {
        std::uint32_t rng = seed * 2654435761u;
        auto next = [&rng]() { rng = rng * 1664525u + 1013904223u; return rng >> 8; };
        Config cfg;
        cfg.rotation.angle = seed % 2;
        TriggerEngine e(cfg);
        CcMixer mixer;
        const unsigned states[] = {100, 103, 200, 201, 300, 402, 504, 99};
        unsigned state = 100;
        Input in;
        std::int64_t now = 0;
        std::map<int, int> count;
        for (int step = 0; step < 5000; ++step) {
            now += 1000 + (next() % 3) * 1000;
            if (next() % 35 == 0) state = states[next() % (sizeof states / sizeof *states)];
            in.game_ok = next() % 300 != 0;
            in.state = state;
            in.pad.connected = next() % 40 != 0;
            if (next() % 30 == 0) { in.pad.lt = next() % 256; in.pad.ry = static_cast<int>(next() % 65536) - 32768; in.pad.rx = static_cast<int>(next() % 65536) - 32768; }
            in.motion.ok = next() % 20 != 0;
            if (next() % 25 == 0) { in.motion.y = (next() % 4000) / 1000.0; in.motion.speed = (next() % 9000) / 1000.0;
                                    for (int a = 0; a < 3; ++a) in.motion.w_dps[a] = next() % 3 == 0 ? static_cast<double>(next() % 6000) - 3000 : 0; }
            e.update(now, in);
            for (int i = 0; i < CcCount; ++i) CHECK(std::isfinite(e.cc_targets()[i]) && e.cc_targets()[i] >= 0 && e.cc_targets()[i] <= 127);
            CHECK(e.energy() >= 0 && e.energy() <= 1);
            for (const auto& m : mixer.update(now, e.cc_targets(), cfg)) {
                CHECK(m.value >= 0 && m.value <= 127 && m.cc >= 20 && m.cc <= 25 && m.channel == 0);
                ++count[m.index];
            }
        }
        const double seconds = static_cast<double>(now) / 1e6;
        for (const auto& kv : count) CHECK(kv.second <= seconds * 60 + 3);          // never above 60 messages per second
    }
}

static void test_ini_cc_sections() {
    Config c;
    ParseResult r;
    parse_ini(
        "[cc_intensity]\ncc = 30\nchannel = 2\nsmooth_ms = 250\nmin_step = 2\nmax_rate_hz = 30\ndecay_s = 10\nmin_value = 5\nmax_value = 120\ncurve = 0.5\n"
        "land_scaled = false\njump = 0.2\nbail = -0.4\nrot_z = 0.3\ngrind_per_s = 0.5\nspeed_per_s = 0.1\n"
        "[cc_rot_z]\ncc = 31\nrange_deg = 7200\nmin_rate = 200\nground_min_rate = 400\ncontext = any\ninvert = true\nstart = 64\ndecay_per_s = 3\n"
        "[cc_manual]\nenabled = false\nonly_while_manual = false\nmode = signed\nidle = 12\n",
        c, r);
    CHECK(r.ok() && r.warnings.empty());
    CHECK(c.cc[CcIntensity].cc == 30 && c.cc[CcIntensity].channel == 2 && c.cc[CcIntensity].smooth_ms == 250 && c.cc[CcIntensity].min_step == 2);
    CHECK(c.cc[CcIntensity].max_rate_hz == 30 && c.intensity.decay_s == 10 && c.intensity.min_value == 5 && c.intensity.max_value == 120);
    CHECK(c.intensity.curve == 0.5 && !c.intensity.land_scaled && c.intensity.event[Jump] == 0.2 && c.intensity.event[Bail] == -0.4);
    CHECK(c.intensity.event[RotZ] == 0.3 && c.intensity.grind_per_s == 0.5 && c.intensity.speed_per_s == 0.1);
    CHECK(c.cc[CcRotZ].cc == 31 && c.rot_cc[2].range_deg == 7200 && c.rot_cc[2].min_rate == 200 && c.rot_cc[2].ground_min_rate == 400);
    CHECK(c.rot_cc[2].context == Context::Any && c.rot_cc[2].invert && c.rot_cc[2].start == 64 && c.rot_cc[2].decay_per_s == 3);
    CHECK(!c.cc[CcManual].enabled && !c.manual_cc.only_while_manual && c.manual_cc.signed_mode && c.manual_cc.idle == 12);
    Config back;
    ParseResult b;
    parse_ini(write_ini(c), back, b);
    CHECK(b.ok() && b.warnings.empty() && write_ini(back) == write_ini(c));      // everything survives a save and a load
}

static void test_ini_cc_defaults_errors_and_warnings() {
    Config c;
    CHECK(c.cc[CcIntensity].cc == 20 && c.cc[CcRotX].cc == 21 && c.cc[CcRotY].cc == 22 && c.cc[CcRotZ].cc == 23 && c.cc[CcManual].cc == 24);
    CHECK(c.rot_cc[1].context == Context::GroundOrAir && c.rot_cc[0].context == Context::Air && c.rot_cc[2].context == Context::Air);
    CHECK(c.rot_cc[0].range_deg == 3600 && c.manual_cc.only_while_manual && !c.manual_cc.signed_mode);
    Config d;
    ParseResult e;
    parse_ini("[cc_rot_x]\ncc = 200\n[cc_intensity]\nmin_value = 100\nmax_value = 50\n[cc_manual]\nmode = banana\n", d, e);
    CHECK(!e.ok() && e.errors.size() == 3);
    Config f;
    ParseResult g;
    parse_ini("[cc_rot_x]\ncc = 22\n", f, g);                                    // the same number as cc_rot_y
    CHECK(g.ok() && g.warnings.size() == 1 && g.warnings[0].find("fight") != std::string::npos);
    Config h;
    ParseResult k;
    parse_ini("[cc_rot_x]\ncc = 22\nchannel = 3\n", h, k);                       // a different channel is fine
    CHECK(k.ok() && k.warnings.empty());
}

// ---------------------------------------------------------------------------------------------
// the mapper (send one note or one controller on repeat, to MIDI-map in Ableton)

static void test_note_names_are_abletons() {
    CHECK(note_name(36) == "C1" && note_name(37) == "C#1" && note_name(60) == "C3" && note_name(0) == "C-2" && note_name(127) == "G8");
    CHECK(note_name(48) == "C2" && note_name(51) == "D#2" && note_name(-1) == "?" && note_name(128) == "?");
}

static void test_mapper_resolves_notes() {
    Config cfg;                                                    // the saved defaults: land = 37, push = 38
    MapTarget t;
    std::string err;
    CHECK(resolve_note(cfg, "37", t, err) && t.kind == MapTarget::Note && t.number == 37 && t.channel == 0);
    CHECK(t.label.find("C#1") != std::string::npos && t.label.find("LAND") != std::string::npos);   // it says whose note it is
    CHECK(t.velocity == 60);                                       // the general velocity
    CHECK(resolve_note(cfg, "land", t, err) && t.number == 37);
    CHECK(resolve_note(cfg, "ROT_Z", t, err) && t.number == 50);
    CHECK(resolve_note(cfg, "note 44", t, err) && t.number == 44 && t.label.find("GRAB") != std::string::npos);
    CHECK(resolve_note(cfg, "99", t, err) && t.number == 99 && t.label.find("the ") == std::string::npos);   // not one of ours: still sent
    CHECK(!resolve_note(cfg, "128", t, err) && !err.empty());
    CHECK(!resolve_note(cfg, "banana", t, err) && err.find("land") != std::string::npos);                    // the error lists the names
    Config d;
    d.channel = 3;
    d.t[Land].channel = 10;
    d.t[Land].velocity = 90;
    CHECK(resolve_note(d, "land", t, err) && t.channel == 9 && t.velocity == 90);                           // the trigger's own settings
    CHECK(resolve_note(d, "38", t, err) && t.channel == 2);                                                  // the general channel
}

static void test_mapper_resolves_controllers() {
    Config cfg;
    MapTarget t;
    std::string err;
    CHECK(resolve_cc(cfg, "20", t, err) && t.kind == MapTarget::Cc && t.number == 20 && t.channel == 0);
    CHECK(t.label.find("INTENSITY") != std::string::npos);
    CHECK(resolve_cc(cfg, "cc21", t, err) && t.number == 21 && t.label.find("ROT_X") != std::string::npos);
    CHECK(resolve_cc(cfg, "intensity", t, err) && t.number == 20);
    CHECK(resolve_cc(cfg, "Manual", t, err) && t.number == 24);
    CHECK(resolve_cc(cfg, "cc_rot_z", t, err) && t.number == 23);                  // the section name works too
    CHECK(resolve_cc(cfg, "74", t, err) && t.number == 74);                          // any number can be sent
    CHECK(!resolve_cc(cfg, "200", t, err) && !err.empty());
    CHECK(!resolve_cc(cfg, "volume", t, err) && err.find("intensity") != std::string::npos);
    Config d;
    d.cc[CcRotY].cc = 77;
    d.cc[CcRotY].channel = 5;
    CHECK(resolve_cc(d, "rot_y", t, err) && t.number == 77 && t.channel == 4);       // follows your edited numbers
    CHECK(resolve_cc(d, "77", t, err) && t.label.find("ROT_Y") != std::string::npos);
}

static void test_mapper_command_parser() {
    Config cfg;
    MapCommand c;
    c = parse_map_command(cfg, "");
    CHECK(c.type == MapCommand::Stop);
    CHECK(parse_map_command(cfg, "  Q ").type == MapCommand::Quit && parse_map_command(cfg, "exit").type == MapCommand::Quit);
    CHECK(parse_map_command(cfg, "list").type == MapCommand::List && parse_map_command(cfg, "?").type == MapCommand::List);
    c = parse_map_command(cfg, "n 37");
    CHECK(c.type == MapCommand::Select && c.target.kind == MapTarget::Note && c.target.number == 37);
    c = parse_map_command(cfg, "note37");
    CHECK(c.type == MapCommand::Select && c.target.kind == MapTarget::Note && c.target.number == 37);
    c = parse_map_command(cfg, "n37");
    CHECK(c.type == MapCommand::Select && c.target.kind == MapTarget::Note && c.target.number == 37);
    c = parse_map_command(cfg, "c 20");
    CHECK(c.type == MapCommand::Select && c.target.kind == MapTarget::Cc && c.target.number == 20);
    c = parse_map_command(cfg, "cc20");
    CHECK(c.type == MapCommand::Select && c.target.kind == MapTarget::Cc && c.target.number == 20);
    c = parse_map_command(cfg, "c20");
    CHECK(c.type == MapCommand::Select && c.target.kind == MapTarget::Cc && c.target.number == 20);
    c = parse_map_command(cfg, "n land");
    CHECK(c.type == MapCommand::Select && c.target.kind == MapTarget::Note && c.target.number == 37);
    c = parse_map_command(cfg, "c intensity");
    CHECK(c.type == MapCommand::Select && c.target.kind == MapTarget::Cc && c.target.number == 20);
    c = parse_map_command(cfg, "c rot_x");
    CHECK(c.type == MapCommand::Select && c.target.kind == MapTarget::Cc && c.target.number == 21);
    c = parse_map_command(cfg, "n rot_x");
    CHECK(c.type == MapCommand::Select && c.target.kind == MapTarget::Note && c.target.number == 48);
    c = parse_map_command(cfg, "land");                                  // unambiguous bare names are fine
    CHECK(c.type == MapCommand::Select && c.target.kind == MapTarget::Note && c.target.number == 37);
    c = parse_map_command(cfg, "intensity");
    CHECK(c.type == MapCommand::Select && c.target.kind == MapTarget::Cc && c.target.number == 20);
    c = parse_map_command(cfg, "manual");                                // both a note and a controller: it asks
    CHECK(c.type == MapCommand::Error && c.message.find("n manual") != std::string::npos && c.message.find("c manual") != std::string::npos);
    c = parse_map_command(cfg, "rot_z");
    CHECK(c.type == MapCommand::Error);
    c = parse_map_command(cfg, "cc_rot_z");                              // the full section name is not ambiguous
    CHECK(c.type == MapCommand::Select && c.target.kind == MapTarget::Cc && c.target.number == 23);
    c = parse_map_command(cfg, "n 500");
    CHECK(c.type == MapCommand::Error && !c.message.empty());
    c = parse_map_command(cfg, "dance");
    CHECK(c.type == MapCommand::Error && c.message.find("list") != std::string::npos);
}

static void test_mapper_glued_flags() {
    std::string v;
    CHECK(glued_value("--test-cc20", "--test-cc", v) && v == "20");
    CHECK(glued_value("--test-note37", "--test-note", v) && v == "37");
    CHECK(glued_value("--test-cc=intensity", "--test-cc", v) && v == "intensity");
    CHECK(!glued_value("--test-cc", "--test-cc", v));                        // nothing glued on
    CHECK(!glued_value("--test-cc=", "--test-cc", v));
    CHECK(!glued_value("--test-midi", "--test-cc", v));
    CHECK(glued_value("--test-note-on", "--test-note", v) == true && v == "-on");   // resolve_note then rejects it with a clear error
}

static void test_mapper_patterns() {
    CHECK(cc_sweep_value(0) == 0 && cc_sweep_value(1500000) == 127 && cc_sweep_value(3000000) == 0);
    CHECK(cc_sweep_value(750000) >= 62 && cc_sweep_value(750000) <= 65);
    CHECK(cc_sweep_value(2250000) >= 62 && cc_sweep_value(2250000) <= 65);
    int prev = 0;
    for (std::int64_t t = 0; t <= 1500000; t += 10000) { const int v = cc_sweep_value(t); CHECK(v >= prev && v >= 0 && v <= 127); prev = v; }   // rises smoothly
    prev = 127;
    for (std::int64_t t = 1500000; t <= 3000000; t += 10000) { const int v = cc_sweep_value(t); CHECK(v <= prev); prev = v; }                 // and falls
    CHECK(cc_sweep_value(7000000) == cc_sweep_value(1000000));                // it repeats
    CHECK(note_pulse_on(0) && note_pulse_on(399999) && !note_pulse_on(400000) && !note_pulse_on(999999) && note_pulse_on(1000000));
    int on = 0;
    for (std::int64_t t = 0; t < 10000000; t += 1000) on += note_pulse_on(t) ? 1 : 0;
    CHECK(on == 4000);                                                         // on 40% of the time
}

// ---------------------------------------------------------------------------------------------
// v5: every trigger can be a pulse or a gate, and the speed controller

static Config mode_config(int trigger, bool gate) {
    Config c = test_config();
    c.t[trigger].gate = gate;
    return c;
}

static void test_defaults_modes_are_unchanged() {
    Config c;
    for (int t = 0; t < TriggerCount; ++t) CHECK(c.t[t].gate == (t == Grind || t == Manual || t == Grab));
    ParseResult r;
    Config d;
    parse_ini(write_ini(c), d, r);
    CHECK(r.ok() && r.warnings.empty() && write_ini(d) == write_ini(c));
    Config e;
    ParseResult g;
    parse_ini("[jump]\nmode = gate\n[grind]\nmode = pulse\n", e, g);
    CHECK(g.ok() && g.warnings.empty() && e.t[Jump].gate && !e.t[Grind].gate);
    Config f;
    ParseResult h;
    parse_ini(write_ini(e), f, h);
    CHECK(h.ok() && f.t[Jump].gate && !f.t[Grind].gate);
}

static void test_jump_as_a_gate_is_held_while_airborne() {
    Sim s(mode_config(Jump, true));
    s.run(10, S(100));
    s.run(1, S(200));
    s.run(900, S(201));                                  // far longer than a pulse
    CHECK(s.ons(Jump) == 1 && s.offs(Jump) == 0);
    s.run(1, S(100));
    s.run(100, S(100));
    CHECK(s.offs(Jump) == 1 && s.ons(Jump) == 1);        // ends after the landing (off delay 0)
}

static void test_land_as_a_gate_keeps_the_drop_velocity() {
    Sim s(mode_config(Land, true));
    flight(s, 3.0, 0.0);
    s.run(300, M(100, 0));
    CHECK(s.ons(Land) == 1 && s.offs(Land) == 0);
    CHECK(on_velocity(s, Land) == 110);
    s.run(1, M(201, 0.2));
    s.run(10, M(201, 0.2));
    CHECK(s.offs(Land) == 1);
}

static void test_bail_as_a_gate_and_rearm() {
    Config c = mode_config(Bail, true);
    c.t[Bail].off_delay_ms = 50;
    Sim s(c);
    s.run(10, S(201));
    s.run(1, S(504));
    s.run(700, S(504));
    CHECK(s.ons(Bail) == 1 && s.offs(Bail) == 0);        // held while the skater is off the board
    s.run(200, S(100));
    CHECK(s.offs(Bail) == 1);
    s.run(1, S(504));                                    // not ridden for 500 ms yet: not re-armed
    s.run(100, S(504));
    CHECK(s.ons(Bail) == 1);
    s.run(100, S(100));
    s.run(700, S(201));
    s.run(1, S(504));
    CHECK(s.ons(Bail) == 2);
}

static void test_push_footplant_nofoot_as_gates() {
    Config c = saved_config();
    c.t[Push].gate = c.t[Footplant].gate = c.t[NoFootGrab].gate = true;
    c.t[Push].delay_ms = 0;
    Sim s(c);
    s.run(5, SP(100));
    s.run(600, SP(100, 0, 0, 0, 0x1000));
    CHECK(s.ons(Push) == 1 && s.offs(Push) == 0);        // held while the button is down
    s.run(10, SP(100));
    CHECK(s.offs(Push) == 1);
    Sim f(c);
    f.run(5, SP(201));
    f.run(100, SP(201, 255));
    f.run(5, SP(201, 255, 0, 0, BTN_X));
    f.run(600, SP(201, 255, 0, 0, BTN_X));
    CHECK(f.ons(Footplant) == 1 && f.offs(Footplant) == 0);
    f.run(10, SP(201, 255));
    CHECK(f.offs(Footplant) == 1);
    f.run(5, SP(201, 255, 0, 0, 0x2000));
    f.run(600, SP(201, 255, 0, 0, 0x2000));
    CHECK(f.ons(NoFootGrab) == 1 && f.offs(NoFootGrab) == 0);
}

static void test_grind_manual_grab_as_pulses() {
    Config c = test_config();
    c.t[Grind].gate = c.t[Manual].gate = c.t[Grab].gate = false;
    Sim g(c);
    g.run(5, S(100));
    g.run(900, S(402));                                  // a long grind is one fixed-length note
    CHECK(g.ons(Grind) == 1 && g.offs(Grind) == 1);
    CHECK(g.off_time(Grind) - g.on_time(Grind) == 300);
    g.run(200, S(100));
    g.run(100, S(402));                                  // a second grind: a second note
    CHECK(g.ons(Grind) == 2);
    Sim m(c);
    m.run(5, SP(100));
    m.run(900, SP(100, 0, 0, 20000));
    CHECK(m.ons(Manual) == 1 && m.offs(Manual) == 1);
    Config d = c;
    d.t[Grab].delay_ms = 200;
    Sim q(d);
    q.run(5, SP(201));
    q.run(100, SP(201, 255));                            // a tap shorter than the delay: nothing
    q.run(300, SP(201, 0));
    CHECK(q.ons(Grab) == 0 && q.e.cancelled(Grab) == 1);
    q.run(600, SP(201, 255));
    CHECK(q.ons(Grab) == 1 && q.offs(Grab) == 1);
}

static void test_speed_cc() {
    Config c = cc_config();
    c.speed_cc.min_speed = 2;
    c.speed_cc.max_speed = 10;
    Sim s(c);
    s.run(5, M(100, 0, 0.0));
    CHECK(near(s.e.cc_targets()[CcSpeed], 0, 1e-9));
    s.run(5, M(100, 0, 6.0));
    CHECK(near(s.e.cc_targets()[CcSpeed], 63.5, 0.01));
    s.run(5, M(100, 0, 30.0));
    CHECK(near(s.e.cc_targets()[CcSpeed], 127, 1e-9));
    s.run(5, S(100));                                    // no motion data: 0
    CHECK(near(s.e.cc_targets()[CcSpeed], 0, 1e-9));
    c.speed_cc.invert = true;
    s.e.set_config(c);
    s.run(5, M(100, 0, 6.0));
    CHECK(near(s.e.cc_targets()[CcSpeed], 63.5, 0.01));
    s.run(5, M(100, 0, 10.0));
    CHECK(near(s.e.cc_targets()[CcSpeed], 0, 1e-9));
    c.speed_cc.invert = false;
    c.speed_cc.curve = 2;
    s.e.set_config(c);
    s.run(5, M(100, 0, 6.0));
    CHECK(near(s.e.cc_targets()[CcSpeed], 127 * 0.25, 0.01));
    Config d;
    ParseResult r;
    parse_ini("[cc_speed]\ncc = 33\nmin_speed = 1\nmax_speed = 15\ncurve = 0.5\ninvert = true\nsmooth_ms = 80\n", d, r);
    CHECK(r.ok() && r.warnings.empty() && d.cc[CcSpeed].cc == 33 && d.speed_cc.max_speed == 15 && d.speed_cc.invert && d.cc[CcSpeed].smooth_ms == 80);
    Config e;
    ParseResult g;
    parse_ini("[cc_speed]\nmin_speed = 9\nmax_speed = 3\n", e, g);
    CHECK(!g.ok());
    CHECK(Config().cc[CcSpeed].cc == 25);
    Config back;
    ParseResult b;
    parse_ini(write_ini(d), back, b);
    CHECK(b.ok() && b.warnings.empty() && write_ini(back) == write_ini(d));
}

static void test_http_lite() {
    http::Request r;
    CHECK(http::parse_head("GET /api/live?since=12&x=1 HTTP/1.1\r\nHost: 127.0.0.1:8431\r\nX-Noiseflip: 1\r\nContent-Length: 5", r));
    CHECK(r.method == "GET" && r.path == "/api/live" && r.query == "since=12&x=1");
    CHECK(r.header("host") == "127.0.0.1:8431" && r.header("x-noiseflip") == "1" && r.header("content-length") == "5");
    CHECK(http::query_param(r.query, "since") == "12" && http::query_param(r.query, "x") == "1" && http::query_param(r.query, "y").empty());
    CHECK(!http::parse_head("DELETE / HTTP/1.1\r\n", r) && !http::parse_head("garbage", r) && !http::parse_head("GET nothing HTTP/1.1\r\n", r));
    CHECK(http::safe_path("/index.html") && http::safe_path("/fonts/Archivo.ttf") && http::safe_path("/a-b_c.png"));
    CHECK(!http::safe_path("/../x") && !http::safe_path("/a/../b") && !http::safe_path("//x") && !http::safe_path("/a\\b") &&
          !http::safe_path("/c:/x") && !http::safe_path("/.hidden") && !http::safe_path("x") && !http::safe_path("/a b") && !http::safe_path("/a%2e"));
    CHECK(std::string(http::mime_for("/a.PNG")) == "image/png" && std::string(http::mime_for("/x.ttf")) == "font/ttf");
    CHECK(http::host_ok("127.0.0.1:8431", 8431) && http::host_ok("LocalHost:8431", 8431));
    CHECK(!http::host_ok("evil.com:8431", 8431) && !http::host_ok("127.0.0.1:80", 8431) && !http::host_ok("", 8431));
    CHECK(http::json_escape("a\"b\\c\n\x01") == "\"a\\\"b\\\\c\\n\\u0001\"");
    std::vector<http::Setting> v;
    std::string why;
    CHECK(http::parse_settings("general|velocity|70\njump|mode|gate\r\n\ngeneral|channel|3\n", v, why) && v.size() == 3);
    CHECK(http::settings_to_ini(v) == "[general]\nvelocity = 70\nchannel = 3\n[jump]\nmode = gate\n");
    CHECK(!http::parse_settings("general|velocity", v, why) && !http::parse_settings("Gen|x|1", v, why) && !http::parse_settings("", v, why));
    CHECK(!http::parse_settings("general|midi_port|a;b", v, why) && !http::parse_settings("general|x|a\x01", v, why));
    // a browser edit applied on top of the current config, as the program does it
    Config c;
    ParseResult res;
    http::parse_settings("jump|mode|gate\ncc_speed|cc|40\ngeneral|velocity|90", v, why);
    parse_ini(http::settings_to_ini(v), c, res);
    CHECK(res.ok() && res.warnings.empty() && c.t[Jump].gate && c.cc[CcSpeed].cc == 40 && c.velocity == 90 && c.channel == 1);
    Config d;
    ParseResult bad;
    http::parse_settings("general|velocity|900", v, why);
    parse_ini(http::settings_to_ini(v), d, bad);
    CHECK(!bad.ok());
    CHECK(http::preset_name_ok("Street 2 (loud)") && http::preset_name_ok("a") && http::preset_name_ok("my-preset_v1.2"));
    CHECK(!http::preset_name_ok("") && !http::preset_name_ok(" a") && !http::preset_name_ok("a ") && !http::preset_name_ok("a.") && !http::preset_name_ok(".a"));
    CHECK(!http::preset_name_ok("_previous") && !http::preset_name_ok("a/b") && !http::preset_name_ok("a\\b") && !http::preset_name_ok("a:b") && !http::preset_name_ok("con"));
    CHECK(!http::preset_name_ok("NUL.txt") && !http::preset_name_ok("lpt1") && !http::preset_name_ok(std::string(49, 'a')) && http::preset_name_ok(std::string(48, 'a')));
    CHECK(!http::preset_name_ok("na\xc3\xafve") && !http::preset_name_ok("a*b") && !http::preset_name_ok("a\nb"));
    const std::string resp = http::response(200, "text/plain", "hi");
    CHECK(resp.find("HTTP/1.1 200 OK") == 0 && resp.find("Content-Length: 2") != std::string::npos && resp.substr(resp.size() - 2) == "hi");
}

int main() {
    test_jump_land_bail_basics();
    test_pop_states_one_note();
    test_flicker_is_one_signal();
    test_grind_is_not_jump_or_land();
    test_bail_rearm();
    test_reset_no_phantom();
    test_grind_gate();
    test_grind_off_delay_exact();
    test_manual_gate_and_hysteresis();
    test_manual_entry_is_strict();
    test_manual_nose_manual_is_same_trigger();
    test_manual_needs_ground();
    test_manual_off_delay_and_min_hold();
    test_manual_cross_axis_and_direction();
    test_grab_delay_and_hold();
    test_grab_tap_shorter_than_delay_is_ignored();
    test_grab_works_on_ground_and_with_rt();
    test_grab_trigger_hysteresis();
    test_no_grab_in_manual_or_grind();
    test_grab_ends_when_manual_or_grind_starts();
    test_push_delay_and_no_repeat();
    test_push_cancelled_if_not_on_ground();
    test_push_not_in_air_or_without_pad();
    test_footplant_and_nofootgrab();
    test_footplant_order_does_not_matter();
    test_footplant_not_in_manual_or_grind();
    test_push_then_grab_becomes_footplant_not_push();
    test_game_lost_ends_gates_and_cancels_push();
    test_disabled_trigger_and_overrides();
    test_config_change_mid_note_keeps_note_off_correct();
    test_release_all();
    test_ini_round_trip();
    test_ini_overrides_and_errors();
    test_ini_buttons_and_axis();
    test_chain_ok();
    test_chain_rejections();
    test_end_to_end_session();
    test_manual_default_delay_ignores_the_crouch();
    test_manual_ignore_states();
    test_land_velocity_follows_the_drop();
    test_land_velocity_uses_apex_minus_landing_not_takeoff();
    test_land_velocity_curve_and_off_switch();
    test_speed_bonus();
    test_rotation_default_thresholds();
    test_rotation_each_axis_and_sign();
    test_rotation_not_on_the_ground_by_default();
    test_rotation_one_note_per_trick_with_tail_and_hysteresis();
    test_rotation_pulse_length_and_gate_mode();
    test_rotation_needs_motion_and_can_be_disabled();
    test_fingerflip();
    test_fingerflip_needs_air_grab_and_enough_spin();
    test_fingerflip_grab_started_during_the_spin();
    test_ini_new_keys();
    test_ini_new_errors_and_warnings();
    test_read_motion_ok();
    test_read_motion_rejections();
    test_angle_kickflip_is_two_notes();
    test_angle_slow_rotation_is_ignored();
    test_angle_axes_are_independent();
    test_angle_double_flip_retriggers_and_cuts();
    test_angle_note_step_climbs_within_a_trick();
    test_angle_negative_spin_and_reversals();
    test_angle_resets_on_landing_and_when_idle();
    test_angle_context_step_and_per_trigger_override();
    test_angle_switch_per_trigger_and_midflight();
    test_angle_stall_cannot_become_an_angle();
    test_angle_fingerflip_counts_from_the_grab();
    test_angle_fingerflip_resets_when_released_and_needs_air();
    test_angle_fingerflip_axes_and_independence();
    test_ini_rotation_section();
    test_defaults_angle_mode_and_rot_y_on_the_ground();
    test_revert_plays_the_shuvit_note();
    test_carving_is_not_a_revert();
    test_manual_revert_counts();
    test_revert_needs_the_ground_or_air_context();
    test_revert_and_air_are_separate();
    test_revert_in_rate_mode_too();
    test_left_stick_switches_the_mode();
    test_the_switch_changes_what_a_kickflip_plays();
    test_the_switch_respects_its_settings();
    test_switch_versus_the_ini_and_per_trigger_choices();
    test_ini_reverts_and_switch_keys();
    test_defaults_are_the_saved_ini();
    test_the_saved_ini_loads();
    test_button_lists_and_repeated_keys();
    test_push_works_with_a_or_x();
    test_push_each_button_is_its_own_press();
    test_footplant_works_with_a_or_x();
    test_footplant_with_x_after_the_saved_delay();
    test_cc_wire_bytes();
    test_mixer_sends_first_value_then_only_changes();
    test_mixer_rate_limit_and_min_step();
    test_mixer_smoothing();
    test_mixer_addresses_and_limits();
    test_intensity_jump_adds_energy_and_it_falls();
    test_intensity_saturates_and_can_go_down();
    test_intensity_land_is_scaled_by_how_hard();
    test_intensity_grows_while_held_and_with_speed();
    test_intensity_mapping_and_rotation_steps();
    test_spin_fader_accumulates_and_stays_after_landing();
    test_spin_fader_opposite_spin_lowers_and_clamps();
    test_spin_fader_ignores_slow_spin_and_uses_each_axis();
    test_spin_fader_on_the_ground_only_for_y_and_only_quick_turns();
    test_spin_fader_settings();
    test_manual_cc_scales_the_stick_range();
    test_manual_cc_follows_the_manual_note();
    test_manual_cc_signed_mode_idle_and_conditions();
    test_cc_random_sessions_stay_legal();
    test_ini_cc_sections();
    test_ini_cc_defaults_errors_and_warnings();
    test_note_names_are_abletons();
    test_mapper_resolves_notes();
    test_mapper_resolves_controllers();
    test_mapper_command_parser();
    test_mapper_glued_flags();
    test_mapper_patterns();
    test_http_lite();
    test_defaults_modes_are_unchanged();
    test_jump_as_a_gate_is_held_while_airborne();
    test_land_as_a_gate_keeps_the_drop_velocity();
    test_bail_as_a_gate_and_rearm();
    test_push_footplant_nofoot_as_gates();
    test_grind_manual_grab_as_pulses();
    test_speed_cc();
    test_wire_encoding_every_note_and_channel();
    test_wire_known_values();
    test_wire_random_sessions_reach_the_synth_unchanged();
    test_audit_recognises_its_own_echo();
    test_audit_ignores_velocity_and_note_off_spelling();
    test_audit_catches_a_second_sender();
    test_audit_ignores_non_note_messages_and_reports_missing_echoes();
    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
