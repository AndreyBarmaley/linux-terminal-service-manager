cmake_minimum_required(VERSION 3.14)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -ggdb3 -O0 -Wall -Werror -Wno-sign-compare -Wno-unused-function -Wno-unused-variable -DLTSM_DEBUG")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O3 -Wall -Wno-sign-compare -Wno-unused-function -Wno-unused-variable")

option(LTSM_BUILD_STD_MAP "usage std::unordered_map" OFF)

include(FindPkgConfig)
set(CMAKE_FIND_FRAMEWORK LAST)

# LTSM common lib
project(LTSM_common VERSION 20210301.1)

pkg_search_module(SDL2 REQUIRED sdl2>=2.0.1)
pkg_search_module(SDL2_IMAGE REQUIRED SDL2_image>=2.0.1)
pkg_search_module(XCB REQUIRED xcb)
pkg_search_module(XCB_SHM REQUIRED xcb-shm)
pkg_search_module(XCB_DAMAGE REQUIRED xcb-damage)
pkg_search_module(XCB_XFIXES REQUIRED xcb-xfixes)
pkg_search_module(XCB_XTEST REQUIRED xcb-xtest)
pkg_search_module(XCB_KEYSYMS REQUIRED xcb-keysyms)

add_compile_options(${XCB_CFLAGS})
add_compile_options(${XCB_SHM_CFLAGS})
add_compile_options(${XCB_XFIXES_CFLAGS})
add_compile_options(${XCB_XTEST_CFLAGS})
add_compile_options(${XCB_KEYSYMS_CFLAGS})
add_compile_options("-DJSMN_STATIC")

if(LTSM_BUILD_STD_MAP)
    add_compile_options("-DLTSM_BUILD_STD_MAP")
endif()

add_library(LTSM_common STATIC)
add_subdirectory(src/common)

link_libraries(${XCB_LIBRARIES})
link_libraries(${XCB_SHM_LIBRARIES})
link_libraries(${XCB_DAMAGE_LIBRARIES})
link_libraries(${XCB_XFIXES_LIBRARIES})
link_libraries(${XCB_XTEST_LIBRARIES})
link_libraries(${XCB_KEYSYMS_LIBRARIES})
link_libraries(pthread)

# LTSM service
project(LTSM_service VERSION 20210301.1)

find_package(sdbus-c++ REQUIRED)
find_library(PAM_LIBRARIES NAMES pam)
find_path(PAM_INCLUDE_PATH NAMES security/pam_appl.h pam_appl.h)

add_executable(LTSM_service src/service/ltsm_service.cpp)
target_sources(LTSM_service PUBLIC ${LTSM_common})

target_include_directories(LTSM_service PUBLIC src/common)
target_include_directories(LTSM_service PUBLIC src/dbus-glue)
target_include_directories(LTSM_service PUBLIC PAM_INCLUDE_PATH)

target_link_libraries(LTSM_service LTSM_common)
target_link_libraries(LTSM_service SDBusCpp::sdbus-c++)
target_link_libraries(LTSM_service ${PAM_LIBRARIES})

set_target_properties(LTSM_service PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

# LTSM connector
project(LTSM_connector VERSION 20210301.1)

find_package(sdbus-c++ REQUIRED)

add_executable(LTSM_connector
    src/connector/ltsm_connector_rdp.cpp
    src/connector/ltsm_vnc_encodings.cpp
    src/connector/ltsm_connector_vnc.cpp
    src/connector/ltsm_connector.cpp)

target_sources(LTSM_connector PUBLIC ${LTSM_common})

target_include_directories(LTSM_connector PUBLIC src/common)
target_include_directories(LTSM_connector PUBLIC src/dbus-glue)

target_link_libraries(LTSM_connector LTSM_common)
target_link_libraries(LTSM_connector SDBusCpp::sdbus-c++)
target_link_libraries(LTSM_connector deflate)

set_target_properties(LTSM_connector PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

# LTSM sdl2x11
project(LTSM_sdl2x11 VERSION 20210301.1)
add_executable(LTSM_sdl2x11 src/sdl2x11/ltsm_sdl2x11.cpp src/sdl2x11/ltsm_sdl_wrapper.cpp)

target_compile_options(LTSM_sdl2x11 PUBLIC ${SDL2_CFLAGS})
target_compile_options(LTSM_sdl2x11 PUBLIC ${SDL2_IMAGE_CFLAGS})

target_include_directories(LTSM_sdl2x11 PUBLIC src/common)

target_link_libraries(LTSM_sdl2x11 LTSM_common)
target_link_libraries(LTSM_sdl2x11 ${SDL2_LIBRARIES})
target_link_libraries(LTSM_sdl2x11 ${SDL2_IMAGE_LIBRARIES})

set_target_properties(LTSM_sdl2x11 PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

# LTSM helper
project(LTSM_helper VERSION 20210301.1)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt5 COMPONENTS Core Gui DBus Widgets REQUIRED)

add_executable(LTSM_helper
    src/qt_helper/main.cpp
    src/qt_helper/ltsm_helperwindow.ui
    src/qt_helper/ltsm_helperwindow.cpp
    src/qt_helper/ltsm_helper.qrc)

target_link_libraries(LTSM_helper Qt5::Core Qt5::Gui Qt5::DBus Qt5::Widgets)
set_target_properties(LTSM_helper PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
