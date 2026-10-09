#-------------------------------------------------
# 报警引擎单元测试（tst_qscadaalarm）
#
# 只依赖 QtCore + QtTest + QtGui（QColor，等级颜色辅助），**不依赖界面**，
# 因此可以在没有显示器的构建机/CI 上直接跑。
# 被测对象：
#   - QScadaAlarmRule   判定/恢复/死区/有效性/消息模板/JSON
#   - QScadaAlarmEvent  状态机四态/幂等迁移/持续时长/JSON（含 state-only 兼容）
#   - QScadaAlarmEngine 即时/延时报警、死区恢复、确认、系统关闭、历史裁剪、
#                       无效值忽略、变化率基线、通知合并
#
# 运行： qmake && make && ./tst_qscadaalarm
#-------------------------------------------------

QT += core testlib gui

CONFIG += console c++11
CONFIG -= app_bundle

TARGET = tst_qscadaalarm
TEMPLATE = app

ALM = $$PWD/../../QScadaAlarm

INCLUDEPATH += $$ALM

SOURCES += \
    tst_qscadaalarm.cpp \
    $$ALM/qscadaalarmengine.cpp \
    $$ALM/qscadaalarmevent.cpp \
    $$ALM/qscadaalarmrule.cpp

HEADERS += \
    $$ALM/qscadaalarmengine.h \
    $$ALM/qscadaalarmevent.h \
    $$ALM/qscadaalarmrule.h \
    $$ALM/qscadaalarmlevel.h
