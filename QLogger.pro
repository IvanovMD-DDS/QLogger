QT       -= gui
QT += core
TARGET = QLogger
TEMPLATE = lib
CONFIG += dll

CONFIG += c++20

include(QLogger.pri)

QMAKE_CXXFLAGS += -std=c++2a
