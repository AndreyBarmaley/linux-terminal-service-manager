# LTSM sessions
project(LTSM_sessions)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt5 COMPONENTS Core Gui DBus Widgets REQUIRED)
        
add_executable(LTSM_sessions
        main.cpp ltsm_sessions.cpp
        ltsm_sessions.ui ltsm_sessions.qrc)

target_link_libraries(LTSM_sessions Qt5::Core Qt5::Gui Qt5::DBus Qt5::Widgets)
set_target_properties(LTSM_sessions PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/../../..)
