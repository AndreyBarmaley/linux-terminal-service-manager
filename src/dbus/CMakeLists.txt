find_package(sdbus-c++ REQUIRED)

if(${sdbus-c++_VERSION} VERSION_LESS_EQUAL "1.1.0")
    message(FATAL_ERROR "SDBUS-C++ minimal version 1.2.0")
endif()

if(${sdbus-c++_VERSION} VERSION_GREATER_EQUAL "2.0.0")
    target_compile_definitions(${CUR_TARGET} PRIVATE -DSDBUS_2_0_API)
endif()

target_link_libraries(${CUR_TARGET} PRIVATE SDBusCpp::sdbus-c++)
target_compile_definitions(${CUR_TARGET} PRIVATE ${sdbus-c++_DEFINITIONS})
        
if(LTSM_SERVER_BUILD_SERVICE)
endif()

if(LTSM_SERVER_BUILD_CONNECTOR)
endif()
