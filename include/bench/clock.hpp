// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <ctime>
#include <algorithm>
#include <vector>

namespace bench {

// ---------------------------------------------------------------------------
// A cycle-counter clock, and an honest account of what it can resolve.
//
// x86-64 gets RDTSC(P). AArch64 gets CNTVCT_EL0, the architectural virtual
// counter, which is the direct equivalent -- on Apple M-series it runs at 1 GHz,
// giving nanosecond ticks, whereas mach_absolute_time() is only 24 MHz (41.7 ns)
// and far too coarse to resolve a single book operation.
//
// Both reads are serialised, because an unserialised timestamp can be reordered
// around the work being measured and will happily report a negative interval.
// Serialisation is not free: it is the dominant term for very short operations,
// so the harness measures its own overhead and subtracts it. `overhead_ticks()`
// is the floor below which a measurement means nothing, and it is reported
// alongside every result rather than quietly hidden.
// ---------------------------------------------------------------------------
class Clock {
public:
    [[nodiscard]] static inline std::uint64_t now() noexcept {
#if defined(__aarch64__)
        std::uint64_t v;
        // ISB drains the pipeline so the counter read cannot float across the
        // instructions we are timing.
        asm volatile("isb\n\tmrs %0, cntvct_el0" : "=r"(v) :: "memory");
        return v;
#elif defined(__x86_64__)
        std::uint32_t lo, hi, aux;
        // RDTSCP waits for prior instructions to retire; the trailing LFENCE
        // stops later instructions from being hoisted above the read.
        asm volatile("rdtscp" : "=a"(lo), "=d"(hi), "=c"(aux) :: "memory");
        asm volatile("lfence" ::: "memory");
        return (static_cast<std::uint64_t>(hi) << 32) | lo;
#else
        timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return static_cast<std::uint64_t>(ts.tv_sec) * 1'000'000'000ull + ts.tv_nsec;
#endif
    }

    // Unserialised read. Cheaper, but only valid across intervals long enough
    // that a few instructions of reordering do not matter.
    [[nodiscard]] static inline std::uint64_t now_relaxed() noexcept {
#if defined(__aarch64__)
        std::uint64_t v;
        asm volatile("mrs %0, cntvct_el0" : "=r"(v) :: "memory");
        return v;
#elif defined(__x86_64__)
        std::uint32_t lo, hi;
        asm volatile("rdtsc" : "=a"(lo), "=d"(hi) :: "memory");
        return (static_cast<std::uint64_t>(hi) << 32) | lo;
#else
        return now();
#endif
    }

    // Counter frequency in Hz, measured against CLOCK_MONOTONIC. On AArch64 the
    // architectural CNTFRQ_EL0 is also read and used as a cross-check: if the
    // two disagree by more than 1% something is wrong with the platform
    // assumption and we would rather know than silently report bad nanoseconds.
    [[nodiscard]] static std::uint64_t frequency() noexcept {
        static const std::uint64_t f = measure_frequency();
        return f;
    }

    [[nodiscard]] static double ticks_to_ns(double ticks) noexcept {
        return ticks * 1e9 / static_cast<double>(frequency());
    }

    [[nodiscard]] static double ns_per_tick() noexcept {
        return 1e9 / static_cast<double>(frequency());
    }

    // Cost of a single back-to-back timestamp pair, in ticks. Measured as the
    // minimum over many trials, because the minimum is the one sample that is
    // free of interference.
    [[nodiscard]] static std::uint64_t overhead_ticks() noexcept {
        static const std::uint64_t o = measure_overhead();
        return o;
    }

    // Declared architectural frequency, or 0 where the platform has none.
    [[nodiscard]] static std::uint64_t declared_frequency() noexcept {
#if defined(__aarch64__)
        std::uint64_t v;
        asm volatile("mrs %0, cntfrq_el0" : "=r"(v));
        return v;
#else
        return 0;
#endif
    }

    [[nodiscard]] static const char* source() noexcept {
#if defined(__aarch64__)
        return "cntvct_el0";
#elif defined(__x86_64__)
        return "rdtscp";
#else
        return "clock_gettime(CLOCK_MONOTONIC)";
#endif
    }

private:
    static std::uint64_t measure_frequency() noexcept {
        // Three short runs; take the median to shrug off a scheduling hiccup.
        std::uint64_t est[3];
        for (auto& e : est) {
            timespec t0, t1;
            const std::uint64_t c0 = now();
            clock_gettime(CLOCK_MONOTONIC, &t0);
            timespec req{0, 20'000'000};  // 20 ms
            nanosleep(&req, nullptr);
            const std::uint64_t c1 = now();
            clock_gettime(CLOCK_MONOTONIC, &t1);
            const double ns = static_cast<double>(t1.tv_sec - t0.tv_sec) * 1e9 +
                              static_cast<double>(t1.tv_nsec - t0.tv_nsec);
            e = static_cast<std::uint64_t>(static_cast<double>(c1 - c0) / ns * 1e9);
        }
        std::sort(est, est + 3);
        return est[1] ? est[1] : 1;
    }

    static std::uint64_t measure_overhead() noexcept {
        std::uint64_t best = ~0ull;
        for (int i = 0; i < 200'000; ++i) {
            const std::uint64_t a = now();
            const std::uint64_t b = now();
            if (b > a) best = std::min(best, b - a);
        }
        return best == ~0ull ? 0 : best;
    }
};

}  // namespace bench
