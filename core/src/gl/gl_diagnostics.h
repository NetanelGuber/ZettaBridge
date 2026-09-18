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

}  // namespace zb
