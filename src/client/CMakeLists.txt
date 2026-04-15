target_sources(${CUR_TARGET} PRIVATE ltsm_client.cpp)
target_compile_definitions(${CUR_TARGET} PRIVATE LTSM_WITH_BOOST)

# boost + openssl warnings
target_compile_options(${CUR_TARGET} PRIVATE -Wno-deprecated-declarations)

find_package(OpenSSL REQUIRED)
target_link_libraries(${CUR_TARGET} PRIVATE OpenSSL::Crypto OpenSSL::SSL)

if(MINGW)
    target_link_libraries(${CUR_TARGET} PRIVATE wsock32 ws2_32 winscard)
endif()

add_subdirectory(pcsc ${CUR_TARGET}_pcsc)
add_subdirectory(fuse ${CUR_TARGET}_fuse)
add_subdirectory(audio ${CUR_TARGET}_audio)
add_subdirectory(pkcs11 ${CUR_TARGET}_pkcs11)
