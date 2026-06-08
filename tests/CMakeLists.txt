add_executable(iris_tests
    test_simd.cpp
    test_perfect_hash.cpp
    test_validator.cpp
    test_inspector.cpp
    test_slow_path.cpp
    test_main.cpp
)
target_link_libraries(iris_tests PRIVATE iris)
target_compile_options(iris_tests PRIVATE ${IRIS_ARCH_FLAGS})
add_test(NAME iris_tests COMMAND iris_tests)
