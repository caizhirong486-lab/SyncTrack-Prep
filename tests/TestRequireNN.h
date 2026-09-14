// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <catch2/catch_test_macros.hpp>

/** Model-less CI may skip NN acceptance cases. A strict STP_REQUIRE_NN build
    turns the same missing/runtime-unavailable condition into a hard failure. */
template <typename Message>
void stpRequireNnOrSkip (bool available, const Message& message)
{
    if (available)
        return;
#if defined(STP_REQUIRE_NN)
    FAIL (message);
#else
    SKIP (message);
#endif
}
