cmake_minimum_required(VERSION 3.13)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -ggdb3 -O0 -Wall -Werror -Wno-sign-compare -Wno-unused-function -Wno-unused-variable -DLTSM_DEBUG")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O3 -Wall -Wno-sign-compare -Wno-unused-function -Wno-unused-variable")

option(LTSM_BUILD_STD_MAP "usage std::unordered_map" OFF)
option(LTSM_BUILD_XCB_ERRORS "usage xcb_errors" OFF)
option(LTSM_BUILD_XKB_COMMON "usage xkb layouts" OFF)
option(LTSM_BUILD_X11VNC "build x11vnc utility" OFF)
option(LTSM_BUILD_VNC2IMAGE "build vnc2image utility" OFF)
option(LTSM_BUILD_TESTS "build test utilities" OFF)
option(LTSM_BUILD_COVERAGE_TESTS "sonarcloud coverage tests" OFF)

if(LTSM_BUILD_COVERAGE_TESTS)
    set(CMAKE_CXX_FLAGS " ${CMAKE_CXX_FLAGS} --coverage -DLTSM_COVERAGE_TEST")
    set(LTSM_BUILD_TESTS ON CACHE BOOL "build test utilities" FORCE)
    set(LTSM_BUILD_X11VNC ON CACHE BOOL "build x11vnc utility" FORCE)
    #set(LTSM_BUILD_VNC2IMAGE ON CACHE BOOL "build vnc2image utility" FORCE)
endif()

include(FindPkgConfig)
set(CMAKE_FIND_FRAMEWORK LAST)

# detect 64 arch
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(LTSM_BUILD_STD_MAP ON CACHE BOOL "enable usage std::unordered_map" FORCE)
endif()

# LTSM common lib
project(LTSM_common VERSION 20220830.1)

pkg_search_module(SDL2 REQUIRED sdl2>=2.0.1)
pkg_search_module(SDL2_IMAGE REQUIRED SDL2_image>=2.0.1)
pkg_search_module(XCB REQUIRED xcb)
pkg_search_module(XCB_SHM REQUIRED xcb-shm)
pkg_search_module(XCB_DAMAGE REQUIRED xcb-damage)
pkg_search_module(XCB_XFIXES REQUIRED xcb-xfixes)
pkg_search_module(XCB_XTEST REQUIRED xcb-xtest)
pkg_search_module(XCB_RANDR REQUIRED xcb-randr)

add_compile_options(${XCB_CFLAGS})
add_compile_options(${XCB_SHM_CFLAGS})
add_compile_options(${XCB_XFIXES_CFLAGS})
add_compile_options(${XCB_XTEST_CFLAGS})
add_compile_options(${XCB_RANDR_CFLAGS})
add_compile_options(-DJSMN_STATIC)

if(LTSM_BUILD_STD_MAP)
    add_compile_options(-DLTSM_BUILD_STD_MAP)
endif()


if(LTSM_BUILD_XCB_ERRORS)
    add_compile_options(-DLTSM_BUILD_XCB_ERRORS)
endif()

add_library(LTSM_common STATIC)
add_subdirectory(src/common)

if(LTSM_BUILD_XKB_COMMON)
    pkg_search_module(XCB_XKB REQUIRED xcb-xkb)
    pkg_search_module(XKBCOMMON_X11 REQUIRED xkbcommon-x11)
    add_compile_options(${XCB_XKB_CFLAGS})
    add_compile_options(${XKBCOMMON_X11_CFLAGS})
    add_compile_options(-DLTSM_WITH_XKBCOMMON)
    link_libraries(${XCB_XKB_LIBRARIES})
    link_libraries(${XKBCOMMON_X11_LIBRARIES})
endif()

link_libraries(${XCB_LIBRARIES})
link_libraries(${XCB_SHM_LIBRARIES})
link_libraries(${XCB_DAMAGE_LIBRARIES})
link_libraries(${XCB_XFIXES_LIBRARIES})
link_libraries(${XCB_XTEST_LIBRARIES})
link_libraries(${XCB_RANDR_LIBRARIES})
link_libraries(pthread)
link_libraries(stdc++fs)

