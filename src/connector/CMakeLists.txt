# LTSM connector

target_sources(${CUR_TARGET} PRIVATE ltsm_connector.cpp ltsm_connector_proto.cpp ltsm_render_primitives.cpp)

# boost + openssl warnings
target_compile_options(${CUR_TARGET} PRIVATE -Wno-deprecated-declarations)

find_package(OpenSSL 3.0)
if(OpenSSL_FOUND)
    target_link_libraries(${CUR_TARGET} PRIVATE OpenSSL::Crypto OpenSSL::SSL)
else()
    pkg_check_modules(OPENSSL REQUIRED libssl3 libcrypto3)
    target_link_directories(${CUR_TARGET} PRIVATE ${OPENSSL_LIBRARY_DIRS})
    target_link_libraries(${CUR_TARGET} PRIVATE ${OPENSSL_LIBRARIES})
endif()

if(LTSM_SERVER_CONNECTOR_RDP)

    pkg_check_modules(RDP2 freerdp2 winpr2)
    pkg_check_modules(RDP3 freerdp3 winpr3)

    if(RDP2_FOUND)
        target_sources(${CUR_TARGET} PRIVATE ltsm_connector_rdp.cpp)
        target_compile_definitions(${CUR_TARGET} PRIVATE LTSM_WITH_RDP)
        target_compile_options(${CUR_TARGET} PRIVATE ${RDP2_CFLAGS})
        target_link_libraries(${CUR_TARGET} PRIVATE ${RDP2_LIBRARIES})
    elseif(RDP3_FOUND)
        target_sources(${CUR_TARGET} PRIVATE ltsm_connector_rdp.cpp)
        target_compile_definitions(${CUR_TARGET} PRIVATE LTSM_WITH_RDP FREERDP3_API)
        target_compile_options(${CUR_TARGET} PRIVATE ${RDP3_CFLAGS})
        target_link_libraries(${CUR_TARGET} PRIVATE ${RDP3_LIBRARIES})
    else()
        message(STATUS "DevLibraries FreeRDP2/3 not found")
    endif()
endif()
