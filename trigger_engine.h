// trigger_engine.h
//
// Turns samples (physics state + gamepad + board motion) into 13 triggers and MIDI note actions.
// No platform code, so it is unit tested on any machine. All numbers come from config.h.
//
//   Pulses (fixed length, one note-on and one note-off each):
//     JUMP        ground -> air
//     LAND        air -> ground; velocity follows the drop height (apex - landing)
//     BAIL        -> bail state, then disarmed until the skater has ridden for bail_rearm_ms
//     PUSH        push button pressed on the ground, fires delay_ms later if still valid
//     FOOTPLANT   grab + footplant button
//     NOFOOTGRAB  grab + nofootgrab button
//   Gates (the note is held while the condition is true):
//     GRIND       physics state in the grind list (grinds and slides are one trigger)
//     MANUAL      on the ground and the stick inside the manual range, for delay_ms (manual and nose manual)
//     GRAB        LT/RT pressed on the ground or in the air, but not in a manual or a grind;
//                 the condition must hold delay_ms before the note starts
//   Every trigger can be either (`mode = pulse|gate` in the ini); the lists above are the defaults.
//   A pulse trigger as a gate is held while its condition lasts (jump: airborne, land: after a landing, bail: off the board,
//   push/footplant/nofootgrab: the buttons held); a gate trigger as a pulse plays one note when its condition starts.
//   Rotation triggers:
//     ROT_X/Y/Z   the board spinning faster than `threshold` degrees per second about that axis
//     FINGERFLIP  grab held + the board spinning (any chosen axis) faster than its threshold, in the air
//   The rotation triggers have two ways of working ([rotation] detect, or a trigger's own detect):
//     rate  : the behaviour above, one note when the spin rate passes the threshold
//     angle : the spin rate is integrated into an angle for each axis and every `step_deg` degrees
//             (180 by default) is one note, so a kickflip is two notes and a double flip four.
//             Axes are independent. FINGERFLIP counts only the turning done while the grab is held.
//   REVERTS: ROT_Y listens on the ground too (a revert is a quick 180 about the vertical axis), so it plays the
//   shuvit's note. On the ground only turns faster than `ground_min_rate` count, so carving never plays.
//   Pressing the switch button (left stick by default) flips between angle and rate mode while playing.
//
// Velocity: the trigger's velocity (or [general]), the land drop mapping for LAND, plus a speed bonus
// that grows with the board's horizontal speed.
//
// Continuous controllers (cc_targets(), values 0-127, sent by CcMixer):
//   INTENSITY  an energy meter: every note adds energy, it falls back over time
//   ROT_X/Y/Z  the board's net spin about each axis kept as a fader that stays after landing
//   MANUAL     the manual stick position scaled from its range to 0-127
//
// "One solid signal": a pulse that is pending or sounding ignores a repeat of itself; a gate only
// starts after its delay and only ends after its off delay, so a flickering input is merged.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <vector>

#include "config.h"

namespace skate {

struct Pad {
    bool connected = false;
    int lx = 0, ly = 0, rx = 0, ry = 0;   // raw XInput, -32768..32767 (up = positive)
    int lt = 0, rt = 0;                   // 0..255
    unsigned buttons = 0;                 // XInput button mask
};

// How the skater and the board move. All in metres / seconds / degrees.
struct Motion {
    bool ok = false;
    double y = 0;                         // height of the skater (world y)
    double speed = 0;                     // horizontal speed of the board, m/s
    double w_dps[3] = {0, 0, 0};          // board spin about world x, y, z in degrees per second
};

struct Input {
    bool game_ok = false;                 // the physics state could be read
    std::uint32_t state = 0;
    Pad pad;
    Motion motion;
};

struct Action {
    bool on = false;
    int trigger = 0;
    int note = 0;
    int velocity = 0;
    int channel = 0;                      // 0-15
    std::uint32_t from_state = 0, to_state = 0;
    double measure = -1;                  // LAND: drop height in m. Rotation triggers (rate mode): spin in deg/s. Otherwise -1
    double angle = -1;                    // rotation triggers, angle mode: the angle reached (180, 360, ...), else -1
    int axis = -1;                        // angle mode: 0, 1, 2 = x, y, z
};

class TriggerEngine {
public:
    explicit TriggerEngine(const Config& c = Config()) : cfg_(c) {
        for (int a = 0; a < 3; ++a) rot_acc_[a] = c.rot_cc[a].start / 127.0;
        for (int i = 0; i < CcCount; ++i) cc_value_[i] = 0;
        cc_value_[CcIntensity] = c.intensity.min_value;
        for (int a = 0; a < 3; ++a) cc_value_[CcRotX + a] = c.rot_cc[a].start;
        cc_value_[CcManual] = c.manual_cc.idle >= 0 ? c.manual_cc.idle : (c.manual_cc.signed_mode ? 64 : 0);
    }

