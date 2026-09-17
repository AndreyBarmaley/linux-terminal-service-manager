# LTSM session

target_sources(${CUR_TARGET} PRIVATE ltsm_display_session.cpp)
target_compile_options(${CUR_TARGET} PUBLIC "-Wno-narrowing")

# asio::stream_file
# pkg_search_module(URING REQUIRED liburing)
# target_compile_definitions(${CUR_TARGET} PRIVATE BOOST_ASIO_HAS_IO_URING)

target_compile_definitions(${CUR_TARGET} PRIVATE BOOST_PROCESS_USE_STD_FS)

if(DEFINED Boost_ROOT)
    #set(Boost_USE_STATIC_LIBS ON)
    #set(Boost_USE_MULTITHREADED ON)
    #list(APPEND CMAKE_PREFIX_PATH "$ENV{LTSM_BOOST_ROOT}/stage/lib/cmake/Boost-1.88.0")
    #find_package(Boost REQUIRED CONFIG COMPONENTS process filesystem)
    #target_link_libraries(${CUR_TARGET} PRIVATE Boost::process Boost::filesystem)
else()
    find_package(Boost REQUIRED CONFIG)
    if(Boost_VERSION VERSION_GREATER "1.85.0")
        find_package(Boost REQUIRED CONFIG COMPONENTS process)
        if(Boost_process_FOUND)
            target_link_libraries(${CUR_TARGET} PRIVATE Boost::process)
        endif()
    endif()
endif()

target_include_directories(${CUR_TARGET} PRIVATE ${URING_INCLUDE_DIR})
target_link_libraries(${CUR_TARGET} PRIVATE ${URING_LIBRARIES})
