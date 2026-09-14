// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <atomic>
#include <cstdint>

/**
 * Single realtime HQ lease per host process (plan: in-process atomic owner
 * token). Plugin scanners and other processes are naturally excluded because
 * the token never leaves this address space.
 *
 * Lease rules:
 *  - only instances that actually play HQ in realtime may CAS-acquire;
 *    construction, prepare, validation and offline renders never take it;
 *  - the owner keeps the lease through stop, seek, Mixdown and DOP, so a
 *    second track never causes tonal drift between engines;
 *  - the lease is released when the owner leaves HQ or is destroyed, and a
 *    successor can only pick it up at its next playback generation.
 */
class HqInstanceLease
{
public:
    static std::uint64_t owner()
    {
        return state.load (std::memory_order_acquire);
    }

    static bool tryAcquire (std::uint64_t id)
    {
        std::uint64_t expected = 0;
        return state.compare_exchange_strong (expected, id,
                                              std::memory_order_acq_rel,
                                              std::memory_order_acquire);
    }

    static bool tryRelease (std::uint64_t id)
    {
        std::uint64_t expected = id;
        return state.compare_exchange_strong (expected, 0,
                                              std::memory_order_acq_rel,
                                              std::memory_order_acquire);
    }

private:
    static std::atomic<std::uint64_t> state;
};

// definition lives in the processor translation unit
