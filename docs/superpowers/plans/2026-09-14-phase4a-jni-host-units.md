# Phase 4a: JNI Host Units Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build and test, on this machine, the host-side building blocks of the JNI bridge. None of them needs ART or a running guest:
- `Java_*` name decoding;
- signature -> shorty;
- AAPCS64 -> AAPCS32 softfp argument marshaling;
- 32-bit handle tables;
- the precompiled arm64 thunk pool.

**Architecture:**
- Pure C++20 units in `core/src/jni/` with public headers in `core/include/zb/`, plus one assembly file.
- Host references and ids are opaque `uint64_t` values, so none of this depends on `jni.h` and everything is unit-testable.
- Later plans (4b-4d, see the end) connect the units to the guest process and to real JNI.

**Tech Stack:** C++20, clang, CMake + Ninja, AArch64 GNU assembly (clang integrated assembler), host tests with the `CHECK` macro from `tests/host/check.h`.

**Spec:** `docs/superpowers/specs/2026-09-14-jni-bridge-design.md`:
- section 1: name decoding;
- section 2: thunk pool and marshaling;
- section 3: handles.

**Verified before writing this plan:**
- The thunk pool and common entry below were assembled with clang and run on this machine.
- A call through thunk 5 with ten integer and two FP arguments delivered slot 5, x0-x7, host stack arguments 8 and 9, the float and the double.
- Return values came back through x0 and d0.
- Thunk 16383 worked.

---

## File Structure

| File | Responsibility |
|---|---|
| `core/include/zb/jni_mangle.h`, `core/src/jni/mangle.cpp` | decode `Java_<class>_<method>[__<args>]` export names |
| `core/include/zb/jni_shorty.h`, `core/src/jni/shorty.cpp` | method descriptor -> shorty (`"(IFFIFF)I"` -> `"IIFFIFF"`) |
| `core/include/zb/native_call.h`, `core/src/jni/native_call.cpp` | `NativeRegs` (saved host registers), host AAPCS64 -> guest AAPCS32 softfp argument layout, return value conversion |
| `core/include/zb/jni_handles.h`, `core/src/jni/handles.cpp` | local frames, global/weak tables, jmethodID/jfieldID table |
| `core/include/zb/native_thunks.h`, `core/src/jni/native_thunks.cpp`, `core/src/jni/thunks.S` | 16384 thunks, common entry, dispatcher hook, slot allocation |
| `tests/host/jni_mangle_test.cpp`, `jni_shorty_test.cpp`, `jni_abi_test.cpp`, `jni_handles_test.cpp`, `native_thunk_test.cpp` | host tests |
| `CMakeLists.txt` | enable the ASM language |
| `core/CMakeLists.txt`, `tests/host/CMakeLists.txt` | build wiring |

Commands used throughout (run from the repo root `/home/Zailox/ZettaBridge`):
- Configure (only needed once, or after editing `project()`):
  ```
  cmake -S . -B build/host -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
  ```
- Build and run one test:
  ```
  ninja -C build/host <test_name> && ctest --test-dir build/host -R <test_name> --output-on-failure
  ```

Conventions (see CLAUDE.md):
- ASCII only in files.
- Patch files with Python `str.replace()` heredocs or the editor, never `sed`.
- Commit locally after each task, never push.

---

### Task 1: `Java_*` export name decoding

**Files:**
- Create: `core/include/zb/jni_mangle.h`
- Create: `core/src/jni/mangle.cpp`
- Create: `tests/host/jni_mangle_test.cpp`
- Modify: `core/CMakeLists.txt` (source list), `tests/host/CMakeLists.txt` (`ZB_HOST_TESTS`)

- [ ] **Step 1: Write the failing test**

`tests/host/jni_mangle_test.cpp`:
```cpp
// JNI export names: "Java_" + mangled class and method, optional "__" + mangled argument types.
// Escapes: _1 = '_', _2 = ';', _3 = '[', _0xxxx = UTF-16 code unit; a plain '_' is '/'.
#include <cstdio>

#include "check.h"
#include "zb/jni_mangle.h"

int main() {
    auto simple = zb::decode_jni_export("Java_org_haxe_lime_Lime_onTouch");
    CHECK(simple && simple->class_name == "org/haxe/lime/Lime" && simple->method == "onTouch");
    CHECK(!simple->arguments);

    auto underscore = zb::decode_jni_export("Java_com_example_My_1Class_do_1it");
    CHECK(underscore && underscore->class_name == "com/example/My_Class" && underscore->method == "do_it");

    auto overloaded = zb::decode_jni_export("Java_pkg_Foo_bar__ILjava_lang_String_2_3I");
    CHECK(overloaded && overloaded->class_name == "pkg/Foo" && overloaded->method == "bar");
    CHECK(overloaded->arguments && *overloaded->arguments == "(ILjava/lang/String;[I)");

    auto no_args = zb::decode_jni_export("Java_pkg_Foo_bar__");
    CHECK(no_args && no_args->arguments && *no_args->arguments == "()");

    auto unicode = zb::decode_jni_export("Java_pkg_Caf_000e9_run");
    CHECK(unicode && unicode->class_name == "pkg/Caf\xC3\xA9" && unicode->method == "run");

    auto default_package = zb::decode_jni_export("Java_Main_start");
    CHECK(default_package && default_package->class_name == "Main" && default_package->method == "start");

    // "__" followed by '0'..'3' is not the argument separator: it is '/' followed by an escape.
    auto jna = zb::decode_jni_export("Java_com_sun_jna_Native__1getPointer");
    CHECK(jna && jna->class_name == "com/sun/jna/Native" && jna->method == "_getPointer");
    CHECK(!jna->arguments);

    auto init_array = zb::decode_jni_export("Java_pkg_Foo__1init___3I");
    CHECK(init_array && init_array->class_name == "pkg/Foo" && init_array->method == "_init");
    CHECK(init_array->arguments && *init_array->arguments == "([I)");

    auto org_internal = zb::decode_jni_export("Java_org__1internal_Foo_bar");
    CHECK(org_internal && org_internal->class_name == "org/_internal/Foo" && org_internal->method == "bar");
    CHECK(!org_internal->arguments);

    auto ref_arg = zb::decode_jni_export("Java_pkg_Foo_bar__Lorg__1x_Y_2");
    CHECK(ref_arg && ref_arg->class_name == "pkg/Foo" && ref_arg->method == "bar");
    CHECK(ref_arg->arguments && *ref_arg->arguments == "(Lorg/_x/Y;)");

    auto array_after_sep = zb::decode_jni_export("Java_pkg_Foo_bar___3I");
    CHECK(array_after_sep && array_after_sep->class_name == "pkg/Foo" && array_after_sep->method == "bar");
    CHECK(array_after_sep->arguments && *array_after_sep->arguments == "([I)");

    auto inner_class = zb::decode_jni_export("Java_pkg_Outer_00024Inner_run");
    CHECK(inner_class && inner_class->class_name == "pkg/Outer$Inner" && inner_class->method == "run");
    CHECK(!inner_class->arguments);

    auto surrogate_pair = zb::decode_jni_export("Java_pkg_X_0d83d_0de00_run");
    CHECK(surrogate_pair && surrogate_pair->class_name == "pkg/X\xED\xA0\xBD\xED\xB8\x80" &&
          surrogate_pair->method == "run");

    CHECK(!zb::decode_jni_export("JNI_OnLoad"));
    CHECK(!zb::decode_jni_export("Java_nomethod"));
    CHECK(!zb::decode_jni_export("Java_pkg_Foo_bar_"));
    CHECK(!zb::decode_jni_export("Java_pkg_Foo_bar_0zz12"));
    CHECK(!zb::decode_jni_export("Java_pkg_Foo_bar__I__J"));
    CHECK(!zb::decode_jni_export("Java_"));
    CHECK(!zb::decode_jni_export("Java__a_b"));
    CHECK(!zb::decode_jni_export("Java_a/b_c"));
    CHECK(!zb::decode_jni_export("Java_pkg_Foo_0002fbar"));
    CHECK(!zb::decode_jni_export("Java_pkg_Foo_00000bar"));
    CHECK(!zb::decode_jni_export("Java_pkg_Foo_b_00061r"));
    CHECK(!zb::decode_jni_export("Java_pkg_Foo_000E9run"));
    CHECK(!zb::decode_jni_export("Java_pkg_Foo_bar_000e"));
    CHECK(!zb::decode_jni_export("Java_pkg_Foo__3bar"));

    std::printf("jni_mangle_test ok\n");
    return 0;
}
```

