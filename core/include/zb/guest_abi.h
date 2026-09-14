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

// struct stat64 on arm EABI. Time members drop the st_ prefix: glibc and bionic define
// st_atime/st_atime_nsec (and mtime/ctime) as macros.
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
    std::uint32_t atime_sec;
    std::uint32_t atime_nsec;
    std::uint32_t mtime_sec;
    std::uint32_t mtime_nsec;
    std::uint32_t ctime_sec;
    std::uint32_t ctime_nsec;
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

// Signal delivery structures (arm kernel layout; offsets checked against bionic's ucontext_t,
// mcontext_t and siginfo_t with the NDK).
struct siginfo32 {
    std::int32_t si_signo;
    std::int32_t si_errno;
    std::int32_t si_code;
    // fields[0] is si_addr for faults, si_pid for kill/tgkill; fields[1] is si_uid.
    std::uint32_t fields[29];
};

struct sigcontext32 {
    std::uint32_t trap_no;
    std::uint32_t error_code;
    std::uint32_t oldmask;
    std::uint32_t regs[16];  // arm_r0..arm_r10, arm_fp, arm_ip, arm_sp, arm_lr, arm_pc
    std::uint32_t cpsr;
    std::uint32_t fault_address;
};

struct ucontext32 {
    std::uint32_t uc_flags;
    std::uint32_t uc_link;
    stack32 uc_stack;
    sigcontext32 uc_mcontext;
    std::uint64_t uc_sigmask;
    std::uint8_t unused[120];
    std::uint32_t uc_regspace[128];
};

struct rt_sigframe32 {
    siginfo32 info;
    ucontext32 uc;
    std::uint32_t retcode[4];
};

struct sigframe32 {
    ucontext32 uc;
    std::uint32_t retcode[4];
};

static_assert(sizeof(siginfo32) == 128);
static_assert(sizeof(sigcontext32) == 84);
static_assert(offsetof(sigcontext32, regs) == 12);
static_assert(offsetof(sigcontext32, cpsr) == 76);
static_assert(offsetof(sigcontext32, fault_address) == 80);
static_assert(offsetof(ucontext32, uc_mcontext) == 20);
static_assert(offsetof(ucontext32, uc_sigmask) == 104);
static_assert(offsetof(ucontext32, uc_regspace) == 232);
static_assert(sizeof(ucontext32) == 744);
static_assert(offsetof(rt_sigframe32, uc) == 128);

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
static_assert(offsetof(stat64, atime_sec) == 72);
static_assert(offsetof(stat64, st_ino) == 96);

}  // namespace zb::g
