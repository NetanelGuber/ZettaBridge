# ZettaBridge — import surface, Orange Roulette 1.0.0 (armeabi)

Guest libs and their undefined-symbol counts (these are the host functions
that need arm32 trampolines):

| guest lib | imports | JNI entry points |
|---|---|---|
| `libApplicationMain.so` | 91 | 1 |
| `liblime.so` | 386 | 19 |
| `libopenal.so` | 71 | 0 |
| `libregexp.so` | 23 | 0 |
| `libstd.so` | 89 | 0 |
| `libzlib.so` | 32 | 0 |

**Total unique imports: 443**

## By category

- **libc / libm** — 258
- **GLES** — 128
- **pthread** — 24
- **OpenAL** — 18
- **libandroid** — 6
- **libstdc++ / C++ ABI** — 5
- **libdl** — 3
- **liblog** — 1

Note: `egl*` count is zero. The GL context is created on the Java side
(GLSurfaceView); the guest only issues draw calls. Removes a whole class of work.

## JNI entry points (Java -> guest native)

### `libApplicationMain.so`

- `Java_org_haxe_HXCPP_main`

### `liblime.so`

- `Java_org_haxe_lime_Lime_callNumericFunction`
- `Java_org_haxe_lime_Lime_callObjectFunction`
- `Java_org_haxe_lime_Lime_getNextWake`
- `Java_org_haxe_lime_Lime_onAccelerate`
- `Java_org_haxe_lime_Lime_onActivity`
- `Java_org_haxe_lime_Lime_onCallback`
- `Java_org_haxe_lime_Lime_onContextLost`
- `Java_org_haxe_lime_Lime_onDeviceOrientationUpdate`
- `Java_org_haxe_lime_Lime_onJoyChange`
- `Java_org_haxe_lime_Lime_onJoyMotion`
- `Java_org_haxe_lime_Lime_onKeyChange`
- `Java_org_haxe_lime_Lime_onNormalOrientationFound`
- `Java_org_haxe_lime_Lime_onOrientationUpdate`
- `Java_org_haxe_lime_Lime_onPoll`
- `Java_org_haxe_lime_Lime_onRender`
- `Java_org_haxe_lime_Lime_onResize`
- `Java_org_haxe_lime_Lime_onTouch`
- `Java_org_haxe_lime_Lime_onTrackball`
- `Java_org_haxe_lime_Lime_releaseReference`

## libc / libm (258)

```
_Unwind_Complete
_Unwind_DeleteException
_Unwind_GetDataRelBase
_Unwind_GetLanguageSpecificData
_Unwind_GetRegionStart
_Unwind_GetTextRelBase
_Unwind_RaiseException
_Unwind_Resume
_Unwind_Resume_or_Rethrow
_Unwind_VRS_Get
_Unwind_VRS_Set
__aeabi_d2f
__aeabi_d2lz
__aeabi_dcmpeq
__aeabi_dcmpgt
__aeabi_f2d
__aeabi_fadd
__aeabi_fcmpeq
__aeabi_fcmpgt
__aeabi_fdiv
__aeabi_fmul
__aeabi_idiv
__aeabi_idivmod
__aeabi_l2d
__aeabi_ldivmod
__aeabi_uidiv
__aeabi_uidivmod
__aeabi_uldivmod
__aeabi_unwind_cpp_pr0
__aeabi_unwind_cpp_pr1
__errno
__gnu_Unwind_Find_exidx
__gnu_unwind_frame
__sF
__srget
__stack_chk_fail
__stack_chk_guard
_ctype_
_tolower_tab_
_toupper_tab_
abort
accept
acos
acosf
alarm
alcCreateContext
alcMakeContextCurrent
alcOpenDevice
alcProcessContext
alcResume
alcSuspend
alcSuspendContext
asin
atan
atan2
atanf
atoi
atol
basename
bind
bsd_signal
btowc
calloc
ceil
chdir
clock_gettime
close
closedir
connect
cos
cosh
crc32
ctime
deflate
deflateEnd
deflateInit2_
deflateReset
dup2
environ
execvp
exit
exp
fclose
fcntl
fdopen
fflush
fgets
floor
fmod
fopen
fork
fprintf
fputc
fputs
fread
free
freeaddrinfo
frexp
fseek
fstat
ftell
fwprintf
fwrite
getaddrinfo
getc
getcwd
getenv
geteuid
gethostbyaddr
gethostbyname_r
gethostname
getpeername
getpid
getpwuid
getsockname
getsockopt
gettimeofday
getwc
gmtime
gmtime_r
inet_addr
inet_ntoa
inet_ntop
inet_pton
inflate
inflateEnd
inflateInit2_
inflateInit_
inflateReset
ioctl
iswctype
iswspace
ldexp
listen
localtime
log
log10
longjmp
lrand48
lseek
malloc
mbrtowc
memchr
memcmp
memcpy
memmove
memset
mkdir
mktime
modf
nanosleep
open
opendir
pipe
poll
pow
powf
printf
putc
putchar
puts
putwc
qsort
raise
read
readdir
readlink
realloc
realpath
recv
recvfrom
rename
rewind
rint
rmdir
sched_get_priority_min
sched_yield
select
send
sendto
setbuf
setenv
setjmp
setlocale
setsockopt
setvbuf
shutdown
sigaction
siglongjmp
sigsetjmp
sin
sinh
snprintf
socket
sprintf
sqrt
sqrtf
srand48
sscanf
stat
strcasecmp
strcat
strchr
strcmp
strcoll
strcpy
strdup
strerror
strerror_r
strftime
strlen
strncasecmp
strncmp
strncpy
strrchr
strstr
strtod
strtok_r
strtol
strtoll
strtoul
strxfrm
swprintf
swscanf
sysconf
system
tan
tanh
time
times
towlower
towupper
ungetc
ungetwc
unlink
usleep
vfprintf
vprintf
vsnprintf
vsprintf
waitpid
wcrtomb
wcschr
wcscmp
wcscoll
wcsftime
wcslen
wcsncmp
wcsxfrm
wctob
wctype
wmemchr
wmemcmp
wmemcpy
wmemmove
wmemset
write
zlibVersion
```

