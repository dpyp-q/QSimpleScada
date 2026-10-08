#ifndef QSCADACNCAXIS_H
#define QSCADACNCAXIS_H

#include <QString>
#include <QStringList>
#include <QVariant>

#include "qscadacncenums.h"
#include "qscadacnctagbinding.h"

/*!
 * \brief 单个进给轴模型（X / Y / Z / A / B / C ...）。
 *
 * 职责：把"一根轴"在加工过程中真正需要被监视的量收在一处，并提供
 * 字段 <-> 位号的绑定能力（见 QScadaCncTagBinding）。
 *
 * 设计取舍：
 *  1. **不存"指令位置"之外的历史**。轨迹回放、圆度分析属于后处理，
 *     放在模型里会让每个采集周期都产生内存分配（现场采集周期常见 50~200ms，
 *     一天几十万个点），模型只保留"最新值"，历史交给独立的历史库模块。
 *  2. **机械坐标与工件坐标都保留，不做换算**。二者之差等于当前工件坐标系
 *     偏置加刀补，但偏置可能是 G54/G55 叠加、还可能被 G52 局部坐标系二次
 *     平移，自己算很容易算错；直接读系统变量最可靠。剩余距离同理，保留
 *     系统给出的值，只在系统不提供该位号时才用"目标 - 当前"兜底。
 *  3. **followingError（跟随误差）独立成字段**：它是数控机床最有效的
 *     预警量——跟随误差持续偏大意味着伺服增益不匹配、导轨缺油或丝杠磨损，
 *     比等到报警停机更早发现问题。
 *  4. **圆度误差不在这里**：多数数控系统不提供在线圆度值，需要球杆仪
 *     或后端对轨迹数据做拟合，硬塞进采集模型会给面试/评审留下"数据来源
 *     说不清"的破绽，故显式不做。
 *
 * 单位约定：位置 mm（旋转轴为度）、进给 mm/min、转速 rpm、倍率与负载为 %。
 * 单位一律不在模型里转换，由点表里的 scale/offset 完成，模型只存工程量数值。
 */
class QScadaCncAxis : public QScadaCncTagBinding
{
    Q_OBJECT
public:
    //! 本模型所有可绑定字段的短名（不含前缀）。配置文件校验、自动生成
    //! 点表模板、HMI 绑定下拉框都用它，避免字段名"只存在于文档里"。
    static QStringList bindableFields();

    //! 服务器约定轴名用大写字母，避免点表里 "x" 与 "X" 生成两根轴。
    explicit QScadaCncAxis(const QString &name, QObject *parent = nullptr);

    QString name() const { return mName; }

    QScadaCncEnums::AxisState state() const { return mState; }
    QString stateText() const { return QScadaCncEnums::axisStateName(mState); }

    double machinePosition() const { return mMachinePosition; }
    double workPosition() const { return mWorkPosition; }
    //! 剩余距离：优先用系统给定的值，没绑该位号时退化为"目标 - 当前"。
    double remainingDistance() const;

    double feedRate() const { return mFeedRate; }
    double feedOverride() const { return mFeedOverride; }
    double loadPercent() const { return mLoadPercent; }
    double followingError() const { return mFollowingError; }

    int alarmCode() const { return mAlarmCode; }
    //! 报警中文说明：位号里带文本就用文本，否则用通用报警号表兜底。
    QString alarmText() const;

    bool homed() const { return mHomed; }
    //! 本次运动的目标位置（mm）。不参与采集，由上位机下发或由程序段解析得到，
    //! 仅用于在系统不给"剩余距离"位号时兜底计算 distanceToGo()。
    double targetPosition() const { return mTargetPosition; }
    void setTargetPosition(double pos);

    bool isMoving() const;
    //! 剩余距离的别名，保留数控习惯叫法（Distance To Go）。
    double distanceToGo() const;
    //! 该轴是否需要引起注意（故障或负载长期偏高）。
    bool isAlarmActive() const;

    // ---- 手工赋值接口（仿真、单元测试、离线演示用；现场数据一律走位号）----
    void setName(const QString &name);
    void setState(QScadaCncEnums::AxisState state);
    void setMachinePosition(double mm);
    void setWorkPosition(double mm);
    void setRemainingDistance(double mm);
    void setFeedRate(double mmPerMin);
    void setFeedOverride(double percent);
    void setLoadPercent(double percent);
    void setFollowingError(double mm);
    void setAlarmCode(int code);
    void setAlarmTextOverride(const QString &text);
    void setHomed(bool on);

    /*!
     * 位置初始化标记。采集驱动刚连上时，未绑定的字段会保持 0.0，
     * 界面显示成 "X:0.000" 会让操作工以为轴真的在原点，因此未收到过
     * 坐标位号前应显示 "--"，由本标记决定。
     */
    bool hasPosition() const { return mHasPosition; }

    //! 便捷绑定：等价于 bindTag("machinePosition", tagKey)。
    void bindMachinePositionTag(const QString &tagKey);

signals:
    void axisChanged(const QString &axisName);
    void stateChanged(const QString &axisName, QScadaCncEnums::AxisState state);

protected:
    bool updateFromTag(const QString &field, const QVariant &value);

private:
    bool setAlarmText(const QString &text);

    QString mName;
    QScadaCncEnums::AxisState mState;
    double mMachinePosition;
    double mWorkPosition;
    double mRemainingDistance;
    double mFeedRate;
    double mFeedOverride;
    double mLoadPercent;
    double mFollowingError;
    int mAlarmCode;
    QString mAlarmText;
    bool mHomed;
    bool mHasPosition;
    double mTargetPosition;
};

#endif // QSCADACNCAXIS_H
