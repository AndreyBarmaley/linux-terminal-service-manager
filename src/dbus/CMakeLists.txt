find_package(sdbus-c++ REQUIRED)

if(${sdbus-c++_VERSION} VERSION_GREATER_EQUAL "1.2.0")
    target_compile_definitions(${CUR_TARGET} PRIVATE -DSDBUS_ADDRESS_SUPPORT)
endif()

if(${sdbus-c++_VERSION} VERSION_GREATER_EQUAL "2.0.0")
    target_compile_definitions(${CUR_TARGET} PRIVATE -DSDBUS_2_0_API)
endif()

target_link_libraries(${CUR_TARGET} PRIVATE SDBusCpp::sdbus-c++)
target_compile_definitions(${CUR_TARGET} PRIVATE -DLTSM_WITH_SDBUS ${sdbus-c++_DEFINITIONS})
        
if(LTSM_SERVER_BUILD_SERVICE)
endif()

if(LTSM_SERVER_BUILD_CONNECTOR)
endif()
