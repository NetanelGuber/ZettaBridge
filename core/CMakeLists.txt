add_library(zbcore STATIC
    src/cp15.cpp
    src/elf_loader.cpp
    src/guest_memory.cpp
    src/guest_thread.cpp
    src/initial_stack.cpp
    src/log.cpp
    src/process.cpp
    src/syscalls.cpp
)
target_include_directories(zbcore PUBLIC include PRIVATE src)
target_link_libraries(zbcore PUBLIC dynarmic)
target_compile_options(zbcore PRIVATE -Wall -Wextra)
