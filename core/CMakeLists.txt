add_library(zbcore STATIC
    src/cp15.cpp
    src/elf_fixups.cpp
    src/elf_loader.cpp
    src/guest_memory.cpp
    src/guest_thread.cpp
    src/initial_stack.cpp
    src/jni/handles.cpp
    src/jni/mangle.cpp
    src/jni/native_call.cpp
    src/jni/native_thunks.cpp
    src/jni/shorty.cpp
    src/jni/thunks.S
    src/log.cpp
    src/proc_files.cpp
    src/process.cpp
    src/signals.cpp
    src/syscalls.cpp
    src/zbridge_c_api.cpp
)
target_include_directories(zbcore PUBLIC include PRIVATE src)
target_link_libraries(zbcore PUBLIC dynarmic)
target_compile_options(zbcore PRIVATE -Wall -Wextra)

if (ANDROID)
    target_link_libraries(zbcore PUBLIC log)

    # libzbridge.so: the translator as loaded by the launcher's :guest process (Phase 3).
    add_library(zbridge SHARED src/zbridge_c_api.cpp android/zbridge_jni.cpp)
    target_link_libraries(zbridge PRIVATE zbcore)
    set_target_properties(zbridge PROPERTIES CXX_VISIBILITY_PRESET hidden)
    target_compile_definitions(zbridge PRIVATE ZB_EXPORT_C_API)
    # --exclude-libs keeps the statically linked libc++ and friends from being exported, so
    # they cannot clash with the app's own libc++_shared.so in the same process.
    target_link_options(zbridge PRIVATE -Wl,--gc-sections -Wl,--strip-all -Wl,--exclude-libs,ALL)
endif()
