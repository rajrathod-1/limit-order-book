// SPDX-License-Identifier: MIT
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "bench/clock.hpp"

namespace bench {

// ---------------------------------------------------------------------------
// Latency recorder.
//
// Samples are stored raw in a preallocated vector, one 32-bit tick delta per
// operation, and percentiles are computed exactly afterwards by sorting. No
// histogram approximation, no bucket boundary error: a reported p99.9 is the
// actual 99.9th sample.
//
// The alternative -- an HdrHistogram-style log-linear bucket array -- was
// rejected on purpose. It trades exactness for bounded memory, and here memory
// is not the constraint: 100 million samples is 400 MB, and a run that large is
// better split anyway. What matters is that recording a sample is a single
// store into a warm, preallocated buffer, so the act of measuring perturbs the
// thing being measured as little as possible.
// ---------------------------------------------------------------------------
class LatencyRecorder {
public:
    LatencyRecorder() = default;
    explicit LatencyRecorder(std::string name, std::size_t capacity = 0) : name_(std::move(name)) {
        if (capacity) reserve(capacity);
    }

    void reserve(std::size_t n) { samples_.reserve(n); }
    void set_name(std::string n) { name_ = std::move(n); }

    // Record a raw tick delta. Overhead subtraction happens at summary time so
    // that this stays a single append.
    inline void add_ticks(std::uint64_t ticks) {
        samples_.push_back(ticks > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<std::uint32_t>(ticks));
    }

    [[nodiscard]] std::size_t count() const noexcept { return samples_.size(); }
    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    void clear() noexcept { samples_.clear(); sorted_ = false; }

    struct Summary {
        std::string   name;
        std::size_t   count = 0;
        double        min_ns = 0, p50_ns = 0, p90_ns = 0, p99_ns = 0, p999_ns = 0, p9999_ns = 0;
        double        max_ns = 0, mean_ns = 0, stddev_ns = 0;
        std::uint64_t overhead_ticks = 0;
        double        resolution_ns = 0;
    };

    // Percentiles are computed on the raw ticks, then converted, with the
    // measured timestamp-pair overhead subtracted. Subtraction is clamped at
    // zero: a sample that came in under the overhead floor is reported as zero
    // rather than as a negative latency.
    [[nodiscard]] Summary summarize() {
        Summary s;
        s.name           = name_;
        s.count          = samples_.size();
        s.overhead_ticks = Clock::overhead_ticks();
        s.resolution_ns  = Clock::ns_per_tick();
        if (samples_.empty()) return s;

        if (!sorted_) { std::sort(samples_.begin(), samples_.end()); sorted_ = true; }

        const auto oh = static_cast<double>(s.overhead_ticks);
        auto at = [&](double q) {
            const auto idx = static_cast<std::size_t>(q * static_cast<double>(samples_.size() - 1) + 0.5);
            return net_ns(samples_[idx], oh);
        };

        s.min_ns   = net_ns(samples_.front(), oh);
        s.p50_ns   = at(0.50);
        s.p90_ns   = at(0.90);
        s.p99_ns   = at(0.99);
        s.p999_ns  = at(0.999);
        s.p9999_ns = at(0.9999);
        s.max_ns   = net_ns(samples_.back(), oh);

        double sum = 0, sumsq = 0;
        for (std::uint32_t v : samples_) {
            const double x = net_ns(v, oh);
            sum += x;
            sumsq += x * x;
        }
        const double n = static_cast<double>(samples_.size());
        s.mean_ns   = sum / n;
        s.stddev_ns = std::sqrt(std::max(0.0, sumsq / n - s.mean_ns * s.mean_ns));
        return s;
    }

private:
    static double net_ns(std::uint32_t ticks, double overhead) noexcept {
        return Clock::ticks_to_ns(std::max(0.0, static_cast<double>(ticks) - overhead));
    }

    std::string                name_;
    std::vector<std::uint32_t> samples_;
    bool                       sorted_ = false;
};

// A scoped timer that records into a recorder on destruction.
class ScopedSample {
public:
    explicit ScopedSample(LatencyRecorder& r) noexcept : r_(r), t0_(Clock::now()) {}
    ~ScopedSample() { r_.add_ticks(Clock::now() - t0_); }
    ScopedSample(const ScopedSample&) = delete;
    ScopedSample& operator=(const ScopedSample&) = delete;
private:
    LatencyRecorder& r_;
    std::uint64_t    t0_;
};

void print_header();
void print_summary(const LatencyRecorder::Summary& s);

inline void print_header() {
    std::printf("%-26s %10s %8s %8s %8s %8s %9s %9s %9s\n",
                "operation", "count", "min", "p50", "p90", "p99", "p99.9", "p99.99", "max");
    std::printf("%-26s %10s %8s %8s %8s %8s %9s %9s %9s\n",
                "--------------------------", "----------", "--------", "--------",
                "--------", "--------", "---------", "---------", "---------");
}

inline void print_summary(const LatencyRecorder::Summary& s) {
    std::printf("%-26s %10zu %8.1f %8.1f %8.1f %8.1f %9.1f %9.1f %9.1f\n",
                s.name.c_str(), s.count, s.min_ns, s.p50_ns, s.p90_ns, s.p99_ns,
                s.p999_ns, s.p9999_ns, s.max_ns);
}

}  // namespace bench
