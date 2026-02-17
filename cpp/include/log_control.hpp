#pragma once

#include <iostream>
#include <streambuf>

namespace hft {

// Keep this false for production-like latency measurements.
inline constexpr bool kEnableHotPathLogging = false;

class NullBuffer : public std::streambuf {
protected:
    int overflow(int c) override {
        return c;
    }
};

class ScopedCoutSilencer {
public:
    explicit ScopedCoutSilencer(bool active) : active_(active), original_(nullptr) {
        if (active_) {
            original_ = std::cout.rdbuf(&null_buffer());
        }
    }

    ~ScopedCoutSilencer() {
        if (active_ && original_) {
            std::cout.rdbuf(original_);
        }
    }

    ScopedCoutSilencer(const ScopedCoutSilencer&) = delete;
    ScopedCoutSilencer& operator=(const ScopedCoutSilencer&) = delete;

private:
    static NullBuffer& null_buffer() {
        static NullBuffer buffer;
        return buffer;
    }

    bool active_;
    std::streambuf* original_;
};

} // namespace hft
