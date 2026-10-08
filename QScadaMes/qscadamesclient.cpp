#include "qscadamesclient.h"
#include "qscadamespayload.h"

#include <QTimer>
#include <QUrl>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QJsonValue>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QSslConfiguration>
#include <QSslSocket>
#include <QSslError>

namespace {

/*!
 * 发送循环的节拍。做成固定小间隔轮询，而不是"每次按需启动一个单次定时器"：
 * 上位机里会有多处触发上报（定时心跳、界面按钮、报警回调、网络恢复），
 * 若每条路径都自己起一个 QTimer，很容易出现重复发送、漏发、或者定时器
 * 没被 stop 而泄漏。统一到一个循环里查状态机，行为最好推理也最好调试。
 * 50ms 对上报场景足够精细，同时让主线程几乎无感。
 */
const int LoopTickMs = 50;

/*!
 * 启动后延后再发第一条报文。给主窗口、采集层留出初始化时间：
 * 上位机刚开机时往往同时在连 PLC、读点表、加载画面，没必要再和它们抢资源。
 */
const int StartupDelayMs = 1000;

/*!
 * 成功后到下一条报文之间的短间隔。不做"能发就立刻发"的激进发送，
 * 是因为积压恢复（例如断网两小时后重连）时若瞬间打几百个请求，
 * 很可能被 MES 侧的风控或限流直接封禁，反而拖慢整体恢复速度。
 */
const int SendGapMs = 100;

/*!
 * 队列落盘去抖时间。每次入队都同步写盘会让高频上报时磁盘 IO 变成瓶颈，
 * 但完全靠"退出时再存"又扛不住断电。2 秒是个折中：最坏情况下丢失
 * 2 秒内的新报文，且这些报文仍在内存队列里、仍在尝试发送。
 */
const int QueueSaveDebounceMs = 2000;

/*!
 * 重试退避的上限。指数退避若不封顶，第 10 次就已经是 8.5 分钟，
 * 恢复会变得非常迟钝；5 分钟足以避免打爆服务端，也保证网络恢复后
 * 一条报文不会被压太久。
 */
const int MaxBackoffMs = 300000;

/*!
 * 判断 HTTP 状态码是否属于"服务端自己的问题"。
 * 5xx：服务端内部错误 / 网关不可用，重发有意义；
 * 429：被限流，等一会儿再来；
 * 408：请求超时，服务端没来得及处理，可以重试。
 * 其余 4xx 是客户端的问题（报文不合法、无权限、接口路径写错），
 * 重发一百次也一样，必须归到业务失败。
 */
bool isRetryableHttpStatus(int status)
{
    return status == 408 || status == 429 || (status >= 500 && status <= 599);
}

} // namespace

QScadaMesClient::QScadaMesClient(QObject *parent)
    : QObject(parent)
    , mQueue(5000)
    , mNetwork(nullptr)
    , mSendTimer(nullptr)
    , mHeartbeatTimer(nullptr)
    , mProductionTimer(nullptr)
    , mQueueSaveTimer(nullptr)
    , mRestartTimer(nullptr)
    , mRunning(false)
    , mOnline(false)
    , mState(Idle)
    , mActiveReply(nullptr)
    , mTimedOut(false)
    , mLastHttpStatus(0)
    , mSentCount(0)
    , mFailedCount(0)
    , mRetryCount(0)
{
    // QNetworkAccessManager 延迟到 start() 时创建：即使某个工程没有把 QtNetwork
    // 链进来，只要不启用 MES，这个类也不会去碰网络子系统。
    mQueue.setMaxSize(mConfig.maxQueueSize);
}

QScadaMesClient::~QScadaMesClient()
{
    // 析构时先停表再放弃在途请求：否则 reply 的 finished 可能在
    // 成员已经析构之后才被投递，造成访问已销毁对象。
    stop();
    delete mNetwork;
    mNetwork = nullptr;
}

