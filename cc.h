// cc.h
//
// Turns the controller targets (0-127, any rate) into MIDI control-change messages: smoothed, rounded, rate limited,
// and only when something changed. No platform code, so it is unit tested.
//
// Per controller ([cc_*] sections of the ini): smooth_ms (softening), min_step (ignore tiny moves, but a value that
// has stopped for 100 ms is still sent exactly), max_rate_hz (messages per second), cc number and channel.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <vector>

#include "config.h"

namespace skate {

struct CcMessage {
    int index = 0;      // CcId
    int channel = 0;    // 0-15
    int cc = 0;
    int value = 0;      // 0-127
};

class CcMixer {
public:
    // Call every loop with the current targets. Returns the messages to send now.
    std::vector<CcMessage> update(std::int64_t now_us, const double (&targets)[CcCount], const Config& cfg) {
        std::vector<CcMessage> out;
        const double dt = last_us_ >= 0 ? std::min(0.2, std::max(0.0, static_cast<double>(now_us - last_us_) / 1e6)) : 0.0;
        last_us_ = now_us;

        for (int i = 0; i < CcCount; ++i) {
            const CcCfg& k = cfg.cc[i];
            State& s = st_[i];
            if (!k.enabled) { s = State(); continue; }

            const double target = std::min(127.0, std::max(0.0, targets[i]));
            const double tau = k.smooth_ms / 1000.0;
            if (!s.init) { s.y = target; s.init = true; }
            else if (tau <= 0) s.y = target;
            else s.y += (target - s.y) * (1.0 - std::exp(-dt / tau));

            const int q = static_cast<int>(std::lround(std::min(127.0, std::max(0.0, s.y))));
            if (q != s.q) { s.q = q; s.q_since = now_us; }

            const int channel = (k.channel > 0 ? k.channel : cfg.channel) - 1;
            const std::int64_t interval = 1000000 / std::max(1, k.max_rate_hz);
            const bool moved_address = s.sent >= 0 && (s.sent_cc != k.cc || s.sent_channel != channel);   // numbers edited while running

            bool send = false;
            if (s.sent < 0 || moved_address) {
                send = true;                                    // the first value, so the receiver starts in step
            } else if (q != s.sent && now_us - s.last_us >= interval) {
                if (std::abs(q - s.sent) >= k.min_step) send = true;
                else if (now_us - s.q_since >= 100000) send = true;   // it has stopped: settle on the exact value
            }
            if (!send) continue;
            s.sent = q;
            s.sent_cc = k.cc;
            s.sent_channel = channel;
            s.last_us = now_us;
            CcMessage m;
            m.index = i; m.channel = channel; m.cc = k.cc; m.value = q;
            out.push_back(m);
        }
        return out;
    }

    // Forget everything: the next update sends every value again.
    void reset() { for (auto& s : st_) s = State(); last_us_ = -1; }

private:
    struct State {
        bool init = false;
        double y = 0;                 // the smoothed value
        int q = -1;                   // y rounded
        std::int64_t q_since = 0;     // since when q has had this value
        int sent = -1, sent_cc = -1, sent_channel = -1;
        std::int64_t last_us = 0;
    };
    State st_[CcCount];
    std::int64_t last_us_ = -1;
};

}  // namespace skate
