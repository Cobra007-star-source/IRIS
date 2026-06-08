add_executable(hello_iris hello_iris.cpp)
target_link_libraries(hello_iris PRIVATE iris)
target_compile_options(hello_iris PRIVATE ${IRIS_ARCH_FLAGS})

add_executable(phase1_demo phase1_demo.cpp)
target_link_libraries(phase1_demo PRIVATE iris)
target_compile_options(phase1_demo PRIVATE ${IRIS_ARCH_FLAGS})

# Cross-ISA asm dump for the JIT serializer (diagnostic). Needs asmjit, which
# the iris lib links publicly only when -DIRIS_ENABLE_JIT=ON.
if(IRIS_ENABLE_JIT)
    add_executable(jit_serializer_dump jit_serializer_dump.cpp)
    target_link_libraries(jit_serializer_dump PRIVATE iris)
    target_compile_options(jit_serializer_dump PRIVATE ${IRIS_ARCH_FLAGS})
endif()
