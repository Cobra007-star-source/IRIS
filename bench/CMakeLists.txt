add_executable(iris_validate iris_validate.cpp)
target_link_libraries(iris_validate PRIVATE iris)
target_compile_options(iris_validate PRIVATE ${IRIS_ARCH_FLAGS})

add_executable(gen_corpus gen_corpus.cpp)
target_link_libraries(gen_corpus PRIVATE iris)
target_compile_options(gen_corpus PRIVATE ${IRIS_ARCH_FLAGS})

if(IRIS_BUILD_SIMDJSON AND TARGET simdjson)
    add_executable(simdjson_bench simdjson_bench.cpp)
    target_link_libraries(simdjson_bench PRIVATE simdjson)
    target_compile_options(simdjson_bench PRIVATE ${IRIS_ARCH_FLAGS})
endif()

if(IRIS_BUILD_BOWTIE)
    add_executable(bowtie_iris bowtie_iris.cpp)
    target_link_libraries(bowtie_iris PRIVATE iris)
    target_compile_options(bowtie_iris PRIVATE ${IRIS_ARCH_FLAGS})
endif()

add_executable(conformance conformance.cpp)
target_link_libraries(conformance PRIVATE iris)
target_compile_options(conformance PRIVATE ${IRIS_ARCH_FLAGS})
