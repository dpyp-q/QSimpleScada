#ifndef QSCADAMODBUSTCPCLIENT_H
#define QSCADAMODBUSTCPCLIENT_H

#include "qscadadatasource.h"
#include "qscadamodbuscodec.h"

#include <QAbstractSocket>

class QTcpSocket;
class QTimer;

/*!
 * \brief Modbus TCP 采集驱动。
 *
 * 特性与设计取舍：
 *
 * 1. **地址合并（block coalescing）**——本项目最关键的工程优化。
 *    点表里几十上百个位号往往散落在不同地址；如果每个位号单独发一次请求，
 *    采集周期会被网络往返时间（RTT）主导，几十个点就要几百毫秒。
 *    这里把地址相邻的位号合并进同一个读请求（同时一次最多读 125 个寄存器），
 *    实际现场通常能把请求数压到原来的十分之一以内。
 *    地址之间允许留一点"空洞"：多读几个无用寄存器几乎不花时间，
 *    而多发一次请求要多一个 RTT，二者代价差两个数量级。
 *
 * 2. **不引入第三方 Modbus 库**。libmodbus 之类的库会带来额外的编译与
 *    部署依赖（现场经常是离线环境），而 Modbus 报文本身非常简单，
 *    自己实现只需几百行，出问题还能直接打印报文逐字节排查。
 *
 * 3. **异步、不阻塞界面**。全部基于 QTcpSocket 的信号驱动，
 *    没有任何 sleep 或 waitForReadyRead，界面在通讯慢时依然可操作。
 *
 * 4. **自动重连 + 指数退避**。设备掉电、网线松动是常态；
 *    退避是为了避免设备还没启动完成就被上位机每秒重连几十次打爆。
 */
class QScadaModbusTcpClient : public QScadaDataSource
{
    Q_OBJECT
public:
    explicit QScadaModbusTcpClient(QObject *parent = nullptr);
    ~QScadaModbusTcpClient() override;

    QString driverName() const override { return QStringLiteral("modbus-tcp"); }

    void setTarget(const QString &host, quint16 port) override;

    //! Modbus 从站地址。TCP 网关下通常为 1；串口转 TCP 时对应实际站号。
    quint8 unitId() const { return mUnitId; }
    void setUnitId(quint8 id) { mUnitId = id; }

    //! 读请求功能码：0x03 保持寄存器（默认）或 0x04 输入寄存器。
    void setReadFunction(quint8 functionCode);
    quint8 readFunction() const { return mReadFunction; }

    /*!
     * 合并地址时允许的最大空洞（以 16 位寄存器为单位）。
     * 设为 0 表示只合并严格连续的地址；设大一些能进一步减少请求数。
     */
    void setMaxAddressGap(int gap) { mMaxAddressGap = qMax(0, gap); }
    int maxAddressGap() const { return mMaxAddressGap; }

    void setResponseTimeout(int ms) { mResponseTimeoutMs = qMax(100, ms); }
    int responseTimeout() const { return mResponseTimeoutMs; }

    void setMaxConsecutiveFailures(int n) { mMaxConsecutiveFailures = qMax(1, n); }
    int maxConsecutiveFailures() const { return mMaxConsecutiveFailures; }

    /*!
     * 上一次构建点表时合并出的请求块数量。
     * 暴露出来是为了能在界面/日志里直观验证合并效果
     * （"120 个位号被合并成 9 次请求"本身就是很有说服力的数据）。
     */
    int pollBlockCount() const { return mBlocks.size(); }
    //! 最近一次轮询实际读回的寄存器总数（含合并空洞）。
    int lastReadRegisterCount() const { return mLastReadRegisterCount; }

public slots:
    void open() override;
    void close() override;
    void writeTag(const QString &key, const QVariant &value) override;

private slots:
    void onConnected();
    void onDisconnected();
    void onReadyRead();
    void onSocketError(QAbstractSocket::SocketError error);
    void onResponseTimeout();
    void poll();

private:
    //! 把点表按地址排序并合并成若干读请求块。
    void buildPollBlocks();
    void sendNextBlock();
    void processFrame(const QByteArray &frame);
    void scheduleReconnect();
    void failCurrentCycle(const QString &reason);

    //! 一个读请求块：起始地址 + 寄存器数量 + 落在该块内的位号。
    struct PollBlock {
        quint16 startAddress;
        quint16 quantity;
        QList<QScadaTagDefinition> tags;
    };

    QTcpSocket *mSocket;
    QTimer *mPollTimer;
    QTimer *mReconnectTimer;
    QTimer *mResponseTimer;

    quint8 mUnitId;
    quint8 mReadFunction;
    quint16 mTransactionId;
    QByteArray mRxBuffer;

    QList<PollBlock> mBlocks;
    int mCurrentBlockIndex;
    int mMaxAddressGap;
    int mResponseTimeoutMs;
    int mMaxConsecutiveFailures;
    int mConsecutiveFailures;
    int mReconnectDelayMs;
    int mLastReadRegisterCount;
    bool mWaitingResponse;
    //! 用户主动 close() 时置位，用来抑制 onDisconnected 里的自动重连，
    //! 否则关掉客户端会立刻被重连逻辑拉起来。
    bool mUserClosed;

    /*!
     * 当前在途请求的类型。必须区分读和写：写请求的响应只用来判定成功/失败，
     * 读请求的响应要拿去解码成工程量。混在一起会把写响应错当成读数据。
     */
    enum PendingRequest { NoPending = 0, PendingRead, PendingWrite };
    PendingRequest mPending;
    quint16 mPendingTransaction;
};

#endif // QSCADAMODBUSTCPCLIENT_H
