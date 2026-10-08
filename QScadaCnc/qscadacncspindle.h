#ifndef QSCADACNCSPINDLE_H
#define QSCADACNCSPINDLE_H

#include <QString>
#include <QStringList>
#include <QVariant>

#include "qscadacncenums.h"
#include "qscadacnctagbinding.h"

/*!
 * \brief 主轴模型（可有多根，用 name 区分，如 "S1" / "S2" / "S"）。
 *
 * 职责：主轴转速、倍率、负载、当前装刀与状态。
 *
 * 设计取舍：
 *  1. **主轴不放进 QScadaCncAxis 列表**。虽然伺服主轴在机械上也是一根轴，
 *     但它在 G 代码里用 S 指令编程、参与恒线速（G96）与定向（M19），
 *     语义与进给轴不同；混在一起会让"轴列表"里冒出一根没有坐标的怪轴，
 *     界面与报警都要额外写特判。
 *  2. **targetSpeed 必须与 speed 分开存**。攻丝、刚性攻丝时系统会限制
 *     实际转速，若只显示实际转速，操作工无法判断"是系统限速还是主轴带不动"，
 *     两个值并排显示可以直接定位是给定问题还是负载问题。
 *  3. **toolNumber 放在主轴上而不是只放在机床上**：刀库预选、主轴松拉刀
 *     是两套独立信号，机床的"当前刀具"是逻辑刀号，主轴上的才是"实际夹着
 *     的那把刀"，换刀过程中两者会短暂不一致，这正是排查撞刀事故的关键信息。
 *
 * 单位：speed/targetSpeed 为 rpm，override 与 loadPercent 为 %。
 */
class QScadaCncSpindle : public QScadaCncTagBinding
{
    Q_OBJECT
public:
    static QStringList bindableFields();

    explicit QScadaCncSpindle(const QString &name = QStringLiteral("S"),
                              QObject *parent = nullptr);

    QString name() const { return mName; }

    QScadaCncEnums::SpindleState state() const { return mState; }
    QString stateText() const { return QScadaCncEnums::spindleStateName(mState); }

    double speed() const { return mSpeed; }
    double targetSpeed() const { return mTargetSpeed; }
    double override() const { return mOverride; }
    double loadPercent() const { return mLoadPercent; }

    int toolNumber() const { return mToolNumber; }
    //! 带 T 前缀的刀号文本，如 "T07"，直接给界面用。
    QString toolText() const;

    int alarmCode() const { return mAlarmCode; }
    QString alarmText() const;

    bool isRunning() const;
    bool isAlarmActive() const;
    /*!
     * 转速是否稳定在给定值附近。
     * 判断"能不能开始切削"靠这个而不是靠 state：刚性攻丝、螺纹车削必须等
     * 转速稳定后才允许进给，否则会乱牙。容差默认 5%，可在参数里调。
     */
    bool isSpeedStable(double tolerancePercent = 5.0) const;

    // ---- 手工赋值接口（仿真 / 单元测试 / 离线演示）----
    void setName(const QString &name);
    void setState(QScadaCncEnums::SpindleState state);
    void setSpeed(double rpm);
    void setTargetSpeed(double rpm);
    void setOverride(double percent);
    void setLoadPercent(double percent);
    void setToolNumber(int tool);
    void setAlarmCode(int code);
    void setAlarmTextOverride(const QString &text);

signals:
    void spindleChanged();

protected:
    bool updateFromTag(const QString &field, const QVariant &value);

private:
    bool setAlarmText(const QString &text);

    QString mName;
    QScadaCncEnums::SpindleState mState;
    double mSpeed;
    double mTargetSpeed;
    double mOverride;
    double mLoadPercent;
    int mToolNumber;
    int mAlarmCode;
    QString mAlarmText;
};

#endif // QSCADACNCSPINDLE_H
