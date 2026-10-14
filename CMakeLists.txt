cmake_minimum_required(VERSION 3.20)

project(Vela VERSION 0.7.6 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(ECM 6.0 REQUIRED NO_MODULE)
set(CMAKE_MODULE_PATH ${ECM_MODULE_PATH})

include(KDEInstallDirs)
include(KDECMakeSettings)
include(KDECompilerSettings NO_POLICY_SCOPE)
include(FeatureSummary)

find_package(Qt6 REQUIRED COMPONENTS Core Widgets Concurrent DBus)

find_package(KF6 REQUIRED COMPONENTS
    CoreAddons
    WidgetsAddons
    KIO
    Bookmarks
    Config
    ConfigWidgets
    XmlGui
    I18n
    GuiAddons
    Solid
    GlobalAccel
    ColorScheme
    JobWidgets
    SyntaxHighlighting
    Service
    Parts           # embedded terminal (Konsole's KPart, loaded at runtime)
)
find_package(Poppler REQUIRED COMPONENTS Qt6)


configure_file(src/config.h.in ${CMAKE_CURRENT_BINARY_DIR}/src/config.h)
include_directories(${CMAKE_CURRENT_BINARY_DIR}/src)

add_subdirectory(src)
add_subdirectory(po)


install(FILES resources/vela.desktop DESTINATION ${KDE_INSTALL_APPDIR})
install(FILES resources/icons/vela.png DESTINATION ${KDE_INSTALL_ICONDIR}/hicolor/256x256/apps OPTIONAL)

# D-Bus activation. Without an org.freedesktop.FileManager1 service file, a
# "show in file manager" request made while Vela is closed activates whichever
# other file manager owns that name (Dolphin ships one) — and that process then
# holds the name for the rest of the session, so Vela can never claim it again.
configure_file(resources/org.kde.vela.service.in
               ${CMAKE_CURRENT_BINARY_DIR}/org.kde.vela.service @ONLY)
configure_file(resources/org.freedesktop.FileManager1.service.in
               ${CMAKE_CURRENT_BINARY_DIR}/org.freedesktop.FileManager1.service @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/org.kde.vela.service
        DESTINATION ${KDE_INSTALL_DBUSSERVICEDIR})
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/org.freedesktop.FileManager1.service
        DESTINATION ${KDE_INSTALL_DBUSSERVICEDIR})

feature_summary(WHAT ALL INCLUDE_QUIET_PACKAGES FATAL_ON_MISSING_REQUIRED_PACKAGES)
