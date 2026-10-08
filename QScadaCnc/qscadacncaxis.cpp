#include "qscadacncaxis.h"

#include <QtGlobal>

namespace {

/*!
 * 浮点量是否"基本没变"。
 * 不能直接用 qFuzzyCompare 比较 0：qFuzzyCompare(0.0, 1e-300) 会返回 false，
 * 而已回零的轴位置恰好长期就是 0，会导致每个采集周期都发一次 axisChanged，
 * 界面上的坐标控件被无意义刷新（现场表现为界面卡顿、日志刷屏）。
 */
bool nearlyEqual(double a, double b)
{
    if (a == b) {
        return true;
    }
    const double diff = a - b;
    const double absDiff = (diff < 0.0) ? -diff : diff;
    return absDiff < 1e-6;
}

bool hasText(const QVariant &value)
{
    if (!value.isValid() || value.isNull()) {
        return false;
    }
    return !value.toString().trimmed().isEmpty();
}

} // namespace

QStringList QScadaCncAxis::bindableFields()
{
    QStringList fields;
    fields << QStringLiteral("state")
           << QStringLiteral("machinePosition")
           << QStringLiteral("workPosition")
           << QStringLiteral("remainingDistance")
           << QStringLiteral("feedRate")
           << QStringLiteral("feedOverride")
           << QStringLiteral("loadPercent")
           << QStringLiteral("followingError")
           << QStringLiteral("alarmCode")
           << QStringLiteral("alarmText")
           << QStringLiteral("homed")
           << QStringLiteral("moving");
    return fields;
}

QScadaCncAxis::QScadaCncAxis(const QString &name, QObject *parent)
    : QScadaCncTagBinding(parent)
    , mName(name.trimmed().toUpper())
    , mState(QScadaCncEnums::AxisDisabled)
    , mMachinePosition(0.0)
    , mWorkPosition(0.0)
    , mRemainingDistance(0.0)
    , mFeedRate(0.0)
    , mFeedOverride(100.0)
    , mLoadPercent(0.0)
    , mFollowingError(0.0)
    , mAlarmCode(0)
    , mHomed(false)
    , mHasPosition(false)
    , mTargetPosition(0.0)
{
    // 轴名同时用作字段前缀：机床把 X/Y/Z 的字段放进同一张映射表时，
    // "X.machinePosition" 和 "Y.machinePosition" 必须能区分开。
    setFieldPrefix(mName + QLatin1Char('.'));

    // 倍率默认 100%：刚上电还没读到倍率位号时，显示 0% 会让操作工以为
    // 倍率被关掉了，进而去查一个根本不存在的问题。
    setFeedOverride(100.0);
}

// ---- 只读派生量 ----

double QScadaCncAxis::remainingDistance() const
{
    return mRemainingDistance;
}

QString QScadaCncAxis::alarmText() const
{
    if (!mAlarmText.isEmpty()) {
        return mAlarmText;
    }
    if (mAlarmCode == 0) {
        return QString();
    }
    return QScadaCncEnums::defaultAlarmText(mAlarmCode);
}

bool QScadaCncAxis::isMoving() const
{
    return mState == QScadaCncEnums::AxisMoving;
}

double QScadaCncAxis::distanceToGo() const
{
    // 系统直接给出剩余距离时以系统为准（它还包含刀补、坐标系偏置的影响）；
    // 没绑该位号时用"目标 - 当前"兜底，此时至少能反映程序段还剩多少行程。
    if (isBound(QStringLiteral("remainingDistance"))) {
        return mRemainingDistance;
    }
    const double diff = mTargetPosition - mMachinePosition;
    const double absDiff = (diff < 0.0) ? -diff : diff;
    return absDiff;
}

bool QScadaCncAxis::isAlarmActive() const
{
    return mAlarmCode != 0 || mState == QScadaCncEnums::AxisFault;
}

// ---- setter ----

void QScadaCncAxis::setName(const QString &name)
{
    const QString upper = name.trimmed().toUpper();
    if (upper.isEmpty() || upper == mName) {
        return;
    }

    mName = upper;
    setFieldPrefix(mName + QLatin1Char('.'));
    emit axisChanged(mName);
}

void QScadaCncAxis::setState(QScadaCncEnums::AxisState state)
{
    if (mState == state) {
        return;
    }

    mState = state;

    // "已回零"这一位有些系统不给独立位号，只能从状态推断；推断出来的值
    // 只在没绑 homed 位号时生效，避免覆盖系统真值。
    if (!isBound(QStringLiteral("homed"))) {
        const bool inferred = (state == QScadaCncEnums::AxisHomed
                               || state == QScadaCncEnums::AxisMoving);
        if (inferred != mHomed) {
            mHomed = inferred;
        }
    }

    emit stateChanged(mName, mState);
    emit axisChanged(mName);
}

void QScadaCncAxis::setMachinePosition(double mm)
{
    if (nearlyEqual(mMachinePosition, mm)) {
        return;
    }

    mMachinePosition = mm;
    mHasPosition = true;
    emit axisChanged(mName);
}

void QScadaCncAxis::setWorkPosition(double mm)
{
    if (nearlyEqual(mWorkPosition, mm)) {
        return;
    }

    mWorkPosition = mm;
    mHasPosition = true;
    emit axisChanged(mName);
}

void QScadaCncAxis::setRemainingDistance(double mm)
{
    if (nearlyEqual(mRemainingDistance, mm)) {
        return;
    }

    mRemainingDistance = mm;
    mHasPosition = true;
    emit axisChanged(mName);
}

