cmake_minimum_required(VERSION 3.16)
project(minikv CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)  # lets VS Code's C++ extension find headers

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Threads REQUIRED)

# Core logic as a library, so the server and the tests share the same code.
add_library(minikv_core
  src/store.cpp
  src/protocol.cpp
  src/aof.cpp
  src/database.cpp
)
target_include_directories(minikv_core PUBLIC src)
target_compile_options(minikv_core PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(minikv_core PUBLIC Threads::Threads)

add_executable(minikv src/main.cpp src/server.cpp)
target_compile_options(minikv PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(minikv PRIVATE minikv_core)

enable_testing()
add_executable(minikv_tests tests/test_main.cpp)
target_compile_options(minikv_tests PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(minikv_tests PRIVATE minikv_core)
add_test(NAME minikv_tests COMMAND minikv_tests)