In `tests/host/CMakeLists.txt`, replace the test list with:
```cmake
set(ZB_HOST_TESTS
    fault_pc_test
    guest_memory_test
    jni_mangle_test
    t1_blob_test
)
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `ninja -C build/host jni_mangle_test`
Expected: FAIL to compile with `fatal error: 'zb/jni_mangle.h' file not found`.

- [ ] **Step 3: Write the implementation**

`core/include/zb/jni_mangle.h`:
```cpp
#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace zb {

// A native method named by a JNI export symbol "Java_<class>_<method>[__<arguments>]".
struct JniExport {
    std::string class_name;  // binary name with slashes, e.g. "org/haxe/lime/Lime"
    std::string method;      // e.g. "onTouch"
    // Argument part of the descriptor for overloaded exports, e.g. "(IFFIFF)"; no return type.
    std::optional<std::string> arguments;
};

// Returns nullopt when the symbol is not a well-formed JNI export name.
std::optional<JniExport> decode_jni_export(std::string_view symbol);

}  // namespace zb
```

`core/src/jni/mangle.cpp`:
```cpp
// Decoding of JNI export names (JNI specification, "Resolving Native Method Names").
#include "zb/jni_mangle.h"

namespace zb {

namespace {

// Appends one UTF-16 code unit as UTF-8 (surrogates stay separate, as in modified UTF-8).
void append_utf8(std::string& out, unsigned unit) {
    if (unit < 0x80) {
        out.push_back(static_cast<char>(unit));
    } else if (unit < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (unit >> 6)));
        out.push_back(static_cast<char>(0x80 | (unit & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xE0 | (unit >> 12)));
        out.push_back(static_cast<char>(0x80 | ((unit >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (unit & 0x3F)));
    }
}

// Lowercase hex digits only: ART's mangler always emits "_0xxxx" in lowercase.
int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

bool is_ascii_alnum(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}

// True for a code unit that has its own canonical form, so ART's mangler would never reach it
// through "_0xxxx": ASCII alphanumerics pass through unescaped, '.' and '/' become a lone '_',
// '_' becomes "_1", ';' becomes "_2", '[' becomes "_3".
bool has_canonical_form(unsigned unit) {
    if (unit > 0x7F) return false;
    const char c = static_cast<char>(unit);
    return is_ascii_alnum(c) || c == '.' || c == '/' || c == '_' || c == ';' || c == '[';
}

// Decodes from pos until the end or a "__" separator (then hit_separator is set and pos is past
// it). A raw character must be [A-Za-z0-9]. A lone '_' (the next character is not '_') becomes
// '/'. "__" is the separator, unless the character after it is '0'..'3': then the first '_' is
// itself a lone '/' and the second '_' starts an escape for the following source character (e.g.
// a method named "_init" mangles as "..__1init", not a separator). "___3" (an array argument
// right after the real separator) still parses as separator + "_3". Escapes: "_1" = '_',
// "_2" = ';', "_3" = '[', "_0xxxx" = a UTF-16 code unit as 4 lowercase hex digits. Returns false
// for a non-alnum raw character, a malformed escape, a trailing '_', or an "_0xxxx" escape for a
// code unit that ART could not have produced that way: 0, or one with its own canonical form.
bool decode_part(std::string_view in, std::size_t& pos, std::string& out, bool& hit_separator) {
    hit_separator = false;
    while (pos < in.size()) {
        const char c = in[pos];
        if (c != '_') {
            if (!is_ascii_alnum(c)) return false;
            out.push_back(c);
            ++pos;
            continue;
        }
        if (pos + 1 >= in.size()) return false;
        switch (in[pos + 1]) {
        case '1':
            out.push_back('_');
            pos += 2;
            break;
        case '2':
            out.push_back(';');
            pos += 2;
            break;
        case '3':
            out.push_back('[');
            pos += 2;
            break;
        case '0': {
            if (pos + 6 > in.size()) return false;
            unsigned unit = 0;
            for (std::size_t i = pos + 2; i < pos + 6; ++i) {
                const int v = hex_value(in[i]);
                if (v < 0) return false;
                unit = unit * 16 + static_cast<unsigned>(v);
            }
            if (unit == 0 || has_canonical_form(unit)) return false;
            append_utf8(out, unit);
            pos += 6;
            break;
        }
        case '_':
            if (pos + 2 < in.size() && in[pos + 2] >= '0' && in[pos + 2] <= '3') {
                // The first '_' is a lone separator; the second one begins an escape for the
                // very next source character.
                out.push_back('/');
                ++pos;
                break;
            }
            hit_separator = true;
            pos += 2;
            return true;
        default:
            out.push_back('/');
            ++pos;
            break;
        }
    }
    return true;
}

}  // namespace

std::optional<JniExport> decode_jni_export(std::string_view symbol) {
    constexpr std::string_view kPrefix = "Java_";
    if (!symbol.starts_with(kPrefix)) return std::nullopt;
    std::size_t pos = kPrefix.size();

    std::string path;
    bool separator = false;
    if (!decode_part(symbol, pos, path, separator)) return std::nullopt;
    if (path.starts_with('/') || path.ends_with('/') || path.find("//") != std::string::npos ||
        path.find(';') != std::string::npos || path.find('[') != std::string::npos) {
        return std::nullopt;
    }
    const std::size_t slash = path.rfind('/');
    if (slash == std::string::npos || slash == 0 || slash + 1 == path.size()) return std::nullopt;

    JniExport result;
    result.class_name = path.substr(0, slash);
    result.method = path.substr(slash + 1);
    if (separator) {
        std::string arguments;
        bool again = false;
        if (!decode_part(symbol, pos, arguments, again) || again) return std::nullopt;
        result.arguments = "(" + arguments + ")";
    }
    return result;
}

}  // namespace zb
```

In `core/CMakeLists.txt`, add the source after `src/initial_stack.cpp`:
```cmake
    src/initial_stack.cpp
    src/jni/mangle.cpp
    src/log.cpp
```

- [ ] **Step 4: Run the test to verify it passes**

Run: `ninja -C build/host jni_mangle_test && ctest --test-dir build/host -R jni_mangle_test --output-on-failure`
Expected: `100% tests passed`.

- [ ] **Step 5: Commit**

```bash
git add core/include/zb/jni_mangle.h core/src/jni/mangle.cpp tests/host/jni_mangle_test.cpp core/CMakeLists.txt tests/host/CMakeLists.txt
git commit -m "jni: decode Java_* export names"
```

---

### Task 2: Signature -> shorty

**Files:**
- Create: `core/include/zb/jni_shorty.h`
- Create: `core/src/jni/shorty.cpp`
- Create: `tests/host/jni_shorty_test.cpp`
- Modify: `core/CMakeLists.txt`, `tests/host/CMakeLists.txt`

- [ ] **Step 1: Write the failing test**

`tests/host/jni_shorty_test.cpp`:
```cpp
// Shorty: return type first, then one letter per parameter; references and arrays are 'L'.
#include <cstdio>
#include <string>

#include "check.h"
#include "zb/jni_shorty.h"

int main() {
    CHECK(zb::shorty_from_signature("(IFFIFF)I") == std::string("IIFFIFF"));
    CHECK(zb::shorty_from_signature("(J)V") == std::string("VJ"));
    CHECK(zb::shorty_from_signature("(JLjava/lang/String;[Ljava/lang/Object;)D") == std::string("DJLL"));
    CHECK(zb::shorty_from_signature("()V") == std::string("V"));
    CHECK(zb::shorty_from_signature("([[I[Z)Lorg/haxe/lime/HaxeObject;") == std::string("LLL"));
    CHECK(zb::shorty_from_signature("(ZBCS)C") == std::string("CZBCS"));
    CHECK(zb::shorty_from_signature("()[I") == std::string("L"));
    CHECK(zb::shorty_from_signature("()J") == std::string("J"));
    CHECK(zb::shorty_from_signature("(D)D") == std::string("DD"));
    CHECK(zb::shorty_from_signature("(Ljava/lang/String;[[Ljava/lang/Object;)Ljava/lang/String;") ==
          std::string("LLL"));
    {
        const std::string sig255 = "(" + std::string(255, '[') + "I)V";
        CHECK(zb::shorty_from_signature(sig255) == std::string("VL"));
    }

    CHECK(!zb::shorty_from_signature("(V)V"));
    CHECK(!zb::shorty_from_signature("(I"));
    CHECK(!zb::shorty_from_signature("(Ljava/lang/String)V"));
    CHECK(!zb::shorty_from_signature("(L;)V"));
    CHECK(!zb::shorty_from_signature("(I)"));
    CHECK(!zb::shorty_from_signature("(I)VX"));
    CHECK(!zb::shorty_from_signature("I)V"));
    CHECK(!zb::shorty_from_signature("([)V"));
    CHECK(!zb::shorty_from_signature("(Lfoo)V;)V"));
    CHECK(!zb::shorty_from_signature("(Ljava/lang/String)IJ;)V"));
    CHECK(!zb::shorty_from_signature("()Lfoo)bar;"));
    CHECK(!zb::shorty_from_signature("(La[b;)V"));
    CHECK(!zb::shorty_from_signature("(La.b;)V"));
    CHECK(!zb::shorty_from_signature("(L/a;)V"));
    CHECK(!zb::shorty_from_signature("(La/;)V"));
    CHECK(!zb::shorty_from_signature("(La//b;)V"));
    CHECK(!zb::shorty_from_signature("()[V"));
    CHECK(!zb::shorty_from_signature("(I)L;"));
    CHECK(!zb::shorty_from_signature(""));
    {
        const std::string sig256 = "(" + std::string(256, '[') + "I)V";
        CHECK(!zb::shorty_from_signature(sig256));
    }

    std::printf("jni_shorty_test ok\n");
    return 0;
}
```

In `tests/host/CMakeLists.txt`:
```cmake
set(ZB_HOST_TESTS
    fault_pc_test
    guest_memory_test
    jni_mangle_test
    jni_shorty_test
    t1_blob_test
)
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `ninja -C build/host jni_shorty_test`
Expected: FAIL to compile with `fatal error: 'zb/jni_shorty.h' file not found`.

- [ ] **Step 3: Write the implementation**

`core/include/zb/jni_shorty.h`:
```cpp
#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace zb {

// Method shorty: the return type letter first, then one letter per parameter. Primitive types
// keep their descriptor letter (Z B C S I J F D, and V for the return); references and arrays
// are 'L'. Example: "(IFFIFF)I" -> "IIFFIFF". Returns nullopt for a malformed descriptor.
std::optional<std::string> shorty_from_signature(std::string_view signature);

}  // namespace zb
```

`core/src/jni/shorty.cpp`:
```cpp
#include "zb/jni_shorty.h"

#include <cstddef>

namespace zb {

namespace {

// Parses one type at pos and returns its shorty letter, or 0 when malformed. 'V' is accepted
// only where void_ok is set (the return type).
char parse_type(std::string_view s, std::size_t& pos, bool void_ok) {
    if (pos >= s.size()) return 0;
    switch (s[pos]) {
    case 'Z':
    case 'B':
    case 'C':
    case 'S':
    case 'I':
    case 'J':
    case 'F':
    case 'D':
        return s[pos++];
    case 'V':
        if (!void_ok) return 0;
        ++pos;
        return 'V';
    case 'L': {
        // The class name runs up to the first ';'; any of "()[." found first means the ';' the
        // caller (or an earlier malformed scan) thought terminated this name actually belongs to
        // an outer construct, so reject rather than swallow it. A binary name may not start or
        // end with '/', nor contain "//".
        const std::size_t end = s.find_first_of(";()[.", pos + 1);
        if (end == std::string_view::npos || s[end] != ';') return 0;
        const std::string_view name = s.substr(pos + 1, end - (pos + 1));
        if (name.empty() || name.front() == '/' || name.back() == '/' ||
            name.find("//") != std::string_view::npos) {
            return 0;
        }
        pos = end + 1;
        return 'L';
    }
    case '[': {
        // The JVM limits array types to 255 dimensions.
        std::size_t dims = 0;
        while (pos < s.size() && s[pos] == '[') {
            ++pos;
            ++dims;
        }
        if (dims > 255) return 0;
        return parse_type(s, pos, false) != 0 ? 'L' : 0;
    }
    default:
        return 0;
    }
}

}  // namespace

std::optional<std::string> shorty_from_signature(std::string_view signature) {
    if (signature.empty() || signature[0] != '(') return std::nullopt;
    std::size_t pos = 1;
    std::string parameters;
    while (pos < signature.size() && signature[pos] != ')') {
        const char type = parse_type(signature, pos, false);
        if (type == 0) return std::nullopt;
        parameters.push_back(type);
    }
    if (pos >= signature.size()) return std::nullopt;
    ++pos;  // ')'
    const char return_type = parse_type(signature, pos, true);
    if (return_type == 0 || pos != signature.size()) return std::nullopt;
    return std::string(1, return_type) + parameters;
}

}  // namespace zb
```

In `core/CMakeLists.txt`:
```cmake
    src/jni/mangle.cpp
    src/jni/shorty.cpp
    src/log.cpp
```

- [ ] **Step 4: Run the test to verify it passes**

Run: `ninja -C build/host jni_shorty_test && ctest --test-dir build/host -R jni_shorty_test --output-on-failure`
Expected: `100% tests passed`.

- [ ] **Step 5: Commit**

```bash
git add core/include/zb/jni_shorty.h core/src/jni/shorty.cpp tests/host/jni_shorty_test.cpp core/CMakeLists.txt tests/host/CMakeLists.txt
git commit -m "jni: method descriptor to shorty"
```

---

### Task 3: AAPCS64 -> AAPCS32 softfp marshaling

**Files:**
- Create: `core/include/zb/native_call.h`
- Create: `core/src/jni/native_call.cpp`
- Create: `tests/host/jni_abi_test.cpp`
- Modify: `core/CMakeLists.txt`, `tests/host/CMakeLists.txt`

**ABI rules this task implements:**
- **Host (AAPCS64, Linux/Android).**
  - Integer-class arguments go in x0-x7 and floating point in d0-d7; a float uses the low 32 bits.
  - Overflow arguments take 8-byte stack slots in argument order.
  - For JNI, x0 = `JNIEnv*` and x1 = `jclass`/`jobject`.
- **Guest (AAPCS32 base standard, softfp, always used at the JNI boundary).**
  - Core registers r0-r3, then 4-byte stack words.
  - A 64-bit value (`J`, `D`) first rounds the next core register up to even. If two registers remain it takes them (low word first), otherwise it goes on the stack aligned to 8 bytes.
  - Once a 64-bit value went to the stack, no later argument uses a core register (NCRN = 4). 32-bit values never split.

- [ ] **Step 1: Write the failing test**

`tests/host/jni_abi_test.cpp`:
```cpp
// Host AAPCS64 JNI registers -> guest AAPCS32 softfp call layout, and guest results -> host.
#include <cstdio>
#include <cstring>

#include "check.h"
#include "zb/native_call.h"

namespace {

std::uint64_t fbits(float f) {
    std::uint32_t b;
    std::memcpy(&b, &f, 4);
    return b;
}

std::uint64_t dbits(double d) {
    std::uint64_t b;
    std::memcpy(&b, &d, 8);
    return b;
}

// In this test a local handle is the host reference plus 0x1000.
std::uint32_t to_handle(std::uint64_t ref) {
    return ref == 0 ? 0 : static_cast<std::uint32_t>(ref + 0x1000);
}

zb::GuestCall marshal(const char* shorty, const zb::NativeRegs& regs) {
    return zb::marshal_native_args(shorty, regs, 0xE000, to_handle);
}

}  // namespace

int main() {
    std::uint64_t stack[8] = {};

    // Lime.onTouch(IFFIFF)I: I F in r2 r3, the rest on the guest stack.
    {
        zb::NativeRegs r{};
        r.x[1] = 0x77;
        r.x[2] = 3;
        r.x[3] = static_cast<std::uint64_t>(-2);
        r.d[0] = fbits(1.5f);
        r.d[1] = fbits(-2.25f);
        r.d[2] = fbits(4.0f);
        r.d[3] = fbits(8.5f);
        r.stack = stack;
        const zb::GuestCall c = marshal("IIFFIFF", r);
        CHECK(c.regs[0] == 0xE000 && c.regs[1] == 0x1077);
        CHECK(c.regs[2] == 3 && c.regs[3] == fbits(1.5f));
        CHECK(c.stack.size() == 4);
        CHECK(c.stack[0] == fbits(-2.25f) && c.stack[1] == 0xFFFFFFFEu);
        CHECK(c.stack[2] == fbits(4.0f) && c.stack[3] == fbits(8.5f));
    }

    // Lime.releaseReference(J)V: the long takes r2:r3, low word first.
    {
        zb::NativeRegs r{};
        r.x[1] = 1;
        r.x[2] = 0x1122334455667788ull;
        r.stack = stack;
        const zb::GuestCall c = marshal("VJ", r);
        CHECK(c.regs[2] == 0x55667788u && c.regs[3] == 0x11223344u && c.stack.empty());
    }

    // (IJ)V: the long cannot start at r3, so it goes on the stack and r3 stays unused.
    {
        zb::NativeRegs r{};
        r.x[1] = 1;
        r.x[2] = 9;
        r.x[3] = 0xAAAABBBBCCCCDDDDull;
        r.stack = stack;
        const zb::GuestCall c = marshal("VIJ", r);
        CHECK(c.regs[2] == 9 && c.regs[3] == 0);
        CHECK(c.stack.size() == 2 && c.stack[0] == 0xCCCCDDDDu && c.stack[1] == 0xAAAABBBBu);
    }

    // (IIIJ)V: the third int is stack word 0, so the long is padded to the 8-byte boundary.
    {
        zb::NativeRegs r{};
        r.x[1] = 1;
        r.x[2] = 1;
        r.x[3] = 2;
        r.x[4] = 3;
        r.x[5] = 0x0000000500000004ull;
        r.stack = stack;
        const zb::GuestCall c = marshal("VIIIJ", r);
        CHECK(c.regs[2] == 1 && c.regs[3] == 2);
        CHECK(c.stack.size() == 4 && c.stack[0] == 3 && c.stack[1] == 0 && c.stack[2] == 4 && c.stack[3] == 5);
    }

    // Eight ints: six come from x2-x7, the last two from the host stack.
    {
        zb::NativeRegs r{};
        r.x[1] = 5;
        for (int i = 2; i < 8; ++i) r.x[i] = static_cast<std::uint64_t>(i * 10 - 10);  // 10..60
        stack[0] = 70;
        stack[1] = 80;
        r.stack = stack;
        const zb::GuestCall c = marshal("VIIIIIIII", r);
        CHECK(c.regs[2] == 10 && c.regs[3] == 20);
        CHECK(c.stack.size() == 6 && c.stack[0] == 30 && c.stack[4] == 70 && c.stack[5] == 80);
    }

    // Narrow types are extended per Java type, a null reference stays 0, a double after a stack
    // word is padded.
    {
        zb::NativeRegs r{};
        r.x[1] = 0;
        r.x[2] = 0x1FF;          // Z -> 0xFF
        r.x[3] = 0xFFFFFF80ull;  // B -> -128
        r.x[4] = 0x1FFFF;        // C -> 0xFFFF
        r.x[5] = 0xFFFF8000ull;  // S -> -32768
        r.x[6] = 0x42;           // L -> handle 0x1042
        r.d[0] = dbits(0.5);     // D
        r.stack = stack;
        const zb::GuestCall c = marshal("VZBCSLD", r);
        CHECK(c.regs[1] == 0);
        CHECK(c.regs[2] == 0xFF && c.regs[3] == 0xFFFFFF80u);
        CHECK(c.stack.size() == 6);
        CHECK(c.stack[0] == 0xFFFF && c.stack[1] == 0xFFFF8000u && c.stack[2] == 0x1042 && c.stack[3] == 0);
        CHECK(c.stack[4] == static_cast<std::uint32_t>(dbits(0.5)));
        CHECK(c.stack[5] == static_cast<std::uint32_t>(dbits(0.5) >> 32));
    }

    // Return values.
    {
        zb::NativeRegs r{};
        auto to_ref = [](std::uint32_t h) -> std::uint64_t { return h == 0 ? 0 : h + 0x7F0000000000ull; };
        zb::store_native_result('I', 0xFFFFFFFE, 0, r, to_ref);
        CHECK(r.x[0] == static_cast<std::uint64_t>(-2));
        zb::store_native_result('Z', 0x101, 0, r, to_ref);
        CHECK(r.x[0] == 1);
        zb::store_native_result('C', 0x1FFFF, 0, r, to_ref);
        CHECK(r.x[0] == 0xFFFF);
        zb::store_native_result('S', 0x8000, 0, r, to_ref);
        CHECK(r.x[0] == static_cast<std::uint64_t>(-32768));
        zb::store_native_result('J', 0x55667788, 0x11223344, r, to_ref);
        CHECK(r.x[0] == 0x1122334455667788ull);
        zb::store_native_result('F', static_cast<std::uint32_t>(fbits(3.5f)), 0, r, to_ref);
        CHECK(static_cast<std::uint32_t>(r.d[0]) == fbits(3.5f));
        zb::store_native_result('D', static_cast<std::uint32_t>(dbits(-1.25)),
                                static_cast<std::uint32_t>(dbits(-1.25) >> 32), r, to_ref);
        CHECK(r.d[0] == dbits(-1.25));
        zb::store_native_result('L', 0x40, 0, r, to_ref);
        CHECK(r.x[0] == 0x7F0000000040ull);
        zb::store_native_result('L', 0, 0, r, to_ref);
        CHECK(r.x[0] == 0);
    }

    std::printf("jni_abi_test ok\n");
    return 0;
}
```

In `tests/host/CMakeLists.txt`:
```cmake
set(ZB_HOST_TESTS
    fault_pc_test
    guest_memory_test
    jni_abi_test
    jni_mangle_test
    jni_shorty_test
    t1_blob_test
)
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `ninja -C build/host jni_abi_test`
Expected: FAIL to compile with `fatal error: 'zb/native_call.h' file not found`.

- [ ] **Step 3: Write the implementation**

`core/include/zb/native_call.h`:
```cpp
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <string_view>
#include <vector>

namespace zb {

// Registers of a JNI native call as saved by zb_native_common (thunks.S). The layout is shared
// with the assembly: x0-x7, the raw bits of d0-d7 (a float uses the low 32 bits), then the
// address of the caller's stack arguments.
struct NativeRegs {
    std::uint64_t x[8];
    std::uint64_t d[8];
    std::uint64_t* stack;
    std::uint64_t pad;
};

// Arguments of a guest AAPCS32 softfp call: r0-r3 plus stack words, lowest address first.
struct GuestCall {
    std::array<std::uint32_t, 4> regs{};
    std::vector<std::uint32_t> stack;
};

using RefToHandle = std::function<std::uint32_t(std::uint64_t host_ref)>;
using HandleToRef = std::function<std::uint64_t(std::uint32_t handle)>;

// Builds the guest call for a native method with the given shorty (return type first):
// r0 = guest JNIEnv*, r1 = handle for x1 (jclass or jobject), then the Java arguments.
GuestCall marshal_native_args(std::string_view shorty, const NativeRegs& regs, std::uint32_t guest_env,
                              const RefToHandle& ref_to_handle);

// Stores the guest result (r0, r1) in regs.x[0] or regs.d[0] according to the return type.
void store_native_result(char return_type, std::uint32_t r0, std::uint32_t r1, NativeRegs& regs,
                         const HandleToRef& handle_to_ref);

}  // namespace zb
```

`core/src/jni/native_call.cpp`:
```cpp
// Argument and result conversion between a host JNI call (AAPCS64) and the guest native
// function (AAPCS32 softfp).
#include "zb/native_call.h"

#include <cstddef>

namespace zb {

namespace {

// AAPCS64 on Linux/Android: integer class in x0-x7, floating point in d0-d7, then 8-byte stack
// slots in argument order.
class HostArgReader {
public:
    explicit HostArgReader(const NativeRegs& regs) : regs_(regs) {}

