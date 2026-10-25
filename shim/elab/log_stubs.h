// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vyges
//
// The `Yosys::` logging calls the SystemVerilog frontend makes, for a build without Yosys.
//
// `log`, `log_warning` and `log_error` reach the embedder through the log callback of the
// elaboration in progress (vyges_slang_elab.cc): a message, its trailing line ends removed.
// `log_error` does not return: it ends the elaboration with an error. Debug logging is off.
#ifndef VYGES_SLANG_ELAB_LOG_STUBS_H
#define VYGES_SLANG_ELAB_LOG_STUBS_H

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace Yosys {

[[gnu::format(printf, 1, 2)]] void log(const char* fmt, ...);
[[gnu::format(printf, 1, 2)]] void log_warning(const char* fmt, ...);
[[gnu::format(printf, 1, 2)]] [[noreturn]] void log_error(const char* fmt, ...);

inline void log_flush() {}

inline int ys_debug(int = 0) { return 0; }

#ifndef log_debug
[[gnu::format(printf, 1, 2)]] inline void log_debug(const char*, ...) {}
#endif

// The number of bits needed to count to `x` - 1: ceil(log2(x)), 0 for x <= 1.
inline int ceil_log2(int x) {
  if (x <= 0) return 0;
  int bits = 0;
  for (unsigned v = static_cast<unsigned>(x) - 1; v != 0; v >>= 1) bits++;
  return bits;
}

[[noreturn]] inline void log_abort() { std::abort(); }

#ifndef log_assert
#define log_assert(cond_)                                                                  \
  do {                                                                                     \
    if (!(cond_)) {                                                                        \
      std::fprintf(stderr, "Assertion failed: %s [%s:%d]\n", #cond_, __FILE__, __LINE__); \
      std::abort();                                                                        \
    }                                                                                      \
  } while (0)
#endif

[[gnu::format(printf, 1, 2)]] inline std::string stringf(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  va_list again;
  va_copy(again, ap);
  const int n = std::vsnprintf(nullptr, 0, fmt, ap);
  va_end(ap);
  std::string s(n > 0 ? n : 0, '\0');
  if (n > 0) std::vsnprintf(s.data(), n + 1, fmt, again);
  va_end(again);
  return s;
}

}  // namespace Yosys

#endif  // VYGES_SLANG_ELAB_LOG_STUBS_H
