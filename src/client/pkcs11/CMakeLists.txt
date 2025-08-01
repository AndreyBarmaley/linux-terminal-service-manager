if(NOT MINGW)
    target_sources(${CUR_TARGET} PRIVATE ltsm_pkcs11_client.cpp)
endif()