    const Config& config() const { return cfg_; }
    // New values take effect immediately. Notes already sounding keep the note/channel they started with.
    void set_config(const Config& c) {
        if (c.rotation.angle != cfg_.rotation.angle) mode_override_.reset();   // the file changed `detect`: the file wins
        for (int a = 0; a < 3; ++a)                                              // a new start value moves the fader to it
            if (c.rot_cc[a].start != cfg_.rot_cc[a].start) rot_acc_[a] = c.rot_cc[a].start / 127.0;
        cfg_ = c;
    }

    // The rotation mode in force right now: the ini's `detect`, or what the switch button last chose.
    bool angle_mode() const { return global_angle(); }
    unsigned mode_switches() const { return mode_switches_; }   // how many times the switch button has been pressed

    // Forget what was seen (e.g. the game stopped giving data). Sounding notes end normally.
    void reset() {
        have_prev_ = false;
        riding_ = false;
        in_air_ = false;
        for (auto& a : angle_) a.clear();
    }

    // Feed one sample. `now_us` is a monotonic time in microseconds. Call this every loop, also when
    // there is no game data (game_ok = false), so note-offs and delays keep running.
    std::vector<Action> update(std::int64_t now_us, const Input& in) {
        std::vector<Action> out;
        cur_ = &in;
        // time since the last sample, capped so a stall (a memory scan, a pause) cannot turn into a huge angle
        dt_us_ = last_update_us_ >= 0 ? std::min<std::int64_t>(std::max<std::int64_t>(now_us - last_update_us_, 0), 50000) : 0;
        last_update_us_ = now_us;
        const bool ok = in.game_ok;
        if (!ok) reset();

        // 1. pulses whose time is up
        for (int t = 0; t < TriggerCount; ++t) {
            Slot& s = slot_[t];
            if (!gate_mode(t) && s.on && now_us >= s.until) release(t, out);
        }

        // 2. what is true right now
        const std::uint32_t st = in.state;
        const bool ground = ok && cfg_.ground.contains(st);
        const bool air = ok && cfg_.air.contains(st);
        const bool grind = ok && cfg_.grind.contains(st);
        const Pad& pad = in.pad;
        const Motion& mo = in.motion;

        // the switch button: flips between angle and rate mode (only a fresh press counts)
        const bool switch_now = pad.connected && cfg_.rotation.switch_button != 0 && (pad.buttons & cfg_.rotation.switch_button) != 0;
        if (switch_now && !switch_prev_) { mode_override_ = !global_angle(); ++mode_switches_; }
        switch_prev_ = switch_now;

        // for the rotation triggers a state can count as "on the ground" without being in the ground list
        // (a manual that has its own state number): [rotation] extra_states
        const bool ground_like = ground || (ok && cfg_.rotation.extra_states.contains(st));

        lt_pressed_ = pad.connected && hysteresis(pad.lt, lt_pressed_);
        rt_pressed_ = pad.connected && hysteresis(pad.rt, rt_pressed_);
        const bool grab_button = (cfg_.grab_lt && lt_pressed_) || (cfg_.grab_rt && rt_pressed_);

        manual_in_range_ = pad.connected && stick_in_range(pad, manual_in_range_);
        const bool manual = ground && manual_in_range_ && !cfg_.manual_ignore.contains(st);

        const bool grab_context = (ground || air) && !manual && !grind;
        const bool grab = grab_context && grab_button;

        // 3. events from the physics state
        if (ok) {
            const bool riding = ground || air;
            if (riding) {
                if (!riding_) { riding_ = true; riding_since_ = now_us; }
                if (!bail_armed_ && now_us - riding_since_ >= ms(cfg_.bail_rearm_ms)) bail_armed_ = true;
            } else {
                riding_ = false;
            }

            // the highest point of this flight, for the landing velocity
            if (air && mo.ok) {
                if (!in_air_) { in_air_ = true; apex_ = mo.y; }
                else apex_ = std::max(apex_, mo.y);
            }

            if (have_prev_ && st != prev_) {
                if (cfg_.bail.contains(st) && !cfg_.bail.contains(prev_)) {
                    if (bail_armed_) { bail_armed_ = false; bail_active_ = true; if (!gate_mode(Bail)) request(Bail, now_us, prev_, st, -1); }
                    else ++slot_[Bail].suppressed;
                } else if (cfg_.ground.contains(prev_) && cfg_.air.contains(st)) {
                    if (!gate_mode(Jump)) request(Jump, now_us, prev_, st, -1);
                } else if (cfg_.air.contains(prev_) && cfg_.ground.contains(st)) {
                    const double drop = (in_air_ && mo.ok) ? std::max(0.0, apex_ - mo.y) : -1.0;
                    if (gate_mode(Land)) { slot_[Land].measure = drop; land_active_ = true; } else request(Land, now_us, prev_, st, drop);
                }
            }
            if (!air) in_air_ = false;
            have_prev_ = true;
            prev_ = st;
        }

        // 4. events from the gamepad (rising edges of a combination)
        // Each of the listed buttons is its own press: pressing X while A is still held is a new push.
        const unsigned push_bits = (ok && pad.connected && ground && !(cfg_.push_block_with_grab && grab_button))
                                       ? (pad.buttons & cfg_.push_button) : 0u;
        const unsigned foot_bits = (pad.connected && grab_context && grab_button) ? (pad.buttons & cfg_.footplant_button) : 0u;
        const unsigned nofoot_bits = (pad.connected && grab_context && grab_button) ? (pad.buttons & cfg_.nofootgrab_button) : 0u;
        if ((push_bits & ~push_prev_) && !gate_mode(Push)) request(Push, now_us, st, st, -1);
        if ((foot_bits & ~foot_prev_) && !gate_mode(Footplant)) request(Footplant, now_us, st, st, -1);
        if ((nofoot_bits & ~nofoot_prev_) && !gate_mode(NoFootGrab)) request(NoFootGrab, now_us, st, st, -1);
        push_prev_ = push_bits;
        foot_prev_ = foot_bits;
        nofoot_prev_ = nofoot_bits;

        // what each trigger's condition is right now: a gate is held while it is true, a pulse in a gate-kind trigger
        // (grind, manual, grab) fires when it becomes true
        const bool bail_state = ok && cfg_.bail.contains(st);
        if (!bail_state) bail_active_ = false;
        cond_now_[Jump] = air;
        if (!ground) land_active_ = false;
        cond_now_[Land] = land_active_ && ground;      // held after a landing, until the skater leaves the ground
        cond_now_[Bail] = bail_active_ && bail_state;
        cond_now_[Grind] = grind;
        cond_now_[Manual] = manual;
        cond_now_[Grab] = grab;
        cond_now_[Push] = push_bits != 0;
        cond_now_[Footplant] = foot_bits != 0;
        cond_now_[NoFootGrab] = nofoot_bits != 0;

        // 5. board rotation and fingerflip
        for (int t = RotX; t <= Fingerflip; ++t) rotation(t, now_us, ok, ground_like, air, grab_button, mo, st, out);

        // 6. pending pulses that are due
        for (int t = 0; t < TriggerCount; ++t) {
            Slot& s = slot_[t];
            if (gate_mode(t) || !s.pending || now_us < s.due) continue;
            s.pending = false;
            bool still_valid = true;
            if (t == Push)  // the skater must still be on the ground, and not have started a grab combo
                still_valid = ground && !(cfg_.push_block_with_grab && grab_button);
            else if (t == Grind || t == Manual || t == Grab) still_valid = cond_now_[t];
            if (still_valid) fire(t, now_us, out);
            else ++s.cancelled;
        }

        // 7. gates
        for (int t = 0; t < RotX; ++t) {
            if (gate_mode(t)) { gate(t, now_us, cond_now_[t], st, out); continue; }
            if (kind_of(t) != Kind::Gate) continue;          // grind, manual, grab as pulses: on the rising edge
            const bool c = cond_now_[t] && cfg_.t[t].enabled;
            if (c && !slot_[t].edge_prev) request(t, now_us, st, st, -1);
            slot_[t].edge_prev = c;
        }
        update_cc(in, ok, ground_like, air, manual);
        cur_ = nullptr;
        return out;
    }

