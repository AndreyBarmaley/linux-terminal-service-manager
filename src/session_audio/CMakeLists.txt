# LTSM session_audio

if(LTSM_SERVER_BUILD_SESSION_AUDIO)

    target_sources(${CUR_TARGET} PRIVATE ltsm_audio_encoder.cpp ltsm_audio_session.cpp)
    pkg_check_modules(OPUS opus)
    pkg_check_modules(PIPEWIRE REQUIRED libpipewire-0.3)
    pkg_check_modules(SPA REQUIRED libspa-0.2)

    if(PIPEWIRE_FOUND)
        target_sources(${CUR_TARGET} PRIVATE ltsm_audio_pipewire.cpp)
        target_compile_definitions(${CUR_TARGET} PUBLIC -DLTSM_WITH_PIPEWIRE)
        target_compile_options(${CUR_TARGET} PRIVATE ${PIPEWIRE_CFLAGS} ${SPA_CFLAGS})
        target_include_directories(${CUR_TARGET} PRIVATE ${PIPEWIRE_INCLUDE_DIR} ${SPA_INCLUDE_DIR})
        target_link_libraries(${CUR_TARGET} PRIVATE ${PIPEWIRE_LIBRARIES} ${SPA_LIBRARIES})
    else()
        message(STATUS "Decoder PIPEWIRE not found")

        pkg_check_modules(PULSEAUDIO libpulse)

        if(PULSEAUDIO_FOUND)
            target_sources(${CUR_TARGET} PRIVATE ltsm_audio_pulse.cpp)
            target_compile_definitions(${CUR_TARGET} PUBLIC -DLTSM_WITH_PULSE)
            target_compile_options(${CUR_TARGET} PRIVATE ${PULSEAUDIO_CFLAGS})
            target_include_directories(${CUR_TARGET} PRIVATE ${PULSEAUDIO_INCLUDE_DIR})
            target_link_libraries(${CUR_TARGET} PRIVATE ${PULSEAUDIO_LIBRARIES})
        else()
            message(STATUS "Decoder PULSEAUDIO not found")
        endif()
    endif()

    if(OPUS_FOUND)
        target_compile_definitions(${CUR_TARGET} PUBLIC -DLTSM_WITH_OPUS)
        target_compile_options(${CUR_TARGET} PRIVATE ${OPUS_CFLAGS})
        target_include_directories(${CUR_TARGET} PRIVATE ${OPUS_INCLUDE_DIR})
        target_link_libraries(${CUR_TARGET} PRIVATE ${OPUS_LIBRARIES})
    else()
        message(STATUS "Decoder OPUS not found")
    endif()

endif()
 