#pragma once

/**
 * \file clock.h
 * Send-cadence helper (plan Rev 3 D2: everything sends every frame, capped
 * at 60 Hz; docs/design/mod-coop/00-network.md §6).
 *
 * The game loop is the clock: the sender runs once per frame and asks the
 * clock whether a tick is due. The clock is fed a monotonic microsecond
 * timestamp (std::chrono::steady_clock) and returns true exactly once per
 * tick, catching up missed intervals in a single step so the cadence never
 * drifts and never bursts.
 *
 * The default rate is 60 Hz (per-frame player/enemy snapshots). Slower
 * gates are constructed with AtRate() — the TimeSync publisher uses
 * `NetClock::AtRate(1)` for its real 1 Hz cadence (M3.5 fix for the
 * "60 Hz TimeSync" spec violation; the previous 60 Hz default made
 * `oneSecondDue` fire 60×/s).
 */

#include <dolphin/types.h>

namespace dusk::net {

class NetClock {
public:
    static constexpr u32 kDefaultRateHz = 60;
    /// 1/60 s, rounded up to whole microseconds.
    static constexpr u64 kIntervalUs = 1000000 / kDefaultRateHz + 1;  // 16667

    /// Armed so the first tick is due kIntervalUs after `nowUs` (0 = epoch;
    /// use the default and feed real timestamps in Tick()). Runs at the
    /// default 60 Hz cadence.
    explicit NetClock(u64 nowUs = 0) { Reset(nowUs); }

    /// Returns a clock at a custom cadence (e.g. `NetClock::AtRate(1)` for
    /// the 1 Hz TimeSync gate). Kept as a named factory rather than a second
    /// integral ctor: `NetClock(u64 nowUs)` takes the arming timestamp, and
    /// a `NetClock(u32 hz)` overload would silently re-bind existing
    /// `NetClock(1000000)` call sites from "arm at 1 s" to "1 MHz ticks".
    static NetClock AtRate(u32 hz, u64 nowUs = 0) {
        NetClock clock;
        clock.rateHz_ = hz > 0 ? hz : kDefaultRateHz;
        clock.intervalUs_ = 1000000 / clock.rateHz_ +
                            (1000000 % clock.rateHz_ != 0 ? 1 : 0);
        clock.Reset(nowUs);
        return clock;
    }

    /// Re-arms the clock at `nowUs` without ticking.
    void Reset(u64 nowUs) {
        nextTickUs_ = nowUs + intervalUs_;
        frame_ = 0;
        lastTickUs_ = nowUs;
    }

    /// Feed the current time. Returns true exactly once per cadence
    /// interval; if more than one interval elapsed, the clock steps past
    /// them in one call (catch-up) and returns true once, keeping the frame
    /// count honest.
    bool Tick(u64 nowUs) {
        if (nowUs < nextTickUs_) {
            return false;
        }
        const u64 steps = (nowUs - nextTickUs_) / intervalUs_ + 1;
        nextTickUs_ += steps * intervalUs_;
        frame_ += 1;
        lastTickUs_ = nowUs;
        return true;
    }

    /// Number of ticks since construction/reset.
    [[nodiscard]] u32 Frame() const { return frame_; }

    [[nodiscard]] u64 LastTickUs() const { return lastTickUs_; }

    /// Cadence in ticks per second (kDefaultRateHz unless AtRate was used).
    [[nodiscard]] u32 RateHz() const { return rateHz_; }

    /// This clock's tick interval in microseconds.
    [[nodiscard]] u64 IntervalUs() const { return intervalUs_; }

private:
    u64 nextTickUs_ = 0;
    u64 lastTickUs_ = 0;
    u32 frame_ = 0;
    u32 rateHz_ = kDefaultRateHz;
    u64 intervalUs_ = kIntervalUs;
};

}  // namespace dusk::net