    // Note-offs for everything still sounding (used on exit).
    std::vector<Action> release_all() {
        std::vector<Action> out;
        for (int t = 0; t < TriggerCount; ++t) {
            slot_[t].pending = false;
            if (slot_[t].on) release(t, out);
        }
        return out;
    }

    // Is the stick inside the manual range (used by --pad-test to help tune [manual_stick])?
    bool in_manual_range(const Pad& p) const { return stick_in_range(p, false); }

    // The velocity a landing from `drop` metres would get (before the speed bonus).
    int land_velocity_for(double drop) const { return static_cast<int>(std::lround(land_velocity(drop))); }

    // The controller values (0-127) as the engine wants them right now. CcMixer smooths and sends them.
    const double (&cc_targets() const)[CcCount] { return cc_value_; }
    double energy() const { return energy_; }                    // the intensity meter, 0 to 1

    bool active(int t) const { return slot_[t].on; }
    unsigned long long fired(int t) const { return slot_[t].fired; }
    unsigned long long suppressed(int t) const { return slot_[t].suppressed; }
    unsigned long long cancelled(int t) const { return slot_[t].cancelled; }

private:
    struct Slot {
        bool on = false;
        int note = 0, velocity = 0, channel = 0;
        std::int64_t until = 0, on_since = 0;
        std::uint32_t from = 0, to = 0;
        double measure = -1;
        // pulses
        bool pending = false;
        std::int64_t due = 0;
        // gates
        bool cond = false;
        std::int64_t cond_since = 0, false_since = -1;
        bool edge_prev = false;     // a gate-kind trigger in pulse mode: its condition last time
        // rotation: a flag that stays up for the tail after the rate drops
        bool flag = false;
        std::int64_t flag_false_since = -1;
        unsigned long long fired = 0, suppressed = 0, cancelled = 0;
    };

