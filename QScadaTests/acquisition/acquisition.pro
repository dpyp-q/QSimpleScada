#-------------------------------------------------
# 采集层单元测试（tst_qscadaacquisition）
#
# 只依赖 QtCore + QtTest + QtNetwork，**不依赖 GUI**，
# 因此可以在没有显示器的构建机/CI 上直接跑。
#
# 运行： qmake && make && ./tst_qscadaacquisition
#-------------------------------------------------

QT += core testlib network
QT -= gui

CONFIG += console c++11
CONFIG -= app_bundle

TARGET = tst_qscadaacquisition
TEMPLATE = app

ACQ = $$PWD/../../QScadaAcquisition

INCLUDEPATH += $$ACQ

SOURCES += \
    tst_qscadaacquisition.cpp \
    $$ACQ/qscadatagdefinition.cpp \
    $$ACQ/qscadamodbuscodec.cpp \
    $$ACQ/qscadaregistermap.cpp \
    $$ACQ/qscadadatasource.cpp \
    $$ACQ/qscadadatahub.cpp

HEADERS += \
    $$ACQ/qscadatagdefinition.h \
    $$ACQ/qscadamodbuscodec.h \
    $$ACQ/qscadaregistermap.h \
    $$ACQ/qscadadatasource.h \
    $$ACQ/qscadadatahub.h \
    $$ACQ/qscadatagvalue.h
