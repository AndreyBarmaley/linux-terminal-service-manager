# LTSM session

target_sources(${CUR_TARGET} PRIVATE ltsm_display_session.cpp)
target_compile_options(${CUR_TARGET} PUBLIC "-Wno-narrowing")

# asio::stream_file
# pkg_search_module(URING REQUIRED liburing)
# target_compile_definitions(${CUR_TARGET} PRIVATE BOOST_ASIO_HAS_IO_URING)

find_package(Boost REQUIRED CONFIG)
target_compile_definitions(${CUR_TARGET} PRIVATE BOOST_PROCESS_USE_STD_FS)

if(Boost_VERSION VERSION_GREATER "1.85.0")
    find_package(Boost REQUIRED CONFIG COMPONENTS process)
    if(Boost_process_FOUND)
        target_link_libraries(${CUR_TARGET} PRIVATE Boost::process)
    endif()
endif()

target_include_directories(${CUR_TARGET} PRIVATE ${URING_INCLUDE_DIR})
target_link_libraries(${CUR_TARGET} PRIVATE ${URING_LIBRARIES})