    static std::int64_t ms(int v) { return static_cast<std::int64_t>(v) * 1000; }

    // In angle mode a rotation trigger is always a series of pulses.
    bool use_angle(int t) const {
        if (!is_rotation(t)) return false;
        const int d = cfg_.t[t].detect;
        return d < 0 ? global_angle() : d == 1;
    }
    bool global_angle() const { return mode_override_ ? *mode_override_ : cfg_.rotation.angle; }
    // Every trigger can be a pulse (a fixed-length note) or a gate (held while its condition lasts): `mode` in the ini.
    // Rotation triggers in angle mode are always a series of pulses.
    bool gate_mode(int t) const { return cfg_.t[t].gate && !use_angle(t); }
    double step_of(int t) const { return cfg_.t[t].step_deg > 0 ? cfg_.t[t].step_deg : cfg_.rotation.step_deg; }
    int note_step_of(int t) const { return cfg_.t[t].note_step != kNoteStepDefault ? cfg_.t[t].note_step : cfg_.rotation.note_step; }

    bool hysteresis(int value, bool was_pressed) const {
        return was_pressed ? value > cfg_.trigger_release : value >= cfg_.trigger_press;
    }

    bool stick_in_range(const Pad& p, bool was_inside) const {
        int v = 0, cross = 0;
        switch (cfg_.manual_axis) {
            case Axis::LX: v = p.lx; cross = p.ly; break;
            case Axis::LY: v = p.ly; cross = p.lx; break;
            case Axis::RX: v = p.rx; cross = p.ry; break;
            default:       v = p.ry; cross = p.rx; break;
        }
        int m = v;
        if (cfg_.manual_direction == Direction::Both) m = std::abs(v);
        else if (cfg_.manual_direction == Direction::Down) m = -v;
        const int h = was_inside ? cfg_.manual_hysteresis : 0;
        if (std::abs(cross) > cfg_.manual_cross_max + h) return false;
        return m >= cfg_.manual_min - h && m <= cfg_.manual_max + h;
    }

