// Stand-in for WIL; filled in as the build asks for it.
#pragma once
#include <cstdlib>
#define FAIL_FAST() std::abort()
#define FAIL_FAST_IF(cond) do { if (cond) std::abort(); } while (0)
