#pragma once
#include "clang/Basic/Version.h"

#if CLANG_VERSION_MAJOR != 18 || CLANG_VERSION_MINOR != 1 ||                   \
    CLANG_VERSION_PATCHLEVEL != 3
#error                                                                         \
    "WPGKit currently supports exactly Clang 18.1.3; run conformance tests before porting"
#endif
