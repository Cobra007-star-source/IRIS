add_executable(validate_bench validate_bench.cpp)
target_link_libraries(validate_bench PRIVATE iris)
target_compile_options(validate_bench PRIVATE ${IRIS_ARCH_FLAGS})
