# Copyright (c) 2019 Stanford University
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in all
# copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.

cmake_minimum_required(VERSION 3.11)

project(HomaRpcBench VERSION 0.0.1 LANGUAGES CXX)

# Tell CMake where to find our custom/3rd-party "Find" modules
list(APPEND CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/cmake/modules)

find_package(Homa COMPONENTS DpdkDriver)
find_package(Roo)
find_package(PerfUtils)

# Source control tool; needed to download external libraries.
find_package(Git REQUIRED)

include(FetchContent)
FetchContent_Declare(
    docopt
    GIT_REPOSITORY https://github.com/docopt/docopt.cpp.git
    GIT_TAG        3dd23e3280f213bacefdf5fcb04857bf52e90917
)

FetchContent_GetProperties(docopt)
if(NOT docopt_POPULATED)
    FetchContent_Populate(docopt)
    add_subdirectory(
        ${docopt_SOURCE_DIR}
        ${docopt_BINARY_DIR}
        EXCLUDE_FROM_ALL
    )
endif()

add_executable(coordinator
    src/CoordinatorMain.cc
)
target_link_libraries(coordinator
    PRIVATE
        Roo::Roo
        Homa::Homa
        Homa::DpdkDriver
        docopt
)

add_executable(client
    src/ClientMain.cc
)
target_link_libraries(client
    PRIVATE
        Roo::Roo
        Homa::Homa
        Homa::DpdkDriver
        docopt
        PerfUtils
)

add_executable(server
    src/ServerMain.cc
)
target_link_libraries(server
    PRIVATE
        Roo::Roo
        Homa::Homa
        Homa::DpdkDriver
        docopt
)

add_executable(dpdk_test
    src/DpdkTestMain.cc
)
target_link_libraries(dpdk_test
    PRIVATE
        Homa::DpdkDriver
        docopt
)