void QScadaMesClient::resetStatistics()
{
    mSentCount = 0;
    mFailedCount = 0;
    mRetryCount = 0;
    mLastSuccessTime = QDateTime();
    mLastError.clear();
    notifyStatistics();
}

void QScadaMesClient::setConfig(const QScadaMesConfig &config)
{
    applyConfig(config);

    // 运行中改配置要立即生效。这里不直接 stop()/start()，而是用一个零延迟的
    // 单次定时器推迟到事件循环的下一轮，原因是：setConfig 很可能是在某个信号
    // 回调里被调用的（例如设置对话框点确定），此时同步 stop()/start() 会
    // 在信号投递过程中销毁并重建在途请求对象，是典型的自伤式写法。
    if (mRunning && mRestartTimer)
        mRestartTimer->start(0);
}

void QScadaMesClient::onRestartTimer()
{
    if (!mRunning)
        return;
    // stop() 会把未发完的报文留在队列里，start() 再继续发，配置热更新不丢数据。
    stop();
    start();
}

void QScadaMesClient::applyConfig(const QScadaMesConfig &config)
{
    mConfig = config;
    mConfig.normalize();

    // 队列容量跟着配置走，改小容量时旧数据会被丢弃并计入 droppedCount()。
    mQueue.setMaxSize(mConfig.maxQueueSize);

    // 车间编号注入报文层：它是部署环境常量，逐个上报方法都传一遍既啰嗦又容易漏。
    QScadaMesPayload::setWorkshopCode(mConfig.workshopCode);
}

bool QScadaMesClient::start()
{
    if (mRunning)
        return true;

    if (!mConfig.enabled) {
        mLastError = QStringLiteral("MES 对接未启用(enabled=false)");
        return false;
    }
    if (!mConfig.isValid()) {
        mLastError = QStringLiteral("MES 配置不完整: 缺少 baseUrl 或 deviceCode");
        return false;
    }

    initializeNetworkManager();

    // 先尝试恢复上次退出时残留的离线队列，再开始接收新报文，
    // 这样恢复出来的报文会排在新报文之前发出，序号保持递增。
    restoreQueue();

    mUptime.restart();
    mState = Idle;
    mActive = Entry();
    mNextAttempt = QDateTime::currentDateTime().addMSecs(StartupDelayMs);
    mTimedOut = false;
    mLastHttpStatus = 0;
    mRunning = true;

    mHeartbeatTimer->start(mConfig.heartbeatIntervalMs);
    if (mConfig.productionReportIntervalMs > 0) {
        mProductionTimer->start(mConfig.productionReportIntervalMs);
    } else {
        mProductionTimer->stop();
    }
    mSendTimer->start(LoopTickMs);

    notifyStatistics();
    return true;
}

void QScadaMesClient::stop()
{
    if (mSendTimer)
        mSendTimer->stop();
    if (mHeartbeatTimer)
        mHeartbeatTimer->stop();
    if (mProductionTimer)
        mProductionTimer->stop();
    if (mRestartTimer)
        mRestartTimer->stop();

    // 在途请求直接放弃：它对应的报文已经存回队列，下次 start() 会重发。
    abortActiveReply();

    if (mQueueSaveTimer && mQueueSaveTimer->isActive()) {
        mQueueSaveTimer->stop();
        saveQueueNow();
    }

    mRunning = false;
    setState(Idle);
    // 主动停止不是"网络断开"，不要误报掉线信号给界面。
    mOnline = false;
}

