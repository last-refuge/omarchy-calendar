QT += core dbus gui qml quick quickcontrols2
!versionAtLeast(QT_VERSION, 6.9.0): error("Omarchy Calendar requires Qt 6.9 or later")
CONFIG += c++20
DEFINES += OMARCHY_CALENDAR_VERSION=\\\"1.0.0\\\"
DEFINES += OMARCHY_CALENDAR_HOMEPAGE_URL=\\\"https://lastrefuge.ai/projects/omarchy-calendar\\\"
DEFINES += OMARCHY_CALENDAR_PRIVACY_URL=\\\"https://lastrefuge.ai/privacy\\\"
DEFINES += OMARCHY_CALENDAR_TERMS_URL=\\\"https://lastrefuge.ai/terms\\\"
DEFINES += OMARCHY_CALENDAR_SUPPORT_EMAIL=\\\"jason@greatspark.com\\\"

TARGET = omarchy-calendar
SOURCES += \
    app/eventstore.cpp \
    app/main.cpp \
    app/preferences.cpp \
    app/quickentryparser.cpp \
    app/timezonehelper.cpp \
    app/themeprovider.cpp

HEADERS += \
    app/eventstore.h \
    app/preferences.h \
    app/quickentryparser.h \
    app/timezonehelper.h \
    app/themeprovider.h
RESOURCES += app/resources/qml.qrc
