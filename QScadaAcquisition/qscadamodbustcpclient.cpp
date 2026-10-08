#include "qscadamodbustcpclient.h"

#include <QTcpSocket>
#include <QTimer>
#include <QDateTime>
#include <QHostAddress>
#include <QtGlobal>

#include <algorithm>

namespace {
//! 首次重连等待时间；之后每次翻倍，直到上限。
const int kInitialReconnectDelayMs = 1000;
const int kMaxReconnectDelayMs     = 30000;
}

QScadaModbusTcpClient::QScadaModbusTcpClient(QObject *parent)
    : QScadaDataSource(parent)
    , mSocket(nullptr)
    , mPollTimer(nullptr)
    , mReconnectTimer(nullptr)
    , mResponseTimer(nullptr)
    , mUnitId(1)
    , mReadFunction(QScadaModbusCodec::ReadHoldingRegisters)
    , mTransactionId(0)
    , mCurrentBlockIndex(0)
    , mMaxAddressGap(8)
    , mResponseTimeoutMs(1000)
    , mMaxConsecutiveFailures(3)
    , mConsecutiveFailures(0)
    , mReconnectDelayMs(kInitialReconnectDelayMs)
    , mLastReadRegisterCount(0)
    , mWaitingResponse(false)
    , mUserClosed(false)
    , mPending(NoPending)
    , mPendingTransaction(0)
{
    mSocket = new QTcpSocket(this);

    // Qt 5.15 起 QAbstractSocket::error 被弃用并改名 errorOccurred，Qt 6 直接删掉了旧名。
    // 这里做版本分支，保证 Qt 5.12 与 Qt 6 都能编过。
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    connect(mSocket, &QAbstractSocket::errorOccurred,
            this, &QScadaModbusTcpClient::onSocketError);
#else
    connect(mSocket, QOverload<QAbstractSocket::SocketError>::of(&QAbstractSocket::error),
            this, &QScadaModbusTcpClient::onSocketError);
#endif
    connect(mSocket, &QTcpSocket::connected,    this, &QScadaModbusTcpClient::onConnected);
    connect(mSocket, &QTcpSocket::disconnected, this, &QScadaModbusTcpClient::onDisconnected);
    connect(mSocket, &QTcpSocket::readyRead,    this, &QScadaModbusTcpClient::onReadyRead);

    mPollTimer = new QTimer(this);
    // 采集定时器用粗精度即可：Qt::PreciseTimer 会提高系统定时器分辨率、增加功耗，
    // 而 SCADA 轮询对几十毫秒的抖动并不敏感。
    mPollTimer->setTimerType(Qt::CoarseTimer);
    connect(mPollTimer, &QTimer::timeout, this, &QScadaModbusTcpClient::poll);

    mReconnectTimer = new QTimer(this);
    mReconnectTimer->setSingleShot(true);
    connect(mReconnectTimer, &QTimer::timeout, this, &QScadaModbusTcpClient::open);

    mResponseTimer = new QTimer(this);
    mResponseTimer->setSingleShot(true);
    connect(mResponseTimer, &QTimer::timeout, this, &QScadaModbusTcpClient::onResponseTimeout);
}

QScadaModbusTcpClient::~QScadaModbusTcpClient()
{
    // 析构时不要再走重连流程。
    mUserClosed = true;
    if (mPollTimer)     mPollTimer->stop();
    if (mReconnectTimer) mReconnectTimer->stop();
    if (mResponseTimer) mResponseTimer->stop();
}

void QScadaModbusTcpClient::setTarget(const QString &host, quint16 port)
{
    QScadaDataSource::setTarget(host, port);

    // 目标变了就必须断开重连，否则会一直读着旧设备的地址。
    if (isConnected() || mSocket->state() != QAbstractSocket::UnconnectedState) {
        close();
        mUserClosed = false;
        open();
    }
}

void QScadaModbusTcpClient::setReadFunction(quint8 functionCode)
{
    // 只有读寄存器类功能码有意义，其它一律退回默认值，避免把写功能码传进来。
    if (functionCode == QScadaModbusCodec::ReadHoldingRegisters
        || functionCode == QScadaModbusCodec::ReadInputRegisters) {
        mReadFunction = functionCode;
    } else {
        mReadFunction = QScadaModbusCodec::ReadHoldingRegisters;
    }
    buildPollBlocks();
}