void QScadaMesClient::initializeNetworkManager()
{
    if (mNetwork)
        return;

    mNetwork = new QNetworkAccessManager(this);

    mSendTimer = new QTimer(this);
    // 循环节拍用粗定时器：它只是"检查一下有没有活干"，
    // 精度要求不高，粗定时器能减少系统定时器唤醒次数，对工控机的功耗与实时性都更友好。
    mSendTimer->setTimerType(Qt::CoarseTimer);
    connect(mSendTimer, &QTimer::timeout, this, &QScadaMesClient::onLoopTick);

    mHeartbeatTimer = new QTimer(this);
    connect(mHeartbeatTimer, &QTimer::timeout, this, &QScadaMesClient::sendHeartbeat);

    mProductionTimer = new QTimer(this);
    connect(mProductionTimer, &QTimer::timeout, this, &QScadaMesClient::onProductionTimer);

    mQueueSaveTimer = new QTimer(this);
    mQueueSaveTimer->setSingleShot(true);
    connect(mQueueSaveTimer, &QTimer::timeout, this, &QScadaMesClient::saveQueueNow);

    mRestartTimer = new QTimer(this);
    mRestartTimer->setSingleShot(true);
    mRestartTimer->setInterval(0);
    connect(mRestartTimer, &QTimer::timeout, this, &QScadaMesClient::onRestartTimer);
}

int QScadaMesClient::maxAttempts() const
{
    // +1 是首次发送：retryCount = 3 表示"最多再补发 3 次"，共 4 次机会。
    return mConfig.retryCount + 1;
}

int QScadaMesClient::backoffDelayMs(int attempts) const
{
    if (attempts < 1)
        attempts = 1;

    qint64 delay = mConfig.retryBackoffMs;
    // 只移位 min(attempts-1, 20)：再往上乘一定会溢出 int，
    // 而溢出后的负间隔会退化成"立刻重发"，把退避机制彻底废掉。
    int shift = attempts - 1;
    if (shift > 20)
        shift = 20;
    delay = delay * (static_cast<qint64>(1) << shift);

    if (delay > MaxBackoffMs)
        delay = MaxBackoffMs;
    if (delay < 0)
        delay = 0;
    return static_cast<int>(delay);
}

void QScadaMesClient::setState(SendState state)
{
    mState = state;
}

void QScadaMesClient::scheduleNextAttempt(int delayMs)
{
    mNextAttempt = QDateTime::currentDateTime().addMSecs(delayMs);
}

void QScadaMesClient::abortActiveReply()
{
    if (!mActiveReply)
        return;
    QNetworkReply *reply = mActiveReply;
    mActiveReply = nullptr;

    // 先断开再 abort：abort 会同步触发 finished，若不先断开就会在
    // stop() 的过程中又跑一遍 onReplyFinished，把已经不打算要的报文重新入队。
    reply->disconnect(this);
    reply->abort();
    reply->deleteLater();
}

// ---------------------------------------------------------------------------
// 状态机
// ---------------------------------------------------------------------------

void QScadaMesClient::onLoopTick()
{
    if (!mRunning || !mNetwork)
        return;

    // 超时保护：QNetworkReply 自己没有超时机制，弱网下请求可能挂几分钟。
    // 用发送循环兼做看门狗，比给每个 reply 单独挂 QTimer 更省资源，
    // 也保证"超时"和"发送失败"走同一条重试路径。
    if (mState == Sending && mActiveReply) {
        int guardMs = mConfig.timeoutMs * 2;
        if (guardMs < 1000)
            guardMs = 1000;
        if (mActiveReply->property("qscadaDeadline").isValid()) {
            const QDateTime deadline =
                mActiveReply->property("qscadaDeadline").toDateTime();
            if (deadline.isValid() && QDateTime::currentDateTime() >= deadline) {
                mTimedOut = true;
                QNetworkReply *reply = mActiveReply;
                mActiveReply = nullptr;
                // 同样先断开，超时后的 finished 由 reply 自己走 deleteLater 收尾。
                reply->disconnect(this);
                reply->abort();
                reply->deleteLater();
                setState(PendingRetry);
                return;
            }
        }
    }

    if (mState == Sending)
        return; // 有请求在途，串行发送，等它回来

    if (mState == Idle) {
        if (mQueue.isEmpty())
            return;
        mActive = Entry();
        setState(PendingSending);
    }

    if (mState != PendingSending && mState != PendingRetry)
        return;

    if (!mNextAttempt.isValid() || QDateTime::currentDateTime() < mNextAttempt)
        return;

    if (mState == PendingSending) {
        // 从队首取一条新报文，重试计数必须归零：
        // 否则上一条报文的失败次数会被这条新报文继承，
        // 表现为"新报文第一次就被告知重试耗尽"，现场极难理解。
        const QJsonObject payload = mQueue.dequeue();
        if (payload.isEmpty()) {
            setState(Idle);
            return;
        }
        mActive.messageType = QScadaMesPayload::messageTypeOf(payload);
        mActive.payload = payload;
        mActive.attempts = 0;
        notifyStatistics();
    }

    sendCurrent();
}

