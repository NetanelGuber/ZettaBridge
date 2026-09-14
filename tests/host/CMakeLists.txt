set(ZB_HOST_TESTS
    fault_pc_test
    guest_memory_test
    t1_blob_test
)

foreach(test_name ${ZB_HOST_TESTS})
    add_executable(${test_name} ${test_name}.cpp)
    target_link_libraries(${test_name} PRIVATE zbcore)
    add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()

# Needs build/guest/hello_static from tools/build_guest.sh.
add_executable(elf_loader_test elf_loader_test.cpp)
target_link_libraries(elf_loader_test PRIVATE zbcore)
add_test(NAME elf_loader_test COMMAND elf_loader_test ${CMAKE_SOURCE_DIR}/build/guest/hello_static)

# Needs tools/build_guest.sh and the extracted sysroot.
add_executable(async_signal_test async_signal_test.cpp)
target_link_libraries(async_signal_test PRIVATE zbcore)
add_test(NAME async_signal_test COMMAND async_signal_test ${CMAKE_SOURCE_DIR}/sysroot ${CMAKE_SOURCE_DIR}/build/guest)