    double land_velocity(double drop) const {
        const LandVelocity& l = cfg_.land;
        double x = (drop - l.min_height) / (l.max_height - l.min_height);
        x = std::min(1.0, std::max(0.0, x));
        x = std::pow(x, l.curve);
        return l.min_velocity + (l.max_velocity - l.min_velocity) * x;
    }

    // Base velocity, the land mapping for LAND, then the speed bonus.
    int velocity_for(int t) const {
        const Slot& s = slot_[t];
        double v = cfg_.t[t].velocity > 0 ? cfg_.t[t].velocity : cfg_.velocity;
        if (t == Land && cfg_.land.enabled && s.measure >= 0) v = land_velocity(s.measure);
        const int bonus = cfg_.t[t].speed_bonus >= 0 ? cfg_.t[t].speed_bonus : cfg_.speed_bonus;
        if (bonus > 0 && cur_ && cur_->motion.ok) {
            const double span = cfg_.speed_max - cfg_.speed_min;
            double x = span > 0 ? (cur_->motion.speed - cfg_.speed_min) / span : (cur_->motion.speed >= cfg_.speed_min ? 1.0 : 0.0);
            x = std::min(1.0, std::max(0.0, x));
            v += bonus * x;
        }
        return std::min(127, std::max(1, static_cast<int>(std::lround(v))));
    }
    int channel_of(int t) const { return (cfg_.t[t].channel > 0 ? cfg_.t[t].channel : cfg_.channel) - 1; }
    int hold_of(int t) const { return cfg_.t[t].hold_ms > 0 ? cfg_.t[t].hold_ms : cfg_.hold_ms; }

    void start_note(int t, std::int64_t now_us, std::uint32_t from, std::uint32_t to, std::vector<Action>& out, int note = -1) {
        Slot& s = slot_[t];
        s.on = true;
        s.on_since = now_us;
        s.note = note >= 0 ? note : cfg_.t[t].note;
        s.velocity = velocity_for(t);
        s.channel = channel_of(t);
        s.from = from;
        s.to = to;
        ++s.fired;
        {   // the intensity meter: every note adds energy (a landing in proportion to how hard it was)
            double w = cfg_.intensity.event[t];
            if (t == Land && cfg_.intensity.land_scaled) w *= s.velocity / 127.0;
            add_energy(w);
        }
        Action a;
        a.on = true; a.trigger = t; a.note = s.note; a.velocity = s.velocity; a.channel = s.channel;
        a.from_state = from; a.to_state = to; a.measure = s.measure;
        out.push_back(a);
    }

    void release(int t, std::vector<Action>& out) {
        Slot& s = slot_[t];
        s.on = false;
        s.false_since = -1;
        Action a;
        a.on = false; a.trigger = t; a.note = s.note; a.velocity = 0; a.channel = s.channel;
        out.push_back(a);
    }

    // A pulse event happened. Ignored while a pulse of the same trigger is pending or sounding.
    void request(int t, std::int64_t now_us, std::uint32_t from, std::uint32_t to, double measure) {
        if (!cfg_.t[t].enabled) return;
        Slot& s = slot_[t];
        if (s.pending || s.on) { ++s.suppressed; return; }
        s.pending = true;
        s.due = now_us + ms(cfg_.t[t].delay_ms);
        s.from = from;
        s.to = to;
        s.measure = measure;
    }

