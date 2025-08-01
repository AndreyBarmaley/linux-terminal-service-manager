# LTSM librfb client

target_compile_definitions(${CUR_TARGET} PUBLIC -DLTSM_DECODING -DLTSM_DECODING_QOI)
target_sources(${CUR_TARGET} PRIVATE librfb_decodings.cpp librfb_client.cpp)

if(APPLE)
    target_sources(${CUR_TARGET} PRIVATE librfb_winclient.cpp)
elseif(UNIX)
    target_sources(${CUR_TARGET} PRIVATE librfb_x11client.cpp)
elseif(MINGW)
    target_sources(${CUR_TARGET} PRIVATE librfb_winclient.cpp)
endif()

pkg_check_modules(LZ4 liblz4)

if(LZ4_FOUND)
    target_compile_options(${CUR_TARGET} PRIVATE ${LZ4_CFLAGS})
    target_compile_definitions(${CUR_TARGET} PUBLIC -DLTSM_DECODING_LZ4)
    target_include_directories(${CUR_TARGET} PRIVATE ${LZ4_INCLUDE_DIR})
    target_link_directories(${CUR_TARGET} PRIVATE ${LZ4_LIBRARY_DIRS})
    target_link_libraries(${CUR_TARGET} PRIVATE ${LZ4_LIBRARIES})
else()
    message(STATUS "Decoder LZ4 not found")
endif()

pkg_check_modules(TURBOJPEG libturbojpeg)

if(TURBOJPEG_FOUND)
    target_compile_options(${CUR_TARGET} PRIVATE ${TURBOJPEG_CFLAGS})
    target_compile_definitions(${CUR_TARGET} PUBLIC -DLTSM_DECODING_TJPG)
    target_include_directories(${CUR_TARGET} PRIVATE ${TURBOJPEG_INCLUDE_DIR})
    target_link_directories(${CUR_TARGET} PRIVATE ${TURBOJPEG_LIBRARY_DIRS})
    target_link_libraries(${CUR_TARGET} PRIVATE ${TURBOJPEG_LIBRARIES})
else()
    message(STATUS "Decoder TurboJPEG not found")
endif()
