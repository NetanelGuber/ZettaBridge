#pragma once

#include <bit>
#include <cstdint>
#include <type_traits>

#include "zb/gl_backend.h"
#include "zb/guest_thread.h"
#include "zb/library_runtime.h"

namespace zb {

// GLES 2.0 host-call dispatcher. Guest code passes AAPCS32 words in r0-r3 and on its stack;
// HostGl translates those words to the portable GlBackend seam after Dynarmic has stopped.
class HostGl {
public:
    class Call {
    public:
        Call(HostGl& host, GuestThread& thread, std::uint32_t index);

        std::uint32_t index() const { return index_; }
        std::uint32_t arg(unsigned position);
        bool valid() const { return valid_; }

        template <typename T>
        T scalar(unsigned position) {
            const std::uint32_t word = arg(position);
            if constexpr (std::is_floating_point_v<T>) {
                static_assert(sizeof(T) == sizeof(word));
                return std::bit_cast<T>(word);
            } else if constexpr (std::is_signed_v<T>) {
                const std::int32_t signed_word = std::bit_cast<std::int32_t>(word);
                return static_cast<T>(signed_word);
            } else {
                return static_cast<T>(word);
            }
        }

        template <typename T>
        void set_result(T value) {
            static_assert(std::is_integral_v<T> || std::is_enum_v<T>);
            thread_.regs()[0] = static_cast<std::uint32_t>(value);
        }

        void fail(GLenum error, const char* reason);

    private:
        HostGl& host_;
        GuestThread& thread_;
        std::uint32_t index_;
        std::uint32_t regs_[4];
        bool valid_ = true;
    };

    HostGl(LibraryRuntime& runtime, GlBackend& backend) : runtime_(runtime), backend_(backend) {}

    // Serves the GLES range 0-141 and leaves 142-160 for HostAssets.
    bool handle_host_call(std::uint32_t index, GuestThread& thread);
    LibraryRuntime& runtime() { return runtime_; }
    GlBackend& backend() { return backend_; }
    void reject(Call& call, GLenum error, const char* reason);

private:
    bool dispatch(Call& call);

    LibraryRuntime& runtime_;
    GlBackend& backend_;
};

}  // namespace zb
