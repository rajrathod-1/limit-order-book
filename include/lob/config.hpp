// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>

#include "lob/types.hpp"

namespace lob {

// Shared by both the array book and the std::map baseline, so the two are
// always configured identically and a benchmark comparison is apples to apples.
struct EngineConfig {
    // The price grid. `scale.floor_price()` is tick 0.
    TickScale scale = kPennyTicks;

    // Width of the price band, in ticks. ArrayBook allocates this many levels
    // per side up front; prices outside the band are rejected and counted. The
    // replay driver sizes this from the data so it never binds in practice.
    std::size_t tick_capacity = 1u << 16;

    // Order pool and index capacity. This is the hard ceiling on simultaneously
    // resting orders, and the reason the hot path never allocates.
    std::size_t max_orders = 1u << 20;
};

}  // namespace lob
