foreach(DIR IN ITEMS json streambuf unixsocket framebuf xrandr x11clip)
    add_executable(LTSM_test_${DIR})

    file(GLOB_RECURSE SOURCELIST "${DIR}/*.cpp")
    target_sources(LTSM_test_${DIR} PRIVATE ${SOURCELIST})

    find_package(GnuTLS REQUIRED)

    target_link_libraries(LTSM_test_${DIR} ${SDL2_LIBRARIES})
    target_link_libraries(LTSM_test_${DIR} ${SDL2_IMAGE_LIBRARIES})
    target_compile_options(LTSM_test_${DIR} PRIVATE ${EXTRA_CFLAGS})
    target_compile_definitions(LTSM_test_${DIR} PRIVATE -DLTSM_WITH_GNUTLS -DLTSM_SOCKET_ZLIB)

    target_include_directories(LTSM_test_${DIR} PRIVATE ${CMAKE_SOURCE_DIR}/src/common)

    target_link_libraries(LTSM_test_${DIR} LTSM_common)
    target_link_libraries(LTSM_test_${DIR} ${GNUTLS_LIBRARIES})
    target_link_libraries(LTSM_test_${DIR} z)

    set_target_properties(LTSM_test_${DIR} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/../..)
endforeach()

foreach(DIR IN ITEMS encodings x11enc)
    add_executable(LTSM_test_${DIR})

    file(GLOB_RECURSE SOURCELIST "${DIR}/*.cpp")
    target_sources(LTSM_test_${DIR} PRIVATE ${SOURCELIST}
        ${CMAKE_SOURCE_DIR}/src/librfb/ltsm_librfb.cpp
        ${CMAKE_SOURCE_DIR}/src/librfb/librfb_encodings.cpp
        ${CMAKE_SOURCE_DIR}/src/channels/channel_system.cpp)

    find_package(GnuTLS REQUIRED)

    pkg_search_module(SDL2 REQUIRED sdl2>=2.0.1)
    pkg_search_module(SDL2_IMAGE REQUIRED SDL2_image>=2.0.1)
    pkg_search_module(LZ4 REQUIRED lz4)
    pkg_search_module(TURBOJPEG REQUIRED turbojpeg)

    target_link_libraries(LTSM_test_${DIR} ${SDL2_LIBRARIES})
    target_link_libraries(LTSM_test_${DIR} ${SDL2_IMAGE_LIBRARIES} ${LZ4_LIBRARIES} ${TURBOJPEG_LIBRARIES})
    target_compile_options(LTSM_test_${DIR} PRIVATE ${EXTRA_CFLAGS})
    target_compile_definitions(LTSM_test_${DIR} PRIVATE -DLTSM_WITH_GNUTLS -DLTSM_SOCKET_ZLIB -DLTSM_ENCODING -DLTSM_ENCODING_FFMPEG)

    target_include_directories(LTSM_test_${DIR} PRIVATE ${CMAKE_SOURCE_DIR}/src/librfb)
    target_include_directories(LTSM_test_${DIR} PRIVATE ${CMAKE_SOURCE_DIR}/src/common)
    target_include_directories(LTSM_test_${DIR} PRIVATE ${CMAKE_SOURCE_DIR}/src/channels)

    if(LTSM_FFMPEG)
        pkg_search_module(AVDEVICE REQUIRED libavdevice)
        pkg_search_module(AVFORMAT REQUIRED libavformat)
        pkg_search_module(AVCODEC REQUIRED libavcodec)
        pkg_search_module(AVSWSCALE REQUIRED libswscale)
        pkg_search_module(AVSWRESAMPLE REQUIRED libswresample)
        pkg_search_module(AVUTIL REQUIRED libavutil)

        target_sources(LTSM_test_${DIR} PRIVATE ${CMAKE_SOURCE_DIR}/src/librfb/ffmpeg_tools.cpp ${CMAKE_SOURCE_DIR}/src/librfb/librfb_ffmpeg.cpp)
    
        target_compile_options(LTSM_test_${DIR} PRIVATE
                ${AVDEVICE_CFLAGS} ${AVFORMAT_CFLAGS} ${AVCODEC_CFLAGS}
                ${AVSWSCALE_CFLAGS} ${AVSWRESAMPLE_CFLAGS} ${AVUTIL_CFLAGS})

        target_link_options(LTSM_test_${DIR} PRIVATE
                ${AVDEVICE_LDFLAGS} ${AVFORMAT_LDFLAGS} ${AVCODEC_LDFLAGS}
                ${AVSWSCALE_LDFLAGS} ${AVSWRESAMPLE_LDFLAGS} ${AVUTIL_LDFLAGS})

        target_link_libraries(LTSM_test_${DIR}
                ${AVDEVICE_LIBRARIES} ${AVFORMAT_LIBRARIES} ${AVCODEC_LIBRARIES}
                ${AVSWSCALE_LIBRARIES} ${AVSWRESAMPLE_LIBRARIES} ${AVUTIL_LIBRARIES})
    endif()

    if(LTSM_PKCS11_AUTH)
        pkg_search_module(P11KIT REQUIRED p11-kit-1)
        target_include_directories(LTSM_test_${DIR} PRIVATE ${CMAKE_SOURCE_DIR}/src/session_pkcs11 ${P11KIT_INCLUDE_DIRS})
    endif()

    target_link_libraries(LTSM_test_${DIR} LTSM_common)
    target_link_libraries(LTSM_test_${DIR} ${SDL2_LIBRARIES})
    target_link_libraries(LTSM_test_${DIR} ${SDL2_IMAGE_LIBRARIES})
    target_link_libraries(LTSM_test_${DIR} ${GNUTLS_LIBRARIES})
    target_link_libraries(LTSM_test_${DIR} z)

    set_target_properties(LTSM_test_${DIR} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/../..)
endforeach()
