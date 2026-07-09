# LTSM librfb ffmpeg

if(LTSM_WITH_FFMPEG)
    pkg_check_modules(FFMPEG libavdevice libavformat libavcodec libswscale libswresample libavutil)

    if(FFMPEG_FOUND)
        if(LTSM_WITH_DECODER)
            target_compile_definitions(${CUR_TARGET} PUBLIC
                -DLTSM_DECODING_H264 -DLTSM_DECODING_MPEG4)
        endif()

        if(LTSM_WITH_DECODER)
            target_compile_definitions(${CUR_TARGET} PUBLIC
                -DLTSM_ENCODING_H264 -DLTSM_ENCODING_MPEG4)
        endif()

        target_sources(${CUR_TARGET} PRIVATE librfb_ffmpeg.cpp)
        target_compile_definitions(${CUR_TARGET} PUBLIC -DLTSM_WITH_FFMPEG)
        target_compile_options(${CUR_TARGET} PRIVATE ${FFMPEG_CFLAGS})
        target_link_options(${CUR_TARGET} PRIVATE ${FFMPEG_LDFLAGS})
        target_link_libraries(${CUR_TARGET} PRIVATE ${FFMPEG_LIBRARIES})
    else()
        message(STATUS "DevLibraries FFMPEG not found")
    endif()
else()
    set(LTSM_WITH_FFMPEG OFF)
endif()
