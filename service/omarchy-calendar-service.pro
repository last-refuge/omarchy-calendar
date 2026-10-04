QT += core dbus network networkauth sql
!versionAtLeast(QT_VERSION, 6.9.0): error("Omarchy Calendar requires Qt 6.9 or later")
QT -= gui
CONFIG += console c++20
CONFIG -= app_bundle
DEFINES += OMARCHY_CALENDAR_VERSION=\\\"1.0.0\\\"

TARGET = omarchy-calendar-service
SOURCES += \
    calendarservice.cpp \
    database.cpp \
    recurrence.cpp \
    googleauth.cpp \
    googlemutations.cpp \
    googlesync.cpp \
    main.cpp

HEADERS += \
    calendarservice.h \
    database.h \
    recurrence.h \
    googleauth.h \
    googlemutations.h \
    googlesync.h \
    secretstore.h

SOURCES += secretstore.cpp
CONFIG += link_pkgconfig
PKGCONFIG += libsecret-1
