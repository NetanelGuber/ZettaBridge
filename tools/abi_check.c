/* Compiled by the NDK for armeabi-v7a only (tools/build_guest.sh, -c).
 * Asserts that bionic's arm layouts match core/include/zb/guest_abi.h. */
#include <signal.h>
#include <stddef.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <time.h>

_Static_assert(sizeof(void*) == 4, "must be built for 32-bit arm");
_Static_assert(sizeof(struct timespec) == 8, "timespec32");
_Static_assert(sizeof(struct timeval) == 8, "timeval32");
_Static_assert(sizeof(struct iovec) == 8, "iovec32");
_Static_assert(sizeof(stack_t) == 12, "stack32");
_Static_assert(offsetof(stack_t, ss_flags) == 4, "stack32.ss_flags");
_Static_assert(offsetof(stack_t, ss_size) == 8, "stack32.ss_size");
_Static_assert(sizeof(struct stat64) == 104, "stat64 size");
_Static_assert(offsetof(struct stat64, st_mode) == 16, "stat64.st_mode");
_Static_assert(offsetof(struct stat64, st_rdev) == 32, "stat64.st_rdev");
_Static_assert(offsetof(struct stat64, st_size) == 48, "stat64.st_size");
_Static_assert(offsetof(struct stat64, st_blksize) == 56, "stat64.st_blksize");
_Static_assert(offsetof(struct stat64, st_blocks) == 64, "stat64.st_blocks");
_Static_assert(offsetof(struct stat64, st_atim) == 72, "stat64.st_atim");
_Static_assert(offsetof(struct stat64, st_ino) == 96, "stat64.st_ino");