# LTSM service
project(LTSM_service VERSION 20220830.1)

find_package(sdbus-c++ REQUIRED)
find_library(PAM_LIBRARIES NAMES pam)
find_path(PAM_INCLUDE_PATH NAMES security/pam_appl.h pam_appl.h)

add_executable(LTSM_service src/service/ltsm_service.cpp)
target_sources(LTSM_service PUBLIC ${LTSM_common})

target_include_directories(LTSM_service PUBLIC src/common)
target_include_directories(LTSM_service PUBLIC src/dbus-glue)
target_include_directories(LTSM_service PUBLIC ${PAM_INCLUDE_PATH})

target_link_libraries(LTSM_service LTSM_common)
target_link_libraries(LTSM_service SDBusCpp::sdbus-c++)
target_link_libraries(LTSM_service ${PAM_LIBRARIES})

set_target_properties(LTSM_service PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

# LTSM connector
project(LTSM_connector VERSION 20220830.1)

option(LTSM_WITH_RDP "add rdp protocol support" ON)
option(LTSM_WITH_SPICE "add spice protocol support" OFF)

find_package(sdbus-c++ REQUIRED)
find_package(GnuTLS REQUIRED)

add_executable(LTSM_connector
    src/connector/ltsm_connector.cpp
    src/connector/ltsm_vnc_encodings.cpp
    src/connector/ltsm_connector_vnc.cpp)

if(LTSM_WITH_RDP)
    pkg_search_module(FreeRDP2 REQUIRED freerdp2)
    pkg_search_module(WinPR2 REQUIRED winpr2)
    target_compile_options(LTSM_connector PUBLIC ${FreeRDP2_CFLAGS} ${WinPR2_CFLAGS})
    target_link_libraries(LTSM_connector ${FreeRDP2_LIBRARIES} ${WinPR2_LIBRARIES})
    target_compile_options(LTSM_connector PUBLIC -DLTSM_WITH_RDP)
    target_sources(LTSM_connector PUBLIC src/connector/ltsm_connector_rdp.cpp)
endif()

if(LTSM_WITH_SPICE)
    pkg_search_module(SpiceServer REQUIRED spice-server)
    target_compile_options(LTSM_connector PUBLIC ${SpiceServer_CFLAGS})
    target_compile_options(LTSM_connector PUBLIC -DLTSM_WITH_SPICE)
    target_link_libraries(LTSM_connector ${SpiceServer_LIBRARIES})
    target_sources(LTSM_connector PUBLIC src/connector/ltsm_connector_spice.cpp)
endif()

target_sources(LTSM_connector PUBLIC ${LTSM_common})
target_compile_options(LTSM_connector PUBLIC -DLTSM_SOCKET_TLS)
target_compile_options(LTSM_connector PUBLIC -DLTSM_SOCKET_ZLIB)
target_compile_options(LTSM_connector PUBLIC -DLTSM_WITH_SDBUS)
target_compile_options(LTSM_connector PUBLIC ${GNUTLS_DEFINITIONS})

target_include_directories(LTSM_connector PUBLIC src/common)
target_include_directories(LTSM_connector PUBLIC src/dbus-glue)
target_include_directories(LTSM_connector PUBLIC ${GNUTLS_INCLUDE_DIR})

target_link_libraries(LTSM_connector LTSM_common)
target_link_libraries(LTSM_connector SDBusCpp::sdbus-c++)
target_link_libraries(LTSM_connector ${GNUTLS_LIBRARIES})
target_link_libraries(LTSM_connector z)

set_target_properties(LTSM_connector PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

# LTSM sdl2x11
project(LTSM_sdl2x11 VERSION 20220830.1)
add_executable(LTSM_sdl2x11 src/sdl2x11/ltsm_sdl2x11.cpp src/sdl2x11/ltsm_sdl_wrapper.cpp)

target_compile_options(LTSM_sdl2x11 PUBLIC ${SDL2_CFLAGS})
target_compile_options(LTSM_sdl2x11 PUBLIC ${SDL2_IMAGE_CFLAGS})

target_include_directories(LTSM_sdl2x11 PUBLIC src/common)

target_link_libraries(LTSM_sdl2x11 LTSM_common)
target_link_libraries(LTSM_sdl2x11 ${SDL2_LIBRARIES})
target_link_libraries(LTSM_sdl2x11 ${SDL2_IMAGE_LIBRARIES})

set_target_properties(LTSM_sdl2x11 PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

# LTSM helper
project(LTSM_helper VERSION 20220830.1)

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

# LTSM sessions
project(LTSM_sessions VERSION 20220830.1)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt5 COMPONENTS Core Gui DBus Widgets REQUIRED)

add_executable(LTSM_sessions
    src/qt_sessions/main.cpp
    src/qt_sessions/ltsm_sessions.ui
    src/qt_sessions/ltsm_sessions.cpp
    src/qt_sessions/ltsm_sessions.qrc)

target_link_libraries(LTSM_sessions Qt5::Core Qt5::Gui Qt5::DBus Qt5::Widgets)
set_target_properties(LTSM_sessions PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

if(LTSM_BUILD_X11VNC)
    # LTSM x11vnc
    project(LTSM_x11vnc VERSION 20220830.1)

    add_executable(LTSM_x11vnc
        src/x11vnc/ltsm_vnc_encodings.cpp
        src/x11vnc/ltsm_connector_vnc.cpp
        src/x11vnc/ltsm_x11vnc.cpp)

    target_sources(LTSM_x11vnc PUBLIC ${LTSM_common})
    target_compile_options(LTSM_x11vnc PUBLIC -DLTSM_SOCKET_TLS)
    target_compile_options(LTSM_x11vnc PUBLIC -DLTSM_SOCKET_ZLIB)
    target_compile_options(LTSM_x11vnc PUBLIC ${GNUTLS_DEFINITIONS})

    target_include_directories(LTSM_x11vnc PUBLIC src/common)
    target_include_directories(LTSM_x11vnc PUBLIC ${GNUTLS_INCLUDE_DIR})
    target_include_directories(LTSM_x11vnc PUBLIC src/x11vnc)
    target_include_directories(LTSM_x11vnc PUBLIC src/connector)

    target_link_libraries(LTSM_x11vnc LTSM_common)
    target_link_libraries(LTSM_x11vnc ${GNUTLS_LIBRARIES})
    target_link_libraries(LTSM_x11vnc z)

    set_target_properties(LTSM_x11vnc PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
endif()

if(LTSM_BUILD_VNC2IMAGE)
    # LTSM vnc2image
    project(LTSM_vnc2image VERSION 20220830.1)

    add_executable(LTSM_vnc2image
        src/vnc2image/ltsm_vnc_encodings.cpp
        src/vnc2image/ltsm_connector_vnc.cpp
        src/vnc2image/ltsm_vnc2image.cpp)

    target_sources(LTSM_vnc2image PUBLIC ${LTSM_common})
    target_compile_options(LTSM_vnc2image PUBLIC -DLTSM_SOCKET_TLS)
    target_compile_options(LTSM_vnc2image PUBLIC -DLTSM_SOCKET_ZLIB)
    target_compile_options(LTSM_vnc2image PUBLIC ${GNUTLS_DEFINITIONS})

    target_include_directories(LTSM_vnc2image PUBLIC src/common)
    target_include_directories(LTSM_vnc2image PUBLIC ${GNUTLS_INCLUDE_DIR})
    target_include_directories(LTSM_vnc2image PUBLIC src/dbus-glue)
    target_include_directories(LTSM_vnc2image PUBLIC src/vnc2image)
    target_include_directories(LTSM_vnc2image PUBLIC src/connector)

    target_link_libraries(LTSM_vnc2image LTSM_common)
    target_link_libraries(LTSM_vnc2image ${GNUTLS_LIBRARIES})
    target_link_libraries(LTSM_vnc2image z)

    set_target_properties(LTSM_vnc2image PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
endif()

if(LTSM_BUILD_TESTS)
    add_subdirectory(src/tests)
endif()
