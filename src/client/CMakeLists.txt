target_sources(${CUR_TARGET} PRIVATE ltsm_client.cpp)
target_compile_definitions(${CUR_TARGET} PRIVATE LTSM_WITH_BOOST LTSM_WITH_OPENSSL)

# boost + openssl warnings
target_compile_options(${CUR_TARGET} PRIVATE -Wno-deprecated-declarations)

find_package(OpenSSL REQUIRED)
target_link_libraries(${CUR_TARGET} PRIVATE OpenSSL::Crypto OpenSSL::SSL)

if(MINGW)
    target_link_libraries(${CUR_TARGET} PRIVATE wsock32 ws2_32 winscard)
endif()

add_subdirectory(pcsc ${CUR_TARGET}_pcsc)
add_subdirectory(fuse ${CUR_TARGET}_fuse)
add_subdirectory(audio ${CUR_TARGET}_audio)
add_subdirectory(pkcs11 ${CUR_TARGET}_pkcs11)

if(APPLE)
    set_target_properties(${CUR_TARGET} PROPERTIES
        MACOS_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "LtsmClientApp"
        MACOSX_BUNDLE_GUI_IDENTIFIER "ru.nice-soft.ltsm"
        MACOSX_BUNDLE_BUNDLE_VERSION "1.0.0"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "1.0"
    )

    set(APP_PATH "${CMAKE_BINARY_DIR}/src/${CUR_TARGET}.app")
    install(TARGETS ${CUR_TARGET} DESTINATION .)

    set(CPACK_GENERATOR "DragNDrop")
    set(CPACK_PACKAGE_NAME "LtsmClientApp")
    set(CPACK_PACKAGE_VENDOR "nice-soft.ru")
    set(CPACK_PACKAGE_VERSION "1.0.0")
    set(CPACK_DMG_VOLUME_NAME "LtsmClientApp Installer")

    install(CODE "
        include(BundleUtilities)
        fixup_bundle(${APP_PATH} \"\" \"/opt/homebrew/lib;/usr/local/lib\")
        " COMPONENT Runtime)

    include(CPack)
endif()
