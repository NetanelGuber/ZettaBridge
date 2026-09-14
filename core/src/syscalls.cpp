#include "zb/syscalls.h"

#include <fcntl.h>
#include <linux/futex.h>
#include <sched.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <sys/utsname.h>
#include <sys/vfs.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "gen/syscall_nrs_arm.h"
#include "zb/guest_abi.h"
#include "zb/guest_memory.h"
#include "zb/guest_thread.h"
#include "zb/log.h"
#include "zb/process.h"

namespace zb {

namespace {

struct SyscallName {
    std::uint32_t nr;
    const char* name;
};

constexpr SyscallName kSyscallNames[] = {
#include "gen/syscall_names_arm.inc"
};

constexpr int kPrSetVma = 0x53564d41;
constexpr int kSigDfl = 0;
constexpr int kSigIgn = 1;

// Keys for Process::first_time, kept apart from raw syscall numbers.
constexpr std::uint64_t kSeenIoctl = 1ULL << 33;
constexpr std::uint64_t kSeenFcntl = 1ULL << 34;
constexpr std::uint64_t kSeenPrctl = 1ULL << 35;
constexpr std::uint64_t kSeenFutex = 1ULL << 36;
constexpr std::uint64_t kSeenSignal = 1ULL << 37;

struct Ctx {
    Process& proc;
    GuestThread& thread;
    GuestMemory& mem;
    std::uint32_t a[6];
    bool stop = false;
};

// ZB_STRACE=1 logs every guest syscall with its first four arguments and result.
bool trace_enabled() {
    static const bool enabled = std::getenv("ZB_STRACE") != nullptr;
    return enabled;
}

std::int32_t result_of(long host_ret) {
    return host_ret == -1 ? -errno : static_cast<std::int32_t>(host_ret);
}

template <typename T>
bool read_guest(GuestMemory& m, std::uint32_t addr, T& out) {
    const std::uint8_t* p = m.host_ptr(addr, sizeof(T), kPageRead);
    if (!p) return false;
    std::memcpy(&out, p, sizeof(T));
    return true;
}

template <typename T>
bool write_guest(GuestMemory& m, std::uint32_t addr, const T& value) {
    std::uint8_t* p = m.host_ptr(addr, sizeof(T), kPageWrite);
    if (!p) return false;
    std::memcpy(p, &value, sizeof(T));
    return true;
}

// Validates a NUL-terminated guest string page by page; nullptr if it is not readable.
const char* guest_cstr(GuestMemory& m, std::uint32_t addr) {
    if (addr == 0) return nullptr;
    std::uint64_t a = addr;
    while (a < kGuestSpaceSize) {
        if (!m.accessible(static_cast<std::uint32_t>(a), 1, kPageRead)) return nullptr;
        const std::uint64_t page_end = page_round_up(a + 1);
        for (; a < page_end; ++a) {
            if (m.base()[a] == 0) return reinterpret_cast<const char*>(m.base() + addr);
        }
    }
    return nullptr;
}

void fill_stat64(g::stat64& out, const struct stat& st) {
    std::memset(&out, 0, sizeof out);
    out.st_dev = st.st_dev;
    out.st_ino_trunc = static_cast<std::uint32_t>(st.st_ino);
    out.st_mode = st.st_mode;
    out.st_nlink = static_cast<std::uint32_t>(st.st_nlink);
    out.st_uid = st.st_uid;
    out.st_gid = st.st_gid;
    out.st_rdev = st.st_rdev;
    out.st_size = st.st_size;
    out.st_blksize = static_cast<std::uint32_t>(st.st_blksize);
    out.st_blocks = static_cast<std::uint64_t>(st.st_blocks);
    out.st_atime_sec = static_cast<std::uint32_t>(st.st_atim.tv_sec);
    out.st_atime_nsec = static_cast<std::uint32_t>(st.st_atim.tv_nsec);
    out.st_mtime_sec = static_cast<std::uint32_t>(st.st_mtim.tv_sec);
    out.st_mtime_nsec = static_cast<std::uint32_t>(st.st_mtim.tv_nsec);
    out.st_ctime_sec = static_cast<std::uint32_t>(st.st_ctim.tv_sec);
    out.st_ctime_nsec = static_cast<std::uint32_t>(st.st_ctim.tv_nsec);
    out.st_ino = st.st_ino;
}

bool read_timespec(GuestMemory& m, std::uint32_t addr, bool time64, timespec& out) {
    if (time64) {
        g::timespec64 t;
        if (!read_guest(m, addr, t)) return false;
        out.tv_sec = static_cast<time_t>(t.tv_sec);
        out.tv_nsec = static_cast<long>(t.tv_nsec);
    } else {
        g::timespec32 t;
        if (!read_guest(m, addr, t)) return false;
        out.tv_sec = t.tv_sec;
        out.tv_nsec = t.tv_nsec;
    }
    return true;
}

bool write_timespec(GuestMemory& m, std::uint32_t addr, bool time64, const timespec& ts) {
    if (time64) return write_guest(m, addr, g::timespec64{ts.tv_sec, ts.tv_nsec});
    return write_guest(m, addr, g::timespec32{static_cast<std::int32_t>(ts.tv_sec), static_cast<std::int32_t>(ts.tv_nsec)});
}

std::int32_t sys_read_write(Ctx& c, bool is_write) {
    const std::uint32_t len = c.a[2];
    std::uint8_t* buf = c.mem.host_ptr(c.a[1], len, is_write ? kPageRead : kPageWrite);
    if (len != 0 && buf == nullptr) return -EFAULT;
    const int fd = static_cast<int>(c.a[0]);
    return result_of(is_write ? ::write(fd, buf, len) : ::read(fd, buf, len));
}

std::int32_t sys_pread_pwrite(Ctx& c, bool is_write) {
    const std::uint32_t len = c.a[2];
    std::uint8_t* buf = c.mem.host_ptr(c.a[1], len, is_write ? kPageRead : kPageWrite);
    if (len != 0 && buf == nullptr) return -EFAULT;
    const off_t offset = static_cast<off_t>(static_cast<std::uint64_t>(c.a[4]) | (static_cast<std::uint64_t>(c.a[5]) << 32));
    const int fd = static_cast<int>(c.a[0]);
    return result_of(is_write ? ::pwrite(fd, buf, len, offset) : ::pread(fd, buf, len, offset));
}

std::int32_t sys_readv_writev(Ctx& c, bool is_write) {
    const std::uint32_t count = c.a[2];
    if (count > IOV_MAX) return -EINVAL;
    std::vector<iovec> iov(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        g::iovec32 v;
        if (!read_guest(c.mem, c.a[1] + i * sizeof(g::iovec32), v)) return -EFAULT;
        std::uint8_t* base = c.mem.host_ptr(v.iov_base, v.iov_len, is_write ? kPageRead : kPageWrite);
        if (v.iov_len != 0 && base == nullptr) return -EFAULT;
        iov[i] = {base, v.iov_len};
    }
    const int fd = static_cast<int>(c.a[0]);
    return result_of(is_write ? ::writev(fd, iov.data(), static_cast<int>(count)) : ::readv(fd, iov.data(), static_cast<int>(count)));
}

std::int32_t sys_brk(Ctx& c) {
    Process& p = c.proc;
    const std::uint32_t want = c.a[0];
    if (want < p.brk_start) return static_cast<std::int32_t>(p.brk_current);
    const std::uint64_t old_top = page_round_up(p.brk_current);
    const std::uint64_t new_top = page_round_up(want);
    if (new_top > old_top) {
        const auto start = static_cast<std::uint32_t>(old_top);
        if (new_top > p.mmap_limit || !c.mem.range_free(start, new_top - old_top) ||
            !c.mem.map_anon(start, new_top - old_top, PROT_READ | PROT_WRITE)) {
            return static_cast<std::int32_t>(p.brk_current);
        }
    } else if (new_top < old_top) {
        c.mem.unmap(static_cast<std::uint32_t>(new_top), old_top - new_top);
        p.invalidate(static_cast<std::uint32_t>(new_top), static_cast<std::uint32_t>(old_top - new_top));
    }
    p.brk_current = want;
    return static_cast<std::int32_t>(want);
}

std::int32_t sys_mmap2(Ctx& c) {
    const std::uint32_t addr = c.a[0];
    const std::uint32_t len = c.a[1];
    const int prot = static_cast<int>(c.a[2]);
    const int flags = static_cast<int>(c.a[3]);
    const int fd = static_cast<int>(c.a[4]);
    const std::uint64_t offset = static_cast<std::uint64_t>(c.a[5]) * kPageSize;

    if (len == 0) return -EINVAL;
    const std::uint64_t size = page_round_up(len);
    const bool fixed = (flags & MAP_FIXED) != 0;
    const bool noreplace = (flags & MAP_FIXED_NOREPLACE) != 0;

    std::uint32_t at = 0;
    if (fixed || noreplace) {
        if (addr & kPageMask) return -EINVAL;
        if (static_cast<std::uint64_t>(addr) + size > kGuestSpaceSize) return -ENOMEM;
        if (noreplace && !c.mem.range_free(addr, size)) return -EEXIST;
        at = addr;
    } else if (addr >= 0x10000 && (addr & kPageMask) == 0 && static_cast<std::uint64_t>(addr) + size <= c.proc.mmap_limit &&
               c.mem.range_free(addr, size)) {
        at = addr;
    } else {
        at = c.mem.find_free(size, c.proc.mmap_limit);
        if (at == 0) return -ENOMEM;
    }

    errno = 0;
    const bool ok = (flags & MAP_ANONYMOUS) ? c.mem.map_anon(at, size, prot)
                                            : c.mem.map_file(at, size, prot, flags, fd, offset);
    if (!ok) return errno ? -errno : -ENOMEM;
    if (flags & MAP_ANONYMOUS) {
        c.proc.forget_mappings(at, size);
    } else {
        char link[64];
        char target[PATH_MAX];
        std::snprintf(link, sizeof link, "/proc/self/fd/%d", fd);
        const ssize_t n = ::readlink(link, target, sizeof target - 1);
        c.proc.record_file_mapping(at, static_cast<std::uint32_t>(size), offset,
                                   n > 0 ? std::string(target, static_cast<std::size_t>(n)) : std::string("fd"));
    }
    c.proc.invalidate(at, static_cast<std::uint32_t>(size));
    return static_cast<std::int32_t>(at);
}

std::int32_t sys_munmap(Ctx& c) {
    const std::uint32_t addr = c.a[0];
    if ((addr & kPageMask) || c.a[1] == 0) return -EINVAL;
    const std::uint64_t size = page_round_up(c.a[1]);
    if (static_cast<std::uint64_t>(addr) + size > kGuestSpaceSize) return -EINVAL;
    if (!c.mem.unmap(addr, size)) return -EINVAL;
    c.proc.forget_mappings(addr, size);
    c.proc.invalidate(addr, static_cast<std::uint32_t>(size));
    return 0;
}

std::int32_t sys_mprotect(Ctx& c) {
    const std::uint32_t addr = c.a[0];
    if (addr & kPageMask) return -EINVAL;
    if (c.a[1] == 0) return 0;
    const std::uint64_t size = page_round_up(c.a[1]);
    if (!c.mem.accessible(addr, size, 0)) return -ENOMEM;
    if (!c.mem.protect(addr, size, static_cast<int>(c.a[2]))) return -EACCES;
    c.proc.invalidate(addr, static_cast<std::uint32_t>(size));
    return 0;
}

std::int32_t sys_madvise(Ctx& c) {
    const std::uint32_t addr = c.a[0];
    if (addr & kPageMask) return -EINVAL;
    if (c.a[1] == 0) return 0;
    const std::uint64_t size = page_round_up(c.a[1]);
    if (!c.mem.accessible(addr, size, 0)) return -ENOMEM;
    return result_of(::madvise(c.mem.base() + addr, size, static_cast<int>(c.a[2])));
}

std::int32_t sys_rt_sigaction(Ctx& c) {
    const std::uint32_t sig = c.a[0];
    if (sig < 1 || sig > 64 || c.a[3] != 8) return -EINVAL;
    g::ksigaction32 act{};
    const bool has_new = c.a[1] != 0;
    if (has_new) {
        if (sig == SIGKILL || sig == SIGSTOP) return -EINVAL;
        if (!read_guest(c.mem, c.a[1], act)) return -EFAULT;
    }
    if (c.a[2] != 0 && !write_guest(c.mem, c.a[2], c.proc.sigactions[sig])) return -EFAULT;
    if (has_new) c.proc.sigactions[sig] = act;
    return 0;
}

std::int32_t sys_rt_sigprocmask(Ctx& c) {
    if (c.a[3] != 8) return -EINVAL;
    const std::uint64_t old_mask = c.thread.sigmask;
    if (c.a[1] != 0) {
        std::uint64_t set;
        if (!read_guest(c.mem, c.a[1], set)) return -EFAULT;
        std::uint64_t mask = old_mask;
        switch (c.a[0]) {
        case SIG_BLOCK: mask |= set; break;
        case SIG_UNBLOCK: mask &= ~set; break;
        case SIG_SETMASK: mask = set; break;
        default: return -EINVAL;
        }
        mask &= ~((1ULL << (SIGKILL - 1)) | (1ULL << (SIGSTOP - 1)));
        c.thread.sigmask = mask;
    }
    if (c.a[2] != 0 && !write_guest(c.mem, c.a[2], old_mask)) return -EFAULT;
    return 0;
}

std::int32_t sys_sigaltstack(Ctx& c) {
    g::stack32 ss{};
    const bool has_new = c.a[0] != 0;
    if (has_new && !read_guest(c.mem, c.a[0], ss)) return -EFAULT;
    if (c.a[1] != 0 && !write_guest(c.mem, c.a[1], c.proc.altstack)) return -EFAULT;
    if (has_new) c.proc.altstack = ss;
    return 0;
}

bool default_ignored(std::uint32_t sig) {
    return sig == SIGCHLD || sig == SIGCONT || sig == SIGURG || sig == SIGWINCH;
}

// kill/tkill/tgkill. Only signals aimed at the guest itself are supported for now.
std::int32_t sys_signal_self(Ctx& c, bool is_self, std::uint32_t sig) {
    if (sig > 64) return -EINVAL;
    if (!is_self) {
        if (c.proc.first_time(kSeenSignal | sig)) log("signal %u to another process or thread refused", sig);
        return -EPERM;
    }
    if (sig == 0) return 0;
    const std::uint32_t handler = c.proc.sigactions[sig].handler;
    if (sig != SIGKILL && (handler == static_cast<std::uint32_t>(kSigIgn) || (handler == kSigDfl && default_ignored(sig)))) {
        return 0;
    }
    if (sig != SIGKILL && handler != kSigDfl) {
        log("guest signal handlers are not delivered yet; treating signal %u as fatal", sig);
    }
    log("guest terminated by signal %u", sig);
    c.proc.request_exit(128 + static_cast<int>(sig));
    c.stop = true;
    return 0;
}

std::int32_t sys_clock_get(Ctx& c, bool res, bool time64) {
    timespec ts;
    const clockid_t clk = static_cast<clockid_t>(static_cast<std::int32_t>(c.a[0]));
    if ((res ? ::clock_getres(clk, &ts) : ::clock_gettime(clk, &ts)) != 0) return -errno;
    if (c.a[1] == 0) return 0;
    return write_timespec(c.mem, c.a[1], time64, ts) ? 0 : -EFAULT;
}

std::int32_t sys_gettimeofday(Ctx& c) {
    timeval tv;
    struct timezone tz;
    if (::gettimeofday(&tv, &tz) != 0) return -errno;
    if (c.a[0] != 0 &&
        !write_guest(c.mem, c.a[0], g::timeval32{static_cast<std::int32_t>(tv.tv_sec), static_cast<std::int32_t>(tv.tv_usec)})) {
        return -EFAULT;
    }
    if (c.a[1] != 0 && !write_guest(c.mem, c.a[1], tz)) return -EFAULT;
    return 0;
}

std::int32_t sys_clock_nanosleep(Ctx& c, bool time64) {
    timespec req;
    timespec rem{};
    if (!read_timespec(c.mem, c.a[2], time64, req)) return -EFAULT;
    const clockid_t clk = static_cast<clockid_t>(static_cast<std::int32_t>(c.a[0]));
    const int rc = ::clock_nanosleep(clk, static_cast<int>(c.a[1]), &req, &rem);
    if (rc == EINTR && c.a[3] != 0) write_timespec(c.mem, c.a[3], time64, rem);
    return -rc;
}

std::int32_t sys_nanosleep(Ctx& c) {
    timespec req;
    timespec rem{};
    if (!read_timespec(c.mem, c.a[0], false, req)) return -EFAULT;
    if (::nanosleep(&req, &rem) == 0) return 0;
    const int err = errno;
    if (err == EINTR && c.a[1] != 0) write_timespec(c.mem, c.a[1], false, rem);
    return -err;
}

std::int32_t sys_stat_common(Ctx& c, int rc, const struct stat& st, std::uint32_t out_addr) {
    if (rc != 0) return -errno;
    g::stat64 out;
    fill_stat64(out, st);
    return write_guest(c.mem, out_addr, out) ? 0 : -EFAULT;
}

std::int32_t sys_statfs_common(Ctx& c, int rc, const struct statfs& st, std::uint32_t out_addr) {
    if (rc != 0) return -errno;
    g::statfs64 out{};
    out.f_type = static_cast<std::uint32_t>(st.f_type);
    out.f_bsize = static_cast<std::uint32_t>(st.f_bsize);
    out.f_blocks = st.f_blocks;
    out.f_bfree = st.f_bfree;
    out.f_bavail = st.f_bavail;
    out.f_files = st.f_files;
    out.f_ffree = st.f_ffree;
    std::memcpy(out.f_fsid, &st.f_fsid, sizeof out.f_fsid);
    out.f_namelen = static_cast<std::uint32_t>(st.f_namelen);
    out.f_frsize = static_cast<std::uint32_t>(st.f_frsize);
    out.f_flags = static_cast<std::uint32_t>(st.f_flags);
    return write_guest(c.mem, out_addr, out) ? 0 : -EFAULT;
}

std::int32_t sys_fstatat64(Ctx& c) {
    const char* path = guest_cstr(c.mem, c.a[1]);
    if (!path) return -EFAULT;
    struct stat st;
    const int rc = ::fstatat(static_cast<int>(c.a[0]), c.proc.translate_path(path).c_str(), &st, static_cast<int>(c.a[3]));
    return sys_stat_common(c, rc, st, c.a[2]);
}

std::int32_t sys_statx(Ctx& c) {
    const char* path = guest_cstr(c.mem, c.a[1]);
    std::uint8_t* buf = c.mem.host_ptr(c.a[4], 256, kPageWrite);
    if (!path || !buf) return -EFAULT;
    const std::string host_path = c.proc.translate_path(path);
    return result_of(::syscall(SYS_statx, static_cast<int>(c.a[0]), host_path.c_str(), static_cast<int>(c.a[2]), c.a[3], buf));
}

std::int32_t sys_ioctl(Ctx& c) {
    const unsigned long request = c.a[1];
    std::uint32_t arg_size = 0;
    std::uint8_t need = kPageRead | kPageWrite;
    switch (request) {
    case TCGETS: arg_size = 36; break;
    case TIOCGWINSZ: arg_size = 8; break;
    case FIONREAD: arg_size = 4; break;
    case FIONBIO: arg_size = 4; need = kPageRead; break;
    default:
        if (c.proc.first_time(kSeenIoctl | request)) log("unsupported ioctl 0x%lx on fd %u", request, c.a[0]);
        return -ENOTTY;
    }
    std::uint8_t* arg = c.mem.host_ptr(c.a[2], arg_size, need);
    if (!arg) return -EFAULT;
    return result_of(::ioctl(static_cast<int>(c.a[0]), request, arg));
}

std::int32_t sys_fcntl64(Ctx& c) {
    const int cmd = static_cast<int>(c.a[1]);
    switch (cmd) {
    case F_DUPFD:
    case F_GETFD:
    case F_SETFD:
    case F_GETFL:
    case F_SETFL:
    case F_DUPFD_CLOEXEC:
        return result_of(::fcntl(static_cast<int>(c.a[0]), cmd, static_cast<long>(c.a[2])));
    default:
        if (c.proc.first_time(kSeenFcntl | static_cast<std::uint32_t>(cmd))) log("unsupported fcntl command %d", cmd);
        return -EINVAL;
    }
}

std::int32_t sys_prctl(Ctx& c) {
    const int option = static_cast<int>(c.a[0]);
    switch (option) {
    case kPrSetVma:
        return 0;
    case PR_GET_DUMPABLE:
    case PR_SET_DUMPABLE:
    case PR_SET_NO_NEW_PRIVS:
    case PR_GET_NO_NEW_PRIVS:
        return result_of(::prctl(option, static_cast<unsigned long>(c.a[1]), static_cast<unsigned long>(c.a[2]),
                                 static_cast<unsigned long>(c.a[3]), static_cast<unsigned long>(c.a[4])));
    case PR_SET_NAME:
    case PR_GET_NAME: {
        std::uint8_t* name = c.mem.host_ptr(c.a[1], 16, option == PR_SET_NAME ? kPageRead : kPageWrite);
        if (!name) return -EFAULT;
        return result_of(::prctl(option, name, 0, 0, 0));
    }
    default:
        if (c.proc.first_time(kSeenPrctl | static_cast<std::uint32_t>(option))) log("unsupported prctl option %d", option);
        return -EINVAL;
    }
}

std::int32_t sys_uname(Ctx& c) {
    utsname u;
    if (::uname(&u) != 0) return -errno;
    std::strncpy(u.machine, "armv8l", sizeof u.machine);
    return write_guest(c.mem, c.a[0], u) ? 0 : -EFAULT;
}

std::int32_t sys_futex(Ctx& c, bool time64) {
    const int op = static_cast<int>(c.a[1]);
    const int cmd = op & FUTEX_CMD_MASK;
    std::uint8_t* uaddr = c.mem.host_ptr(c.a[0], 4, kPageRead);
    if (!uaddr) return -EFAULT;
    switch (cmd) {
    case FUTEX_WAIT:
    case FUTEX_WAIT_BITSET: {
        timespec ts;
        timespec* tsp = nullptr;
        if (c.a[3] != 0) {
            if (!read_timespec(c.mem, c.a[3], time64, ts)) return -EFAULT;
            tsp = &ts;
        }
        return result_of(::syscall(SYS_futex, uaddr, op, c.a[2], tsp, nullptr, c.a[5]));
    }
    case FUTEX_WAKE:
    case FUTEX_WAKE_BITSET:
        return result_of(::syscall(SYS_futex, uaddr, op, c.a[2], nullptr, nullptr, c.a[5]));
    default:
        if (c.proc.first_time(kSeenFutex | static_cast<std::uint32_t>(cmd))) log("unsupported futex command %d", cmd);
        return -ENOSYS;
    }
}

std::int32_t sys_ugetrlimit(Ctx& c) {
    rlimit rl;
    if (::getrlimit(static_cast<int>(c.a[0]), &rl) != 0) return -errno;
    auto clamp = [](rlim_t v) -> std::uint32_t { return v > 0xFFFFFFFFu ? 0xFFFFFFFFu : static_cast<std::uint32_t>(v); };
    return write_guest(c.mem, c.a[1], g::rlimit32{clamp(rl.rlim_cur), clamp(rl.rlim_max)}) ? 0 : -EFAULT;
}

std::int32_t sys_prlimit64(Ctx& c) {
    std::uint8_t* new_limit = nullptr;
    std::uint8_t* old_limit = nullptr;
    if (c.a[2] != 0 && !(new_limit = c.mem.host_ptr(c.a[2], 16, kPageRead))) return -EFAULT;
    if (c.a[3] != 0 && !(old_limit = c.mem.host_ptr(c.a[3], 16, kPageWrite))) return -EFAULT;
    return result_of(::syscall(SYS_prlimit64, static_cast<pid_t>(c.a[0]), static_cast<int>(c.a[1]), new_limit, old_limit));
}

std::int32_t sys_llseek(Ctx& c) {
    const off_t offset = static_cast<off_t>((static_cast<std::uint64_t>(c.a[1]) << 32) | c.a[2]);
    const off_t r = ::lseek(static_cast<int>(c.a[0]), offset, static_cast<int>(c.a[4]));
    if (r == -1) return -errno;
    return write_guest(c.mem, c.a[3], static_cast<std::int64_t>(r)) ? 0 : -EFAULT;
}

std::int32_t sys_lseek(Ctx& c) {
    const off_t r = ::lseek(static_cast<int>(c.a[0]), static_cast<std::int32_t>(c.a[1]), static_cast<int>(c.a[2]));
    if (r == -1) return -errno;
    if (r > INT32_MAX) return -EOVERFLOW;
    return static_cast<std::int32_t>(r);
}

std::int32_t sys_pipe2(Ctx& c) {
    std::uint8_t* out = c.mem.host_ptr(c.a[0], 8, kPageWrite);
    if (!out) return -EFAULT;
    int fds[2];
    if (::pipe2(fds, static_cast<int>(c.a[1])) != 0) return -errno;
    std::memcpy(out, fds, sizeof fds);
    return 0;
}

std::int32_t sys_path_call(Ctx& c, std::uint32_t path_arg, long (*call)(Ctx&, const char*)) {
    const char* path = guest_cstr(c.mem, c.a[path_arg]);
    if (!path) return -EFAULT;
    const std::string host_path = c.proc.translate_path(path);
    return result_of(call(c, host_path.c_str()));
}

}  // namespace

const char* syscall_name(std::uint32_t nr) {
    for (const auto& entry : kSyscallNames) {
        if (entry.nr == nr) return entry.name;
    }
    return "?";
}

bool handle_syscall(Process& proc, GuestThread& thread) {
    auto& regs = thread.regs();
    Ctx c{proc, thread, proc.memory(), {regs[0], regs[1], regs[2], regs[3], regs[4], regs[5]}};
    const std::uint32_t nr = regs[7];
    std::int32_t res = -ENOSYS;

    switch (nr) {
    case NR_exit:
    case NR_exit_group:
        if (trace_enabled()) log("%s(%d)", syscall_name(nr), static_cast<int>(c.a[0]));
        proc.request_exit(static_cast<int>(c.a[0] & 0xff));
        return false;

    case NR_read: res = sys_read_write(c, false); break;
    case NR_write: res = sys_read_write(c, true); break;
    case NR_pread64: res = sys_pread_pwrite(c, false); break;
    case NR_pwrite64: res = sys_pread_pwrite(c, true); break;
    case NR_readv: res = sys_readv_writev(c, false); break;
    case NR_writev: res = sys_readv_writev(c, true); break;
    case NR_close: res = result_of(::close(static_cast<int>(c.a[0]))); break;
    case NR_dup: res = result_of(::dup(static_cast<int>(c.a[0]))); break;
    case NR_dup3: res = result_of(::dup3(static_cast<int>(c.a[0]), static_cast<int>(c.a[1]), static_cast<int>(c.a[2]))); break;
    case NR_pipe2: res = sys_pipe2(c); break;
    case NR_lseek: res = sys_lseek(c); break;
    case NR__llseek: res = sys_llseek(c); break;
    case NR_fcntl64: res = sys_fcntl64(c); break;
    case NR_ioctl: res = sys_ioctl(c); break;

    case NR_openat:
        res = sys_path_call(c, 1, [](Ctx& x, const char* p) -> long {
            return ::syscall(SYS_openat, static_cast<int>(x.a[0]), p, static_cast<int>(x.a[2]), static_cast<mode_t>(x.a[3]));
        });
        break;
    case NR_faccessat:
        res = sys_path_call(c, 1, [](Ctx& x, const char* p) -> long {
            return ::syscall(SYS_faccessat, static_cast<int>(x.a[0]), p, static_cast<int>(x.a[2]));
        });
        break;
    case NR_faccessat2:
        res = sys_path_call(c, 1, [](Ctx& x, const char* p) -> long {
            return ::syscall(SYS_faccessat2, static_cast<int>(x.a[0]), p, static_cast<int>(x.a[2]), static_cast<int>(x.a[3]));
        });
        break;
    case NR_unlinkat:
        res = sys_path_call(c, 1, [](Ctx& x, const char* p) -> long {
            return ::unlinkat(static_cast<int>(x.a[0]), p, static_cast<int>(x.a[2]));
        });
        break;
    case NR_mkdirat:
        res = sys_path_call(c, 1, [](Ctx& x, const char* p) -> long {
            return ::mkdirat(static_cast<int>(x.a[0]), p, static_cast<mode_t>(x.a[2]));
        });
        break;
    case NR_readlinkat: {
        const char* path = guest_cstr(c.mem, c.a[1]);
        std::uint8_t* buf = c.mem.host_ptr(c.a[2], c.a[3], kPageWrite);
        if (!path || (c.a[3] != 0 && !buf)) {
            res = -EFAULT;
            break;
        }
        if (std::strcmp(path, "/proc/self/exe") == 0) {
            const std::string& exe = proc.exe_path();
            const std::size_t n = std::min<std::size_t>(exe.size(), c.a[3]);
            std::memcpy(buf, exe.data(), n);
            res = static_cast<std::int32_t>(n);
            break;
        }
        const std::string host_path = proc.translate_path(path);
        res = result_of(::readlinkat(static_cast<int>(c.a[0]), host_path.c_str(), reinterpret_cast<char*>(buf), c.a[3]));
        break;
    }
    case NR_getcwd: {
        std::uint8_t* buf = c.mem.host_ptr(c.a[0], c.a[1], kPageWrite);
        res = (c.a[1] != 0 && !buf) ? -EFAULT : result_of(::syscall(SYS_getcwd, buf, c.a[1]));
        break;
    }
    case NR_fstat64: {
        struct stat st;
        const int rc = ::fstat(static_cast<int>(c.a[0]), &st);
        res = sys_stat_common(c, rc, st, c.a[1]);
        break;
    }
    case NR_fstatat64: res = sys_fstatat64(c); break;
    case NR_statx: res = sys_statx(c); break;
    case NR_fstatfs64: {
        if (c.a[1] != sizeof(g::statfs64) && c.a[1] != g::kStatfs64UserSize) {
            res = -EINVAL;
            break;
        }
        struct statfs st;
        const int rc = ::fstatfs(static_cast<int>(c.a[0]), &st);
        res = sys_statfs_common(c, rc, st, c.a[2]);
        break;
    }
    case NR_statfs64: {
        const char* path = guest_cstr(c.mem, c.a[0]);
        if (!path) {
            res = -EFAULT;
            break;
        }
        if (c.a[1] != sizeof(g::statfs64) && c.a[1] != g::kStatfs64UserSize) {
            res = -EINVAL;
            break;
        }
        struct statfs st;
        const int rc = ::statfs(proc.translate_path(path).c_str(), &st);
        res = sys_statfs_common(c, rc, st, c.a[2]);
        break;
    }

    case NR_brk: res = sys_brk(c); break;
    case NR_mmap2: res = sys_mmap2(c); break;
    case NR_munmap: res = sys_munmap(c); break;
    case NR_mprotect: res = sys_mprotect(c); break;
    case NR_madvise: res = sys_madvise(c); break;
    case NR_mremap: res = -ENOMEM; break;

    case NR_ARM_set_tls:
        thread.set_tls(c.a[0]);
        res = 0;
        break;
    case NR_ARM_get_tls: res = static_cast<std::int32_t>(thread.tls()); break;
    case NR_ARM_cacheflush:
        if (c.a[1] > c.a[0]) proc.invalidate(c.a[0], c.a[1] - c.a[0]);
        res = 0;
        break;

    case NR_set_tid_address:
        proc.clear_child_tid = c.a[0];
        res = result_of(::syscall(SYS_gettid));
        break;
    case NR_getpid: res = result_of(::getpid()); break;
    case NR_getppid: res = result_of(::getppid()); break;
    case NR_gettid: res = result_of(::syscall(SYS_gettid)); break;
    case NR_getuid32: res = static_cast<std::int32_t>(::getuid()); break;
    case NR_geteuid32: res = static_cast<std::int32_t>(::geteuid()); break;
    case NR_getgid32: res = static_cast<std::int32_t>(::getgid()); break;
    case NR_getegid32: res = static_cast<std::int32_t>(::getegid()); break;

    case NR_rt_sigaction: res = sys_rt_sigaction(c); break;
    case NR_rt_sigprocmask: res = sys_rt_sigprocmask(c); break;
    case NR_sigaltstack: res = sys_sigaltstack(c); break;
    case NR_kill:
        res = sys_signal_self(c, static_cast<pid_t>(c.a[0]) == ::getpid() || c.a[0] == 0, c.a[1]);
        break;
    case NR_tkill:
        res = sys_signal_self(c, static_cast<pid_t>(c.a[0]) == ::syscall(SYS_gettid), c.a[1]);
        break;
    case NR_tgkill:
        res = sys_signal_self(c, static_cast<pid_t>(c.a[0]) == ::getpid() && static_cast<pid_t>(c.a[1]) == ::syscall(SYS_gettid), c.a[2]);
        break;

    case NR_clock_gettime: res = sys_clock_get(c, false, false); break;
    case NR_clock_getres: res = sys_clock_get(c, true, false); break;
    case NR_clock_gettime64: res = sys_clock_get(c, false, true); break;
    case NR_clock_getres_time64: res = sys_clock_get(c, true, true); break;
    case NR_gettimeofday: res = sys_gettimeofday(c); break;
    case NR_nanosleep: res = sys_nanosleep(c); break;
    case NR_clock_nanosleep: res = sys_clock_nanosleep(c, false); break;
    case NR_clock_nanosleep_time64: res = sys_clock_nanosleep(c, true); break;

    case NR_futex: res = sys_futex(c, false); break;
    case NR_futex_time64: res = sys_futex(c, true); break;
    case NR_sched_yield: res = result_of(::sched_yield()); break;
    case NR_sched_getaffinity: {
        std::uint8_t* mask = c.mem.host_ptr(c.a[2], c.a[1], kPageWrite);
        res = !mask ? -EFAULT : result_of(::syscall(SYS_sched_getaffinity, static_cast<pid_t>(c.a[0]), c.a[1], mask));
        break;
    }
    case NR_getrandom: {
        std::uint8_t* buf = c.mem.host_ptr(c.a[0], c.a[1], kPageWrite);
        res = (c.a[1] != 0 && !buf) ? -EFAULT : result_of(::syscall(SYS_getrandom, buf, c.a[1], c.a[2]));
        break;
    }
    case NR_prctl: res = sys_prctl(c); break;
    case NR_uname: res = sys_uname(c); break;
    case NR_ugetrlimit: res = sys_ugetrlimit(c); break;
    case NR_prlimit64: res = sys_prlimit64(c); break;

    case NR_personality: {
        const std::uint32_t previous = proc.personality;
        if (c.a[0] != 0xFFFFFFFFu) proc.personality = c.a[0];
        res = static_cast<std::int32_t>(previous);
        break;
    }
    case NR_sched_getscheduler: res = result_of(::sched_getscheduler(static_cast<pid_t>(c.a[0]))); break;
    case NR_socket:
        res = result_of(::syscall(SYS_socket, static_cast<int>(c.a[0]), static_cast<int>(c.a[1]), static_cast<int>(c.a[2])));
        break;
    case NR_connect: {
        std::uint8_t* addr = c.mem.host_ptr(c.a[1], c.a[2], kPageRead);
        res = (c.a[2] != 0 && !addr) ? -EFAULT
                                     : result_of(::syscall(SYS_connect, static_cast<int>(c.a[0]), addr, c.a[2]));
        break;
    }
    case NR_rt_tgsigqueueinfo:
        res = sys_signal_self(c, static_cast<pid_t>(c.a[0]) == ::getpid() && static_cast<pid_t>(c.a[1]) == ::syscall(SYS_gettid), c.a[2]);
        break;

    default:
        if (proc.first_time(nr)) log("unimplemented syscall %s (%u)", syscall_name(nr), nr);
        res = -ENOSYS;
        break;
    }

    if (trace_enabled()) {
        log("%s(0x%x, 0x%x, 0x%x, 0x%x) = %d", syscall_name(nr), c.a[0], c.a[1], c.a[2], c.a[3], res);
    }
    if (c.stop) return false;
    regs[0] = static_cast<std::uint32_t>(res);
    return true;
}

}  // namespace zb
