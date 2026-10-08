#ifndef QSCADADATASOURCE_H
#define QSCADADATASOURCE_H

#include <QObject>
#include <QString>
#include <QVariant>
#include <QList>
#include <QDateTime>

#include "qscadatagdefinition.h"

/*!
 * \brief 采集驱动抽象接口。
 *
 * 所有协议驱动（Modbus TCP/RTU、OPC UA、西门子 S7、MQTT、串口自定义协议……）
 * 都实现该接口。上层——采集管理器、报警引擎、历史库、界面——只依赖这一个抽象，
 * 因此新增一种协议不需要改动任何上层代码。这是本项目最重要的可扩展点。
 *
 * 线程模型：驱动对象在创建它的线程中工作，内部使用 QTcpSocket / QTimer 做
 * 异步 IO，不阻塞界面线程；采集结果通过信号（自动队列连接）投递回界面线程。
 */
class QScadaDataSource : public QObject
{
    Q_OBJECT
public:
    //! 连接状态，界面据此显示通讯质量并触发断线告警。
    enum ConnectionState {
        Disconnected = 0,
        Connecting,
        Connected,
        Reconnecting,
        Faulted
    };
    Q_ENUM(ConnectionState)

    explicit QScadaDataSource(QObject *parent = nullptr);
    virtual ~QScadaDataSource();

    //! 驱动标识，如 "modbus-tcp" / "simulator" / "opcua"。
    virtual QString driverName() const = 0;

    //! 目标设备地址（IP 或串口名），与 QScadaBoardController 的设备 IP 对应。
    virtual void setTarget(const QString &host, quint16 port);
    QString targetHost() const { return mHost; }
    quint16 targetPort() const { return mPort; }

    //! 采集点表：驱动据此决定读哪些寄存器、如何合并成一次请求。
    void setTags(const QList<QScadaTagDefinition> &tags);
    QList<QScadaTagDefinition> tags() const { return mTags; }
    QScadaTagDefinition tag(const QString &key) const;
    bool hasTag(const QString &key) const;

    //! 采集周期（毫秒）。
    int pollInterval() const { return mPollInterval; }
    void setPollInterval(int ms);

    /*!
     * 自动重连。现场设备掉电、网线松动是常态，上位机必须能自愈，
     * 否则需要人工重启软件。重试间隔按指数退避，避免设备未就绪时被打爆。
     */
    void setAutoReconnect(bool on, int initialDelayMs = 1000, int maxDelayMs = 30000);
    bool autoReconnect() const { return mAutoReconnect; }
    int initialReconnectDelay() const { return mReconnectDelayMs; }
    int maxReconnectDelay() const { return mMaxReconnectDelayMs; }

    ConnectionState state() const { return mState; }
    bool isConnected() const { return mState == Connected; }

    //! 通讯统计，用于界面显示与故障排查。
    quint64 successCount() const { return mSuccessCount; }
    quint64 failureCount() const { return mFailureCount; }
    //! 通讯成功率，0.0 ~ 1.0；从未通讯时返回 0。
    double successRate() const;
    virtual QString lastError() const { return mLastError; }
    QDateTime lastSuccessTime() const { return mLastSuccessTime; }

    static QString stateName(ConnectionState s);

public slots:
    virtual void open() = 0;
    virtual void close() = 0;

    /*!
     * 单次写值（反向控制，例如下发目标位置、启停命令）。
     * 基类默认不支持；Modbus 等可写协议在子类中重写。
     * 失败时发出 errorOccurred。
     */
    virtual void writeTag(const QString &key, const QVariant &value);

signals:
    void stateChanged(QScadaDataSource::ConnectionState state);
    //! 采集到的新值（已换算为工程量）。无效 QVariant 表示该点本次读取失败。
    void valueChanged(const QString &tagKey, const QVariant &value);
    void errorOccurred(const QString &message);
    void statisticsChanged();

protected:
    void setState(ConnectionState s);
    void reportError(const QString &msg);
    void countSuccess();
    void countFailure();

    /*!
     * 子类解析出寄存器原始数据后调用这里，统一做类型解码与工程量换算，
     * 避免每个驱动各写一遍解码逻辑。
     */
    void publishRaw(const QScadaTagDefinition &tag, const QByteArray &raw);

    QString mHost;
    quint16 mPort;

private:
    QList<QScadaTagDefinition> mTags;
    int mPollInterval;
    bool mAutoReconnect;
    int mReconnectDelayMs;
    int mMaxReconnectDelayMs;
    ConnectionState mState;
    quint64 mSuccessCount;
    quint64 mFailureCount;
    QString mLastError;
    QDateTime mLastSuccessTime;
};

Q_DECLARE_METATYPE(QScadaDataSource::ConnectionState)

#endif // QSCADADATASOURCE_H
