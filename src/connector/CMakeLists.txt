# LTSM connector

target_sources(${CUR_TARGET} PRIVATE ltsm_connector.cpp ltsm_connector_proto.cpp ltsm_render_primitives.cpp)

if(LTSM_SERVER_CONNECTOR_RDP)

    pkg_check_modules(RDP2 freerdp2 winpr2)

    if(RDP2_FOUND)
        target_sources(${CUR_TARGET} PRIVATE ltsm_connector_rdp.cpp)
        target_compile_definitions(${CUR_TARGET} PUBLIC -DLTSM_WITH_RDP)
        target_compile_options(${CUR_TARGET} PRIVATE ${RDP2_CFLAGS})
        target_link_libraries(${CUR_TARGET} PRIVATE ${RDP2_LIBRARIES})
    else()
        message(STATUS "DevLibraries FreeRDP2 not found")
    endif()
endif()