    void fire(int t, std::int64_t now_us, std::vector<Action>& out) {
        Slot& s = slot_[t];
        start_note(t, now_us, s.from, s.to, out);
        s.until = now_us + ms(hold_of(t));
    }

    void gate(int t, std::int64_t now_us, bool cond_raw, std::uint32_t state, std::vector<Action>& out) {
        Slot& s = slot_[t];
        const TriggerCfg& k = cfg_.t[t];
        const bool cond = cond_raw && k.enabled;
        if (!s.on) {
            if (cond) {
                if (!s.cond) { s.cond = true; s.cond_since = now_us; }
                if (now_us - s.cond_since >= ms(k.delay_ms)) { start_note(t, now_us, state, state, out); s.false_since = -1; }
            } else {
                s.cond = false;
            }
        } else {
            if (cond) {
                s.false_since = -1;
            } else {
                if (s.false_since < 0) s.false_since = now_us;
                if (now_us - s.false_since >= ms(k.off_delay_ms) && now_us - s.on_since >= ms(k.min_hold_ms)) {
                    s.cond = false;
                    release(t, out);
                }
            }
        }
    }

    // Board rotation (ROT_X/Y/Z) and fingerflip.
    void rotation(int t, std::int64_t now_us, bool ok, bool ground, bool air, bool grab_button, const Motion& mo,
                  std::uint32_t state, std::vector<Action>& out) {
        Slot& s = slot_[t];
        const TriggerCfg& k = cfg_.t[t];

        if (use_angle(t)) {                      // the second way: a note for every step_deg turned
            s.flag = false;
            s.flag_false_since = -1;
            angle_rotation(t, now_us, ok, ground, air, grab_button, mo, state, out);
            return;
        }
        angle_[t].clear();

        // the spin rate this trigger looks at, or -1 when it does not apply right now
        double w = -1;
        if (ok && mo.ok && k.enabled) {
            const bool where = k.context == Context::Air ? air : (k.context == Context::GroundOrAir ? (ground || air) : true);
            if (where) {
                if (t == Fingerflip) {
                    if (grab_button) {
                        w = 0;
                        for (int a = 0; a < 3; ++a)
                            if (k.axes & (1 << a)) w = std::max(w, std::fabs(mo.w_dps[a]));
                    }
                } else {
                    w = std::fabs(mo.w_dps[t - RotX]);
                }
            }
        }
        const bool cond = w >= 0 && (s.flag ? w > k.release : w >= k.threshold);

        // the flag: up as soon as the rate is reached, down after the tail
        if (cond) {
            s.flag_false_since = -1;
            if (!s.flag) {
                s.flag = true;
                if (!gate_mode(t)) request(t, now_us, state, state, w);   // pulse mode: one note per rotation
            }
        } else if (s.flag) {
            if (s.flag_false_since < 0) s.flag_false_since = now_us;
            if (now_us - s.flag_false_since >= ms(k.off_delay_ms)) s.flag = false;
        }
        if (gate_mode(t)) {
            if (cond) s.measure = w;
            gate(t, now_us, cond, state, out);
        }
    }

    // The energy meter moves a share of what is missing (up) or of what is there (down), so it stays between 0 and 1.
    void add_energy(double w) {
        energy_ += w >= 0 ? w * (1.0 - energy_) : w * energy_;
        energy_ = std::min(1.0, std::max(0.0, energy_));
    }

    double manual_axis_value(const Pad& p) const {
        switch (cfg_.manual_axis) {
            case Axis::LX: return p.lx;
            case Axis::LY: return p.ly;
            case Axis::RX: return p.rx;
            default:       return p.ry;
        }
    }