void QScadaCncAxis::setFeedRate(double mmPerMin)
{
    if (nearlyEqual(mFeedRate, mmPerMin)) {
        return;
    }

    mFeedRate = mmPerMin;
    emit axisChanged(mName);
}

void QScadaCncAxis::setFeedOverride(double percent)
{
    // 现场偶尔读到负倍率（寄存器按有符号解释错了），显示负值会让人误判，
    // 这里直接夹到 0~200：200% 已覆盖绝大多数系统的上限。
    double value = percent;
    if (value < 0.0) {
        value = 0.0;
    } else if (value > 200.0) {
        value = 200.0;
    }

    if (nearlyEqual(mFeedOverride, value)) {
        return;
    }

    mFeedOverride = value;
    emit axisChanged(mName);
}

void QScadaCncAxis::setLoadPercent(double percent)
{
    double value = percent;
    if (value < 0.0) {
        value = 0.0;
    }

    if (nearlyEqual(mLoadPercent, value)) {
        return;
    }

    mLoadPercent = value;
    emit axisChanged(mName);
}

void QScadaCncAxis::setFollowingError(double mm)
{
    if (nearlyEqual(mFollowingError, mm)) {
        return;
    }

    mFollowingError = mm;
    emit axisChanged(mName);
}

void QScadaCncAxis::setAlarmCode(int code)
{
    if (mAlarmCode == code) {
        return;
    }

    mAlarmCode = code;
    emit axisChanged(mName);
}

bool QScadaCncAxis::setAlarmText(const QString &text)
{
    if (mAlarmText == text) {
        return false;
    }

    mAlarmText = text;
    emit axisChanged(mName);
    return true;
}

void QScadaCncAxis::setAlarmTextOverride(const QString &text)
{
    setAlarmText(text.trimmed());
}

void QScadaCncAxis::setHomed(bool on)
{
    if (mHomed == on) {
        return;
    }

    mHomed = on;
    emit axisChanged(mName);
}

void QScadaCncAxis::setTargetPosition(double pos)
{
    if (nearlyEqual(mTargetPosition, pos)) {
        return;
    }

    mTargetPosition = pos;
    if (!isBound(QStringLiteral("remainingDistance"))) {
        emit axisChanged(mName);
    }
}

void QScadaCncAxis::bindMachinePositionTag(const QString &tagKey)
{
    bindTag(QStringLiteral("machinePosition"), tagKey);
}

// ---- 位号分发 ----

bool QScadaCncAxis::updateFromTag(const QString &field, const QVariant &value)
{
    const QString f = qualify(field);

    if (f == QStringLiteral("state")) {
        QScadaCncEnums::AxisState state = mState;
        if (QScadaCncEnums::axisStateFromValue(value, &state)) {
            setState(state);
        } else if (value.isValid() && !value.isNull()) {
            // 数值型状态字（很多系统给的是整数状态字而不是枚举）：
            // 取整后查表，查不到就保持原状态，不要瞎猜成 Fault。
            bool ok = false;
            const int raw = value.toInt(&ok);
            if (ok) {
                setState(static_cast<QScadaCncEnums::AxisState>(raw));
            }
        }
        return true;
    }
    if (f == QStringLiteral("machinePosition")) {
        setMachinePosition(value.toDouble());
        return true;
    }
    if (f == QStringLiteral("workPosition")) {
        setWorkPosition(value.toDouble());
        return true;
    }
    if (f == QStringLiteral("remainingDistance")) {
        setRemainingDistance(value.toDouble());
        return true;
    }
    if (f == QStringLiteral("feedRate")) {
        setFeedRate(value.toDouble());
        return true;
    }
    if (f == QStringLiteral("feedOverride")) {
        setFeedOverride(value.toDouble());
        return true;
    }
    if (f == QStringLiteral("loadPercent")) {
        setLoadPercent(value.toDouble());
        return true;
    }
    if (f == QStringLiteral("followingError")) {
        setFollowingError(value.toDouble());
        return true;
    }
    if (f == QStringLiteral("alarmCode")) {
        setAlarmCode(value.toInt());
        return true;
    }
    if (f == QStringLiteral("alarmText")) {
        setAlarmText(hasText(value) ? value.toString().trimmed() : QString());
        return true;
    }
    if (f == QStringLiteral("homed")) {
        // 有些系统把"已回零"和"运动中"合并成状态字，homed 位号可能是
        // 位状态量（bool），也可能是 0/1 整数，QVariant 两种都能转 bool。
        setHomed(value.toBool());
        return true;
    }
    if (f == QStringLiteral("moving")) {
        // 只绑了一根"运动中"信号线的情况在现场很常见（老机床就三根线：
        // 运行/停止/报警）。这种点必须小心处理：
        //  - 有"运动中"状态字时不要覆盖回零中/保持/故障这些更有信息量的状态；
        //  - 也不能因为没绑状态位号就谎报"就绪"，此前是什么状态就留什么状态。
        const bool moving = value.toBool();
        if (moving) {
            if (mState != QScadaCncEnums::AxisFault && mState != QScadaCncEnums::AxisHoming) {
                setState(QScadaCncEnums::AxisMoving);
            }
        } else if (mState == QScadaCncEnums::AxisMoving) {
            setState(QScadaCncEnums::AxisReady);
        }
        return true;
    }

    return false;
}