    std::uint64_t next_int() { return next_int_ < 8 ? regs_.x[next_int_++] : regs_.stack[next_stack_++]; }
    std::uint64_t next_fp() { return next_fp_ < 8 ? regs_.d[next_fp_++] : regs_.stack[next_stack_++]; }

private:
    const NativeRegs& regs_;
    int next_int_ = 2;  // x0 = JNIEnv*, x1 = jclass/jobject
    int next_fp_ = 0;
    std::size_t next_stack_ = 0;
};

// AAPCS32 base standard: core registers r0-r3, then 4-byte stack words.
class GuestArgWriter {
public:
    explicit GuestArgWriter(GuestCall& call) : call_(call) {}

    void put32(std::uint32_t value) {
        if (ncrn_ < 4) {
            call_.regs[ncrn_++] = value;
        } else {
            call_.stack.push_back(value);
        }
    }

    void put64(std::uint64_t value) {
        const auto lo = static_cast<std::uint32_t>(value);
        const auto hi = static_cast<std::uint32_t>(value >> 32);
        if (ncrn_ % 2 != 0) ++ncrn_;  // doubleword alignment: next even register
        if (ncrn_ <= 2) {
            call_.regs[ncrn_] = lo;
            call_.regs[ncrn_ + 1] = hi;
            ncrn_ += 2;
            return;
        }
        ncrn_ = 4;  // no core register is used after this
        if (call_.stack.size() % 2 != 0) call_.stack.push_back(0);  // 8-byte stack alignment
        call_.stack.push_back(lo);
        call_.stack.push_back(hi);
    }

private:
    GuestCall& call_;
    int ncrn_ = 0;
};

std::uint32_t sign_extend32(std::int32_t value) {
    return static_cast<std::uint32_t>(value);
}

std::uint64_t sign_extend64(std::int64_t value) {
    return static_cast<std::uint64_t>(value);
}

}  // namespace

GuestCall marshal_native_args(std::string_view shorty, const NativeRegs& regs, std::uint32_t guest_env,
                              const RefToHandle& ref_to_handle) {
    GuestCall call;
    GuestArgWriter out(call);
    HostArgReader in(regs);
    out.put32(guest_env);
    out.put32(ref_to_handle(regs.x[1]));
    for (std::size_t i = 1; i < shorty.size(); ++i) {
        switch (shorty[i]) {
        case 'Z':
            out.put32(static_cast<std::uint8_t>(in.next_int()));
            break;
        case 'B':
            out.put32(sign_extend32(static_cast<std::int8_t>(in.next_int())));
            break;
        case 'C':
            out.put32(static_cast<std::uint16_t>(in.next_int()));
            break;
        case 'S':
            out.put32(sign_extend32(static_cast<std::int16_t>(in.next_int())));
            break;
        case 'I':
            out.put32(static_cast<std::uint32_t>(in.next_int()));
            break;
        case 'J':
            out.put64(in.next_int());
            break;
        case 'F':
            out.put32(static_cast<std::uint32_t>(in.next_fp()));
            break;
        case 'D':
            out.put64(in.next_fp());
            break;
        case 'L':
            out.put32(ref_to_handle(in.next_int()));
            break;
        default:
            break;  // shorties come from shorty_from_signature, which yields only the letters above
        }
    }
    return call;
}

void store_native_result(char return_type, std::uint32_t r0, std::uint32_t r1, NativeRegs& regs,
                         const HandleToRef& handle_to_ref) {
    const std::uint64_t pair = (static_cast<std::uint64_t>(r1) << 32) | r0;
    switch (return_type) {
    case 'Z':
        regs.x[0] = static_cast<std::uint8_t>(r0);
        break;
    case 'B':
        regs.x[0] = sign_extend64(static_cast<std::int8_t>(r0));
        break;
    case 'C':
        regs.x[0] = static_cast<std::uint16_t>(r0);
        break;
    case 'S':
        regs.x[0] = sign_extend64(static_cast<std::int16_t>(r0));
        break;
    case 'I':
        regs.x[0] = sign_extend64(static_cast<std::int32_t>(r0));
        break;
    case 'J':
        regs.x[0] = pair;
        break;
    case 'F':
        regs.d[0] = r0;
        break;
    case 'D':
        regs.d[0] = pair;
        break;
    case 'L':
        regs.x[0] = handle_to_ref(r0);
        break;
    default:
        break;  // 'V'
    }
}

}  // namespace zb
```

In `core/CMakeLists.txt`:
```cmake
    src/jni/mangle.cpp
    src/jni/native_call.cpp
    src/jni/shorty.cpp
    src/log.cpp
```

- [ ] **Step 4: Run the test to verify it passes**

Run: `ninja -C build/host jni_abi_test && ctest --test-dir build/host -R jni_abi_test --output-on-failure`
Expected: `100% tests passed`.

- [ ] **Step 5: Commit**

```bash
git add core/include/zb/native_call.h core/src/jni/native_call.cpp tests/host/jni_abi_test.cpp core/CMakeLists.txt tests/host/CMakeLists.txt
git commit -m "jni: AAPCS64 to AAPCS32 softfp native call marshaling"
```

---

### Task 4: Handle tables

**Files:**
- Create: `core/include/zb/jni_handles.h`
- Create: `core/src/jni/handles.cpp`
- Create: `tests/host/jni_handles_test.cpp`
- Modify: `core/CMakeLists.txt`, `tests/host/CMakeLists.txt`

- [ ] **Step 1: Write the failing test**

`tests/host/jni_handles_test.cpp`:
```cpp
// 32-bit guest handles: kind in the low 2 bits, table index above; 0 is null.
#include <cstdio>

#include "check.h"
#include "zb/jni_handles.h"

int main() {
    using zb::HandleKind;
    using Ref = std::optional<std::uint64_t>;

    CHECK(!zb::handle_kind(0));
    CHECK(zb::handle_kind(zb::make_handle(HandleKind::Global, 7)) == HandleKind::Global);
    CHECK(zb::handle_index(zb::make_handle(HandleKind::WeakGlobal, 7)) == 7);

    zb::LocalHandles locals;
    CHECK(locals.add(0) == 0);
    CHECK(locals.get(0) == Ref(0));
    const std::uint32_t a = locals.add(0x7F00000000A0ull);  // base frame created on demand
    CHECK(locals.frame_count() == 1);
    CHECK(a != 0 && zb::handle_kind(a) == HandleKind::Local);
    CHECK(locals.get(a) == Ref(0x7F00000000A0ull));

    locals.push_frame();
    const std::uint32_t b = locals.add(0x7F00000000B0ull);
    const std::uint32_t c = locals.add(0x7F00000000C0ull);
    CHECK(locals.remove(b) == Ref(0x7F00000000B0ull));
    CHECK(!locals.get(b));     // deleted
    CHECK(!locals.remove(b));  // double delete is invalid
    const std::vector<std::uint64_t> released = locals.pop_frame();
    CHECK(released.size() == 1 && released[0] == 0x7F00000000C0ull);
    CHECK(!locals.get(c));                          // gone with its frame
    CHECK(locals.get(a) == Ref(0x7F00000000A0ull));  // outer frame intact
    CHECK(!locals.get(zb::make_handle(HandleKind::Global, 0)));  // wrong kind
    CHECK(!locals.get(zb::make_handle(HandleKind::Local, 99)));  // out of range
    CHECK(locals.remove(0) == Ref(0));                           // DeleteLocalRef(NULL) is allowed
    CHECK(locals.pop_frame().size() == 1);
    CHECK(locals.pop_frame().empty() && locals.frame_count() == 0);

    zb::GlobalHandles globals(HandleKind::Global);
    const std::uint32_t g1 = globals.add(0x10);
    const std::uint32_t g2 = globals.add(0x20);
    CHECK(zb::handle_kind(g1) == HandleKind::Global && g1 != g2);
    CHECK(globals.get(g2) == Ref(0x20));
    CHECK(globals.remove(g1) == Ref(0x10));
    CHECK(!globals.get(g1) && !globals.remove(g1));
    CHECK(globals.add(0x30) == g1);  // freed slot reused
    CHECK(!globals.get(zb::make_handle(HandleKind::WeakGlobal, zb::handle_index(g2))));  // kind mismatch

    zb::IdTable ids;
    CHECK(ids.intern(0) == 0);
    const std::uint32_t m1 = ids.intern(0x7F0000001000ull);
    const std::uint32_t m2 = ids.intern(0x7F0000002000ull);
    CHECK(m1 == 1 && m2 == 2 && ids.intern(0x7F0000001000ull) == m1);
    CHECK(ids.get(m2) == Ref(0x7F0000002000ull));
    CHECK(!ids.get(3));

    std::printf("jni_handles_test ok\n");
    return 0;
}
```

In `tests/host/CMakeLists.txt`:
```cmake
set(ZB_HOST_TESTS
    fault_pc_test
    guest_memory_test
    jni_abi_test
    jni_handles_test
    jni_mangle_test
    jni_shorty_test
    t1_blob_test
)
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `ninja -C build/host jni_handles_test`
Expected: FAIL to compile with `fatal error: 'zb/jni_handles.h' file not found`.

- [ ] **Step 3: Write the implementation**

`core/include/zb/jni_handles.h`:
```cpp
#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

namespace zb {

// Guest-visible JNI references are 32-bit handles: 0 is null, the low 2 bits give the kind and
// the bits above index a table of host references, stored as opaque 64-bit values.
enum class HandleKind : std::uint32_t { Local = 1, Global = 2, WeakGlobal = 3 };

constexpr std::uint32_t make_handle(HandleKind kind, std::uint32_t index) {
    return (index << 2) | static_cast<std::uint32_t>(kind);
}

constexpr std::uint32_t handle_index(std::uint32_t handle) {
    return handle >> 2;
}

// nullopt for the null handle and for kind bits 00.
std::optional<HandleKind> handle_kind(std::uint32_t handle);

// Local references of one host thread: a stack of frames (the native call frame plus
// PushLocalFrame frames). Popping a frame invalidates its handles. Not thread-safe: one
// instance per host thread.
class LocalHandles {
public:
    void push_frame();
    // Pops the top frame and returns the host references it still held, so the caller can
    // release them on the host. Returns an empty list when there is no frame.
    std::vector<std::uint64_t> pop_frame();
    // Adds a host reference to the top frame, creating a base frame if needed. 0 for null.
    std::uint32_t add(std::uint64_t host_ref);
    // The host reference; 0 for the null handle; nullopt for an invalid or deleted handle.
    std::optional<std::uint64_t> get(std::uint32_t handle) const;
    // DeleteLocalRef: frees the slot and returns the host reference; 0 for the null handle;
    // nullopt for an invalid handle.
    std::optional<std::uint64_t> remove(std::uint32_t handle);
    std::size_t frame_count() const { return frame_starts_.size(); }

private:
    std::vector<std::uint64_t> refs_;  // 0 marks a deleted slot
    std::vector<std::size_t> frame_starts_;
};

// Global or weak global references of the process. Thread-safe; freed slots are reused.
class GlobalHandles {
public:
    explicit GlobalHandles(HandleKind kind) : kind_(kind) {}
    std::uint32_t add(std::uint64_t host_ref);
    std::optional<std::uint64_t> get(std::uint32_t handle) const;
    std::optional<std::uint64_t> remove(std::uint32_t handle);

private:
    HandleKind kind_;
    mutable std::mutex mutex_;
    std::vector<std::uint64_t> refs_;
    std::vector<std::uint32_t> free_;
};

// jmethodID / jfieldID values: append-only and deduplicated, since host ids stay valid for the
// process lifetime. Guest ids start at 1; 0 is null. Thread-safe.
class IdTable {
public:
    std::uint32_t intern(std::uint64_t host_id);
    std::optional<std::uint64_t> get(std::uint32_t id) const;

private:
    mutable std::mutex mutex_;
    std::vector<std::uint64_t> ids_;
    std::unordered_map<std::uint64_t, std::uint32_t> index_;
};

}  // namespace zb
```

`core/src/jni/handles.cpp`:
```cpp
#include "zb/jni_handles.h"

namespace zb {

std::optional<HandleKind> handle_kind(std::uint32_t handle) {
    const std::uint32_t bits = handle & 3u;
    if (bits == 0) return std::nullopt;
    return static_cast<HandleKind>(bits);
}

void LocalHandles::push_frame() {
    frame_starts_.push_back(refs_.size());
}

std::vector<std::uint64_t> LocalHandles::pop_frame() {
    std::vector<std::uint64_t> released;
    if (frame_starts_.empty()) return released;
    const std::size_t start = frame_starts_.back();
    frame_starts_.pop_back();
    for (std::size_t i = start; i < refs_.size(); ++i) {
        if (refs_[i] != 0) released.push_back(refs_[i]);
    }
    refs_.resize(start);
    return released;
}

std::uint32_t LocalHandles::add(std::uint64_t host_ref) {
    if (host_ref == 0) return 0;
    if (frame_starts_.empty()) push_frame();
    refs_.push_back(host_ref);
    return make_handle(HandleKind::Local, static_cast<std::uint32_t>(refs_.size() - 1));
}

std::optional<std::uint64_t> LocalHandles::get(std::uint32_t handle) const {
    if (handle == 0) return 0;
    if (handle_kind(handle) != HandleKind::Local) return std::nullopt;
    const std::uint32_t index = handle_index(handle);
    if (index >= refs_.size() || refs_[index] == 0) return std::nullopt;
    return refs_[index];
}

std::optional<std::uint64_t> LocalHandles::remove(std::uint32_t handle) {
    if (handle == 0) return 0;
    const std::optional<std::uint64_t> ref = get(handle);
    if (!ref) return std::nullopt;
    refs_[handle_index(handle)] = 0;
    return ref;
}

std::uint32_t GlobalHandles::add(std::uint64_t host_ref) {
    if (host_ref == 0) return 0;
    std::lock_guard<std::mutex> lock(mutex_);
    std::uint32_t index;
    if (!free_.empty()) {
        index = free_.back();
        free_.pop_back();
        refs_[index] = host_ref;
    } else {
        refs_.push_back(host_ref);
        index = static_cast<std::uint32_t>(refs_.size() - 1);
    }
    return make_handle(kind_, index);
}

std::optional<std::uint64_t> GlobalHandles::get(std::uint32_t handle) const {
    if (handle == 0) return 0;
    if (handle_kind(handle) != kind_) return std::nullopt;
    std::lock_guard<std::mutex> lock(mutex_);
    const std::uint32_t index = handle_index(handle);
    if (index >= refs_.size() || refs_[index] == 0) return std::nullopt;
    return refs_[index];
}

std::optional<std::uint64_t> GlobalHandles::remove(std::uint32_t handle) {
    if (handle == 0) return 0;
    if (handle_kind(handle) != kind_) return std::nullopt;
    std::lock_guard<std::mutex> lock(mutex_);
    const std::uint32_t index = handle_index(handle);
    if (index >= refs_.size() || refs_[index] == 0) return std::nullopt;
    const std::uint64_t ref = refs_[index];
    refs_[index] = 0;
    free_.push_back(index);
    return ref;
}

std::uint32_t IdTable::intern(std::uint64_t host_id) {
    if (host_id == 0) return 0;
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = index_.find(host_id);
    if (it != index_.end()) return it->second;
    ids_.push_back(host_id);
    const auto id = static_cast<std::uint32_t>(ids_.size());
    index_.emplace(host_id, id);
    return id;
}

std::optional<std::uint64_t> IdTable::get(std::uint32_t id) const {
    if (id == 0) return 0;
    std::lock_guard<std::mutex> lock(mutex_);
    if (id > ids_.size()) return std::nullopt;
    return ids_[id - 1];
}

}  // namespace zb
```

In `core/CMakeLists.txt`:
```cmake
    src/jni/handles.cpp
    src/jni/mangle.cpp
    src/jni/native_call.cpp
    src/jni/shorty.cpp
    src/log.cpp
```

- [ ] **Step 4: Run the test to verify it passes**

Run: `ninja -C build/host jni_handles_test && ctest --test-dir build/host -R jni_handles_test --output-on-failure`
Expected: `100% tests passed`.

- [ ] **Step 5: Commit**

```bash
git add core/include/zb/jni_handles.h core/src/jni/handles.cpp tests/host/jni_handles_test.cpp core/CMakeLists.txt tests/host/CMakeLists.txt
git commit -m "jni: 32-bit handle tables for local, global and id references"
```

---

### Task 5: Thunk pool, common entry, dispatcher and slots

**Files:**
- Create: `core/src/jni/thunks.S`
- Create: `core/include/zb/native_thunks.h`
- Create: `core/src/jni/native_thunks.cpp`
- Create: `tests/host/native_thunk_test.cpp`
- Modify: `CMakeLists.txt:2` (languages), `core/CMakeLists.txt`, `tests/host/CMakeLists.txt`

- [ ] **Step 1: Write the failing test**

`tests/host/native_thunk_test.cpp`:
```cpp
// Calls precompiled thunks like JNI functions and checks what the dispatcher receives: the slot
// number, x0-x7, d0-d7, host stack arguments, and the result coming back through x0 / d0.
#include <cstdio>
#include <cstring>

#include "check.h"
#include "zb/native_thunks.h"

namespace {

std::uint32_t g_slot = 0xFFFFFFFF;
zb::NativeRegs g_seen{};

void record(std::uint32_t slot, zb::NativeRegs& regs) {
    g_slot = slot;
    g_seen = regs;
    regs.x[0] = 0x1122334455667788ull;
    const double result = 2.5;
    std::memcpy(&regs.d[0], &result, 8);
}

}  // namespace

int main() {
    zb::set_native_dispatcher(record);

    // Ten integer-class arguments: x0-x7 take env, class and six ints; 8 and 9 go on the stack.
    using Wide = std::int64_t (*)(void*, void*, std::int64_t, std::int64_t, std::int64_t, std::int64_t,
                                  std::int64_t, std::int64_t, std::int64_t, std::int64_t, float, double);
    const auto wide = reinterpret_cast<Wide>(zb::native_thunk_address(5));
    const std::int64_t result = wide(reinterpret_cast<void*>(0xE0), reinterpret_cast<void*>(0xC1), 2, 3, 4, 5,
                                     6, 7, 8, 9, 1.5f, 3.25);
    CHECK(g_slot == 5);
    CHECK(result == 0x1122334455667788ll);
    CHECK(g_seen.x[0] == 0xE0 && g_seen.x[1] == 0xC1 && g_seen.x[2] == 2 && g_seen.x[7] == 7);
    CHECK(g_seen.stack[0] == 8 && g_seen.stack[1] == 9);
    float f;
    std::memcpy(&f, &g_seen.d[0], 4);
    CHECK(f == 1.5f);
    double d;
    std::memcpy(&d, &g_seen.d[1], 8);
    CHECK(d == 3.25);

    // The last thunk, returning a double through d0.
    using Fp = double (*)(void*, void*);
    const auto last = reinterpret_cast<Fp>(zb::native_thunk_address(zb::kNativeThunkCount - 1));
    CHECK(last(nullptr, nullptr) == 2.5);
    CHECK(g_slot == zb::kNativeThunkCount - 1);
    CHECK(zb::native_thunk_address(zb::kNativeThunkCount) == nullptr);

    zb::NativeSlots slots(2);
    CHECK(slots.allocate({0x10001, "VI", true}) == 0);
    CHECK(slots.allocate({0x20000, "IIFFIFF", false}) == 1);
    CHECK(slots.allocate({0x30000, "V", true}) == -1);  // exhausted
    CHECK(slots.target(1) != nullptr && slots.target(1)->shorty == "IIFFIFF" && !slots.target(1)->is_static);
    CHECK(slots.target(2) == nullptr);

    std::printf("native_thunk_test ok\n");
    return 0;
}
```

In `tests/host/CMakeLists.txt`:
```cmake
set(ZB_HOST_TESTS
    fault_pc_test
    guest_memory_test
    jni_abi_test
    jni_handles_test
    jni_mangle_test
    jni_shorty_test
    native_thunk_test
    t1_blob_test
)
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `ninja -C build/host native_thunk_test`
Expected: FAIL to compile with `fatal error: 'zb/native_thunks.h' file not found`.

- [ ] **Step 3: Enable assembly in the top-level CMakeLists.txt**

Change line 2 of `CMakeLists.txt` from:
```cmake
project(zettabridge LANGUAGES C CXX)
```
to:
```cmake
project(zettabridge LANGUAGES C CXX ASM)
```

- [ ] **Step 4: Write the assembly**

`core/src/jni/thunks.S`:
```asm
// Native method thunks (AArch64). Each of the 16384 thunks is 8 bytes and loads its own address
// into x16 before jumping to the common entry, which derives the slot number from it. No code
// is generated at run time. The count must match kNativeThunkCount in zb/native_thunks.h.

