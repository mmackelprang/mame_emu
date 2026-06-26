// glibc >= 2.34 redefined SIGSTKSZ from a compile-time constant to a sysconf()
// call, which breaks the bundled Catch v1 POSIX FatalConditionHandler's
// `static char altStackMem[SIGSTKSZ]` (an array bound needs a constant
// expression). Pin it to a fixed 32 KiB -- matching Catch's own Windows-path
// stack guarantee -- for this, the sole Catch implementation translation unit.
// A crashing test still fails the run via a non-zero exit; we only forgo
// Catch's signal-name diagnostic. glibc-only so MSVC/MinGW/macOS are untouched.
#include <csignal>
#if defined(__GLIBC__) && defined(SIGSTKSZ)
#undef SIGSTKSZ
#define SIGSTKSZ 32768
#endif
#define CATCH_CONFIG_MAIN
#include "catch.hpp"
