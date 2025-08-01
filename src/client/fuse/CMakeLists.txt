if(LTSM_CLIENT_FOLDER_REDIRECT)
    target_sources(${CUR_TARGET} PRIVATE ltsm_fuse_client.cpp)
    target_compile_definitions(${CUR_TARGET} PUBLIC -DLTSM_WITH_FUSE)
endif()
