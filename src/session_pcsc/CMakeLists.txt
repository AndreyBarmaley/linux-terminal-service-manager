# LTSM session_pcsc

if(LTSM_SERVER_BUILD_SESSION_PCSC)
    pkg_check_modules(PCSCLITE REQUIRED libpcsclite)
    target_sources(${CUR_TARGET} PRIVATE ltsm_pcsc_session.cpp)

    target_compile_options(${CUR_TARGET} PRIVATE ${PCSCLITE_CFLAGS})
    target_include_directories(${CUR_TARGET} PRIVATE ${PCSCLITE_INCLUDE_DIR})
    target_link_libraries(${CUR_TARGET} PRIVATE ${PCSCLITE_LIBRARIES})
endif()
