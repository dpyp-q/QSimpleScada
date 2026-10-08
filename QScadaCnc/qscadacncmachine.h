#ifndef QSCADACNCMACHINE_H
#define QSCADACNCMACHINE_H

#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariant>

#include "../qscadaconfig.h"

#include "qscadacncenums.h"
#include "qscadacncaxis.h"
#include "qscadacncprogram.h"
#include "qscadacncspindle.h"

/*!
 * \brief 整台机床模型：把进给轴、主轴、加工程序、刀具与整机状态聚合成一个对象。
 *
 * ## 为什么要有"机床"这一层
 *
 * 采集层只认识一堆互不相干的位号（Tag），界面层需要的是"这台机床现在怎么样"。
 * 中间缺一层语义聚合，每个界面控件就会各自去拼位号、各自判断"哪些位号组合
 * 起来才算报警"，结果是同一条报警在不同界面上颜色不一样、屏蔽逻辑不一致。
 * 本类把这件事收敛成唯一入口：
 *   - 位号 -> 语义字段的分发（onTagValue）；
 *   - 语义状态 -> 界面状态色（toScadaStatus）；
 *   - 报警汇总（任何轴/主轴报警都从 anyAlarmRaised 出去）。
 *
 * ## 字段命名（机床上所有可绑定字段的全局键）
 *
 * | 对象 | 前缀 | 示例 |
 * |------|------|------|
 * | 轴   | 轴名 + "." | `X.machinePosition`、`Z.followingError` |
 * | 主轴 | 主轴名 + "."（默认 S.） | `S.speed`、`S.toolNumber` |
 * | 程序 | `O.` | `O.currentLine`、`O.partsCompleted` |
 * | 机床 | `M.` | `M.machineState`、`M.activeAlarmCode` |
 *
 * 前缀存在的唯一理由是"一张映射表要装下所有对象"：没有前缀，X 轴和 Y 轴的
 * "machinePosition" 就会互相覆盖。调用 `bindTag()` 时短名会被自动补全，
 * 上层不必记这些前缀。
 *
 * ## 位号来源说明（面试常问："这些数据到底从哪读"）
 *
 *   - 西门子 828D / 840D sl：走 OPC UA 或 HMI 变量服务，地址形如
 *     `/Channel/MachineAxis/actToolBasePos[u1,1]`、`/Channel/State/actTNumber`，
 *     有现成的系统变量可用，单位 mm，免换算；
 *   - 三菱 M80 / M70：走以太网 MELSEC 协议（QnA 兼容帧）读 R/D 寄存器区，
 *     坐标常常在梯图里被放到自定义 R 区，字节序与字序要在点表里配；
 *   - FANUC 0i / 31i：走 FOCAS2 接口（或 Modbus 网关），机械坐标从
 *     `cnc_rdposition` 取，报警号从 `cnc_alarm` 取；
 *   - 老机床：常常只能靠一个 Modbus TCP 网关 + 少量 I/O 点，此时只有
 *     "运行/停止/报警"三根线，模型里其它字段保持未绑定状态即可，界面显示 "--"。
 *
 * 因为地址差异被完全关在点表里，本模块不需要知道上面任何一条细节。
 *
 * ## 线程约定
 *
 * 模型对象属于界面线程；采集驱动通过信号跨线程投递（自动队列连接），
 * 因此 onTagValue 槽内部不需要加锁。
 */
class QScadaCncMachine : public QScadaCncTagBinding
{
    Q_OBJECT
public:
    //! 机床自身的可绑定字段（不含子对象字段）。
    static QStringList bindableFields();

    explicit QScadaCncMachine(QObject *parent = nullptr);
    ~QScadaCncMachine();

    // ---- 静态信息 ----
    QString machineName() const { return mMachineName; }
    void setMachineName(const QString &name);
    //! 型号/系统型号，如 "VMC850 / SIEMENS 828D"，用于看板标题与报警记录。
    QString machineModel() const { return mMachineModel; }
    void setMachineModel(const QString &model);

    // ---- 整机状态 ----
    QScadaCncEnums::MachineState state() const { return mState; }
    QString stateText() const { return QScadaCncEnums::machineStateName(mState); }
    void setState(QScadaCncEnums::MachineState state);

    // ---- 聚合对象（所有权归本类，axes() 返回的指针在对象存活期内有效）----
    QList<QScadaCncAxis *> axes() const { return mAxes; }
    QScadaCncAxis *addAxis(const QString &name);
    //! 按名字取轴（自动转大写）；不存在返回 nullptr，调用方必须判空。
    QScadaCncAxis *axis(const QString &name) const;
    QStringList axisNames() const;

    QScadaCncSpindle *spindle() const { return mSpindle; }
    QScadaCncProgram *program() const { return mProgram; }

