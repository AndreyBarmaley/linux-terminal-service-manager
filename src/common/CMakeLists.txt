if(APPLE)
    target_include_directories(${CUR_TARGET} PRIVATE mingw)
elseif(UNIX)
    #find_library(XKBFILE_LIBRARY NAMES xkbfile libxkbfile REQUIRED)
    #find_library(X11_LIBRARY NAMES X11 libX11 REQUIRED)
    #target_link_libraries(${CUR_TARGET} PUBLIC ${XKBFILE_LIBRARY} ${X11_LIBRARY})
elseif(MINGW)
    target_include_directories(${CUR_TARGET} PUBLIC mingw)
    target_link_libraries(${CUR_TARGET} PUBLIC wsock32 winscard)
endif()

target_compile_definitions(${CUR_TARGET} PUBLIC -DJSMN_STATIC)

target_sources(${CUR_TARGET} PRIVATE
    ltsm_application.cpp
    ltsm_tools.cpp
    ltsm_streambuf.cpp
    ltsm_sockets.cpp
    ltsm_framebuffer.cpp
    ltsm_xcb_types.cpp)

if(LTSM_WITH_FB_PARALLELS)
    target_compile_definitions(${CUR_TARGET} PUBLIC -DLTSM_WITH_FB_PARALLELS)
endif()

if(LTSM_WITH_STD_MAP)
    target_compile_definitions(${CUR_TARGET} PUBLIC -DLTSM_WITH_STD_MAP)
endif()

if(LTSM_WITH_JSON)
    target_compile_definitions(${CUR_TARGET} PUBLIC -DLTSM_WITH_JSON)
    target_sources(${CUR_TARGET} PRIVATE ltsm_json_wrapper.cpp)
endif()

if(LTSM_WITH_PNG)
    find_package(PNG)
    if(PNG_FOUND)
        target_compile_definitions(${CUR_TARGET} PUBLIC -DLTSM_WITH_PNG)
        target_compile_definitions(${CUR_TARGET} PRIVATE ${PNG_DEFINITIONS})
        target_include_directories(${CUR_TARGET} PRIVATE ${PNG_INCLUDE_DIR})
        target_link_libraries(${CUR_TARGET} PRIVATE ${PNG_LIBRARIES})
    else()
        set(LTSM_WITH_PNG OFF)
        message(STATUS "PNG not found")
    endif()
endif()

if(LTSM_WITH_GNUTLS)
    find_package(GnuTLS REQUIRED)
    if(GnuTLS_FOUND)
        target_compile_definitions(${CUR_TARGET} PUBLIC -DLTSM_WITH_GNUTLS)
        target_compile_definitions(${CUR_TARGET} PRIVATE ${GNUTLS_DEFINITIONS})
        target_include_directories(${CUR_TARGET} PRIVATE ${GNUTLS_INCLUDE_DIR})
        target_link_libraries(${CUR_TARGET} PRIVATE ${GNUTLS_LIBRARIES})
    else()
        set(LTSM_WITH_GNUTLS OFF)
        message(STATUS "GnuTLS not found")
    endif()
endif()

if(LTSM_WITH_GSSAPI)
    pkg_check_modules(GSSAPI krb5-gssapi)
    if(GSSAPI_FOUND)
        target_sources(${CUR_TARGET} PRIVATE ltsm_gsslayer.cpp)
        target_compile_definitions(${CUR_TARGET} PUBLIC -DLTSM_WITH_GSSAPI)
        target_compile_definitions(${CUR_TARGET} PRIVATE ${GSSAPI_DEFINITIONS})
        target_include_directories(${CUR_TARGET} PRIVATE ${GSSAPI_INCLUDE_DIR})
        target_link_libraries(${CUR_TARGET} PRIVATE ${GSSAPI_LIBRARIES})
    else()
        set(LTSM_WITH_GSSAPI OFF)
        message(STATUS "GssApi not found")
    endif()
endif()