## GLES (128)

```
glActiveTexture
glAttachShader
glBindAttribLocation
glBindBuffer
glBindFramebuffer
glBindRenderbuffer
glBindTexture
glBlendColor
glBlendEquation
glBlendEquationSeparate
glBlendFunc
glBlendFuncSeparate
glBufferData
glBufferSubData
glCheckFramebufferStatus
glClear
glClearColor
glClearDepthf
glClearStencil
glColorMask
glCompileShader
glCompressedTexImage2D
glCompressedTexSubImage2D
glCopyTexImage2D
glCopyTexSubImage2D
glCreateProgram
glCreateShader
glCullFace
glDeleteBuffers
glDeleteFramebuffers
glDeleteProgram
glDeleteRenderbuffers
glDeleteShader
glDeleteTextures
glDepthFunc
glDepthMask
glDepthRangef
glDetachShader
glDisable
glDisableVertexAttribArray
glDrawArrays
glDrawElements
glEnable
glEnableVertexAttribArray
glFinish
glFlush
glFramebufferRenderbuffer
glFramebufferTexture2D
glFrontFace
glGenBuffers
glGenFramebuffers
glGenRenderbuffers
glGenTextures
glGenerateMipmap
glGetActiveAttrib
glGetActiveUniform
glGetAttribLocation
glGetBufferParameteriv
glGetError
glGetFloatv
glGetFramebufferAttachmentParameteriv
glGetIntegerv
glGetProgramInfoLog
glGetProgramiv
glGetRenderbufferParameteriv
glGetShaderInfoLog
glGetShaderPrecisionFormat
glGetShaderSource
glGetShaderiv
glGetString
glGetTexParameteriv
glGetUniformLocation
glGetUniformfv
glGetUniformiv
glGetVertexAttribPointerv
glGetVertexAttribiv
glHint
glIsEnabled
glLineWidth
glLinkProgram
glPixelStorei
glPolygonOffset
glReadPixels
glRenderbufferStorage
glSampleCoverage
glScissor
glShaderSource
glStencilFunc
glStencilFuncSeparate
glStencilMask
glStencilMaskSeparate
glStencilOp
glStencilOpSeparate
glTexImage2D
glTexParameterf
glTexParameteri
glTexSubImage2D
glUniform1f
glUniform1fv
glUniform1i
glUniform1iv
glUniform2f
glUniform2fv
glUniform2i
glUniform2iv
glUniform3f
glUniform3fv
glUniform3i
glUniform3iv
glUniform4f
glUniform4fv
glUniform4i
glUniform4iv
glUniformMatrix2fv
glUniformMatrix3fv
glUniformMatrix4fv
glUseProgram
glValidateProgram
glVertexAttrib1f
glVertexAttrib1fv
glVertexAttrib2f
glVertexAttrib2fv
glVertexAttrib3f
glVertexAttrib3fv
glVertexAttrib4f
glVertexAttrib4fv
glVertexAttribPointer
glViewport
```

## pthread (24)

```
pthread_cond_broadcast
pthread_cond_destroy
pthread_cond_init
pthread_cond_signal
pthread_cond_timedwait
pthread_cond_wait
pthread_create
pthread_detach
pthread_getspecific
pthread_join
pthread_key_create
pthread_key_delete
pthread_mutex_destroy
pthread_mutex_init
pthread_mutex_lock
pthread_mutex_trylock
pthread_mutex_unlock
pthread_mutexattr_destroy
pthread_mutexattr_init
pthread_mutexattr_settype
pthread_once
pthread_self
pthread_setschedparam
pthread_setspecific
```

## OpenAL (18)

```
alBufferData
alDeleteBuffers
alDeleteSources
alGenBuffers
alGenSources
alGetBufferi
alGetError
alGetSource3f
alGetSourcef
alGetSourcei
alSource3f
alSourcePause
alSourcePlay
alSourceQueueBuffers
alSourceStop
alSourceUnqueueBuffers
alSourcef
alSourcei
```

## libandroid (6)

```
AAssetManager_fromJava
AAssetManager_open
AAsset_close
AAsset_getLength
AAsset_openFileDescriptor
AAsset_read
```

## libstdc++ / C++ ABI (5)

```
__cxa_atexit
__cxa_begin_cleanup
__cxa_call_unexpected
__cxa_finalize
__cxa_type_match
```

## libdl (3)

```
dlerror
dlopen
dlsym
```

## liblog (1)

```
__android_log_print
```

