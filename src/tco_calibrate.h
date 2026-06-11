// tco_calibrate.h -- heap-OOM (broken-TCO) threshold calibration for ]suites.
// Isolated in its own translation unit so <windows.h>'s macro footprint stays
// out of Listener.cpp (and the rest of the interpreter headers).
#pragma once
#include <iosfwd>

// Find this machine's heap-OOM (broken-TCO) iteration threshold by spawning
// child copies of THIS interpreter on a deep non-tail recursion proxy, exactly
// as bench/calibrate_tco.ps1 does -- a trial fails by running OUT OF MEMORY, so
// it must run in a throwaway child process, not in the calling session.
// Returns the -I: soak count (threshold rounded up to the next million), or 0
// if calibration could not run (non-Windows, spawn failure, or no overflow
// within the ramp cap).  Streams human-readable progress to `out`.
long long calibrate_tco_threshold(std::ostream& out);
