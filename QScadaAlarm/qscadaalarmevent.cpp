#include "qscadaalarmevent.h"

#include <QJsonValue>
#include <QUuid>

QScadaAlarmEvent::QScadaAlarmEvent()
    : mLevel(QScadaAlarmLevels::Warning)
    , mTriggerValue(0.0)
    , mLimitValue(0.0)
    , mActive(false)
    , mAcknowledged(false)
{
}

QScadaAlarmEvent::AlarmState QScadaAlarmEvent::state() const
{
    // 状态完全由两个维度投影出来，不额外存字段，避免出现
    // "状态字段说已确认、ackedBy 却是空的"这种自相矛盾的记录。
    if (mActive)
        return mAcknowledged ? Acknowledged : Active;
    return mAcknowledged ? Closed : Cleared;
}

QString QScadaAlarmEvent::stateName(AlarmState state)
{
    switch (state) {
    case Active:       return QStringLiteral("active");
    case Cleared:      return QStringLiteral("cleared");
    case Acknowledged: return QStringLiteral("acknowledged");
    case Closed:       return QStringLiteral("closed");
    }
    return QStringLiteral("active");
}

QScadaAlarmEvent::AlarmState QScadaAlarmEvent::stateFromName(const QString &name)
{
    const QString n = name.trimmed().toLower();
    if (n == QLatin1String("cleared") || n == QLatin1String("rtn"))
        return Cleared;
    if (n == QLatin1String("acknowledged") || n == QLatin1String("acked"))
        return Acknowledged;
    if (n == QLatin1String("closed"))
        return Closed;
    return Active;
}

QString QScadaAlarmEvent::stateDisplayText() const
{
    switch (state()) {
    case Active:       return QStringLiteral("越限未确认");
    case Cleared:      return QStringLiteral("已恢复未确认");
    case Acknowledged: return QStringLiteral("越限已确认");
    case Closed:       return QStringLiteral("已关闭");
    }
    return QStringLiteral("越限未确认");
}

void QScadaAlarmEvent::markCleared(const QDateTime &when)
{
    mActive = false;
    // 只在第一次恢复时落时间戳：后续抖动不能改写原始恢复时刻，
    // 否则历史里的持续时长会被越算越长（或越短），报表就没法用了。
    if (!mClearedAt.isValid())
        mClearedAt = when;
}

void QScadaAlarmEvent::markAcknowledged(const QString &operatorName, const QDateTime &when)
{
    mAcknowledged = true;
    if (!mAckedAt.isValid()) {
        mAckedAt = when;
        mAckedBy = operatorName;
    }
}

qint64 QScadaAlarmEvent::durationMs() const
{
    if (!mRaisedAt.isValid())
        return 0;

    const QDateTime end = mClearedAt.isValid() ? mClearedAt : QDateTime::currentDateTime();
    const qint64 ms = mRaisedAt.msecsTo(end);
    return (ms > 0) ? ms : 0;
}

QString QScadaAlarmEvent::durationText() const
{
    const qint64 ms = durationMs();
    if (ms < 1000)
        return QStringLiteral("%1 毫秒").arg(ms);

    const qint64 totalSeconds = ms / 1000;
    if (totalSeconds < 60)
        return QStringLiteral("%1 秒").arg(totalSeconds);

    const qint64 minutes = totalSeconds / 60;
    const qint64 seconds = totalSeconds % 60;
    if (minutes < 60)
        return QStringLiteral("%1 分 %2 秒").arg(minutes).arg(seconds);

    const qint64 hours = minutes / 60;
    return QStringLiteral("%1 小时 %2 分").arg(hours).arg(minutes % 60);
}

QJsonObject QScadaAlarmEvent::toJson() const
{
    QJsonObject obj;
    obj.insert(QStringLiteral("eventId"), mEventId);
    obj.insert(QStringLiteral("ruleId"), mRuleId);
    obj.insert(QStringLiteral("tagKey"), mTagKey);
    obj.insert(QStringLiteral("level"), QScadaAlarmLevels::levelName(mLevel));
    obj.insert(QStringLiteral("message"), mMessage);
    obj.insert(QStringLiteral("triggerValue"), mTriggerValue);
    obj.insert(QStringLiteral("limitValue"), mLimitValue);
    // 带毫秒的 ISO8601：报警排查经常要看到秒以下的先后关系，
    // 而且字符串形式跨时区、跨数据库都安全。
    obj.insert(QStringLiteral("raisedAt"), mRaisedAt.toString(Qt::ISODateWithMs));
    obj.insert(QStringLiteral("clearedAt"), mClearedAt.toString(Qt::ISODateWithMs));
    obj.insert(QStringLiteral("ackedAt"), mAckedAt.toString(Qt::ISODateWithMs));
    obj.insert(QStringLiteral("ackedBy"), mAckedBy);
    obj.insert(QStringLiteral("active"), mActive);
    obj.insert(QStringLiteral("acknowledged"), mAcknowledged);
    // state 是冗余字段：给 MES / 报表这类只读消费方直接用，
    // 免得每个消费方都要自己按两个布尔量推导一遍。
    obj.insert(QStringLiteral("state"), stateName(state()));
    return obj;
}

QScadaAlarmEvent QScadaAlarmEvent::fromJson(const QJsonObject &obj)
{
    QScadaAlarmEvent event;
    event.mEventId = obj.value(QStringLiteral("eventId")).toString();
    event.mRuleId = obj.value(QStringLiteral("ruleId")).toString();
    event.mTagKey = obj.value(QStringLiteral("tagKey")).toString();
    event.mLevel = QScadaAlarmLevels::levelFromName(obj.value(QStringLiteral("level")).toString());
    event.mMessage = obj.value(QStringLiteral("message")).toString();
    event.mTriggerValue = obj.value(QStringLiteral("triggerValue")).toDouble(0.0);
    event.mLimitValue = obj.value(QStringLiteral("limitValue")).toDouble(0.0);
    event.mRaisedAt = QDateTime::fromString(obj.value(QStringLiteral("raisedAt")).toString(),
                                           Qt::ISODateWithMs);
    event.mClearedAt = QDateTime::fromString(obj.value(QStringLiteral("clearedAt")).toString(),
                                            Qt::ISODateWithMs);
    event.mAckedAt = QDateTime::fromString(obj.value(QStringLiteral("ackedAt")).toString(),
                                          Qt::ISODateWithMs);
    event.mAckedBy = obj.value(QStringLiteral("ackedBy")).toString();

    if (obj.contains(QStringLiteral("active")) || obj.contains(QStringLiteral("acknowledged"))) {
        event.mActive = obj.value(QStringLiteral("active")).toBool(false);
        event.mAcknowledged = obj.value(QStringLiteral("acknowledged")).toBool(false);
    } else {
        // 兼容只存了一个 state 字段的外部数据（例如别的系统写进 MES 的记录），
        // 按状态表反推出两个维度，避免这类老数据加载后状态全变成"越限中"。
        switch (stateFromName(obj.value(QStringLiteral("state")).toString())) {
        case Active:
            event.mActive = true;
            event.mAcknowledged = false;
            break;
        case Acknowledged:
            event.mActive = true;
            event.mAcknowledged = true;
            break;
        case Cleared:
            event.mActive = false;
            event.mAcknowledged = false;
            break;
        case Closed:
            event.mActive = false;
            event.mAcknowledged = true;
            break;
        }
    }

    return event;
}

QString QScadaAlarmEvent::newEventId()
{
    // UUID 而不是自增序号：历史库是跨重启、跨班次累积的，自增序号一重启就重号，
    // 确认操作按 id 回写时会改错记录。
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}