void QScadaModbusTcpClient::open()
{
    if (mHost.isEmpty()) {
        reportError(QStringLiteral("未设置目标设备地址，无法建立连接"));
        return;
    }
    if (mSocket->state() != QAbstractSocket::UnconnectedState)
        return;

    mUserClosed = false;
    mReconnectTimer->stop();
    mConsecutiveFailures = 0;
    mRxBuffer.clear();

    buildPollBlocks();

    setState(Connecting);
    mSocket->connectToHost(mHost, mPort);
}

void QScadaModbusTcpClient::close()
{
    // 先置位再 abort()：abort() 会同步发出 disconnected，
    // 若不提前置位，onDisconnected 会立刻把连接又拉起来。
    mUserClosed = true;

    mPollTimer->stop();
    mReconnectTimer->stop();
    mResponseTimer->stop();
    mWaitingResponse = false;
    mPending = NoPending;
    mRxBuffer.clear();

    if (mSocket->state() != QAbstractSocket::UnconnectedState)
        mSocket->abort();

    setState(Disconnected);
}

void QScadaModbusTcpClient::onConnected()
{
    mConsecutiveFailures = 0;
    mReconnectDelayMs = kInitialReconnectDelayMs;
    setState(Connected);

    if (mBlocks.isEmpty())
        buildPollBlocks();

    // 连上后立刻采一次：现场希望切到画面就能看到当前值，
    // 而不是盯着空画面等一个采集周期。
    QTimer::singleShot(0, this, &QScadaModbusTcpClient::poll);
    mPollTimer->start(pollInterval());
}

void QScadaModbusTcpClient::onDisconnected()
{
    mPollTimer->stop();
    mResponseTimer->stop();
    mWaitingResponse = false;
    mPending = NoPending;

    setState(Disconnected);

    if (mUserClosed)
        return;

    reportError(QStringLiteral("与设备 %1:%2 的连接已断开").arg(mHost).arg(mPort));
    scheduleReconnect();
}

void QScadaModbusTcpClient::onSocketError(QAbstractSocket::SocketError error)
{
    Q_UNUSED(error)

    if (mUserClosed)
        return;

    reportError(QStringLiteral("网络错误: %1").arg(mSocket->errorString()));

    // 连接被拒绝/超时等情况下，QTcpSocket 不一定发出 disconnected 信号，
    // 这里兜底：只要状态已经回到未连接，就安排重连。
    if (mSocket->state() == QAbstractSocket::UnconnectedState)
        scheduleReconnect();
}

void QScadaModbusTcpClient::onResponseTimeout()
{
    mWaitingResponse = false;
    mPending = NoPending;
    countFailure();
    failCurrentCycle(QStringLiteral("等待设备响应超时(%1 ms)").arg(mResponseTimeoutMs));
}

void QScadaModbusTcpClient::scheduleReconnect()
{
    if (!autoReconnect() || mUserClosed)
        return;

    setState(Reconnecting);
    mReconnectTimer->start(mReconnectDelayMs);

    // 指数退避：设备重启往往要几十秒，如果固定 1 秒重试，
    // 半小时内会产生上千次失败日志，把真正有用的信息淹没。
    mReconnectDelayMs = qMin(mReconnectDelayMs * 2, kMaxReconnectDelayMs);
}

/*!
 * 把点表按地址排序后贪心合并成尽可能少的读请求块。
 *
 * 合并的两个约束：
 *  - 单次请求的寄存器数不能超过 Modbus 规范的 125（功能码 03/04）；
 *  - 地址空洞不能超过 maxAddressGap，否则"多读几个无用寄存器"反而浪费带宽。
 */
