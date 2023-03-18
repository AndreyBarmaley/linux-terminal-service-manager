foreach(DIR IN ITEMS json streambuf unixsocket framebuf)
    add_executable(LTSM_test_${DIR})

    file(GLOB_RECURSE SOURCELIST "${DIR}/*.cpp")
    target_sources(LTSM_test_${DIR} PRIVATE ${SOURCELIST})

    find_package(GnuTLS REQUIRED)
    find_library(GNUTLSXX_LIBRARY NAMES gnutlsxx libgnutlsxx REQUIRED)

    target_link_libraries(LTSM_test_${DIR} ${SDL2_LIBRARIES})
    target_link_libraries(LTSM_test_${DIR} ${SDL2_IMAGE_LIBRARIES})
    target_compile_options(LTSM_test_${DIR} PRIVATE ${EXTRA_CFLAGS})
    target_compile_options(LTSM_test_${DIR} PRIVATE -DLTSM_WITH_GNUTLS -DGNUTLS_GNUTLSXX_NO_HEADERONLY -DLTSM_SOCKET_ZLIB)

    target_include_directories(LTSM_test_${DIR} PRIVATE ${CMAKE_SOURCE_DIR}/src/common)

    target_link_libraries(LTSM_test_${DIR} LTSM_common)
    target_link_libraries(LTSM_test_${DIR} ${GNUTLSXX_LIBRARY} ${GNUTLS_LIBRARIES})
    target_link_libraries(LTSM_test_${DIR} z)

    set_target_properties(LTSM_test_${DIR} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/../..)
endforeach()

foreach(DIR IN ITEMS encodings)
    add_executable(LTSM_test_${DIR})

    file(GLOB_RECURSE SOURCELIST "${DIR}/*.cpp")
    target_sources(LTSM_test_${DIR} PRIVATE ${SOURCELIST}
        ${CMAKE_SOURCE_DIR}/src/librfb/ltsm_librfb.cpp
        ${CMAKE_SOURCE_DIR}/src/librfb/librfb_encodings.cpp
        ${CMAKE_SOURCE_DIR}/src/channels/channel_system.cpp)

    find_package(GnuTLS REQUIRED)
    find_library(GNUTLSXX_LIBRARY NAMES gnutlsxx libgnutlsxx REQUIRED)

    pkg_search_module(SDL2 REQUIRED sdl2>=2.0.1)
    pkg_search_module(SDL2_IMAGE REQUIRED SDL2_image>=2.0.1)

    target_link_libraries(LTSM_test_${DIR} ${SDL2_LIBRARIES})
    target_link_libraries(LTSM_test_${DIR} ${SDL2_IMAGE_LIBRARIES})
    target_compile_options(LTSM_test_${DIR} PRIVATE ${EXTRA_CFLAGS})
    target_compile_options(LTSM_test_${DIR} PRIVATE -DLTSM_WITH_GNUTLS -DGNUTLS_GNUTLSXX_NO_HEADERONLY -DLTSM_SOCKET_ZLIB)

    target_include_directories(LTSM_test_${DIR} PRIVATE ${CMAKE_SOURCE_DIR}/src/librfb)
    target_include_directories(LTSM_test_${DIR} PRIVATE ${CMAKE_SOURCE_DIR}/src/common)
    target_include_directories(LTSM_test_${DIR} PRIVATE ${CMAKE_SOURCE_DIR}/src/channels)

    target_link_libraries(LTSM_test_${DIR} LTSM_common)
    target_link_libraries(LTSM_test_${DIR} ${SDL2_LIBRARIES})
    target_link_libraries(LTSM_test_${DIR} ${SDL2_IMAGE_LIBRARIES})
    target_link_libraries(LTSM_test_${DIR} ${GNUTLSXX_LIBRARY} ${GNUTLS_LIBRARIES})
    target_link_libraries(LTSM_test_${DIR} z)

    set_target_properties(LTSM_test_${DIR} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/../..)
endforeach()