void QScadaMesClient::sendCurrent()
{
    if (!mNetwork || !mActive.isValid()) {
        mActive = Entry();
        setState(Idle);
        return;
    }

    // 每个报文类型一个 REST 接口：/heartbeat、/production、/alarm ...
    // 不用"统一接口 + 报文体里带类型"的写法，是因为分开的接口在 MES 侧
    // 更容易做权限控制与限流（例如心跳允许高频、产量必须落在工单上）。
    const QString url = mConfig.baseUrl + QLatin1Char('/') + mActive.messageType;

    QNetworkRequest request{QUrl(url)};
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/json"));
    if (!mConfig.apiKey.isEmpty())
        request.setRawHeader("X-Api-Key", mConfig.apiKey.toUtf8());

    if (!mConfig.verifySsl && url.startsWith(QLatin1String("https"), Qt::CaseInsensitive)) {
        // 只在 https 上动 SSL 配置：对 http 请求设置会触发 Qt 的运行时告警。
        // 工业现场大量使用自签证书，这是必须提供的逃生口；
        // 但绝不能默认关闭，否则等于放弃了中间人攻击防护。
        QSslConfiguration sslConfig = QSslConfiguration::defaultConfiguration();
        sslConfig.setPeerVerifyMode(QSslSocket::VerifyNone);
        request.setSslConfiguration(sslConfig);
    }

    mTimedOut = false;
    mLastHttpStatus = 0;
    QNetworkReply *reply = mNetwork->post(
        request, QJsonDocument(mActive.payload).toJson(QJsonDocument::Compact));
    mActiveReply = reply;

    // 用动态属性记下本条请求的截止时间，超时看门狗据此判断。
    // 放进 reply 而不是成员变量，是为了让"哪条请求、什么时候到期"
    // 始终绑定在一起，不会因为状态切换而错位。
    reply->setProperty("qscadaDeadline",
                       QDateTime::currentDateTime().addMSecs(mConfig.timeoutMs));

    // 只连接 finished。不使用 errorOccurred：那是 Qt 5.15 才引入的信号，
    // 在 Qt 5.12 上根本不存在，连编译都过不了；而 finished 配合 reply->error()
    // 在 Qt 5.12 与 Qt 6 上语义完全一致，是唯一安全的选择。
    connect(reply, &QNetworkReply::finished, this, &QScadaMesClient::onReplyFinished);

    setState(Sending);
}

