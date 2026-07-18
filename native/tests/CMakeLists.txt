add_executable(lumacore_tests test_ring_buffer.cpp)
target_link_libraries(lumacore_tests PRIVATE lumacore_logic)

add_test(NAME lumacore_tests COMMAND lumacore_tests)