void QScadaModbusTcpClient::buildPollBlocks()
{
    mBlocks.clear();

    QList<QScadaTagDefinition> sorted;
    const QList<QScadaTagDefinition> all = tags();
    sorted.reserve(all.size());
    for (int i = 0; i < all.size(); ++i) {
        if (all.at(i).enabled())
            sorted.append(all.at(i));
    }

    std::sort(sorted.begin(), sorted.end(),
              [](const QScadaTagDefinition &a, const QScadaTagDefinition &b) {
                  return a.address() < b.address();
              });

    int i = 0;
    while (i < sorted.size()) {
        PollBlock block;
        block.startAddress = sorted.at(i).address();
        block.quantity = static_cast<quint16>(sorted.at(i).registerCount());
        block.tags.append(sorted.at(i));
        ++i;

        while (i < sorted.size()) {
            const QScadaTagDefinition &candidate = sorted.at(i);
            const int candidateEnd = static_cast<int>(candidate.address()) + candidate.registerCount();
            const int span = candidateEnd - static_cast<int>(block.startAddress);

            if (span > QScadaModbusCodec::MaxReadRegisters)
                break; // 再并进来就超过单次读取上限

            const int blockEnd = static_cast<int>(block.startAddress) + block.quantity;
            const int gap = static_cast<int>(candidate.address()) - blockEnd;
            if (gap > mMaxAddressGap)
                break; // 空洞太大，另起一块更划算

            block.quantity = static_cast<quint16>(span);
            block.tags.append(candidate);
            ++i;
        }

        mBlocks.append(block);
    }
}

void QScadaModbusTcpClient::poll()
{
    if (!isConnected())
        return;

    // 上一轮还没回来就不重入，交给响应超时定时器处理。
    if (mWaitingResponse)
        return;

    // 点表可能在运行期间被界面改动，每轮重建一次。位号数量在几百级别时
    // 这点排序开销远小于一次网络往返，没必要做增量更新。
    buildPollBlocks();

    mCurrentBlockIndex = 0;
    mLastReadRegisterCount = 0;
    for (int i = 0; i < mBlocks.size(); ++i)
        mLastReadRegisterCount += mBlocks.at(i).quantity;

    if (mBlocks.isEmpty())
        return; // 没有启用的位号，不算失败

    sendNextBlock();
}

void QScadaModbusTcpClient::sendNextBlock()
{
    if (mCurrentBlockIndex >= static_cast<int>(mBlocks.size())) {
        // 一轮所有块都读成功
        countSuccess();
        mConsecutiveFailures = 0;
        mWaitingResponse = false;
        mPending = NoPending;
        return;
    }

    const PollBlock &block = mBlocks.at(mCurrentBlockIndex);
    mPendingTransaction = ++mTransactionId;

    const QByteArray request = QScadaModbusCodec::buildReadRequest(
        mPendingTransaction, mUnitId, mReadFunction, block.startAddress, block.quantity);

    mPending = PendingRead;
    mWaitingResponse = true;
    mRxBuffer.clear();
    mSocket->write(request);
    mResponseTimer->start(mResponseTimeoutMs);
}

void QScadaModbusTcpClient::onReadyRead()
{
    mRxBuffer.append(mSocket->readAll());

    // TCP 是字节流：一次 readyRead 可能只到了半个报文，也可能一次来了两条。
    // 必须按 MBAP 头的长度字段自己拆包，否则会偶发解析失败——这类问题在现场
    // 极难复现（取决于网络分片），却会让人怀疑是设备的问题。
    const int expected = QScadaModbusCodec::expectedResponseLength(mRxBuffer);
    if (expected < 0)
        return; // 数据还不够，等下一次 readyRead

    if (expected == 0) {
        // 头部非法说明字节流已经失步，再继续解析只会读到垃圾数据。
        countFailure();
        failCurrentCycle(QStringLiteral("Modbus 报文头非法，字节流失步"));
        return;
    }

    if (mRxBuffer.size() < expected)
        return; // 还没收全

    // 本客户端严格一问一答、不流水线发送，因此只可能有一个在途请求，
    // 缓冲区里多余的字节属于协议异常，直接丢弃而不是留在里面污染下一次解析。
    const QByteArray frame = mRxBuffer.left(expected);
    mRxBuffer.clear();

    processFrame(frame);
}