void QScadaMesClient::onReplyFinished()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
    if (!reply)
        return;

    if (reply == mActiveReply)
        mActiveReply = nullptr;

    // 异常路径：例如 stop() 已经放弃过这条请求，或者状态被重启流程重置。
    // 此时不重试也不重新入队，只回收对象，避免把用户已经清空的报文又塞回队列。
    if (mState != Sending || !mActive.isValid()) {
        reply->deleteLater();
        return;
    }

    const bool timedOut = mTimedOut;
    const FailureInfo failure = classifyReply(reply);
    const Entry entry = mActive;
    const QString messageId =
        entry.payload.value(QStringLiteral("messageId")).toString();

    reply->deleteLater();
    mTimedOut = false;

    if (failure.kind == NoFailure) {
        succeedEntry(entry, messageId);
        return;
    }

    if (failure.kind == NetworkFailure) {
        // 网络不可用：报文没有问题，只是送不出去。这里**不消耗重试次数**，
        // 而是整条队列挂起，等网络恢复后再按原顺序继续发。
        // 若在这里消耗重试额度，一次半小时的断网就会把队列里的报文
        // 全部判死刑丢弃，等网络恢复时反而一条都发不出去了。
        const QString reason = timedOut
            ? QStringLiteral("连接超时(%1 ms)").arg(mConfig.timeoutMs)
            : failure.message;
        mLastError = reason;
        mRetryCount++;

        // 插回队首而不是队尾：保持与 MES 之间原有的报文顺序。
        mQueue.prepend(entry.payload);
        mActive = Entry();

        // 重试节奏用心跳周期（或退避上限）而不是 50ms 循环节拍，
        // 既不会在断网期间空转打爆 CPU，也能在网络恢复后较快自愈。
        int retryDelay = mConfig.heartbeatIntervalMs;
        if (retryDelay > MaxBackoffMs)
            retryDelay = MaxBackoffMs;
        if (retryDelay < mConfig.retryBackoffMs)
            retryDelay = mConfig.retryBackoffMs;
        scheduleNextAttempt(retryDelay);

        // 断网期间队列可能在涨，此时立刻落盘一次：
        // "断网 + 断电 + 重启"是最坏组合，落盘能把损失压到最小。
        saveQueueNow();
        setOnline(false);
        setState(PendingRetry);
        notifyStatistics();
        return;
    }

    if (failure.kind == BusinessFailure) {
        // 业务失败：MES 已经明确答复"这条数据我不要"（code != 0）或 4xx。
        // 报文本身有问题，重发多少次结果都一样，继续重试只会占着队首
        // 把后面本来能成功的报文一起堵住——所以立刻出队、计数、通知上层。
        mLastError = failure.message;
        failEntry(entry, failure.message);
        return;
    }

    // 可重试的传输层失败（5xx / 429 / 408）：等指数退避后重发同一条报文。
    // 注意入口处在分类时已经排除了确定的网络不可用，所以走到这里
    // 说明是服务端返回了响应、只是它自己没处理好。
    const int attemptsUsed = entry.attempts + 1;
    if (attemptsUsed >= maxAttempts()) {
        failEntry(entry, failure.message);
        return;
    }

    mRetryCount++;
    mActive.attempts = attemptsUsed;
    mLastError = failure.message;
    const int delay = backoffDelayMs(attemptsUsed);
    scheduleNextAttempt(delay);
    setState(PendingRetry);
    notifyStatistics();
}

