// SPDX-License-Identifier: AGPL-3.0-or-later
#include <catch2/catch_session.hpp>

#include "dsp/MossFormerMaskNet.h"

int main (int argc, char* argv[])
{
    const int result = Catch::Session().run (argc, argv);
    MossFormerMaskNet::instance().resetForTests();
    return result;
}
