cmake_minimum_required(VERSION 3.14)

target_sources(LTSM_common PUBLIC ltsm_application.cpp)
target_sources(LTSM_common PUBLIC ltsm_json_wrapper.cpp)
target_sources(LTSM_common PUBLIC ltsm_tools.cpp)
target_sources(LTSM_common PUBLIC ltsm_xcb_wrapper.cpp)
target_sources(LTSM_common PUBLIC libxcb-errors/xcb_errors.c libxcb-errors/extensions.c)
