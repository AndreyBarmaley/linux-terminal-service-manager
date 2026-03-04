if(LTSM_CLIENT_AUDIO_REDIRECT)
    if(UNIX AND NOT APPLE)
        set(LTSM_CLIENT_PLAYBACK_PULSE OFF CACHE BOOL "client: audio playback pulseaudio")
    endif()

    set(LTSM_CLIENT_PLAYBACK_OPENAL ON CACHE BOOL "client: audio playback openal")

    pkg_check_modules(OPUS opus)

    if(OPUS_FOUND)
        target_compile_definitions(${CUR_TARGET} PUBLIC -DLTSM_WITH_OPUS)
        target_compile_options(${CUR_TARGET} PRIVATE ${OPUS_CFLAGS})
        target_include_directories(${CUR_TARGET} PRIVATE ${OPUS_INCLUDE_DIR})
        target_link_directories(${CUR_TARGET} PRIVATE ${OPUS_LIBRARY_DIRS})
        target_link_libraries(${CUR_TARGET} PRIVATE ${OPUS_LIBRARIES})
    else()
        message(STATUS "Decoder OPUS not found")
    endif()

    target_sources(${CUR_TARGET} PRIVATE ltsm_audio_client.cpp ltsm_audio_decoder.cpp)

    if(LTSM_CLIENT_PLAYBACK_OPENAL)
        find_package(OpenAL)

        if(OPENAL_FOUND)
            target_sources(${CUR_TARGET} PRIVATE ltsm_audio_openal.cpp)
            target_compile_definitions(${CUR_TARGET} PRIVATE -DLTSM_WITH_PLAYBACK_OPENAL)
            # maybe linker error: incorrect size
            target_compile_options(${CUR_TARGET} PRIVATE -fPIC)

            target_compile_options(${CUR_TARGET} PRIVATE ${OPENAL_CFLAGS})
            target_include_directories(${CUR_TARGET} PRIVATE ${OPENAL_INCLUDE_DIR})
            target_link_directories(${CUR_TARGET} PRIVATE ${OPENAL_LIBRARY_DIRS})
            target_link_libraries(${CUR_TARGET} PRIVATE ${OPENAL_LIBRARY})
        else()
            message(STATUS "OpenAL not found")
            unset(LTSM_CLIENT_PLAYBACK_OPENAL CACHE)
        endif()
    endif()

    if(LTSM_CLIENT_PLAYBACK_PULSE)
        pkg_check_modules(PULSEAUDIO REQUIRED libpulse)

        if(PULSEAUDIO_FOUND)
            pkg_check_modules(PULSESIMPLE REQUIRED libpulse-simple)
    
            target_sources(${CUR_TARGET} PRIVATE ltsm_pulse_simple.cpp)
            target_compile_definitions(${CUR_TARGET} PRIVATE -DLTSM_WITH_PLAYBACK_PULSE)

            target_compile_options(${CUR_TARGET} PRIVATE ${PULSEAUDIO_CFLAGS})
            target_include_directories(${CUR_TARGET} PRIVATE ${PULSEAUDIO_INCLUDE_DIR})
            target_link_libraries(${CUR_TARGET} PRIVATE ${PULSEAUDIO_LIBRARIES} ${PULSESIMPLE_LIBRARIES})
        else()
            message(STATUS "PulseAudio not found")
            unset(LTSM_CLIENT_PLAYBACK_PULSE CACHE)
        endif()
    endif()

    if(LTSM_CLIENT_PLAYBACK_OPENAL OR LTSM_CLIENT_PLAYBACK_PULSE)
        target_compile_definitions(${CUR_TARGET} PUBLIC -DLTSM_WITH_AUDIO)
    else()
        unset(LTSM_CLIENT_AUDIO_REDIRECT CACHE)
    endif()
else()
    unset(LTSM_CLIENT_PLAYBACK_PULSE CACHE)
    unset(LTSM_CLIENT_PLAYBACK_OPENAL CACHE)
endif()
