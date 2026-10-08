#include "qscadacncspindle.h"

#include <QChar>
#include <QString>

#include <cmath>

namespace {

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

QStringList QScadaCncSpindle::bindableFields()
{
    QStringList fields;
    fields << QStringLiteral("state")
           << QStringLiteral("speed")
           << QStringLiteral("targetSpeed")
           << QStringLiteral("override")
           << QStringLiteral("loadPercent")
           << QStringLiteral("toolNumber")
           << QStringLiteral("alarmCode")
           << QStringLiteral("alarmText");
    return fields;
}

QScadaCncSpindle::QScadaCncSpindle(const QString &name, QObject *parent)
    : QScadaCncTagBinding(parent)
    , mName(name.trimmed().isEmpty() ? QStringLiteral("S") : name.trimmed())
    , mState(QScadaCncEnums::SpindleStopped)
    , mSpeed(0.0)
    , mTargetSpeed(0.0)
    , mOverride(100.0)
    , mLoadPercent(0.0)
    , mToolNumber(0)
    , mAlarmCode(0)
{
    setFieldPrefix(mName + QLatin1Char('.'));
    setOverride(100.0);
}

QString QScadaCncSpindle::toolText() const
{
    if (mToolNumber <= 0) {
        return QStringLiteral("-");
    }
    // 刀号习惯写成两位（T01 ~ T99），三位刀库则自然变成 T100，不做截断。
    return QStringLiteral("T%1").arg(mToolNumber, 2, 10, QLatin1Char('0'));
}

QString QScadaCncSpindle::alarmText() const
{
    if (!mAlarmText.isEmpty()) {
        return mAlarmText;
    }
    if (mAlarmCode == 0) {
        return QString();
    }
    return QScadaCncEnums::defaultAlarmText(mAlarmCode);
}

bool QScadaCncSpindle::isRunning() const
{
    return mState == QScadaCncEnums::SpindleRunning;
}

bool QScadaCncSpindle::isAlarmActive() const
{
    return mAlarmCode != 0 || mState == QScadaCncEnums::SpindleFault;
}

bool QScadaCncSpindle::isSpeedStable(double tolerancePercent) const
{
    if (mState != QScadaCncEnums::SpindleRunning) {
        return false;
    }
    if (mTargetSpeed <= 0.0) {
        return false;
    }

    double tol = tolerancePercent;
    if (tol < 0.0) {
        tol = 0.0;
    }

    const double deviation = std::fabs(mSpeed - mTargetSpeed);
    return deviation <= mTargetSpeed * tol / 100.0;
}

// ---- setter ----

void QScadaCncSpindle::setName(const QString &name)
{
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty() || trimmed == mName) {
        return;
    }

    mName = trimmed;
    setFieldPrefix(mName + QLatin1Char('.'));
    emit spindleChanged();
}

void QScadaCncSpindle::setState(QScadaCncEnums::SpindleState state)
{
    if (mState == state) {
        return;
    }

    mState = state;
    emit spindleChanged();
}

void QScadaCncSpindle::setSpeed(double rpm)
{
    double value = rpm;
    if (value < 0.0) {
        value = 0.0; // 反向旋转用符号表达的点表很少见，负值一律按 0 显示更安全
    }

    if (nearlyEqual(mSpeed, value)) {
        return;
    }

    mSpeed = value;
    emit spindleChanged();
}

void QScadaCncSpindle::setTargetSpeed(double rpm)
{
    double value = rpm;
    if (value < 0.0) {
        value = 0.0;
    }

    if (nearlyEqual(mTargetSpeed, value)) {
        return;
    }

    mTargetSpeed = value;
    emit spindleChanged();
}

void QScadaCncSpindle::setOverride(double percent)
{
    double value = percent;
    if (value < 0.0) {
        value = 0.0;
    } else if (value > 200.0) {
        value = 200.0;
    }

    if (nearlyEqual(mOverride, value)) {
        return;
    }

    mOverride = value;
    emit spindleChanged();
}

void QScadaCncSpindle::setLoadPercent(double percent)
{
    double value = percent;
    if (value < 0.0) {
        value = 0.0;
    }

    if (nearlyEqual(mLoadPercent, value)) {
        return;
    }

    mLoadPercent = value;
    emit spindleChanged();
}

void QScadaCncSpindle::setToolNumber(int tool)
{
    if (mToolNumber == tool) {
        return;
    }

    mToolNumber = tool;
    emit spindleChanged();
}

void QScadaCncSpindle::setAlarmCode(int code)
{
    if (mAlarmCode == code) {
        return;
    }

    mAlarmCode = code;
    emit spindleChanged();
}

bool QScadaCncSpindle::setAlarmText(const QString &text)
{
    if (mAlarmText == text) {
        return false;
    }

    mAlarmText = text;
    emit spindleChanged();
    return true;
}

void QScadaCncSpindle::setAlarmTextOverride(const QString &text)
{
    setAlarmText(text.trimmed());
}

// ---- 位号分发 ----

bool QScadaCncSpindle::updateFromTag(const QString &field, const QVariant &value)
{
    const QString f = qualify(field);

    if (f == QStringLiteral("state")) {
        QScadaCncEnums::SpindleState state = mState;
        if (QScadaCncEnums::spindleStateFromValue(value, &state)) {
            setState(state);
        } else if (value.isValid() && !value.isNull()) {
            bool ok = false;
            const int raw = value.toInt(&ok);
            if (ok) {
                setState(static_cast<QScadaCncEnums::SpindleState>(raw));
            }
        }
        return true;
    }
    if (f == QStringLiteral("speed")) {
        setSpeed(value.toDouble());
        return true;
    }
    if (f == QStringLiteral("targetSpeed")) {
        setTargetSpeed(value.toDouble());
        return true;
    }
    if (f == QStringLiteral("override")) {
        setOverride(value.toDouble());
        return true;
    }
    if (f == QStringLiteral("loadPercent")) {
        setLoadPercent(value.toDouble());
        return true;
    }
    if (f == QStringLiteral("toolNumber")) {
        setToolNumber(value.toInt());
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

    return false;
}
