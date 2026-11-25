# The control panel: a small Qt Quick window that talks to the running c1-tune over its
# socket. Kept apart from the app itself so the part that stays running links no toolkit.
QT += core gui qml quick quickcontrols2 network

CONFIG += c++17 release
TARGET = c1-tune-panel
TEMPLATE = app
DEFINES += PANEL_VERSION=\\\"$$PANEL_VERSION\\\"

HEADERS += backend.h theme.h
SOURCES += main.cpp backend.cpp theme.cpp
RESOURCES += resources.qrc
