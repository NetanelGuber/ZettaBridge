#pragma once

// 32-bit ARM EABI guest structure layouts, spelled out with fixed-width fields.
// tools/abi_check.c asserts the same sizes and offsets against bionic's own headers.

#include <cstddef>
#include <cstdint>

namespace zb::g {

struct timespec32 {
    std::int32_t tv_sec;
    std::int32_t tv_nsec;
};

struct timeval32 {
    std::int32_t tv_sec;
    std::int32_t tv_usec;
};

struct timespec64 {
    std::int64_t tv_sec;
    std::int64_t tv_nsec;
};

struct iovec32 {
    std::uint32_t iov_base;
    std::uint32_t iov_len;
};

struct stack32 {
    std::uint32_t ss_sp;
    std::int32_t ss_flags;
    std::uint32_t ss_size;
};

struct rlimit32 {
    std::uint32_t rlim_cur;
    std::uint32_t rlim_max;
};

// Kernel struct sigaction as seen by rt_sigaction on arm (sigset is two 32-bit words).
struct ksigaction32 {
    std::uint32_t handler;
    std::uint32_t flags;
    std::uint32_t restorer;
    std::uint32_t mask[2];
};

// struct stat64 on arm EABI. Member names avoid st_atime/st_mtime/st_ctime (libc macros).
struct stat64 {
    std::uint64_t st_dev;
    std::uint8_t pad0[4];
    std::uint32_t st_ino_trunc;
    std::uint32_t st_mode;
    std::uint32_t st_nlink;
    std::uint32_t st_uid;
    std::uint32_t st_gid;
    std::uint64_t st_rdev;
    std::uint8_t pad3[4];
    std::uint32_t pad4;
    std::int64_t st_size;
    std::uint32_t st_blksize;
    std::uint32_t pad5;
    std::uint64_t st_blocks;
    std::uint32_t st_atime_sec;
    std::uint32_t st_atime_nsec;
    std::uint32_t st_mtime_sec;
    std::uint32_t st_mtime_nsec;
    std::uint32_t st_ctime_sec;
    std::uint32_t st_ctime_nsec;
    std::uint64_t st_ino;
};

// struct statfs64 on arm EABI: packed, aligned(4) in the kernel headers, so 84 bytes.
// bionic's own definition is unpacked (88 bytes) and passes 88 as the size argument; the
// arm64 kernel accepts 88 for 32-bit callers and writes these 84 bytes (same offsets).
inline constexpr std::uint32_t kStatfs64UserSize = 88;
#pragma pack(push, 4)
struct statfs64 {
    std::uint32_t f_type;
    std::uint32_t f_bsize;
    std::uint64_t f_blocks;
    std::uint64_t f_bfree;
    std::uint64_t f_bavail;
    std::uint64_t f_files;
    std::uint64_t f_ffree;
    std::int32_t f_fsid[2];
    std::uint32_t f_namelen;
    std::uint32_t f_frsize;
    std::uint32_t f_flags;
    std::uint32_t f_spare[4];
};
#pragma pack(pop)

static_assert(sizeof(statfs64) == 84);
static_assert(offsetof(statfs64, f_blocks) == 8);
static_assert(offsetof(statfs64, f_fsid) == 48);
static_assert(offsetof(statfs64, f_flags) == 64);
static_assert(sizeof(timespec32) == 8);
static_assert(sizeof(timeval32) == 8);
static_assert(sizeof(timespec64) == 16);
static_assert(sizeof(iovec32) == 8);
static_assert(sizeof(stack32) == 12);
static_assert(sizeof(rlimit32) == 8);
static_assert(sizeof(ksigaction32) == 20);
static_assert(sizeof(stat64) == 104);
static_assert(offsetof(stat64, st_mode) == 16);
static_assert(offsetof(stat64, st_rdev) == 32);
static_assert(offsetof(stat64, st_size) == 48);
static_assert(offsetof(stat64, st_blksize) == 56);
static_assert(offsetof(stat64, st_blocks) == 64);
static_assert(offsetof(stat64, st_atime_sec) == 72);
static_assert(offsetof(stat64, st_ino) == 96);

}  // namespace zb::g
