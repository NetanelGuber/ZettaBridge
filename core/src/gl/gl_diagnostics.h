#pragma once

#include "zb/host_gl.h"

namespace zb {

// Records GL visibility facts for an accepted GL host call into the runtime report
// (gl_diagnostics.cpp). May query the driver through host.backend().
void gl_diagnose(HostGl& host, HostGl::Call& call);

// Called before dispatch for glDrawArrays/glDrawElements only, so it can read the
// framebuffer before the driver actually draws. Cheap: it does real work only for the
// first 3 draws that have the glyph atlas texture bound.
void gl_diagnose_before(HostGl& host, HostGl::Call& call);

struct GlMapDiagnostic {
    std::uint64_t id = 0;
    std::uintptr_t context = 0;
    GLuint buffer = 0;
};

// Bounded mirror diagnostics for glMapBufferRange. The returned identity follows a mapping into
// its flush/unmap records; id zero means diagnostics are disabled or the map is outside the cap.
GlMapDiagnostic gl_diagnose_map(HostGl& host, GLenum target, GLintptr offset, GLsizeiptr length,
                                GLbitfield access, std::uint32_t guest, const std::uint8_t* mirror,
                                const std::uint8_t* driver);
void gl_diagnose_map_flush(const GlMapDiagnostic& mapping, GLintptr offset, GLsizeiptr length,
                           const std::uint8_t* mirror, const std::uint8_t* driver);
void gl_diagnose_map_unmap(const GlMapDiagnostic& mapping, std::uint64_t length,
                           const std::uint8_t* mirror, const std::uint8_t* driver);
void gl_diagnose_map_collision(HostGl& host, GLenum target,
                               const GlMapDiagnostic& existing);

}  // namespace zb