QScadaMesClient::FailureInfo QScadaMesClient::classifyReply(QNetworkReply *reply) const
{
    FailureInfo info;

    // Qt 6 里 reply->error() 的含义没变（仍返回 QNetworkReply::NetworkError），
    // 但 4xx/5xx 也会被映射成 ContentNotFoundError / InternalServerError 等值，
    // 所以必须结合 HTTP 状态码与响应体一起判断，不能只看 error()。
    info.httpStatus =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QNetworkReply::NetworkError netError = reply->error();
    const QByteArray body = reply->readAll();

    if (netError == QNetworkReply::NoError) {
        // 传输层成功。但很多 MES 会返回 200 + HTML 错误页（例如反向代理
        // 把 /api/v1 配错了），所以还要校验响应体确实是约定的 JSON。
        QJsonParseError parseError;
        const QJsonDocument doc = QJsonDocument::fromJson(body, &parseError);
        if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
            info.kind = ProtocolFailure;
            info.message = QStringLiteral("响应不是合法 JSON(HTTP %1): %2")
                               .arg(info.httpStatus == 0 ? 200 : info.httpStatus)
                               .arg(QString::fromUtf8(body.left(200)));
            return info;
        }

        const QJsonObject obj = doc.object();
        const int code = obj.value(QStringLiteral("code")).toInt(-1);
        if (code != 0) {
            // 业务失败：HTTP 层没问题，是 MES 在业务上拒绝了这条数据。
            info.kind = BusinessFailure;
            const QString mesMessage =
                obj.value(QStringLiteral("message")).toString();
            info.message = QStringLiteral("MES 业务失败(HTTP %1, code=%2): %3")
                               .arg(info.httpStatus)
                               .arg(code)
                               .arg(mesMessage.isEmpty() ? QStringLiteral("(无 message)")
                                                         : mesMessage);
            return info;
        }

        info.kind = NoFailure;
        return info;
    }

    // 走到这里说明传输层或 HTTP 层报了错。优先看错误类型：
    // 连接类错误说明网络确实不通，此时无论状态码是什么都按"挂起"处理，
    // 因为这类问题重试再多次也没用，只能等线路恢复。
    switch (netError) {
    case QNetworkReply::ConnectionRefusedError:
    case QNetworkReply::RemoteHostClosedError:
    case QNetworkReply::HostNotFoundError:
    case QNetworkReply::TimeoutError:
    case QNetworkReply::TemporaryNetworkFailureError:
    case QNetworkReply::NetworkSessionFailedError:
    case QNetworkReply::ProxyConnectionRefusedError:
    case QNetworkReply::ProxyConnectionClosedError:
    case QNetworkReply::ProxyNotFoundError:
    case QNetworkReply::ProxyTimeoutError:
    case QNetworkReply::OperationCanceledError: // abort()：看门狗超时或 stop()
        info.kind = NetworkFailure;
        info.message = QStringLiteral("网络不可用: %1").arg(reply->errorString());
        return info;
    default:
        break;
    }

    // 非连接类错误：服务端确实回了一个状态码，按状态码决定是否值得重试。
    if (info.httpStatus > 0) {
        if (isRetryableHttpStatus(info.httpStatus)) {
            info.kind = RetryableFailure;
            info.message = QStringLiteral("服务端错误(HTTP %1): %2")
                               .arg(info.httpStatus)
                               .arg(reply->errorString());
            return info;
        }
        // 其余 4xx：路径写错、密钥无效、报文格式不符合约定。
        // 这些必须归到业务失败，否则会把一个拼错的接口地址重试一辈子。
        info.kind = BusinessFailure;
        info.message = QStringLiteral("请求被拒绝(HTTP %1): %2")
                           .arg(info.httpStatus)
                           .arg(reply->errorString());
        return info;
    }

    // 既没有 HTTP 状态码、错误类型也不属于已知的连接类错误（例如 SSL 握手失败）。
    // 这类问题重发通常无效，归到业务失败，避免堆积。
    info.kind = BusinessFailure;
    info.message = QStringLiteral("请求失败(无状态码): %1").arg(reply->errorString());
    return info;
}

void QScadaMesClient::succeedEntry(const Entry &entry, const QString &messageId)
{
    mSentCount++;
    mLastSuccessTime = QDateTime::currentDateTime();
    mLastError.clear();

    const QString messageType = entry.messageType;
    const QString id = messageId;
    mActive = Entry();

    // 能收到合法响应就说明链路是通的，这里顺带把在线状态拉回来。
    setOnline(true);

    if (messageType == QLatin1String("heartbeat"))
        emit heartbeatSent();
    emit reportSent(messageType, id);

    // 成功后缓一小口气再发下一条，避免积压恢复时瞬间打满 MES。
    scheduleNextAttempt(SendGapMs);
    setState(mQueue.isEmpty() ? Idle : PendingSending);
    notifyStatistics();
}

