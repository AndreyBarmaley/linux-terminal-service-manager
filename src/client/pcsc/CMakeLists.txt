# LTSM pcsc

if(MINGW)
    # internal support
    target_sources(${CUR_TARGET} PRIVATE ltsm_pcsc_client.cpp)
    target_compile_definitions(${CUR_TARGET} PUBLIC -DLTSM_WITH_PCSC)

elseif(LTSM_CLIENT_SCARD_REDIRECT)

    pkg_check_modules(PCSCLITE libpcsclite)

    if(PCSCLITE_FOUND)
        target_sources(${CUR_TARGET} PRIVATE ltsm_pcsc_client.cpp)
        target_compile_definitions(${CUR_TARGET} PUBLIC -DLTSM_WITH_PCSC)
        target_compile_options(${CUR_TARGET} PRIVATE ${PCSCLITE_CFLAGS})
        target_include_directories(${CUR_TARGET} PRIVATE ${PCSCLITE_INCLUDE_DIR})
        target_link_directories(${CUR_TARGET} PRIVATE ${PCSCLITE_LIBRARY_DIRS})
        target_link_libraries(${CUR_TARGET} PRIVATE ${PCSCLITE_LIBRARIES})

        if(APPLE)
            target_link_libraries(${CUR_TARGET} PRIVATE "-framework PCSC")
        endif()
    else()
        set(LTSM_CLIENT_SCARD_REDIRECT OFF)
        message(STATUS "PCSC-LITE not found")
    endif()
endif()
