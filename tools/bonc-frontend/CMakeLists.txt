add_executable(bonc-frontend
  main.cpp
)

set(KLEE_LIBS kleeCore)

target_link_libraries(bonc-frontend ${KLEE_LIBS})
target_include_directories(bonc-frontend PRIVATE ${KLEE_INCLUDE_DIRS} ${LLVM_INCLUDE_DIRS})
target_compile_options(bonc-frontend PRIVATE ${KLEE_COMPONENT_CXX_FLAGS})
target_compile_definitions(bonc-frontend PRIVATE ${KLEE_COMPONENT_CXX_DEFINES})

install(TARGETS bonc-frontend RUNTIME DESTINATION bin)

# The KLEE binary depends on the runtimes
add_dependencies(bonc-frontend BuildKLEERuntimes)