    .text
    .balign 4
    .globl zb_native_thunk_base
    .hidden zb_native_thunk_base
zb_native_thunk_base:
    .rept 16384
    adr x16, .
    b zb_native_common
    .endr

// Saves x0-x7, d0-d7 and the caller's stack argument pointer as zb::NativeRegs (144 bytes),
// calls zb_native_dispatch(slot, regs), then returns regs.x[0] in x0 and regs.d[0] in d0.
    .globl zb_native_common
    .hidden zb_native_common
zb_native_common:
    stp x29, x30, [sp, #-16]!
    mov x29, sp
    sub sp, sp, #144
    stp x0, x1, [sp, #0]
    stp x2, x3, [sp, #16]
    stp x4, x5, [sp, #32]
    stp x6, x7, [sp, #48]
    stp d0, d1, [sp, #64]
    stp d2, d3, [sp, #80]
    stp d4, d5, [sp, #96]
    stp d6, d7, [sp, #112]
    add x9, x29, #16            // caller's stack arguments start at the entry sp
    str x9, [sp, #128]
    adr x10, zb_native_thunk_base
    sub x0, x16, x10
    lsr x0, x0, #3              // slot = (thunk address - base) / 8
    mov x1, sp
    bl zb_native_dispatch
    ldr x0, [sp, #0]
    ldr d0, [sp, #64]
    mov sp, x29
    ldp x29, x30, [sp], #16
    ret

    .section .note.GNU-stack,"",%progbits
```

- [ ] **Step 5: Write the C++ side**

`core/include/zb/native_thunks.h`:
```cpp
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include "zb/native_call.h"

namespace zb {

// Number of precompiled thunks; must match the .rept count in core/src/jni/thunks.S.
constexpr std::size_t kNativeThunkCount = 16384;

// What a registered native method runs: a guest function and how to marshal its arguments.
struct NativeTarget {
    std::uint32_t guest_function = 0;  // Thumb bit included
    std::string shorty;                // return type first
    bool is_static = false;
};

// Receives every thunk call with the slot number. It reads the arguments from regs and stores
// the result in regs.x[0] / regs.d[0].
using NativeDispatcher = void (*)(std::uint32_t slot, NativeRegs& regs);
void set_native_dispatcher(NativeDispatcher dispatcher);

// Address of a thunk, usable as the fnPtr of RegisterNatives; nullptr when out of range.
void* native_thunk_address(std::uint32_t slot);

// Assigns thunk slots to native methods. Slots are never freed (UnregisterNatives keeps them),
// so target() needs no lock.
class NativeSlots {
public:
    // capacity is capped at kNativeThunkCount.
    explicit NativeSlots(std::size_t capacity = kNativeThunkCount);
    // The slot number, or -1 when every slot is taken.
    std::int32_t allocate(NativeTarget target);
    // The target of an allocated slot, or nullptr.
    const NativeTarget* target(std::uint32_t slot) const;

private:
    std::size_t capacity_;
    std::mutex mutex_;
    std::unique_ptr<NativeTarget[]> targets_;
    std::atomic<std::uint32_t> count_{0};
};

}  // namespace zb
```

`core/src/jni/native_thunks.cpp`:
```cpp
#include "zb/native_thunks.h"

#include <cstddef>

extern "C" {
// Both defined in or called from thunks.S.
extern const char zb_native_thunk_base[];
void zb_native_dispatch(std::uint32_t slot, zb::NativeRegs* regs);
}

namespace zb {

namespace {

std::atomic<NativeDispatcher> g_dispatcher{nullptr};

}  // namespace

// thunks.S saves the registers with exactly this layout.
static_assert(offsetof(NativeRegs, x) == 0);
static_assert(offsetof(NativeRegs, d) == 64);
static_assert(offsetof(NativeRegs, stack) == 128);
static_assert(sizeof(NativeRegs) == 144);

void set_native_dispatcher(NativeDispatcher dispatcher) {
    g_dispatcher.store(dispatcher);
}

void* native_thunk_address(std::uint32_t slot) {
    if (slot >= kNativeThunkCount) return nullptr;
    return const_cast<char*>(zb_native_thunk_base) + 8 * static_cast<std::size_t>(slot);
}

NativeSlots::NativeSlots(std::size_t capacity)
    : capacity_(capacity < kNativeThunkCount ? capacity : kNativeThunkCount),
      targets_(new NativeTarget[capacity_]) {}

std::int32_t NativeSlots::allocate(NativeTarget target) {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::uint32_t slot = count_.load();
    if (slot >= capacity_) return -1;
    targets_[slot] = std::move(target);
    count_.store(slot + 1);  // published only after the entry is complete
    return static_cast<std::int32_t>(slot);
}

const NativeTarget* NativeSlots::target(std::uint32_t slot) const {
    return slot < count_.load() ? &targets_[slot] : nullptr;
}

}  // namespace zb

void zb_native_dispatch(std::uint32_t slot, zb::NativeRegs* regs) {
    const zb::NativeDispatcher dispatcher = zb::g_dispatcher.load();
    if (dispatcher != nullptr) dispatcher(slot, *regs);
}
```

In `core/CMakeLists.txt`:
```cmake
    src/jni/handles.cpp
    src/jni/mangle.cpp
    src/jni/native_call.cpp
    src/jni/native_thunks.cpp
    src/jni/shorty.cpp
    src/jni/thunks.S
    src/log.cpp
```

- [ ] **Step 6: Reconfigure and run the test to verify it passes**

Run: `cmake -S . -B build/host -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ && ninja -C build/host native_thunk_test && ctest --test-dir build/host -R native_thunk_test --output-on-failure`
Expected: `100% tests passed`.

- [ ] **Step 7: Check that the Android build still links**

Run:
```
N=$HOME/android-ndk-r29; cmake -S . -B build/android-arm64 -G Ninja -DCMAKE_TOOLCHAIN_FILE=$N/build/cmake/android.toolchain.cmake -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 -DCMAKE_BUILD_TYPE=Release -DZB_BUILD_TESTS=OFF -DBoost_INCLUDE_DIR=$PWD/build/boost-headers && ninja -C build/android-arm64 zbridge zbrun
```
Expected: `Linking CXX shared library core/libzbridge.so` with no errors. The thunks are not referenced yet, so `--gc-sections` may drop them, which is fine until plan 4d.

- [ ] **Step 8: Commit**

```bash
git add CMakeLists.txt core/src/jni/thunks.S core/include/zb/native_thunks.h core/src/jni/native_thunks.cpp tests/host/native_thunk_test.cpp core/CMakeLists.txt tests/host/CMakeLists.txt
git commit -m "jni: precompiled arm64 native thunk pool with dispatcher and slot allocation"
```

---

### Task 6: Full regression run and status docs

**Files:**
- Modify: `CLAUDE.md` (code map, current state)
- Modify: `AGENTS.md` (progress)

- [ ] **Step 1: Run everything**

Run: `ninja -C build/host && ctest --test-dir build/host --output-on-failure && tools/build_guest.sh && tools/run_guest_tests.sh`
Expected: `100% tests passed, 0 tests failed out of 10`, then `all guest tests passed`.

- [ ] **Step 2: Update CLAUDE.md**

In the "Code map" list, after the `process` bullet, add:
```markdown
- `jni_mangle`, `jni_shorty`, `native_call`, `jni_handles`, `native_thunks` (`core/src/jni/`):
  host units of the JNI bridge (Phase 4a): export name decoding, shorties, AAPCS32
  marshaling, 32-bit handles, the precompiled arm64 thunk pool.
```
In "Current state", replace the `Next:` bullet with:
```markdown
- Phase 4a (JNI host units) done: `docs/superpowers/plans/2026-09-14-phase4a-jni-host-units.md`.
- Next: plan 4b (library-mode guest process, host->guest calls, carriers).
```

- [ ] **Step 3: Update AGENTS.md**

In "State", replace the Todo 1 bullet about merging with:
```markdown
- The Phase 0 launcher branch was merged into `phase1-zbrun` on 2026-09-14.
```
In "Next", add this line directly under the `## Next: Phase 4 plan and implementation` heading:
```markdown
Progress: plan 4a is done (steps 2-4 of the list below, including the thunk pool); continue with plan 4b.
```

- [ ] **Step 4: Commit**

```bash
git add CLAUDE.md AGENTS.md
git commit -m "docs: Phase 4a done"
```

---

## Following plans (written after 4a lands)

Each builds on the code above and is written with the real interfaces in hand.

- **4b: library-mode guest process and host->guest calls.**
  - `GuestThread` gains a call helper: registers and stack from `GuestCall`, `lr` = the `svc #0x5AFFFF` return address, run until return, restore.
  - `zbhost` (arm32) parks and serves `dlopen`/`dlsym`/call requests.
  - Carrier threads per the part 3 spec "Threads".
  - Acceptance: a host test loads a guest `.so` through `zbhost` and calls functions with every shorty type through `marshal_native_args`, from two host threads at once.
- **4c: guest `JNIEnv` and host JNI.**
  - `tools/gen_jni.py` (slot table and host-call list from `jni.h`, with the per-slot host-fallback config).
  - `guest/zbjni/zbjni.c` -> `libzbjni.so`.
  - `core/src/jni/host_jni.cpp` with a `JniBackend` interface, and a mock backend for `zbrun`.
  - Acceptance: guest test `jni_mock_dynamic` covers every `Call*` form and the buffer release modes on this machine.
  - Host RegisterNatives strips one leading '!' (the pre-O fast JNI marker that ART still accepts)
    before computing the shorty. The GetMethodID/GetStaticMethodID host calls return the shorty,
    computed on the host from the signature ART accepted, so the guest has no second descriptor
    parser.
- **4d: Android integration.**
  - `libzbproxy.so`, `ZBridge.onProxyLoaded`, `core/src/jni/loader.cpp` (dlopen, `Java_*` binding, guest `JNI_OnLoad`).
  - Launcher class-loader changes (`com.zettabridge.core` delegation, `findLibrary` proxies).
  - Device test T7 and the Orange Roulette smoke test (spec "Phase 4 acceptance").
  - If the real RegisterNatives fails, the thunk slot allocated for it must be reusable (add a
    release path to NativeSlots), so failed registrations do not leak slots.
