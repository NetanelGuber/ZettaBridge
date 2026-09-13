#include "zb/log.h"

#include <cstdarg>
#include <cstdio>

namespace zb {

void log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::fputs("[zb] ", stderr);
    std::vfprintf(stderr, fmt, ap);
    std::fputc('\n', stderr);
    va_end(ap);
}

}  // namespace zb
