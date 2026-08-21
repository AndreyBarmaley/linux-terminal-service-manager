# LTSM session

find_package(Boost 1.78 COMPONENTS process)
target_sources(${CUR_TARGET} PRIVATE ltsm_display_session.cpp)

target_compile_options(${CUR_TARGET} PUBLIC "-Wno-narrowing")

# asio::stream_file
# pkg_search_module(URING REQUIRED liburing)
# target_compile_definitions(${CUR_TARGET} PRIVATE BOOST_ASIO_HAS_IO_URING)

if(Boost_process_FOUND)
    target_link_libraries(${CUR_TARGET} PRIVATE Boost::process)
endif()

target_include_directories(${CUR_TARGET} PRIVATE ${URING_INCLUDE_DIR})
target_link_libraries(${CUR_TARGET} PRIVATE ${URING_LIBRARIES})
