cmake_minimum_required(VERSION 3.13)

target_sources(LTSM_common PUBLIC ltsm_application.cpp)
target_sources(LTSM_common PUBLIC ltsm_json_wrapper.cpp)
target_sources(LTSM_common PUBLIC ltsm_tools.cpp)
target_sources(LTSM_common PUBLIC ltsm_streambuf.cpp)
target_sources(LTSM_common PUBLIC ltsm_xcb_wrapper.cpp)

if(LTSM_BUILD_XCB_ERRORS)
    target_sources(LTSM_common PUBLIC libxcb-errors/xcb_errors.c libxcb-errors/extensions.c)
endif()
