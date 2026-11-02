cmake_minimum_required(VERSION 3.20)
project(bsa_reader VERSION 0.1.0 LANGUAGES CXX)

option(BSA_READER_BUILD_TOOLS "Build the bsatool command line program" ${PROJECT_IS_TOP_LEVEL})
option(BSA_READER_BUILD_TESTS "Build and register the unit tests" ${PROJECT_IS_TOP_LEVEL})

add_library(bsa_reader STATIC
    src/hash.cpp
    src/inflate.cpp
    src/lz4f.cpp
    src/reader.cpp)
add_library(bsa::reader ALIAS bsa_reader)
target_include_directories(bsa_reader PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_compile_features(bsa_reader PUBLIC cxx_std_17)
if(MSVC)
    target_compile_options(bsa_reader PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(bsa_reader PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(BSA_READER_BUILD_TOOLS)
    add_executable(bsatool tools/bsatool.cpp)
    target_link_libraries(bsatool PRIVATE bsa::reader)
endif()

if(BSA_READER_BUILD_TESTS)
    enable_testing()
    add_executable(bsa_reader_tests tests/test_reader.cpp)
    target_link_libraries(bsa_reader_tests PRIVATE bsa::reader)
    target_include_directories(bsa_reader_tests PRIVATE src)
    add_test(NAME unit COMMAND bsa_reader_tests)
endif()

include(GNUInstallDirs)
install(TARGETS bsa_reader EXPORT bsa_readerTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT bsa_readerTargets NAMESPACE bsa:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/bsa_reader)
