foreach(DIR IN ITEMS json streambuf unixsocket framebuf)
    add_executable(LTSM_test_${DIR} ${DIR}/main.cpp)

    find_package(GnuTLS REQUIRED)
    find_library(GNUTLSXX_LIBRARY NAMES gnutlsxx libgnutlsxx REQUIRED)

    target_compile_options(LTSM_test_${DIR} PRIVATE ${EXTRA_CFLAGS})
    target_compile_options(LTSM_test_${DIR} PRIVATE -DLTSM_WITH_GNUTLS -DLTSM_SOCKET_ZLIB)

    target_include_directories(LTSM_test_${DIR} PRIVATE ${CMAKE_SOURCE_DIR}/src/common)

    target_link_libraries(LTSM_test_${DIR} LTSM_common)
    target_link_libraries(LTSM_test_${DIR} ${GNUTLSXX_LIBRARY} ${GNUTLS_LIBRARIES})
    target_link_libraries(LTSM_test_${DIR} z)

    set_target_properties(LTSM_test_${DIR} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/../..)
endforeach()