if(LTSM_WITH_PKCS11)
    pkg_check_modules(P11KIT p11-kit-1)
    if(P11KIT_FOUND)
        target_sources(${CUR_TARGET} PRIVATE ltsm_pkcs11_wrapper.cpp)
        target_compile_options(${CUR_TARGET} PRIVATE ${P11KIT_CFLAGS})
        target_compile_definitions(${CUR_TARGET} PUBLIC -DLTSM_PKCS11_AUTH)
        target_include_directories(${CUR_TARGET} PRIVATE ${P11KIT_INCLUDE_DIRS})
        target_link_directories(${CUR_TARGET} PRIVATE ${P11KIT_LIBRARY_DIRS})
        target_link_libraries(${CUR_TARGET} PRIVATE ${P11KIT_LIBRARIES})
        if(UNIX)
            target_link_libraries(${CUR_TARGET} PRIVATE dl)
        endif()
    else()
        set(LTSM_WITH_PKCS11 OFF)
        message(STATUS "P11KIT not found")
    endif()
endif()

if(LTSM_WITH_SDL)
    pkg_check_modules(SDL2 REQUIRED sdl2>=2.0.1)
    if(SDL2_FOUND)
        target_sources(${CUR_TARGET} PRIVATE ltsm_sdl_wrapper.cpp)
        target_compile_options(${CUR_TARGET} PRIVATE ${SDL2_CFLAGS})
        target_compile_definitions(${CUR_TARGET} PUBLIC -DLTSM_WITH_SDL)
        target_link_directories(${CUR_TARGET} PRIVATE ${SDL2_LIBRARY_DIRS})
        target_link_libraries(${CUR_TARGET} PRIVATE ${SDL2_LIBRARIES})
    else()
        set(LTSM_WITH_SDL OFF)
        message(STATUS "SDL2 not found")
    endif()
endif()

if(LTSM_FORCE_SYSLOG)
    target_compile_definitions(${CUR_TARGET} PUBLIC -DLTSM_FORCE_SYSLOG)
endif()

if(LTSM_WITH_SYSTEMD)
    if(UNIX AND NOT APPLE)
        pkg_check_modules(SYSTEMD libsystemd)

        if(SYSTEMD_FOUND)
            target_compile_definitions(${CUR_TARGET} PUBLIC -DLTSM_WITH_SYSTEMD)
            target_compile_definitions(${CUR_TARGET} PRIVATE ${SYSTEMD_DEFINITIONS})
            target_include_directories(${CUR_TARGET} PRIVATE ${SYSTEMD_INCLUDE_DIR})
            target_link_libraries(${CUR_TARGET} PRIVATE ${SYSTEMD_LIBRARIES})
        else()
            set(LTSM_WITH_SYSTEMD OFF)
            message(STATUS "SystemD not found")
        endif()
    else()
        set(LTSM_WITH_SYSTEMD OFF)
    endif()
endif()

if(APPLE)
    # skip xcb
elseif(UNIX)
    pkg_check_modules(XCB_ALL REQUIRED xcb xcb-shm xcb-damage xcb-xfixes xcb-xtest xcb-randr)
    pkg_check_modules(XCB_XKB REQUIRED xcb-xkb xkbcommon-x11)

    target_compile_options(${CUR_TARGET} PRIVATE ${XCB_ALL_CFLAGS} ${XCB_XKB_CFLAGS})

    target_include_directories(${CUR_TARGET} PRIVATE ${XCB_ALL_INCLUDE_DIR} ${XCB_XKB_INCLUDE_DIR})
    target_link_libraries(${CUR_TARGET} PUBLIC ${XCB_ALL_LIBRARIES} ${XCB_XKB_LIBRARIES})
    target_link_libraries(${CUR_TARGET} PUBLIC pthread stdc++fs)

    target_sources(${CUR_TARGET} PRIVATE ltsm_xcb_wrapper.cpp)

    if(LTSM_WITH_XCB_ERRORS)
        target_compile_definitions(${CUR_TARGET} PUBLIC -DLTSM_BUILD_XCB_ERRORS)
        target_sources(${CUR_TARGET} PRIVATE libxcb-errors/xcb_errors.c libxcb-errors/extensions.c)
    else()
        set(LTSM_BUILD_XCB_ERRORS OFF)
    endif()

elseif(MINGW)
    target_compile_options(${CUR_TARGET} PRIVATE -fpermissive)
    target_link_libraries(${CUR_TARGET} PUBLIC pthread stdc++fs)
endif()

target_link_libraries(${CUR_TARGET} PUBLIC z)