    // ---- 机床级字段 ----
    int currentTool() const { return mCurrentTool; }
    //! "T07" 形式，界面直接用。
    QString currentToolText() const;
    QString workOffset() const { return mWorkOffset; }
    double feedRateOverrideGlobal() const { return mFeedRateOverrideGlobal; }
    void setFeedRateOverrideGlobal(double percent);
    int activeAlarmCode() const { return mActiveAlarmCode; }
    QString activeAlarmText() const;

    /*!
     * 把机床状态映射到界面图元的 `QScadaStatus`。
     * 映射原则（与车间习惯一致）：
     *   报警/故障 -> 红；自动运行 -> 绿；暂停/停机/待机 -> 黄（需关注但不致命）；
     *   离线/维护 -> 灰。这样一块看板上颜色本身就能当"要不要过去看一眼"的信号。
     */
    QScadaStatus toScadaStatus() const;

    /*!
     * 一行状态摘要，供看板顶部或报警短信使用。
     * 例："运行中 | X:-12.340 Y:8.900 Z:-45.000 | S:8000rpm | O1234 L128/540 | 已加工 37 件"
     */
    QString statusSummary() const;

    bool hasAlarm() const;
    //! 任一轴在运动（用于判断"是否可以开防护门"这类联锁提示）。
    bool anyAxisMoving() const;

    // ---- 绑定快捷方式（内部委托给对应子对象，前缀由子对象补齐）----
    void bindAxisTag(const QString &axisName, const QString &field, const QString &tagKey);
    QString axisTag(const QString &axisName, const QString &field) const;
    void bindSpindleTag(const QString &field, const QString &tagKey);
    QString spindleTag(const QString &field) const;
    void bindProgramTag(const QString &field, const QString &tagKey);
    QString programTag(const QString &field) const;

    /*!
     * 全部已绑定字段的"字段全名 -> 位号"快照。
     * 用途：启动时把它和点表里的位号集合对比，能立刻发现点表改了而绑定表没改
     * （或多出一个没人用的孤点），这是联调阶段最常见的低级错误来源。
     */
    QHash<QString, QString> fieldTagMap() const;

public slots:
    /*!
     * 采集值统一入口。
     * 参数顺序与 `QScadaDataSource::valueChanged(QString tagKey, QVariant value)`
     * 完全一致，可直接 connect 到任意采集驱动上。
     * 内部按字段前缀把值分发给对应的轴/主轴/程序，再处理机床自己的字段。
     */
    void onTagValue(const QString &tagKey, const QVariant &value);
    //! 采集断开或从未连上时调用：整机进入离线态。
    void setOffline();

signals:
    void machineStateChanged(QScadaCncEnums::MachineState state);
    void axisChanged(const QString &axisName);
    void programChanged();
    /*!
     * 报警产生。source 是报警来源（轴名 / 主轴名 / "MACHINE"），
     * alarmCode 是原始报警号，message 是可直接显示的中文文本。
     * 只有"报警号从 0 变成非 0"时才发，避免每个采集周期重复弹一条。
     */
    void anyAlarmRaised(const QString &source, int alarmCode, const QString &message);
    //! 报警清除，界面据此撤下报警条目（历史记录仍由报警库保留）。
    void alarmCleared(const QString &source, int alarmCode);
    //! 任意一个已绑定字段被新值更新（tagKey 用于诊断"哪个点没进来"）。
    void tagObserved(const QString &tagKey);

protected:
    bool updateFromTag(const QString &field, const QVariant &value);

private slots:
    void onAxisChanged(const QString &axisName);
    void onAxisStateChanged(const QString &axisName, QScadaCncEnums::AxisState state);
    void onSpindleChanged();
    void onProgramChanged();
    //! 子对象把"某字段被更新"冒泡上来，本槽只负责转发与诊断。
    void onTagApplied(const QString &field, const QString &tagKey);

private:
    void connectAxis(QScadaCncAxis *axis);
    bool dispatchToAxis(const QString &field, const QVariant &value);
    bool dispatchToSpindleOrProgram(const QString &field, const QVariant &value);
    //! 机床级报警号的统一入口（同时负责发 anyAlarmRaised / alarmCleared）。
    void setActiveAlarmCode(int code, const QString &text);
    //! 轴 / 主轴报警的统一出口：报警状态从"无"变成"有"时才发信号，
    //! 避免每个采集周期重复弹同一条报警。
    void raiseAlarm(const QString &source, int code, const QString &message);
    //! 报警全部消失后把整机从 Alarm 拉回 Idle，否则看板会一直红着。
    void clearAlarmStateIfRecovered();

    QString mMachineName;
    QString mMachineModel;
    QScadaCncEnums::MachineState mState;
    QList<QScadaCncAxis *> mAxes;
    QScadaCncSpindle *mSpindle;
    QScadaCncProgram *mProgram;
    int mCurrentTool;
    QString mWorkOffset;
    double mFeedRateOverrideGlobal;
    int mActiveAlarmCode;
    QString mActiveAlarmText;
};

#endif // QSCADACNCMACHINE_H
