#ifndef CORE_LIBS_PRX_COMMON_SLOWLOG_HPP
#define CORE_LIBS_PRX_COMMON_SLOWLOG_HPP

#include <chrono>
#include <cstdlib>

// Stutter diagnosis, off unless APS5_SLOW_FRAME_MS=<n> is in the environment (debug_env.txt in a game's workspace).
// Then frames whose flip interval reaches n ms are printed with their timing breakdown, and so are the other stalls that
// can cause them: GC suspensions, pipeline compiles. Every line carries t_ms, one steady clock for every library, so
// the lines can be lined up afterwards.
namespace aps5 {

inline double SlowFrameMs() {
    static const double threshold = [] {
        const char* text = std::getenv("APS5_SLOW_FRAME_MS");
        return text != nullptr ? std::strtod(text, nullptr) : 0.0;
    }();
    return threshold;
}

inline bool SlowLogEnabled() { return SlowFrameMs() > 0.0; }

// Shorter stalls than this are not printed: a frame lasts 16.7 ms at 60 FPS.
inline constexpr double SlowStallMs = 2.0;

inline double SteadyMs(std::chrono::steady_clock::time_point time = std::chrono::steady_clock::now()) {
    return std::chrono::duration<double, std::milli>(time.time_since_epoch()).count();
}

inline double ElapsedMs(std::chrono::steady_clock::time_point since) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - since).count();
}

}

#endif
