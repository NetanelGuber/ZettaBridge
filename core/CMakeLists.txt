add_library(zbcore STATIC
    src/cp15.cpp
    src/elf_fixups.cpp
    src/elf_loader.cpp
    src/guest_memory.cpp
    src/guest_thread.cpp
    src/initial_stack.cpp
    src/log.cpp
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
    add_library(zbridge SHARED src/zbridge_c_api.cpp)
    target_link_libraries(zbridge PRIVATE zbcore)
    set_target_properties(zbridge PROPERTIES CXX_VISIBILITY_PRESET hidden)
    target_compile_definitions(zbridge PRIVATE ZB_EXPORT_C_API)
endif()
