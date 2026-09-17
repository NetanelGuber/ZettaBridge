#pragma once

#include "zb/host_gl.h"

namespace zb {

// Records GL visibility facts for an accepted GL host call into the runtime report
// (gl_diagnostics.cpp). May query the driver through host.backend().
void gl_diagnose(HostGl& host, HostGl::Call& call);

}  // namespace zb
