target_sources(${CUR_TARGET} PRIVATE ltsm_vnc2image.cpp)

# boost + openssl warnings
target_compile_options(${CUR_TARGET} PRIVATE -Wno-deprecated-declarations)

find_package(OpenSSL 3.0)
if(OpenSSL_FOUND)
    target_link_libraries(${CUR_TARGET} PRIVATE OpenSSL::Crypto OpenSSL::SSL)
else()
    pkg_check_modules(OPENSSL REQUIRED libssl3 libcrypto3)
    target_link_directories(${CUR_TARGET} PRIVATE ${OPENSSL_LIBRARY_DIRS})
    target_link_libraries(${CUR_TARGET} PRIVATE ${OPENSSL_LIBRARIES})
endif()
