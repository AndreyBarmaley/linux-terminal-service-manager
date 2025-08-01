# LTSM librfb server

target_compile_definitions(${CUR_TARGET} PUBLIC -DLTSM_ENCODING -DLTSM_ENCODING_QOI)
target_sources(${CUR_TARGET} PRIVATE librfb_encodings.cpp librfb_server.cpp librfb_x11server.cpp)

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
