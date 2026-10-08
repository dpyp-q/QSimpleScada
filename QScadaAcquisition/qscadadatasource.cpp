#include "qscadadatasource.h"

QScadaDataSource::QScadaDataSource(QObject *parent)
    : QObject(parent)
    , mPort(0)
    , mPollInterval(1000)
    , mAutoReconnect(true)
    , mReconnectDelayMs(1000)
    , mMaxReconnectDelayMs(30000)
    , mState(Disconnected)
    , mSuccessCount(0)
    , mFailureCount(0)
{
}

QScadaDataSource::~QScadaDataSource()
{
}

void QScadaDataSource::setTarget(const QString &host, quint16 port)
{
    mHost = host;
    mPort = port;
}

void QScadaDataSource::setTags(const QList<QScadaTagDefinition> &tags)
{
    mTags = tags;
}

QScadaTagDefinition QScadaDataSource::tag(const QString &key) const
{
    for (int i = 0; i < mTags.size(); ++i) {
        if (mTags.at(i).key() == key)
            return mTags.at(i);
    }
    return QScadaTagDefinition();
}

bool QScadaDataSource::hasTag(const QString &key) const
{
    return !tag(key).isNull();
}

void QScadaDataSource::setPollInterval(int ms)
{
    // 下限保护：现场曾经出现过把周期设成 0 导致把 PLC 打满的情况。
    mPollInterval = qMax(10, ms);
}

void QScadaDataSource::setAutoReconnect(bool on, int initialDelayMs, int maxDelayMs)
{
    mAutoReconnect = on;
    mReconnectDelayMs = qMax(100, initialDelayMs);
    mMaxReconnectDelayMs = qMax(mReconnectDelayMs, maxDelayMs);
}

double QScadaDataSource::successRate() const
{
    const quint64 total = mSuccessCount + mFailureCount;
    if (total == 0)
        return 0.0;
    return static_cast<double>(mSuccessCount) / static_cast<double>(total);
}

QString QScadaDataSource::stateName(ConnectionState s)
{
    switch (s) {
    case Disconnected: return QStringLiteral("未连接");
    case Connecting:   return QStringLiteral("连接中");
    case Connected:    return QStringLiteral("已连接");
    case Reconnecting: return QStringLiteral("重连中");
    case Faulted:      return QStringLiteral("故障");
    }
    return QStringLiteral("未知");
}

void QScadaDataSource::writeTag(const QString &key, const QVariant &value)
{
    Q_UNUSED(key)
    Q_UNUSED(value)
    reportError(QStringLiteral("驱动 %1 不支持写值").arg(driverName()));
}

void QScadaDataSource::setState(ConnectionState s)
{
    if (mState == s)
        return;
    mState = s;
    emit stateChanged(mState);
}

void QScadaDataSource::reportError(const QString &msg)
{
    mLastError = msg;
    emit errorOccurred(msg);
}

void QScadaDataSource::countSuccess()
{
    ++mSuccessCount;
    mLastSuccessTime = QDateTime::currentDateTime();
    emit statisticsChanged();
}

void QScadaDataSource::countFailure()
{
    ++mFailureCount;
    emit statisticsChanged();
}

void QScadaDataSource::publishRaw(const QScadaTagDefinition &t, const QByteArray &raw)
{
    emit valueChanged(t.key(), t.decode(raw));
}
