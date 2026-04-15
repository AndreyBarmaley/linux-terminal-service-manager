target_sources(${CUR_TARGET} PRIVATE ltsm_vnc2image.cpp)
target_compile_definitions(${CUR_TARGET} PRIVATE LTSM_WITH_BOOST)

# boost + openssl warnings
target_compile_options(${CUR_TARGET} PRIVATE -Wno-deprecated-declarations)

find_package(OpenSSL REQUIRED)
target_link_libraries(${CUR_TARGET} PUBLIC OpenSSL::Crypto OpenSSL::SSL)
