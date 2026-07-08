// Single translation unit that pulls in the nanosvg implementations. Kept
// separate from our code, with third-party warnings silenced (nanosvg is old
// C and trips -Wall/-Wextra; it is not our code to fix).
#if defined(__GNUC__)
# pragma GCC diagnostic ignored "-Wunused-function"
# pragma GCC diagnostic ignored "-Wunused-but-set-variable"
# pragma GCC diagnostic ignored "-Wunused-parameter"
# pragma GCC diagnostic ignored "-Wsign-compare"
# pragma GCC diagnostic ignored "-Wmisleading-indentation"
# pragma GCC diagnostic ignored "-Wmissing-prototypes"
# pragma GCC diagnostic ignored "-Wmissing-declarations"
#endif

#include <stdio.h>
#include <string.h>
#include <math.h>

#define NANOSVG_IMPLEMENTATION
#define NANOSVG_ALL_COLOR_KEYWORDS
#include "nanosvg/nanosvg.h"

#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvg/nanosvgrast.h"