void QScadaModbusTcpClient::processFrame(const QByteArray &frame)
{
    const QScadaModbusCodec::Response response = QScadaModbusCodec::parseResponse(frame);

    if (!response.valid) {
        mResponseTimer->stop();
        mWaitingResponse = false;
        mPending = NoPending;
        countFailure();
        failCurrentCycle(QStringLiteral("报文解析失败: %1").arg(response.errorString));
        return;
    }

    // 事务号对不上 = 上一次超时请求的迟到响应。
    // 必须丢弃：如果拿它当成本次请求的结果，整块数据都会错位，
    // 表现出来就是"数值偶尔乱跳"，比彻底读不到还难查。
    if (response.transactionId != mPendingTransaction)
        return;

    mResponseTimer->stop();
    mWaitingResponse = false;

    if (mPending == PendingWrite) {
        mPending = NoPending;
        if (response.isException) {
            countFailure();
            reportError(QStringLiteral("写值被设备拒绝: %1").arg(response.errorString));
        } else {
            countSuccess();
        }
        return;
    }

    mPending = NoPending;

    if (response.isException) {
        countFailure();
        reportError(QStringLiteral("设备返回异常: %1").arg(response.errorString));

        // 异常码是设备的明确答复，说明链路是通的，不应据此判定掉线。
        // 跳过这个块继续读其它块，避免一个配错的坏地址拖垮整轮采集——
        // 现场最常见的情况就是点表里混进了一个设备不存在的地址。
        ++mCurrentBlockIndex;
        sendNextBlock();
        return;
    }

    if (mCurrentBlockIndex < mBlocks.size()) {
        const PollBlock &block = mBlocks.at(mCurrentBlockIndex);
        for (int i = 0; i < block.tags.size(); ++i) {
            const QScadaTagDefinition &t = block.tags.at(i);
            const int offset = (static_cast<int>(t.address()) - static_cast<int>(block.startAddress)) * 2;
            const int need = t.registerCount() * 2;

            // 设备返回的数据可能比请求的短（尤其是网关做了裁剪），
            // 越界读会让程序崩溃，这里直接跳过该点。
            if (offset < 0 || offset + need > response.data.size())
                continue;

            publishRaw(t, response.data.mid(offset, need));
        }
    }

    ++mCurrentBlockIndex;
    sendNextBlock();
}

void QScadaModbusTcpClient::failCurrentCycle(const QString &reason)
{
    ++mConsecutiveFailures;

    mPollTimer->stop();
    mResponseTimer->stop();
    mWaitingResponse = false;
    mPending = NoPending;
    mRxBuffer.clear();

    if (mConsecutiveFailures >= mMaxConsecutiveFailures) {
        // 连续多次失败基本可以判定链路已死（网线掉了、设备重启、网关断电）。
        // 主动断开并走重连流程，而不是继续往一个死连接里写数据。
        reportError(QStringLiteral("连续 %1 次采集失败(%2)，准备重新建立连接")
                        .arg(mConsecutiveFailures)
                        .arg(reason));
        if (mSocket->state() != QAbstractSocket::UnconnectedState) {
            mSocket->abort(); // 会触发 onDisconnected -> scheduleReconnect
        } else {
            scheduleReconnect();
        }
        return;
    }

    // 偶发失败不影响整体，等下一个采集周期继续。
    if (isConnected())
        mPollTimer->start(pollInterval());
}

void QScadaModbusTcpClient::writeTag(const QString &key, const QVariant &value)
{
    const QScadaTagDefinition t = tag(key);
    if (t.isNull()) {
        reportError(QStringLiteral("写值失败：点表中不存在位号 %1").arg(key));
        return;
    }
    if (!isConnected()) {
        reportError(QStringLiteral("写值失败：设备未连接"));
        return;
    }

    const quint16 tid = ++mTransactionId;
    QByteArray request;

    if (t.dataType() == QScadaTagDefinition::DataTypeBool) {
        request = QScadaModbusCodec::buildWriteSingleCoil(tid, mUnitId, t.address(), value.toBool());
    } else {
        const QList<quint16> regs = QScadaModbusCodec::toRegisters(t.encode(value));
        if (regs.size() == 1) {
            request = QScadaModbusCodec::buildWriteSingleRegister(tid, mUnitId, t.address(), regs.at(0));
        } else {
            request = QScadaModbusCodec::buildWriteMultipleRegisters(tid, mUnitId, t.address(), regs);
        }
    }

    mSocket->write(request);

    if (mPending == PendingRead) {
        // 采集请求正在途中。写请求照样发出去，但不去认领它的响应，
        // 以免把采集的数据流打乱；写失败会被设备的下一轮采集值反映出来。
        return;
    }

    mPending = PendingWrite;
    mPendingTransaction = tid;
    mWaitingResponse = true;
    mResponseTimer->start(mResponseTimeoutMs);
}
