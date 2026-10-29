#pragma once

#include <algorithm>
#include <cstdint>

namespace comms {

// Classic token bucket. A rate of 0 means unlimited.
class TokenBucket {
public:
    TokenBucket() = default;
    TokenBucket(double rate_per_second, double burst) { configure(rate_per_second, burst); }

    void configure(double rate_per_second, double burst)
    {
        rate_per_ms_ = rate_per_second / 1000.0;
        capacity_ = std::max(burst, 1.0);
        tokens_ = std::min(tokens_ < 0 ? capacity_ : tokens_, capacity_);
    }

    bool unlimited() const { return rate_per_ms_ <= 0.0; }
    double capacity() const { return capacity_; }

    // Takes `cost` tokens if available while leaving at least `reserve` behind.
    bool consume(double cost, uint64_t now_ms, double reserve = 0.0)
    {
        if (unlimited())
            return true;
        refill(now_ms);
        if (tokens_ - cost < reserve)
            return false;
        tokens_ -= cost;
        return true;
    }

private:
    void refill(uint64_t now_ms)
    {
        if (last_ms_ != 0 && now_ms > last_ms_)
            tokens_ = std::min(capacity_, tokens_ + (now_ms - last_ms_) * rate_per_ms_);
        last_ms_ = now_ms;
    }

    double   rate_per_ms_ = 0.0;
    double   capacity_ = 1.0;
    double   tokens_ = -1.0; // full on first configure
    uint64_t last_ms_ = 0;
};

// Bytes counted over one-second windows, for reporting kbps.
class ThroughputMeter {
public:
    void add(uint64_t bytes) { bytes_ += bytes; }

    void tick(uint64_t now_ms)
    {
        if (window_start_ms_ == 0) {
            window_start_ms_ = now_ms;
            return;
        }
        const uint64_t elapsed = now_ms - window_start_ms_;
        if (elapsed < 1000)
            return;
        kbps_ = static_cast<uint32_t>(bytes_ * 8 / elapsed); // bits per ms == kbit/s
        bytes_ = 0;
        window_start_ms_ = now_ms;
    }

    uint32_t kbps() const { return kbps_; }

private:
    uint64_t bytes_ = 0;
    uint64_t window_start_ms_ = 0;
    uint32_t kbps_ = 0;
};

} // namespace comms