    // The five controller values: intensity, three board-spin faders and the manual stick.
    void update_cc(const Input& in, bool ok, bool ground_like, bool air, bool manual) {
        const double dt = static_cast<double>(dt_us_) / 1e6;
        const IntensityCfg& n = cfg_.intensity;

        // --- intensity: falls back, and grows while a grind, a manual or a grab is held
        if (n.decay_s > 0) energy_ *= std::exp(-dt / n.decay_s);
        if (slot_[Grind].on) add_energy(n.grind_per_s * dt);
        if (slot_[Manual].on) add_energy(n.manual_per_s * dt);
        if (slot_[Grab].on) add_energy(n.grab_per_s * dt);
        if (ok && in.motion.ok && n.speed_per_s != 0) {
            const double span = cfg_.speed_max - cfg_.speed_min;
            const double x = span > 0 ? std::min(1.0, std::max(0.0, (in.motion.speed - cfg_.speed_min) / span)) : 0.0;
            add_energy(n.speed_per_s * x * dt);
        }
        cc_value_[CcIntensity] = n.min_value + (n.max_value - n.min_value) * std::pow(std::min(1.0, std::max(0.0, energy_)), n.curve);

        // --- board spin faders: net spin about each axis, kept after landing
        for (int a = 0; a < 3; ++a) {
            const RotationCc& r = cfg_.rot_cc[a];
            if (ok && in.motion.ok && cfg_.cc[CcRotX + a].enabled) {
                const bool where = r.context == Context::Air ? air : (r.context == Context::GroundOrAir ? (ground_like || air) : true);
                if (where) {
                    const double w = in.motion.w_dps[a] * (r.invert ? -1.0 : 1.0);
                    const int floor_rate = air ? r.min_rate : r.ground_min_rate;      // on the ground only quick turns count
                    if (std::fabs(w) >= floor_rate) rot_acc_[a] += w * dt / r.range_deg;
                }
            }
            if (r.decay_per_s > 0) {                                                  // drift back to the start value
                const double home = r.start / 127.0, step = r.decay_per_s / 127.0 * dt;
                if (rot_acc_[a] > home) rot_acc_[a] = std::max(home, rot_acc_[a] - step);
                else rot_acc_[a] = std::min(home, rot_acc_[a] + step);
            }
            rot_acc_[a] = std::min(1.0, std::max(0.0, rot_acc_[a]));
            cc_value_[CcRotX + a] = rot_acc_[a] * 127.0;
        }

        // --- the manual stick
        const ManualCc& m = cfg_.manual_cc;
        double value = m.idle >= 0 ? m.idle : (m.signed_mode ? 64 : 0);
        const bool active = in.pad.connected && (m.only_while_manual ? slot_[Manual].on : manual);
        if (active) {
            const double raw = manual_axis_value(in.pad);
            const double span = std::max(1, cfg_.manual_max - cfg_.manual_min);
            const double mag = std::min(1.0, std::max(0.0, (std::fabs(raw) - cfg_.manual_min) / span));
            value = m.signed_mode ? 64.0 + (raw >= 0 ? 1.0 : -1.0) * 63.0 * mag : 127.0 * mag;
        }
        cc_value_[CcManual] = value;

        // --- the board's speed
        const SpeedCc& sp = cfg_.speed_cc;
        double sv = 0;
        if (ok && in.motion.ok) {
            const double span = std::max(1e-9, sp.max_speed - sp.min_speed);
            double x = std::min(1.0, std::max(0.0, (in.motion.speed - sp.min_speed) / span));
            x = std::pow(x, sp.curve);
            sv = 127.0 * (sp.invert ? 1.0 - x : x);
        }
        cc_value_[CcSpeed] = sv;
    }

