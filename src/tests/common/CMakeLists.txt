project(ltsm_common_tests)

foreach(CUR_TARGET ltsm_test_zlib ltsm_test_binarybuf ltsm_test_streambuf ltsm_test_application ltsm_test_asyncmutex
         ltsm_test_xcbtypes ltsm_test_framebuffer ltsm_test_tools ltsm_test_jsonwrapper ltsm_test_bytestream)

    CommonDefaults(JSON ZLIB SDL XCB)
    add_executable(${CUR_TARGET} src/${CUR_TARGET}.cpp)

    pkg_search_module(SDL2_IMAGE REQUIRED SDL2_image)
    find_package(GTest REQUIRED)
    enable_testing()

    target_include_directories(${CUR_TARGET} PRIVATE ${CMAKE_SOURCE_DIR}/src/common)
    add_subdirectory(${CMAKE_SOURCE_DIR}/src/common ${CUR_TARGET}_common)

    target_link_libraries(${CUR_TARGET} PRIVATE ${SDL2_IMAGE_LIBRARIES})
    target_link_libraries(${CUR_TARGET} PRIVATE GTest::gmock)

    set_target_properties(${CUR_TARGET} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/../../..)
    #add_test(NAME LtsmCommonTests COMMAND ${CUR_TARGET})
endforeach()
