// Generated once from core/include/zb/gl_backend.h for GlDriverBackend (Phase 5 Task 8).
// Do not edit gl_backend.h and expect this to update: regenerate by hand if the GLES surface
// changes (see tools/gen_gles.py's HANDWRITTEN/manual lists for the source of truth signatures).
// glGetError is hand-written in gl_driver_backend.cpp, not generated here.
    void glActiveTexture(GLenum texture) override { ::glActiveTexture(texture); }
    void glAttachShader(GLuint program, GLuint shader) override { ::glAttachShader(program, shader); }
    void glBindAttribLocation(GLuint program, GLuint index, const GLchar *name) override { ::glBindAttribLocation(program, index, name); }
    void glBindBuffer(GLenum target, GLuint buffer) override { ::glBindBuffer(target, buffer); }
    void glBindFramebuffer(GLenum target, GLuint framebuffer) override { ::glBindFramebuffer(target, framebuffer); }
    void glBindRenderbuffer(GLenum target, GLuint renderbuffer) override { ::glBindRenderbuffer(target, renderbuffer); }
    void glBindTexture(GLenum target, GLuint texture) override { ::glBindTexture(target, texture); }
    void glBlendColor(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha) override { ::glBlendColor(red, green, blue, alpha); }
    void glBlendEquation(GLenum mode) override { ::glBlendEquation(mode); }
    void glBlendEquationSeparate(GLenum modeRGB, GLenum modeAlpha) override { ::glBlendEquationSeparate(modeRGB, modeAlpha); }
    void glBlendFunc(GLenum sfactor, GLenum dfactor) override { ::glBlendFunc(sfactor, dfactor); }
    void glBlendFuncSeparate(GLenum sfactorRGB, GLenum dfactorRGB, GLenum sfactorAlpha, GLenum dfactorAlpha) override { ::glBlendFuncSeparate(sfactorRGB, dfactorRGB, sfactorAlpha, dfactorAlpha); }
    void glBufferData(GLenum target, GLsizeiptr size, const void *data, GLenum usage) override { ::glBufferData(target, size, data, usage); }
    void glBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const void *data) override { ::glBufferSubData(target, offset, size, data); }
    GLenum glCheckFramebufferStatus(GLenum target) override { return static_cast<GLenum>(::glCheckFramebufferStatus(target)); }
    void glClear(GLbitfield mask) override { ::glClear(mask); }
    void glClearColor(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha) override { ::glClearColor(red, green, blue, alpha); }
    void glClearDepthf(GLfloat d) override { ::glClearDepthf(d); }
    void glClearStencil(GLint s) override { ::glClearStencil(s); }
    void glColorMask(GLboolean red, GLboolean green, GLboolean blue, GLboolean alpha) override { ::glColorMask(red, green, blue, alpha); }
    void glCompileShader(GLuint shader) override { ::glCompileShader(shader); }
    void glCompressedTexImage2D(GLenum target, GLint level, GLenum internalformat, GLsizei width, GLsizei height, GLint border, GLsizei imageSize, const void *data) override { ::glCompressedTexImage2D(target, level, internalformat, width, height, border, imageSize, data); }
    void glCompressedTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLsizei imageSize, const void *data) override { ::glCompressedTexSubImage2D(target, level, xoffset, yoffset, width, height, format, imageSize, data); }
    void glCopyTexImage2D(GLenum target, GLint level, GLenum internalformat, GLint x, GLint y, GLsizei width, GLsizei height, GLint border) override { ::glCopyTexImage2D(target, level, internalformat, x, y, width, height, border); }
    void glCopyTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint x, GLint y, GLsizei width, GLsizei height) override { ::glCopyTexSubImage2D(target, level, xoffset, yoffset, x, y, width, height); }
    GLuint glCreateProgram(void) override { return static_cast<GLuint>(::glCreateProgram()); }
    GLuint glCreateShader(GLenum type) override { return static_cast<GLuint>(::glCreateShader(type)); }
    void glCullFace(GLenum mode) override { ::glCullFace(mode); }
    void glDeleteBuffers(GLsizei n, const GLuint *buffers) override { ::glDeleteBuffers(n, buffers); }
    void glDeleteFramebuffers(GLsizei n, const GLuint *framebuffers) override { ::glDeleteFramebuffers(n, framebuffers); }
    void glDeleteProgram(GLuint program) override { ::glDeleteProgram(program); }
    void glDeleteRenderbuffers(GLsizei n, const GLuint *renderbuffers) override { ::glDeleteRenderbuffers(n, renderbuffers); }
    void glDeleteShader(GLuint shader) override { ::glDeleteShader(shader); }
    void glDeleteTextures(GLsizei n, const GLuint *textures) override { ::glDeleteTextures(n, textures); }
    void glDepthFunc(GLenum func) override { ::glDepthFunc(func); }
    void glDepthMask(GLboolean flag) override { ::glDepthMask(flag); }
    void glDepthRangef(GLfloat n, GLfloat f) override { ::glDepthRangef(n, f); }
    void glDetachShader(GLuint program, GLuint shader) override { ::glDetachShader(program, shader); }
    void glDisable(GLenum cap) override { ::glDisable(cap); }
    void glDisableVertexAttribArray(GLuint index) override { ::glDisableVertexAttribArray(index); }
    void glDrawArrays(GLenum mode, GLint first, GLsizei count) override { ::glDrawArrays(mode, first, count); }
    void glDrawElements(GLenum mode, GLsizei count, GLenum type, const void *indices) override { ::glDrawElements(mode, count, type, indices); }
    void glEnable(GLenum cap) override { ::glEnable(cap); }
    void glEnableVertexAttribArray(GLuint index) override { ::glEnableVertexAttribArray(index); }
    void glFinish(void) override { ::glFinish(); }
    void glFlush(void) override { ::glFlush(); }
    void glFramebufferRenderbuffer(GLenum target, GLenum attachment, GLenum renderbuffertarget, GLuint renderbuffer) override { ::glFramebufferRenderbuffer(target, attachment, renderbuffertarget, renderbuffer); }
    void glFramebufferTexture2D(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level) override { ::glFramebufferTexture2D(target, attachment, textarget, texture, level); }
    void glFrontFace(GLenum mode) override { ::glFrontFace(mode); }
    void glGenBuffers(GLsizei n, GLuint *buffers) override { ::glGenBuffers(n, buffers); }
    void glGenFramebuffers(GLsizei n, GLuint *framebuffers) override { ::glGenFramebuffers(n, framebuffers); }
    void glGenRenderbuffers(GLsizei n, GLuint *renderbuffers) override { ::glGenRenderbuffers(n, renderbuffers); }
    void glGenTextures(GLsizei n, GLuint *textures) override { ::glGenTextures(n, textures); }
    void glGenerateMipmap(GLenum target) override { ::glGenerateMipmap(target); }
    void glGetActiveAttrib(GLuint program, GLuint index, GLsizei bufSize, GLsizei *length, GLint *size, GLenum *type, GLchar *name) override { ::glGetActiveAttrib(program, index, bufSize, length, size, type, name); }
    void glGetActiveUniform(GLuint program, GLuint index, GLsizei bufSize, GLsizei *length, GLint *size, GLenum *type, GLchar *name) override { ::glGetActiveUniform(program, index, bufSize, length, size, type, name); }
    void glGetAttachedShaders(GLuint program, GLsizei maxCount, GLsizei *count, GLuint *shaders) override { ::glGetAttachedShaders(program, maxCount, count, shaders); }
    GLint glGetAttribLocation(GLuint program, const GLchar *name) override { return static_cast<GLint>(::glGetAttribLocation(program, name)); }
    void glGetBooleanv(GLenum pname, GLboolean *data) override { ::glGetBooleanv(pname, data); }
    void glGetBufferParameteriv(GLenum target, GLenum pname, GLint *params) override { ::glGetBufferParameteriv(target, pname, params); }
    void glGetFloatv(GLenum pname, GLfloat *data) override { ::glGetFloatv(pname, data); }
    void glGetFramebufferAttachmentParameteriv(GLenum target, GLenum attachment, GLenum pname, GLint *params) override { ::glGetFramebufferAttachmentParameteriv(target, attachment, pname, params); }
    void glGetIntegerv(GLenum pname, GLint *data) override { ::glGetIntegerv(pname, data); }
    void glGetProgramInfoLog(GLuint program, GLsizei bufSize, GLsizei *length, GLchar *infoLog) override { ::glGetProgramInfoLog(program, bufSize, length, infoLog); }
    void glGetProgramiv(GLuint program, GLenum pname, GLint *params) override { ::glGetProgramiv(program, pname, params); }
    void glGetRenderbufferParameteriv(GLenum target, GLenum pname, GLint *params) override { ::glGetRenderbufferParameteriv(target, pname, params); }
    void glGetShaderInfoLog(GLuint shader, GLsizei bufSize, GLsizei *length, GLchar *infoLog) override { ::glGetShaderInfoLog(shader, bufSize, length, infoLog); }
    void glGetShaderPrecisionFormat(GLenum shadertype, GLenum precisiontype, GLint *range, GLint *precision) override { ::glGetShaderPrecisionFormat(shadertype, precisiontype, range, precision); }
    void glGetShaderSource(GLuint shader, GLsizei bufSize, GLsizei *length, GLchar *source) override { ::glGetShaderSource(shader, bufSize, length, source); }
    void glGetShaderiv(GLuint shader, GLenum pname, GLint *params) override { ::glGetShaderiv(shader, pname, params); }
    const GLubyte * glGetString(GLenum name) override { return static_cast<const GLubyte *>(::glGetString(name)); }
    void glGetTexParameterfv(GLenum target, GLenum pname, GLfloat *params) override { ::glGetTexParameterfv(target, pname, params); }
    void glGetTexParameteriv(GLenum target, GLenum pname, GLint *params) override { ::glGetTexParameteriv(target, pname, params); }
    GLint glGetUniformLocation(GLuint program, const GLchar *name) override { return static_cast<GLint>(::glGetUniformLocation(program, name)); }
    void glGetUniformfv(GLuint program, GLint location, GLfloat *params) override { ::glGetUniformfv(program, location, params); }
    void glGetUniformiv(GLuint program, GLint location, GLint *params) override { ::glGetUniformiv(program, location, params); }
    void glGetVertexAttribPointerv(GLuint index, GLenum pname, void **pointer) override { ::glGetVertexAttribPointerv(index, pname, pointer); }
    void glGetVertexAttribfv(GLuint index, GLenum pname, GLfloat *params) override { ::glGetVertexAttribfv(index, pname, params); }
    void glGetVertexAttribiv(GLuint index, GLenum pname, GLint *params) override { ::glGetVertexAttribiv(index, pname, params); }
    void glHint(GLenum target, GLenum mode) override { ::glHint(target, mode); }
    GLboolean glIsBuffer(GLuint buffer) override { return static_cast<GLboolean>(::glIsBuffer(buffer)); }
    GLboolean glIsEnabled(GLenum cap) override { return static_cast<GLboolean>(::glIsEnabled(cap)); }
    GLboolean glIsFramebuffer(GLuint framebuffer) override { return static_cast<GLboolean>(::glIsFramebuffer(framebuffer)); }
    GLboolean glIsProgram(GLuint program) override { return static_cast<GLboolean>(::glIsProgram(program)); }
    GLboolean glIsRenderbuffer(GLuint renderbuffer) override { return static_cast<GLboolean>(::glIsRenderbuffer(renderbuffer)); }
    GLboolean glIsShader(GLuint shader) override { return static_cast<GLboolean>(::glIsShader(shader)); }
    GLboolean glIsTexture(GLuint texture) override { return static_cast<GLboolean>(::glIsTexture(texture)); }
    void glLineWidth(GLfloat width) override { ::glLineWidth(width); }
    void glLinkProgram(GLuint program) override { ::glLinkProgram(program); }
    void glPixelStorei(GLenum pname, GLint param) override { ::glPixelStorei(pname, param); }
    void glPolygonOffset(GLfloat factor, GLfloat units) override { ::glPolygonOffset(factor, units); }
    void glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void *pixels) override { ::glReadPixels(x, y, width, height, format, type, pixels); }
    void glReleaseShaderCompiler(void) override { ::glReleaseShaderCompiler(); }
    void glRenderbufferStorage(GLenum target, GLenum internalformat, GLsizei width, GLsizei height) override { ::glRenderbufferStorage(target, internalformat, width, height); }
    void glSampleCoverage(GLfloat value, GLboolean invert) override { ::glSampleCoverage(value, invert); }
    void glScissor(GLint x, GLint y, GLsizei width, GLsizei height) override { ::glScissor(x, y, width, height); }
    void glShaderBinary(GLsizei count, const GLuint *shaders, GLenum binaryFormat, const void *binary, GLsizei length) override { ::glShaderBinary(count, shaders, binaryFormat, binary, length); }
    void glShaderSource(GLuint shader, GLsizei count, const GLchar *const*string, const GLint *length) override { ::glShaderSource(shader, count, string, length); }
    void glStencilFunc(GLenum func, GLint ref, GLuint mask) override { ::glStencilFunc(func, ref, mask); }
    void glStencilFuncSeparate(GLenum face, GLenum func, GLint ref, GLuint mask) override { ::glStencilFuncSeparate(face, func, ref, mask); }
    void glStencilMask(GLuint mask) override { ::glStencilMask(mask); }
    void glStencilMaskSeparate(GLenum face, GLuint mask) override { ::glStencilMaskSeparate(face, mask); }
    void glStencilOp(GLenum fail, GLenum zfail, GLenum zpass) override { ::glStencilOp(fail, zfail, zpass); }
    void glStencilOpSeparate(GLenum face, GLenum sfail, GLenum dpfail, GLenum dppass) override { ::glStencilOpSeparate(face, sfail, dpfail, dppass); }
    void glTexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const void *pixels) override { ::glTexImage2D(target, level, internalformat, width, height, border, format, type, pixels); }
    void glTexParameterf(GLenum target, GLenum pname, GLfloat param) override { ::glTexParameterf(target, pname, param); }
    void glTexParameterfv(GLenum target, GLenum pname, const GLfloat *params) override { ::glTexParameterfv(target, pname, params); }
    void glTexParameteri(GLenum target, GLenum pname, GLint param) override { ::glTexParameteri(target, pname, param); }
    void glTexParameteriv(GLenum target, GLenum pname, const GLint *params) override { ::glTexParameteriv(target, pname, params); }
    void glTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type, const void *pixels) override { ::glTexSubImage2D(target, level, xoffset, yoffset, width, height, format, type, pixels); }
    void glUniform1f(GLint location, GLfloat v0) override { ::glUniform1f(location, v0); }
    void glUniform1fv(GLint location, GLsizei count, const GLfloat *value) override { ::glUniform1fv(location, count, value); }
    void glUniform1i(GLint location, GLint v0) override { ::glUniform1i(location, v0); }
    void glUniform1iv(GLint location, GLsizei count, const GLint *value) override { ::glUniform1iv(location, count, value); }
    void glUniform2f(GLint location, GLfloat v0, GLfloat v1) override { ::glUniform2f(location, v0, v1); }
    void glUniform2fv(GLint location, GLsizei count, const GLfloat *value) override { ::glUniform2fv(location, count, value); }
    void glUniform2i(GLint location, GLint v0, GLint v1) override { ::glUniform2i(location, v0, v1); }
    void glUniform2iv(GLint location, GLsizei count, const GLint *value) override { ::glUniform2iv(location, count, value); }
    void glUniform3f(GLint location, GLfloat v0, GLfloat v1, GLfloat v2) override { ::glUniform3f(location, v0, v1, v2); }
    void glUniform3fv(GLint location, GLsizei count, const GLfloat *value) override { ::glUniform3fv(location, count, value); }
    void glUniform3i(GLint location, GLint v0, GLint v1, GLint v2) override { ::glUniform3i(location, v0, v1, v2); }
    void glUniform3iv(GLint location, GLsizei count, const GLint *value) override { ::glUniform3iv(location, count, value); }
    void glUniform4f(GLint location, GLfloat v0, GLfloat v1, GLfloat v2, GLfloat v3) override { ::glUniform4f(location, v0, v1, v2, v3); }
    void glUniform4fv(GLint location, GLsizei count, const GLfloat *value) override { ::glUniform4fv(location, count, value); }
    void glUniform4i(GLint location, GLint v0, GLint v1, GLint v2, GLint v3) override { ::glUniform4i(location, v0, v1, v2, v3); }
    void glUniform4iv(GLint location, GLsizei count, const GLint *value) override { ::glUniform4iv(location, count, value); }
    void glUniformMatrix2fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value) override { ::glUniformMatrix2fv(location, count, transpose, value); }
    void glUniformMatrix3fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value) override { ::glUniformMatrix3fv(location, count, transpose, value); }
    void glUniformMatrix4fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value) override { ::glUniformMatrix4fv(location, count, transpose, value); }
    void glUseProgram(GLuint program) override { ::glUseProgram(program); }
    void glValidateProgram(GLuint program) override { ::glValidateProgram(program); }
    void glVertexAttrib1f(GLuint index, GLfloat x) override { ::glVertexAttrib1f(index, x); }
    void glVertexAttrib1fv(GLuint index, const GLfloat *v) override { ::glVertexAttrib1fv(index, v); }
    void glVertexAttrib2f(GLuint index, GLfloat x, GLfloat y) override { ::glVertexAttrib2f(index, x, y); }
    void glVertexAttrib2fv(GLuint index, const GLfloat *v) override { ::glVertexAttrib2fv(index, v); }
    void glVertexAttrib3f(GLuint index, GLfloat x, GLfloat y, GLfloat z) override { ::glVertexAttrib3f(index, x, y, z); }
    void glVertexAttrib3fv(GLuint index, const GLfloat *v) override { ::glVertexAttrib3fv(index, v); }
    void glVertexAttrib4f(GLuint index, GLfloat x, GLfloat y, GLfloat z, GLfloat w) override { ::glVertexAttrib4f(index, x, y, z, w); }
    void glVertexAttrib4fv(GLuint index, const GLfloat *v) override { ::glVertexAttrib4fv(index, v); }
    void glVertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void *pointer) override { ::glVertexAttribPointer(index, size, type, normalized, stride, pointer); }
    void glViewport(GLint x, GLint y, GLsizei width, GLsizei height) override { ::glViewport(x, y, width, height); }
