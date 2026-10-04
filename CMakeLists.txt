cmake_minimum_required(VERSION 3.21)

project(omarchy-calendar VERSION 1.0.0 LANGUAGES CXX)

include(GNUInstallDirs)
set(OMARCHY_CALENDAR_OAUTH_ENV_FILE "" CACHE FILEPATH
    "Optional production Google OAuth environment file to install")
set(OMARCHY_CALENDAR_HOMEPAGE_URL "https://lastrefuge.ai/projects/omarchy-calendar" CACHE STRING
    "Public product homepage used by the application")
set(OMARCHY_CALENDAR_PRIVACY_URL "https://lastrefuge.ai/privacy" CACHE STRING
    "Public privacy policy used by the application")
set(OMARCHY_CALENDAR_TERMS_URL "https://lastrefuge.ai/terms" CACHE STRING
    "Public terms of use used by the application")
set(OMARCHY_CALENDAR_SUPPORT_EMAIL "jason@greatspark.com" CACHE STRING
    "Public support email used by the application")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

# GoogleAuth uses the token and scope APIs introduced in Qt NetworkAuth 6.9.
find_package(Qt6 6.9 REQUIRED COMPONENTS Core DBus Gui Network NetworkAuth Qml Quick QuickControls2 Sql)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBSECRET REQUIRED IMPORTED_TARGET libsecret-1)

qt_standard_project_setup(REQUIRES 6.9)
# Arch packages install shared libraries in the system search path.
set(CMAKE_INSTALL_RPATH "")

qt_add_executable(omarchy-calendar
    app/eventstore.cpp
    app/eventstore.h
    app/main.cpp
    app/preferences.cpp
    app/preferences.h
    app/quickentryparser.cpp
    app/quickentryparser.h
    app/themeprovider.cpp
    app/themeprovider.h
    app/timezonehelper.cpp
    app/timezonehelper.h
    app/version.h
    app/resources/qml.qrc
)

target_compile_definitions(omarchy-calendar PRIVATE
    OMARCHY_CALENDAR_HOMEPAGE_URL="${OMARCHY_CALENDAR_HOMEPAGE_URL}"
    OMARCHY_CALENDAR_PRIVACY_URL="${OMARCHY_CALENDAR_PRIVACY_URL}"
    OMARCHY_CALENDAR_TERMS_URL="${OMARCHY_CALENDAR_TERMS_URL}"
    OMARCHY_CALENDAR_SUPPORT_EMAIL="${OMARCHY_CALENDAR_SUPPORT_EMAIL}"
)

target_link_libraries(omarchy-calendar PRIVATE
    Qt6::Core
    Qt6::DBus
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    Qt6::QuickControls2
)

qt_add_executable(omarchy-calendar-service
    service/calendarservice.cpp
    service/calendarservice.h
    service/database.cpp
    service/database.h
    service/googleauth.cpp
    service/googleauth.h
    service/googlemutations.cpp
    service/googlemutations.h
    service/googlesync.cpp
    service/googlesync.h
    service/main.cpp
    service/recurrence.cpp
    service/recurrence.h
    service/secretstore.cpp
    service/secretstore.h
    app/version.h
)

target_link_libraries(omarchy-calendar-service PRIVATE
    Qt6::Core
    Qt6::DBus
    Qt6::Network
    Qt6::NetworkAuth
    Qt6::Sql
    PkgConfig::LIBSECRET
)

install(TARGETS omarchy-calendar omarchy-calendar-service
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES packaging/desktop/org.omarchy.Calendar.desktop
    DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES packaging/icons/org.omarchy.Calendar.svg
    DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
install(FILES packaging/metainfo/org.omarchy.Calendar.metainfo.xml
    DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)
install(FILES packaging/dbus/org.omarchy.Calendar.service
    DESTINATION ${CMAKE_INSTALL_DATADIR}/dbus-1/services)
install(FILES packaging/systemd/omarchy-calendar.service
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/systemd/user)
install(FILES README.md CHANGELOG.md SECURITY.md
    DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/omarchy-calendar)
install(FILES LICENSE
    DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/omarchy-calendar)
if(OMARCHY_CALENDAR_OAUTH_ENV_FILE)
    install(FILES ${OMARCHY_CALENDAR_OAUTH_ENV_FILE}
        DESTINATION ${CMAKE_INSTALL_DATADIR}/omarchy-calendar
        RENAME google-oauth.env)
endif()