void QScadaMesClient::failEntry(const Entry &entry, const QString &reason)
{
    // 走到这里说明这条报文已经"最终失败"：重试耗尽、被业务拒绝、
    // 或者协议无法解析。此时必须出队，否则队首会被永久占住，
    // 后面的报文一条都发不出去。
    mFailedCount++;
    mLastError = reason;

    const QString messageType = entry.messageType;
    mActive = Entry();

    // 失败原因必须带上报文类型：界面日志里只写"上报失败"是没法排查的。
    emit reportFailed(messageType, reason);

    // 一条坏报文往往意味着后面的报文也过不去（例如密钥过期），
    // 给它一个退避间隔，避免连续刷屏式失败。
    const int delay = backoffDelayMs(1);
    scheduleNextAttempt(delay);
    setState(mQueue.isEmpty() ? Idle : PendingSending);
    notifyStatistics();
}

void QScadaMesClient::setOnline(bool online)
{
    if (mOnline == online)
        return;
    mOnline = online;
    emit connectionStateChanged(mOnline);
}

void QScadaMesClient::notifyStatistics()
{
    emit statisticsChanged();
}

// ---------------------------------------------------------------------------
// 上报入口：全部先入队，由发送循环统一发出，绝不在调用点直接发请求。
// 这样做的好处是：界面点击上报的响应时间是"入队"的微秒级，
// 网络多慢都不会让按钮卡住；同时所有报文走同一条重试/落盘路径，行为一致。
// ---------------------------------------------------------------------------

bool QScadaMesClient::pushPayload(const QString &messageType, const QJsonObject &payload)
{
    if (payload.isEmpty() || messageType.isEmpty())
        return false;

    if (!mRunning || !mConfig.enabled) {
        // 未启动时不能悄悄入队：队列可能永远不会被消费，最后撑爆内存。
        // 这里明确丢弃并通知上层，让问题立刻可见。
        mLastError = QStringLiteral("MES 客户端未运行，报文已丢弃: %1").arg(messageType);
        emit reportFailed(messageType, mLastError);
        return false;
    }

    mQueue.enqueue(payload);

    // 落盘去抖：高频上报时不能每条都写磁盘，但也不能只等退出时再写
    // （工控机断电是常态）。2 秒一次是这两者之间的折中。
    if (!mConfig.queueFilePath.isEmpty()) {
        if (mQueueSaveTimer)
            mQueueSaveTimer->start(QueueSaveDebounceMs);
    }

    notifyStatistics();
    return true;
}

void QScadaMesClient::sendHeartbeat()
{
    // uptime 让 MES 能区分"设备重启"和"网络闪断"：
    // 两者在上位机侧看起来都是"一段时间没有数据"，但运维处置完全不同。
    const qint64 uptime = mUptime.isValid() ? mUptime.elapsed() : -1;
    const QJsonObject payload = QScadaMesPayload::heartbeat(mConfig.deviceCode,
                                                            QDateTime::currentDateTime(),
                                                            QString(),
                                                            uptime);
    pushPayload(QScadaMesPayload::messageTypeOf(payload), payload);
}

void QScadaMesClient::reportProduction(const QString &workOrderNo,
                                       const QString &partNo,
                                       int goodCount,
                                       int scrapCount)
{
    const QJsonObject payload = QScadaMesPayload::productionCount(mConfig.deviceCode,
                                                                  workOrderNo,
                                                                  partNo,
                                                                  goodCount,
                                                                  scrapCount,
                                                                  QDateTime::currentDateTime());
    pushPayload(QScadaMesPayload::messageTypeOf(payload), payload);
}

void QScadaMesClient::reportAlarm(const QString &alarmCode,
                                  const QString &level,
                                  const QString &message,
                                  const QVariant &triggerValue)
{
    const QJsonObject payload = QScadaMesPayload::alarmReport(mConfig.deviceCode,
                                                              alarmCode,
                                                              level,
                                                              message,
                                                              triggerValue,
                                                              QDateTime::currentDateTime());
    pushPayload(QScadaMesPayload::messageTypeOf(payload), payload);
}

