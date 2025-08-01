# LTSM session_audio

if(LTSM_SERVER_BUILD_SESSION_AUDIO)

    target_sources(${CUR_TARGET} PRIVATE ltsm_audio_pulse.cpp ltsm_audio_encoder.cpp ltsm_audio_session.cpp)
    pkg_check_modules(OPUS opus)

    if(OPUS_FOUND)
        target_compile_definitions(${CUR_TARGET} PUBLIC -DLTSM_WITH_OPUS)
        target_compile_options(${CUR_TARGET} PRIVATE ${OPUS_CFLAGS})
        target_include_directories(${CUR_TARGET} PRIVATE ${OPUS_INCLUDE_DIR})
        target_link_libraries(${CUR_TARGET} PRIVATE ${OPUS_LIBRARIES})
    else()
        message(STATUS "Decoder OPUS not found")
    endif()

    pkg_check_modules(PULSEAUDIO REQUIRED libpulse)

    target_compile_options(${CUR_TARGET} PRIVATE ${PULSEAUDIO_CFLAGS})
    target_include_directories(${CUR_TARGET} PRIVATE ${PULSEAUDIO_INCLUDE_DIR})
    target_link_libraries(${CUR_TARGET} PRIVATE ${PULSEAUDIO_LIBRARIES})
endif()
 