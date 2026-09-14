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

foreach(test_name ${ZB_HOST_TESTS})
    add_executable(${test_name} ${test_name}.cpp)
    target_link_libraries(${test_name} PRIVATE zbcore)
    add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()

# Needs build/guest/hello_static from tools/build_guest.sh.
add_executable(elf_loader_test elf_loader_test.cpp)
target_link_libraries(elf_loader_test PRIVATE zbcore)
add_test(NAME elf_loader_test COMMAND elf_loader_test ${CMAKE_SOURCE_DIR}/build/guest/hello_static)

# Needs build/guest/host_call_static from tools/build_guest.sh.
add_executable(guest_call_test guest_call_test.cpp)
target_link_libraries(guest_call_test PRIVATE zbcore)
add_test(NAME guest_call_test COMMAND guest_call_test ${CMAKE_SOURCE_DIR}/build/guest/host_call_static)

add_executable(host_call_dispatch_test host_call_dispatch_test.cpp)
target_link_libraries(host_call_dispatch_test PRIVATE zbcore)
add_test(NAME host_call_dispatch_test COMMAND host_call_dispatch_test
    ${CMAKE_SOURCE_DIR}/build/guest/host_call_static)

add_executable(zbhost_protocol_test zbhost_protocol_test.cpp)
target_link_libraries(zbhost_protocol_test PRIVATE zbcore)
add_test(NAME zbhost_protocol_test COMMAND zbhost_protocol_test
    ${CMAKE_SOURCE_DIR}/sysroot
    ${CMAKE_SOURCE_DIR}/build/guest/zbhost
    ${CMAKE_SOURCE_DIR}/build/guest/lib)

# Needs tools/build_guest.sh and the extracted sysroot.
add_executable(library_runtime_test library_runtime_test.cpp)
target_link_libraries(library_runtime_test PRIVATE zbcore)
# Hold a real signal-forwarding reader at post_signal without a production test hook.
target_link_options(library_runtime_test PRIVATE
    -Wl,--wrap=_ZN2zb11GuestThread11post_signalERKNS_1g9siginfo32E)
add_test(NAME library_runtime_test COMMAND library_runtime_test
    ${CMAKE_SOURCE_DIR}/sysroot
    ${CMAKE_SOURCE_DIR}/build/guest/zbhost
    ${CMAKE_SOURCE_DIR}/build/guest/lib/libzbcallprobe.so)

# Needs tools/build_guest.sh and the extracted sysroot.
add_executable(async_signal_test async_signal_test.cpp)
target_link_libraries(async_signal_test PRIVATE zbcore)
add_test(NAME async_signal_test COMMAND async_signal_test ${CMAKE_SOURCE_DIR}/sysroot ${CMAKE_SOURCE_DIR}/build/guest)
