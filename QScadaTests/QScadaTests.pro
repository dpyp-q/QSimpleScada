#-------------------------------------------------
# QSimpleScada 全部单元测试（subdirs 根工程）
#
# 每个模块一组独立可执行测试，便于单独运行、单独定位失败：
#   acquisition/  → tst_qscadaacquisition  采集层（Modbus/位号/点表/数据中枢）
#   alarm/        → tst_qscadaalarm        报警引擎（规则/事件/引擎判定）
#
# 所有测试只依赖 QtCore + QtTest（+ acquisition 需要 QtNetwork），
# **不依赖 GUI**，因此可以在没有显示器的构建机/CI 上直接跑。
#
# 运行： qmake && mingw32-make && 分别运行各子目录下生成的测试可执行文件
#-------------------------------------------------

TEMPLATE = subdirs

SUBDIRS += \
    acquisition \
    alarm
