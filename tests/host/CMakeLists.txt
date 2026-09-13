set(ZB_HOST_TESTS
)

foreach(test_name ${ZB_HOST_TESTS})
    add_executable(${test_name} ${test_name}.cpp)
    target_link_libraries(${test_name} PRIVATE zbcore)
    add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()
