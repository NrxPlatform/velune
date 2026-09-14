cmake_minimum_required(VERSION 3.20)
project(jsengine VERSION 0.1.0 LANGUAGES CXX)

option(JSENGINE_BUILD_TESTS "Build jsengine tests" ON)
option(JSENGINE_BUILD_CLI "Build jsengine CLI" ON)
option(JSENGINE_BUILD_EXAMPLES "Build jsengine examples" ON)
option(JSENGINE_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(JSENGINE_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(JSENGINE_BUILD_FUZZ_TARGETS "Build libFuzzer frontend target (Clang only)" OFF)

include(GNUInstallDirs)

add_library(jsengine)
add_library(jsengine::jsengine ALIAS jsengine)
target_compile_features(jsengine PUBLIC cxx_std_20)
target_sources(jsengine
    PRIVATE
        src/frontend/frontend.cpp
        src/frontend/static_semantics.cpp
        src/compiler/compiler.cpp
        src/compiler/scope.cpp
        src/runtime/runtime.cpp
        src/runtime/atom_table.cpp
        src/runtime/environment.cpp
        src/runtime/context.cpp
        src/runtime/abstract_operations.cpp
        src/runtime/value.cpp
        src/runtime/error.cpp
        src/runtime/regexp.cpp
        src/bytecode/opcode.cpp
        src/bytecode/builder.cpp
        src/bytecode/verifier.cpp
        src/bytecode/disassembler.cpp
        src/vm/vm.cpp
        src/module/module.cpp
)
target_include_directories(jsengine
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

include(cmake/Warnings.cmake)
jsengine_set_warnings(jsengine)

if(JSENGINE_ENABLE_SANITIZERS)
    include(cmake/Sanitizers.cmake)
    jsengine_enable_sanitizers(jsengine)
endif()

if(JSENGINE_BUILD_CLI)
    add_executable(js cli/main.cpp)
    target_link_libraries(js PRIVATE jsengine::jsengine)
    jsengine_set_warnings(js)
    if(JSENGINE_ENABLE_SANITIZERS)
        jsengine_enable_sanitizers(js)
    endif()
endif()

if(JSENGINE_BUILD_EXAMPLES)
    add_executable(jsengine_embed_example examples/embed.cpp)
    target_link_libraries(jsengine_embed_example PRIVATE jsengine::jsengine)
    jsengine_set_warnings(jsengine_embed_example)

    add_executable(jsengine_bytecode_example examples/bytecode_vm.cpp)
    target_link_libraries(jsengine_bytecode_example PRIVATE jsengine::jsengine)
    jsengine_set_warnings(jsengine_bytecode_example)

    add_executable(jsengine_frontend_example examples/frontend_parse.cpp)
    target_link_libraries(jsengine_frontend_example PRIVATE jsengine::jsengine)
    jsengine_set_warnings(jsengine_frontend_example)

    add_executable(jsengine_compiler_example examples/compiler_pipeline.cpp)
    target_link_libraries(jsengine_compiler_example PRIVATE jsengine::jsengine)
    jsengine_set_warnings(jsengine_compiler_example)

    add_executable(jsengine_scope_example examples/scope_resolution.cpp)
    target_link_libraries(jsengine_scope_example PRIVATE jsengine::jsengine)
    jsengine_set_warnings(jsengine_scope_example)

    add_executable(jsengine_function_example examples/function_frames.cpp)
    target_link_libraries(jsengine_function_example PRIVATE jsengine::jsengine)
    jsengine_set_warnings(jsengine_function_example)

    add_executable(jsengine_closure_example examples/closures.cpp)
    target_link_libraries(jsengine_closure_example PRIVATE jsengine::jsengine)
    jsengine_set_warnings(jsengine_closure_example)

    add_executable(jsengine_object_example examples/objects.cpp)
    target_link_libraries(jsengine_object_example PRIVATE jsengine::jsengine)
    jsengine_set_warnings(jsengine_object_example)

    add_executable(jsengine_array_example examples/arrays_builtins.cpp)
    target_link_libraries(jsengine_array_example PRIVATE jsengine::jsengine)
    jsengine_set_warnings(jsengine_array_example)

    add_executable(jsengine_prototype_example examples/prototypes_this.cpp)
    target_link_libraries(jsengine_prototype_example PRIVATE jsengine::jsengine)
    jsengine_set_warnings(jsengine_prototype_example)

    add_executable(jsengine_exception_example examples/exceptions.cpp)
    target_link_libraries(jsengine_exception_example PRIVATE jsengine::jsengine)
    jsengine_set_warnings(jsengine_exception_example)

    add_executable(jsengine_gc_example examples/garbage_collection.cpp)
    target_link_libraries(jsengine_gc_example PRIVATE jsengine::jsengine)
    jsengine_set_warnings(jsengine_gc_example)

    add_executable(jsengine_class_example examples/constructors_classes.cpp)
    target_link_libraries(jsengine_class_example PRIVATE jsengine::jsengine)
    jsengine_set_warnings(jsengine_class_example)

    add_executable(jsengine_iteration_example examples/iteration.cpp)
    target_link_libraries(jsengine_iteration_example PRIVATE jsengine::jsengine)
    jsengine_set_warnings(jsengine_iteration_example)

    add_executable(jsengine_module_example examples/modules.cpp)
    target_link_libraries(jsengine_module_example PRIVATE jsengine::jsengine)
    jsengine_set_warnings(jsengine_module_example)

    add_executable(jsengine_promise_example examples/promises_jobs.cpp)
    target_link_libraries(jsengine_promise_example PRIVATE jsengine::jsengine)
    jsengine_set_warnings(jsengine_promise_example)

    if(JSENGINE_ENABLE_SANITIZERS)
        jsengine_enable_sanitizers(jsengine_embed_example)
        jsengine_enable_sanitizers(jsengine_bytecode_example)
        jsengine_enable_sanitizers(jsengine_frontend_example)
        jsengine_enable_sanitizers(jsengine_compiler_example)
        jsengine_enable_sanitizers(jsengine_scope_example)
        jsengine_enable_sanitizers(jsengine_function_example)
        jsengine_enable_sanitizers(jsengine_closure_example)
        jsengine_enable_sanitizers(jsengine_object_example)
        jsengine_enable_sanitizers(jsengine_prototype_example)
        jsengine_enable_sanitizers(jsengine_array_example)
        jsengine_enable_sanitizers(jsengine_exception_example)
        jsengine_enable_sanitizers(jsengine_gc_example)
        jsengine_enable_sanitizers(jsengine_class_example)
        jsengine_enable_sanitizers(jsengine_iteration_example)
        jsengine_enable_sanitizers(jsengine_module_example)
        jsengine_enable_sanitizers(jsengine_promise_example)
    endif()
    add_executable(jsengine_regexp_example examples/regexp.cpp)
    target_link_libraries(jsengine_regexp_example PRIVATE jsengine::jsengine)
    jsengine_set_warnings(jsengine_regexp_example)

    add_executable(jsengine_generator_example examples/generators.cpp)
    target_link_libraries(jsengine_generator_example PRIVATE jsengine::jsengine)
    jsengine_set_warnings(jsengine_generator_example)

    add_executable(jsengine_semantic_core_example examples/semantic_core.cpp)
    target_link_libraries(jsengine_semantic_core_example PRIVATE jsengine::jsengine)
    jsengine_set_warnings(jsengine_semantic_core_example)

    add_executable(jsengine_control_flow_example examples/operators_control_flow.cpp)
    target_link_libraries(jsengine_control_flow_example PRIVATE jsengine::jsengine)
    jsengine_set_warnings(jsengine_control_flow_example)

    add_executable(jsengine_function_call_example examples/function_call_semantics.cpp)
    target_link_libraries(jsengine_function_call_example PRIVATE jsengine::jsengine)
    jsengine_set_warnings(jsengine_function_call_example)
    if(JSENGINE_ENABLE_SANITIZERS)
        jsengine_enable_sanitizers(jsengine_generator_example)
        jsengine_enable_sanitizers(jsengine_regexp_example)
        jsengine_enable_sanitizers(jsengine_semantic_core_example)
        jsengine_enable_sanitizers(jsengine_control_flow_example)
        jsengine_enable_sanitizers(jsengine_function_call_example)
    endif()
endif()

if(JSENGINE_BUILD_TESTS)
    enable_testing()
    add_executable(jsengine_tests
        tests/test_main.cpp
        tests/value_tests.cpp
        tests/property_key_symbol_tests.cpp
        tests/property_descriptor_tests.cpp
        tests/runtime_context_tests.cpp
        tests/abstract_operations_tests.cpp
        tests/result_tests.cpp
        tests/bytecode_tests.cpp
        tests/vm_tests.cpp
        tests/frontend/frontend_tests.cpp
        tests/compiler/compiler_tests.cpp
        tests/compiler/scope_tests.cpp
        tests/compiler/function_tests.cpp
        tests/compiler/closure_tests.cpp
        tests/compiler/object_tests.cpp
        tests/compiler/prototype_this_tests.cpp
        tests/compiler/array_builtin_tests.cpp
        tests/compiler/exception_tests.cpp
        tests/gc_tests.cpp
        tests/compiler/class_constructor_tests.cpp
        tests/compiler/iteration_tests.cpp
        tests/module/module_tests.cpp
        tests/compiler/promise_job_tests.cpp
        tests/compiler/generator_tests.cpp
        tests/compiler/regexp_tests.cpp
    )
    target_link_libraries(jsengine_tests PRIVATE jsengine::jsengine)
    target_include_directories(jsengine_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    jsengine_set_warnings(jsengine_tests)
    if(JSENGINE_ENABLE_SANITIZERS)
        jsengine_enable_sanitizers(jsengine_tests)
    endif()
    add_test(NAME jsengine.unit COMMAND jsengine_tests)
endif()

include(cmake/Test262.cmake)


install(TARGETS jsengine
    EXPORT jsengineTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT jsengineTargets
    FILE jsengineTargets.cmake
    NAMESPACE jsengine::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/jsengine
)

if(JSENGINE_BUILD_FUZZ_TARGETS)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "JSENGINE_BUILD_FUZZ_TARGETS requires Clang/libFuzzer")
    endif()
    add_executable(jsengine_frontend_fuzz fuzz/frontend_fuzz.cpp)
    target_link_libraries(jsengine_frontend_fuzz PRIVATE jsengine::jsengine)
    target_compile_options(jsengine_frontend_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
    target_link_options(jsengine_frontend_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
    jsengine_set_warnings(jsengine_frontend_fuzz)
endif()
