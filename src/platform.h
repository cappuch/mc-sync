#ifndef MCSYNC_PLATFORM_H
#define MCSYNC_PLATFORM_H

#if defined(__linux__)
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include <limits.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#endif /* MCSYNC_PLATFORM_H */
