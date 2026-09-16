#pragma once

#include <bit>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <type_traits>
#include <utility>

#include "zb/gl_backend.h"
#include "zb/guest_thread.h"
#include "zb/library_runtime.h"

namespace zb {

// GLES 2.0 host-call dispatcher. Guest code passes AAPCS32 words in r0-r3 and on its stack;
// HostGl translates those words to the portable GlBackend seam after Dynarmic has stopped.
class HostGl {
public:
    using GuestAllocator = std::function<std::optional<std::uint32_t>(std::size_t)>;
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

        template <typename T>
        std::uint64_t length(T value, std::uint64_t multiplier = 1) {
            static_assert(std::is_integral_v<T>);
            if constexpr (std::is_signed_v<T>) {
                if (value < 0) {
                    fail(kGlInvalidValue, "array length is negative");
                    return 0;
                }
            }
            const std::uint64_t unsigned_value = static_cast<std::uint64_t>(value);
            if (multiplier != 0 &&
                unsigned_value > std::numeric_limits<std::uint64_t>::max() / multiplier) {
                fail(kGlInvalidValue, "array length overflowed");
                return 0;
            }
            return unsigned_value * multiplier;
        }

        template <typename T>
        T* pointer(unsigned position, std::uint64_t elements, std::uint8_t need) {
            const std::uint32_t address = arg(position);
            constexpr std::uint64_t element_size = [] {
                if constexpr (std::is_void_v<std::remove_cv_t<T>>) {
                    return std::uint64_t{1};
                } else {
                    return static_cast<std::uint64_t>(sizeof(T));
                }
            }();
            if (elements > kGuestSpaceSize / element_size) {
                fail(kGlInvalidValue, "array byte size exceeds the guest address space");
                return nullptr;
            }
            const std::uint64_t bytes = elements * element_size;
            if (address == 0) return nullptr;
            std::uint8_t* host = host_.runtime().memory().host_ptr(address, bytes, need);
            if (host == nullptr) {
                fail(kGlInvalidValue, "array is outside accessible guest memory");
                return nullptr;
            }
            return reinterpret_cast<T*>(host);
        }

        void fail(GLenum error, const char* reason);

    private:
        HostGl& host_;
        GuestThread& thread_;
        std::uint32_t index_;
        std::uint32_t regs_[4];
        bool valid_ = true;
    };

    HostGl(LibraryRuntime& runtime, GlBackend& backend, GuestAllocator allocator = {})
        : runtime_(runtime), backend_(backend), allocator_(std::move(allocator)) {}

    // Serves the GLES range 0-141 and leaves 142-160 for HostAssets.
    bool handle_host_call(std::uint32_t index, GuestThread& thread);
    LibraryRuntime& runtime() { return runtime_; }
    GlBackend& backend() { return backend_; }
    void reject(Call& call, GLenum error, const char* reason);
    void note_pixel_store(GLenum pname, GLint param);
    void invalidate_uniforms(GLuint program);
    GLint pixel_alignment(bool pack) const;
    std::optional<std::uint64_t> uniform_elements(GLuint program, GLint location);
    std::optional<std::uint32_t> allocate_guest(std::size_t size);

private:
    bool dispatch(Call& call);

    LibraryRuntime& runtime_;
    GlBackend& backend_;
    GuestAllocator allocator_;
};

std::uint64_t gl_pname_count(GlBackend& backend, GLenum pname);
std::optional<std::uint64_t> gl_pixel_bytes(GLenum format, GLenum type, GLsizei width,
                                            GLsizei height, GLint alignment);

}  // namespace zb
