# LTSM session

find_package(Boost 1.78 COMPONENTS process)
target_sources(${CUR_TARGET} PRIVATE ltsm_display_session.cpp)

target_compile_options(${CUR_TARGET} PUBLIC "-Wno-narrowing")

if(Boost_process_FOUND)
    target_link_libraries(${CUR_TARGET} PRIVATE Boost::process)
endif()
