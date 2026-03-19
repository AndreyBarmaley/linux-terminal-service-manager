# LTSM helper
target_sources(${CUR_TARGET} PRIVATE main.cpp ltsm_helperwindow.cpp
        ltsm_helperwindow.ui ltsm_helper.qrc)

target_link_libraries(${CUR_TARGET} PRIVATE Qt5::Core Qt5::Gui Qt5::Widgets Qt5::Network)
target_compile_definitions(${CUR_TARGET} PRIVATE -DQT_NO_KEYWORDS)

if(LTSM_WITH_PKCS11)
    target_sources(${CUR_TARGET} PRIVATE ltsm_pkcs11_session.cpp)
endif()
