// Unattended test flight, see autotest.cpp.
#pragma once

namespace autotest {

// Once per frame, before drone::tick().
void tick();

// Stops a running test.
void abort();

}  // namespace autotest
