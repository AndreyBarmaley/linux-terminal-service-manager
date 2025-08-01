# LTSM session_fuse

if(LTSM_SERVER_BUILD_SESSION_FUSE)
    pkg_search_module(FUSE3 REQUIRED fuse3)

    target_sources(${CUR_TARGET} PRIVATE ltsm_fuse_session.cpp)
    target_compile_options(${CUR_TARGET} PRIVATE ${FUSE3_CFLAGS})
    target_include_directories(${CUR_TARGET} PRIVATE ${FUSE3_INCLUDE_DIR})
    target_link_libraries(${CUR_TARGET} PRIVATE ${FUSE3_LIBRARIES})
endif()
