// SPDX-License-Identifier: AGPL-3.0-or-later
#include <catch2/catch_session.hpp>

#include <cstdio>
#include <cstdlib>
#include <iostream>

int main (int argc, char* argv[])
{
    const int result = Catch::Session().run (argc, argv);
    std::cout.flush();
    std::cerr.flush();
    std::fflush (nullptr);

    // The shared ORT session intentionally has process lifetime. ORT 1.22's
    // macOS thread pool can throw while the loader tears down its globals
    // after main(), so short-lived test hosts must leave process cleanup to
    // the OS while preserving Catch's real result code.
    std::_Exit (result);
}
