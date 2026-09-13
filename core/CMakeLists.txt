add_library(zbcore STATIC
    src/log.cpp
)
target_include_directories(zbcore PUBLIC include PRIVATE src)
target_link_libraries(zbcore PUBLIC dynarmic)
target_compile_options(zbcore PRIVATE -Wall -Wextra)
