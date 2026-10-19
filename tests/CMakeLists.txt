# SPDX-License-Identifier: Apache-2.0

include(FetchContent)
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.17.0
  FIND_PACKAGE_ARGS NAMES GTest
)
FetchContent_MakeAvailable(googletest)

add_library(libbtop_test)
target_include_directories(libbtop_test PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(libbtop_test libbtop GTest::gtest_main)

add_executable(btop_test cpu_names.cpp tools.cpp)
target_link_libraries(btop_test libbtop_test)

include(GoogleTest)
gtest_discover_tests(btop_test)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_executable(intel_gpu_memory_test intel_gpu_memory.cpp ../src/linux/intel_gpu_memory.cpp)
  target_include_directories(intel_gpu_memory_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
  target_link_libraries(intel_gpu_memory_test PRIVATE GTest::gtest_main)
  target_compile_options(intel_gpu_memory_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
  gtest_discover_tests(intel_gpu_memory_test)
endif()
