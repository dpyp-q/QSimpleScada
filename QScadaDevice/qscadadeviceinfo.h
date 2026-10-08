#ifndef VDEVICEINFO_H
#define VDEVICEINFO_H

#include <QHostAddress>

#include "../qscadaconfig.h"

class QZeroConfService;

class QScadaDeviceInfo
{
public:
    void init(QScadaDeviceInfo *);

    QString name() const;
    void setName(const QString &name);

    QString type() const;
    void setType(const QString &type);

    QString domain() const;
    void setDomain(const QString &domain);

    QString host() const;
    void setHost(const QString &host);

    QHostAddress ip() const;
    void setIp(const QHostAddress &ip);

    QHostAddress ipv6() const;
    void setIpv6(const QHostAddress &ipv6);

    quint32 interfaceIndex() const;
    void setInterfaceIndex(const quint32 &interfaceIndex);

    quint16 port() const;
    void setPort(const quint16 &port);

    bool operator ==(const QScadaDeviceInfo& other);

    QScadaStatus deviceStatus() const;
    void setDeviceStatus(const QScadaStatus &deviceStatus);

    int unitCount() const;
    void setUnitCount(int unitCount);

    bool operator<(const QScadaDeviceInfo &deviceInfo);

    QList<int> boardIds() const;
    void appendBoardId(int);

private:
    QString mName;
    QString	mType;
    QString	mDomain;
    QString	mHost;
    QHostAddress mIp;
    QHostAddress mIpv6;
    // 以下成员原本既无构造函数也无默认值：在赋值前读取它们是未定义行为，
    // 而 operator== 恰恰会去比较它们，等于拿未初始化内存做判断。
    // 这里用类内初始值给出一组安全默认（端口按 Modbus TCP 惯例取 502）。
    quint32 mInterfaceIndex = 0;
    quint16	mPort = 502;
    QScadaStatus mDeviceStatus = QScadaStatusDefault;
    int mUnitCount = 0;
    QList<int> mBoardIds;
};

#endif // VDEVICEINFO_H
