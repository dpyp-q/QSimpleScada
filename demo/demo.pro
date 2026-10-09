# QSimpleScada 二次开发演示宿主工程。
# 链接主库 QSimpleScada0.dll，展示"原框架视图 + 二次开发采集层"的完整数据流。
QT += widgets quick quickwidgets network sql

TEMPLATE = app
TARGET = QSimpleScadaDemo
CONFIG += c++11 console

INCLUDEPATH += \
    .. \
    ../QScadaBoard \
    ../QScadaObject \
    ../QScadaDevice \
    ../QScadaEntity \
    ../QScadaAcquisition \
    ../QScadaAlarm \
    ../QScadaCnc \
    ../QScadaMes \
    ../QScadaStorage

# 注意：demo 的构建目录是 <repo>\build-mingw\demo，相对仓库根多一层。
# 这里用 ../../ 回到仓库根下的 QSimpleScadaLib（DLL + 导入库同目录）。
LIBS += -L../../QSimpleScadaLib -lQSimpleScada0

SOURCES += main.cpp
