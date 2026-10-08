#include "qscadadatahub.h"

#include <QTimer>
#include <QDateTime>
#include <QDebug>

// qscadaQualityName() 由 QScadaStorage/qscadadataquality.h 以内联函数提供，
// 这里不再重复定义（否则会与那个内联定义冲突）。

QScadaDataHub::QScadaDataHub(QObject *parent)
    : QObject(parent)
    , mRateTimer(nullptr)
    , mPublishedCount(0)
    , mRateWindowCount(0)
    , mPointsPerSecond(0.0)
{
    // 跨线程排队连接要求类型已注册到元类型系统，否则信号会被静默丢弃
    // （现场表现就是"数据时有时无"，极难排查）。
    qRegisterMetaType<QScadaQuality>("QScadaQuality");
    qRegisterMetaType<QScadaTagValue>("QScadaTagValue");

    mRateTimer = new QTimer(this);
    mRateTimer->setInterval(1000);
    mRateTimer->setTimerType(Qt::CoarseTimer);
    connect(mRateTimer, &QTimer::timeout, this, &QScadaDataHub::onRateTick);
    mRateClock.start();
    mRateTimer->start();
}

void QScadaDataHub::registerTags(const QList<QScadaTagDefinition> &definitions)
{
    for (int i = 0; i < definitions.size(); ++i)
        registerTag(definitions.at(i));
}

void QScadaDataHub::registerTag(const QScadaTagDefinition &definition)
{
    if (definition.isNull())
        return;
    mDefinitions.insert(definition.key(), definition);
}

void QScadaDataHub::clearDefinitions()
{
    mDefinitions.clear();
}

QScadaTagDefinition QScadaDataHub::definition(const QString &key) const
{
    return mDefinitions.value(key, QScadaTagDefinition());
}

QScadaTagValue QScadaDataHub::tagValue(const QString &key) const
{
    return mValues.value(key, QScadaTagValue());
}

bool QScadaDataHub::hasValue(const QString &key) const
{
    return mValues.contains(key);
}

QList<QScadaTagValue> QScadaDataHub::snapshot() const
{
    return mValues.values();
}

QList<QScadaTagValue> QScadaDataHub::snapshotForDevice(const QString &deviceIp) const
{
    QList<QScadaTagValue> out;
    QHash<QString, QScadaTagValue>::const_iterator it = mValues.constBegin();
    for (; it != mValues.constEnd(); ++it) {
        if (it.value().deviceIp == deviceIp)
            out.append(it.value());
    }
    return out;
}

QScadaQuality QScadaDataHub::deviceQuality(const QString &deviceIp) const
{
    return mDeviceQuality.value(deviceIp, QualityGood);
}

void QScadaDataHub::publish(const QString &tagKey, const QVariant &value, QScadaQuality quality)
{
    if (tagKey.isEmpty())
        return;

    QScadaTagValue tv;
    tv.key = tagKey;
    tv.value = value;
    tv.timestamp = QDateTime::currentDateTime();
    tv.quality = quality;

    // 位号元数据可能已经注册过，带上设备归属，便于按设备聚合与断线降级。
    const QScadaTagDefinition def = mDefinitions.value(tagKey, QScadaTagDefinition());
    if (!def.isNull())
        tv.deviceIp = def.deviceIp();

    // 设备整体断线时，单个驱动即使报出值也一律降级：
    // 这类"陈旧值"混进历史库比没有数据更危险。
    if (!tv.deviceIp.isEmpty()) {
        const QScadaQuality devQuality = mDeviceQuality.value(tv.deviceIp, QualityGood);
        if (devQuality != QualityGood)
            tv.quality = devQuality;
    }

    mValues.insert(tagKey, tv);
    ++mPublishedCount;
    ++mRateWindowCount;

    emit tagValueChanged(tv);
}

void QScadaDataHub::setDeviceQuality(const QString &deviceIp, QScadaQuality quality)
{
    if (deviceIp.isEmpty())
        return;

    const QScadaQuality previous = mDeviceQuality.value(deviceIp, QualityGood);
    if (previous == quality)
        return;

    mDeviceQuality.insert(deviceIp, quality);
    emit deviceQualityChanged(deviceIp, quality);

    // 设备状态变化会改变它名下所有缓存值的质量，必须逐个重发，
    // 否则界面会一直显示断线前的旧值且看不出来。
    QList<QScadaTagValue> updated;
    QHash<QString, QScadaTagValue>::iterator it = mValues.begin();
    for (; it != mValues.end(); ++it) {
        if (it.value().deviceIp != deviceIp)
            continue;
        it.value().quality = quality;
        updated.append(it.value());
    }

    for (int i = 0; i < updated.size(); ++i)
        emit tagValueChanged(updated.at(i));
}

void QScadaDataHub::reset()
{
    mValues.clear();
    mDeviceQuality.clear();
    mPublishedCount = 0;
    mRateWindowCount = 0;
    mPointsPerSecond = 0.0;
    mRateClock.restart();
    emit statisticsChanged();
}

void QScadaDataHub::onRateTick()
{
    const qint64 elapsed = mRateClock.restart();
    if (elapsed <= 0) {
        mPointsPerSecond = 0.0;
    } else {
        // 用实际经过的毫秒数而不是假设 1000ms：定时器在系统繁忙时会漂移，
        // 按标称值计算会让统计偏高。
        mPointsPerSecond = static_cast<double>(mRateWindowCount) * 1000.0
                           / static_cast<double>(elapsed);
    }
    mRateWindowCount = 0;
    emit statisticsChanged();
}
