project(ltsm_librfb_tests)

foreach(CUR_TARGET ltsm_test_codecs)

    CommonDefaults(JSON ZLIB XCB ENCODER DECODER SDL OPENSSL GNUTLS)
    add_executable(${CUR_TARGET} src/${CUR_TARGET}.cpp)

    find_package(GTest REQUIRED)
    enable_testing()

    target_include_directories(${CUR_TARGET} PRIVATE ${CMAKE_SOURCE_DIR}/src/common ${CMAKE_SOURCE_DIR}/src/channels ${CMAKE_SOURCE_DIR}/src/librfb)
    add_subdirectory(${CMAKE_SOURCE_DIR}/src/common ${CUR_TARGET}_common)
    add_subdirectory(${CMAKE_SOURCE_DIR}/src/librfb ${CUR_TARGET}_librfb)
    add_subdirectory(${CMAKE_SOURCE_DIR}/src/channels ${CUR_TARGET}_channels)

    target_link_libraries(${CUR_TARGET} PRIVATE GTest::gmock)

    add_test(NAME ${CUR_TARGET} COMMAND ${CUR_TARGET})
endforeach()
