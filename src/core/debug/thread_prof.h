#pragma once
// Sampling profiler for one thread: `vrprof [ms]` logs where the game thread
// spends its time, by module and by 64-byte code bin, so a frame-rate change
// starts from a measurement. Costs nothing until started.

#include <cstdint>

namespace bvr::debug {

void thread_prof_start(uint32_t tid, uint32_t ms);

} // namespace bvr::debug