void QScadaMesClient::reportEquipmentStatus(const QString &statusCode,
                                            const QString &statusText)
{
    const QJsonObject payload = QScadaMesPayload::equipmentStatus(mConfig.deviceCode,
                                                                  statusCode,
                                                                  statusText,
                                                                  QDateTime::currentDateTime());
    pushPayload(QScadaMesPayload::messageTypeOf(payload), payload);
}

void QScadaMesClient::reportOee(double availability,
                                double performance,
                                double quality,
                                double oee)
{
    const QJsonObject payload = QScadaMesPayload::oeeReport(mConfig.deviceCode,
                                                            availability,
                                                            performance,
                                                            quality,
                                                            oee,
                                                            QDateTime::currentDateTime());
    pushPayload(QScadaMesPayload::messageTypeOf(payload), payload);
}

void QScadaMesClient::reportTagSnapshot(const QMap<QString, QVariant> &values)
{
    const QJsonObject payload = QScadaMesPayload::tagSnapshot(mConfig.deviceCode,
                                                              values,
                                                              QDateTime::currentDateTime());
    pushPayload(QScadaMesPayload::messageTypeOf(payload), payload);
}

void QScadaMesClient::onProductionTimer()
{
    // 产量是业务数据，库不知道怎么算，只负责按节拍提醒上层。
    emit productionReportDue();
}

// ---------------------------------------------------------------------------
// 离线队列持久化
// ---------------------------------------------------------------------------

bool QScadaMesClient::persistQueue(const QString &path)
{
    const QString target = path.isEmpty() ? mConfig.queueFilePath : path;
    if (target.isEmpty()) {
        mLastError = QStringLiteral("未配置离线队列持久化路径(queueFilePath)，无法保存");
        return false;
    }

    QString errorString;
    if (!mQueue.saveToFile(target, &errorString)) {
        mLastError = errorString;
        return false;
    }
    return true;
}

bool QScadaMesClient::restoreQueue(const QString &path)
{
    // 显式传入的 path 只影响这一次恢复。"恢复 A 文件却写回 B 文件"是最容易
    // 埋下隐患的行为（例如现场用临时文件导入一批积压数据，之后所有落盘
    // 都跑到那个临时文件里去了），所以这里刻意**不**修改 mConfig.queueFilePath：
    // 持久化目标地址只由配置决定，职责单一。
    const QString target = path.isEmpty() ? mConfig.queueFilePath : path;
    if (target.isEmpty())
        return false; // 没配置持久化属于正常情况，不算错误

    QString errorString;
    if (!mQueue.loadFromFile(target, &errorString)) {
        // 恢复失败不能阻断 start()：宁可丢历史队列，也不能让上位机起不来。
        // 但必须记录原因，否则运维会以为"恢复过了"。
        mLastError = QStringLiteral("离线队列恢复失败: %1").arg(errorString);
        return false;
    }
    notifyStatistics();
    return true;
}

void QScadaMesClient::saveQueueNow()
{
    if (mConfig.queueFilePath.isEmpty())
        return;
    persistQueue(QString());
}

QString QScadaMesClient::messageTypeName(const QString &messageType)
{
    // 界面只显示中文，避免现场人员看到 equipment_status 这种内部标识。
    if (messageType == QLatin1String("heartbeat"))
        return QStringLiteral("心跳");
    if (messageType == QLatin1String("production"))
        return QStringLiteral("产量");
    if (messageType == QLatin1String("equipment_status"))
        return QStringLiteral("设备状态");
    if (messageType == QLatin1String("alarm"))
        return QStringLiteral("报警");
    if (messageType == QLatin1String("oee"))
        return QStringLiteral("OEE");
    if (messageType == QLatin1String("tag_snapshot"))
        return QStringLiteral("位号快照");
    return messageType;
}
