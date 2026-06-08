add_executable(hello_iris hello_iris.cpp)
target_link_libraries(hello_iris PRIVATE iris)
target_compile_options(hello_iris PRIVATE ${IRIS_ARCH_FLAGS})

add_executable(phase1_demo phase1_demo.cpp)
target_link_libraries(phase1_demo PRIVATE iris)
target_compile_options(phase1_demo PRIVATE ${IRIS_ARCH_FLAGS})
