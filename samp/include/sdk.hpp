#pragma once
// Minimal SA-MP plugin SDK shim (classic 0.3.7 R5 plugin model).
#define HAVE_STDINT_H 1
#include "../sdk/amx/amx.h"
#include "../sdk/plugincommon.h"

typedef void (*logprintf_t)(const char* format, ...);
