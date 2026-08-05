#pragma once

/**
 * \file clock.h
 * 60 Hz send-cadence helper (plan Rev 3 D2: everything sends every frame,
 * capped at 60 Hz; docs/design/mod-coop/00-network.md §6).
 *
 * The game loop is the clock: the sender runs once per frame and asks the
 * clock whether a 60 Hz tick is due. The clock is fed a monotonic microsecond
 * timestamp (std::chrono::steady_clock) and returns true exactly once per
 * tick, catching up missed intervals in a single step so the cadence never
 * drifts and never bursts.
 */

#include <dolphin/types.h>

namespace dusk::net {

class NetClock {
public:
    static constexpr u32 kRateHz = 60;
    /// 1/60 s, rounded up to whole microseconds.
    static constexpr u64 kIntervalUs = 1000000 / kRateHz + 1;  // 16667

    /// Armed so the first tick is due kIntervalUs after `nowUs` (0 = epoch;
    /// use the default and feed real timestamps in Update()).
    explicit NetClock(u64 nowUs = 0) { Reset(nowUs); }

    /// Re-arms the clock at `nowUs` without ticking.
    void Reset(u64 nowUs) {
        nextTickUs_ = nowUs + kIntervalUs;
        frame_ = 0;
        lastTickUs_ = nowUs;
    }

    /// Feed the current time. Returns true exactly once per 60 Hz interval;
    /// if more than one interval elapsed, the clock steps past them in one
    /// call (catch-up) and returns true once, keeping the frame count honest.
    bool Tick(u64 nowUs) {
        if (nowUs < nextTickUs_) {
            return false;
        }
        const u64 steps = (nowUs - nextTickUs_) / kIntervalUs + 1;
        nextTickUs_ += steps * kIntervalUs;
        frame_ += 1;
        lastTickUs_ = nowUs;
        return true;
    }

    /// Number of ticks since construction/reset.
    [[nodiscard]] u32 Frame() const { return frame_; }

    [[nodiscard]] u64 LastTickUs() const { return lastTickUs_; }
    static constexpr u64 IntervalUs() { return kIntervalUs; }

private:
    u64 nextTickUs_ = 0;
    u64 lastTickUs_ = 0;
    u32 frame_ = 0;
};

}  // namespace dusk::net
