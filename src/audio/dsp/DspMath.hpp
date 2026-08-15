#pragma once

namespace naikav::dsp {

// Pi, defined locally rather than taken from <cmath>'s M_PI.
//
// M_PI is a POSIX extension, not standard C++. libstdc++ exposes it from
// <cmath> by default, but MSVC only defines it when _USE_MATH_DEFINES is
// set *before* the first <cmath>/<math.h> include -- a whole-translation-
// unit ordering constraint that a header-only module in a shared include
// path cannot reliably guarantee. (SDL3's SDL_stdinc.h also defines M_PI
// as a fallback, which is why the test binary, which includes SDL before
// these headers, used to build on MSVC while every other target failed.)
//
// A constexpr constant sidesteps the ordering problem entirely and is
// exactly as precise: long double's mantissa cannot represent more digits
// than are written here.
inline constexpr double kPi = 3.14159265358979323846;

} // namespace naikav::dsp
