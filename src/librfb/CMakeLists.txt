# LTSM librfb

if(LTSM_WITH_DECODER)

    add_subdirectory(client ${CMAKE_CURRENT_BINARY_DIR}/librfb_client)

    target_include_directories(${CUR_TARGET} PRIVATE client)
    target_compile_definitions(${CUR_TARGET} PUBLIC -DLTSM_DECODING -DLTSM_DECODING_QOI)

    target_sources(${CUR_TARGET} PRIVATE ltsm_librfb.cpp librfb_extclip.cpp)

    if(LTSM_WITH_FFMPEG)
        add_subdirectory(ffmpeg ${CMAKE_CURRENT_BINARY_DIR}/${CUR_TARGET}_ffmpeg)

        if(FFMPEG_FOUND)
            target_compile_definitions(${CUR_TARGET} PUBLIC -DLTSM_DECODING_FFMPEG)
            target_include_directories(${CUR_TARGET} PRIVATE ffmpeg)
        endif()
    endif()

endif()

if(LTSM_WITH_ENCODER)

    add_subdirectory(server ${CMAKE_CURRENT_BINARY_DIR}/librfb_server)

    target_include_directories(${CUR_TARGET} PRIVATE server)
    target_compile_definitions(${CUR_TARGET} PUBLIC -DLTSM_ENCODING -DLTSM_ENCODING_QOI)

    target_sources(${CUR_TARGET} PRIVATE ltsm_librfb.cpp librfb_extclip.cpp)

    if(LTSM_WITH_FFMPEG)
        add_subdirectory(ffmpeg ${CMAKE_CURRENT_BINARY_DIR}/${CUR_TARGET}_ffmpeg)

        if(FFMPEG_FOUND)
            target_compile_definitions(${CUR_TARGET} PUBLIC -DLTSM_ENCODING_FFMPEG)
            target_include_directories(${CUR_TARGET} PRIVATE ffmpeg)
        endif()
    endif()

endif()