    // Angle mode: turns the spin rate into an angle for each axis and plays a note for every step_deg.
    void angle_rotation(int t, std::int64_t now_us, bool ok, bool ground, bool air, bool grab_button, const Motion& mo,
                        std::uint32_t state, std::vector<Action>& out) {
        Angle& a = angle_[t];
        const TriggerCfg& k = cfg_.t[t];
        bool where = k.context == Context::Air ? air : (k.context == Context::GroundOrAir ? (ground || air) : true);
        if (t == Fingerflip) where = where && grab_button;       // only the turning done while it is held
        if (!(ok && mo.ok && k.enabled && where)) { a.clear(); return; }

        // the ground and the air are separate: the angle of a revert never carries over into the next jump
        const int phase = air ? 1 : 0;
        if (a.phase != phase) { a.clear_angles(); a.idle_since = -1; a.phase = phase; }
        const int min_rate = phase == 0 ? cfg_.rotation.ground_min_rate : cfg_.rotation.min_rate;

        const int axes = t == Fingerflip ? k.axes : (1 << (t - RotX));
        const double dt = static_cast<double>(dt_us_) / 1e6;
        double fastest = 0;
        for (int ax = 0; ax < 3; ++ax) {
            if (!(axes & (1 << ax))) continue;
            a.theta[ax] += mo.w_dps[ax] * dt;                    // signed: the direction can change
            fastest = std::max(fastest, std::fabs(mo.w_dps[ax]));
        }

        // not rotating for a while: forget the partial angle (slow ollie pitch, drift, a finished trick)
        if (fastest < min_rate) {
            if (a.idle_since < 0) a.idle_since = now_us;
            else if (now_us - a.idle_since >= ms(cfg_.rotation.idle_reset_ms)) { a.clear_angles(); a.idle_since = now_us; }
        } else {
            a.idle_since = -1;
        }

        const double step = step_of(t);
        for (int ax = 0; ax < 3; ++ax) {
            if (!(axes & (1 << ax))) continue;
            const int reached = static_cast<int>(std::floor(std::fabs(a.theta[ax]) / step));
            while (a.fired[ax] < reached) {                      // a step never plays twice, even if the board turns back
                ++a.fired[ax];
                ++a.events;
                angle_event(t, ax, a.fired[ax] * step, a.events, now_us, state, out);
            }
        }
    }

    // One step of an angle-mode rotation: a fresh note that cuts the one still sounding.
    void angle_event(int t, int axis, double angle, int event_in_trick, std::int64_t now_us, std::uint32_t state,
                     std::vector<Action>& out) {
        Slot& s = slot_[t];
        s.pending = false;
        if (s.on) release(t, out);
        const int note = std::min(127, std::max(0, cfg_.t[t].note + (event_in_trick - 1) * note_step_of(t)));
        s.measure = -1;
        start_note(t, now_us, state, state, out, note);
        out.back().angle = angle;
        out.back().axis = axis;
        s.until = now_us + ms(cfg_.rotation.hold_ms);
    }

    struct Angle {
        double theta[3] = {0, 0, 0};     // degrees turned about each axis since the trick began
        int fired[3] = {0, 0, 0};        // steps already played, per axis
        int events = 0;                  // steps played in this trick (for note_step)
        std::int64_t idle_since = -1;
        int phase = -1;                  // 0 = on the ground, 1 = in the air
        void clear_angles() { for (int i = 0; i < 3; ++i) { theta[i] = 0; fired[i] = 0; } events = 0; }
        void clear() { clear_angles(); idle_since = -1; phase = -1; }
    };

    Config cfg_;
    Slot slot_[TriggerCount];
    Angle angle_[TriggerCount];
    std::int64_t last_update_us_ = -1, dt_us_ = 0;
    const Input* cur_ = nullptr;

    bool have_prev_ = false;
    std::uint32_t prev_ = 0;
    bool riding_ = false, bail_armed_ = true, bail_active_ = false, land_active_ = false;
    bool cond_now_[TriggerCount] = {};
    std::int64_t riding_since_ = 0;
    bool in_air_ = false;
    double apex_ = 0;
    bool lt_pressed_ = false, rt_pressed_ = false, manual_in_range_ = false;
    unsigned push_prev_ = 0, foot_prev_ = 0, nofoot_prev_ = 0;     // the buttons of each combination that were down last time
    bool switch_prev_ = false;
    double energy_ = 0;                  // the intensity meter, 0 to 1
    double rot_acc_[3] = {0, 0, 0};      // the three spin faders, 0 to 1 of range_deg
    double cc_value_[CcCount];           // what the controllers should be, 0-127
    std::optional<bool> mode_override_;
    unsigned mode_switches_ = 0;
};

}  // namespace skate
