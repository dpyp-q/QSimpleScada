#ifndef QSCADAMESCLIENT_H
#define QSCADAMESCLIENT_H

#include <QObject>
#include <QString>
#include <QDateTime>
#include <QElapsedTimer>
#include <QJsonObject>
#include <QMap>
#include <QVariant>

#include "qscadamesconfig.h"
#include "qscadamesofflinequeue.h"

class QTimer;
class QNetworkAccessManager;
class QNetworkReply;

/*!
 * \brief 基于 QNetworkAccessManager 的 MES REST 上报客户端。
 *
 * ============================== 设计目标 ==============================
 * 上位机的第一职责是"让产线看得见、操作得了"。MES 上报再重要也只是附属功能，
 * 绝不能因为它连不上、服务端卡住，就让界面转圈或者丢数据。因此本类的所有
 * 设计都围绕三条约束：
 *
 *  1. **绝不阻塞界面线程**。全部走 QNetworkAccessManager 的异步接口，
 *     发送节奏由 QTimer 驱动，代码里没有一处 waitForReadyRead / 同步 socket。
 *
 *  2. **绝不静默丢数据**。所有上报先入有界离线队列，发送成功才出队；
 *     失败按策略重试，重试仍失败就重新入队等下一轮。
 *     唯一会丢数据的情况是队列写满，且必须通过 droppedCount() 可查。
 *
 *  3. **失败必须分类**。这是 MES 对接最容易被忽略、也最影响现场的一点：
 *     - **网络失败**（连不上、超时、DNS 解析不了、5xx/429）：报文本身没问题，
 *       只是时机不对，重传有意义，且不消耗重试额度，队列挂起等待恢复即可；
 *     - **业务失败**（HTTP 200 但 MES 返回 code != 0，如"工位未绑定工单"、
 *       "密钥无此接口权限"）：报文已经被服务端**读懂并拒绝**了，再发一百次
 *       结果完全一样。此时若继续重试，只会白占带宽、把队首堵死，
 *       让后面本来能成功的报文一起发不出去——这是"一个坏报文拖垮整条队列"
 *       的典型故障。所以业务失败立刻出队、计数、发信号交给上层处理。
 *
 * ============================== 发送状态机 ==============================
 * 队列里始终只有一个"在途"报文（mActive），状态在四个值之间流转：
 *
 *   Idle         队列空，空转等待
 *   PendingSending  已取出队首，等待 mNextAttempt 到点后发第一次请求
 *   Sending       HTTP 请求在途，同时受 timeoutMs 超时保护
 *   PendingRetry 请求失败，等待退避时间后重发同一条报文
 *
 * 取单条串行而不是并发多请求，理由是：产量报文之间有时序关系，
 * 并发发送会让 MES 侧收到乱序；而且弱网下并发只会让所有请求一起超时。
 *
 * 线程约束：整个对象在创建它的线程（通常是界面线程）中使用，
 * 定时器与网络回调都在同一线程，因此内部状态不需要加锁。
 */
class QScadaMesClient : public QObject
{
    Q_OBJECT
public:
    //! 内部发送状态机，暴露出来仅为便于调试与单元测试断言。
    enum SendState {
        Idle = 0,
        PendingSending,
        Sending,
        PendingRetry
    };
    Q_ENUM(SendState)

    explicit QScadaMesClient(QObject *parent = nullptr);
    virtual ~QScadaMesClient();

    /*!
     * 设置配置。运行中调用会按新配置重启（先 stop() 再 start()），
     * 这样现场改上报周期、改 MES 地址后不用重启上位机进程。
     * 注意：队列会被保留，不会因为改配置丢掉待上报数据。
     */
    void setConfig(const QScadaMesConfig &config);
    QScadaMesConfig config() const { return mConfig; }

    //! 启动 / 停止上报。start() 时若配置不合法会直接返回 false 并记录 lastError。
    bool start();
    void stop();
    bool isRunning() const { return mRunning; }

    // ---- 统计：界面状态栏、诊断页直接用这些数字做展示 ----
    quint64 sentCount() const { return mSentCount; }
    /*!
     * 永久失败的报文条数（重试耗尽、被判定为业务失败、或校验不通过的）。
     * 注意它统计的是"这条报文最终没送出去"，与 retryCount() 不同：
     * 后者统计的是"重试了多少次"，一条报文可能贡献多次重试。
     */
    quint64 failedCount() const { return mFailedCount; }
    //! 累计重试次数（含网络失败后的重发，用于评估线路质量）。
    quint64 retryCount() const { return mRetryCount; }
    quint64 droppedCount() const { return mQueue.droppedCount(); }
    int queueSize() const { return mQueue.size(); }
    QDateTime lastSuccessTime() const { return mLastSuccessTime; }
    QString lastError() const { return mLastError; }
    //! 当前是否认为 MES 可达（由真实的请求结果推断，不做额外探测）。
    bool isOnline() const { return mOnline; }
    SendState sendState() const { return mState; }

    //! 离线队列（只读用途：查看待发条数、丢弃计数、做诊断导出）。
    const QScadaMesOfflineQueue *queue() const { return &mQueue; }

    /*!
     * 把离线队列落盘 / 从盘恢复。路径通常来自 QScadaMesConfig::queueFilePath。
     * 恢复失败不影响 start()：宁可丢历史队列，也不能让上位机起不来。
     */
    bool persistQueue(const QString &path = QString());
    bool restoreQueue(const QString &path = QString());

    //! 清空 MES 相关的全部统计（用于"重置诊断计数"按钮）。
    void resetStatistics();

    /*!
     * 报文类型名（heartbeat / production / equipment_status / alarm / oee /
     * tag_snapshot），供界面显示与日志使用。
     */
    static QString messageTypeName(const QString &messageType);

public slots:
    //! 立即上报一次心跳（start() 后也会自动按 heartbeatIntervalMs 上报）。
    void sendHeartbeat();

    /*!
     * 产量上报。goodCount / scrapCount 传**增量**：产量天然是累加业务，
     * 传增量才能安全重传（幂等靠 messageId，重复条数靠 MES 去重）。
     */
    void reportProduction(const QString &workOrderNo,
                          const QString &partNo,
                          int goodCount,
                          int scrapCount);

    //! 报警上报。level 约定 info / warning / fault / critical。
    void reportAlarm(const QString &alarmCode,
                     const QString &level,
                     const QString &message,
                     const QVariant &triggerValue = QVariant());

    //! 设备状态上报，statusCode 约定 running / idle / fault / stop。
    void reportEquipmentStatus(const QString &statusCode, const QString &statusText);

    //! OEE 上报，四个比值均为 0.0 ~ 1.0。
    void reportOee(double availability, double performance, double quality, double oee);

    /*!
     * 关键位号快照上报。
     * 单独留这个槽是为了把采集层与 MES 层解耦：采集侧只需在值变化时发一个信号，
     * 由上层决定什么时候、把哪些位号打包上报，MES 层不必认识 QScadaDataSource。
     */
    void reportTagSnapshot(const QMap<QString, QVariant> &values);

signals:
    void heartbeatSent();
    //! 一条报文被 MES 成功接收（messageType 见 messageTypeName()）。
    void reportSent(const QString &messageType, const QString &messageId);
    /*!
     * 一条报文最终失败（重试耗尽、业务被拒、或未启动时丢弃）。
     * 参数是"类型: 可读原因"，界面可直接显示在日志窗口。
     */
    void reportFailed(const QString &messageType, const QString &errorString);
    //! MES 可达性变化。界面据此点亮"通讯正常/断开"指示灯并触发断线告警。
    void connectionStateChanged(bool online);
    //! 任何统计数字变化后发出，界面不必自己轮询。
    void statisticsChanged();
    /*!
     * 产量自动上报周期到点。库自身不知道"当前工单、已生产多少件"——
     * 那是业务数据，所以这里只发信号，由上层查库后调用 reportProduction()。
     */
    void productionReportDue();

private slots:
    //! 发送循环心跳：驱动状态机前进，是整个客户端唯一的调度入口。
    void onLoopTick();
    void onReplyFinished();
    void onProductionTimer();
    //! 配置变更后的延迟重启，避免在信号回调中同步 stop()/start()。
    void onRestartTimer();

private:
    /*!
     * 队列条目。除了报文本身，还要记住消息类型与已重试次数：
     *  - messageType 用于拼 endpoint 路径（每个报文类型一个 REST 接口），
     *    重发时必须沿用同一个地址，否则会出现"第一次发到 /production，
     *    重试发到 /alarm"这种低级错误；
     *  - attempts 是"这一条报文"的重试计数，放在条目里而不是全局，
     *    才能保证新报文不会继承上一条的重试额度。
     */
    struct Entry {
        QString messageType;
        QJsonObject payload;
        int attempts;

        Entry() : attempts(0) {}
        bool isValid() const { return !messageType.isEmpty() && !payload.isEmpty(); }
    };

    //! 请求结果的分类，直接决定"重试 / 出队 / 挂起"三种处理路径。
    enum FailureKind {
        NoFailure = 0,
        RetryableFailure,   //!< 网络错误、超时、5xx、429：值得重试
        NetworkFailure,     //!< 确定的网络不可用：挂起队列，不消耗重试额度
        BusinessFailure,    //!< MES 明确拒绝或 4xx：重试无意义
        ProtocolFailure     //!< HTTP 200 但响应不是合法 JSON：多半是打到了错误地址
    };

    struct FailureInfo {
        FailureKind kind;
        QString message;
        int httpStatus;

        FailureInfo() : kind(NoFailure), httpStatus(0) {}
    };

    void applyConfig(const QScadaMesConfig &config);
    void initializeNetworkManager();
    void abortActiveReply();

    //! 把一条报文交给离线队列。返回 false 表示客户端未运行或未启用，报文被丢弃。
    bool pushPayload(const QString &messageType, const QJsonObject &payload);
    void saveQueueNow();

    //! 真正发起一次 HTTP 请求（只在 Sending 状态被调用）。
    void sendCurrent();
    void scheduleNextAttempt(int delayMs);
    void setState(SendState state);
    //! 指数退避：retryBackoffMs * 2^(attempts-1)，并做上限保护防止溢出。
    int backoffDelayMs(int attempts) const;

    FailureInfo classifyReply(QNetworkReply *reply) const;
    int maxAttempts() const;

    void failEntry(const Entry &entry, const QString &reason);
    void succeedEntry(const Entry &entry, const QString &messageId);
    void setOnline(bool online);
    void notifyStatistics();

private:
    QScadaMesConfig mConfig;
    QScadaMesOfflineQueue mQueue;

    QNetworkAccessManager *mNetwork;
    QTimer *mSendTimer;         //!< 发送循环（固定 50ms 心跳，便于统一处理超时与重试）
    QTimer *mHeartbeatTimer;
    QTimer *mProductionTimer;
    QTimer *mQueueSaveTimer;    //!< 队列落盘去抖，避免高频入队时反复写盘
    QTimer *mRestartTimer;      //!< 配置热更新后的延迟重启

    bool mRunning;
    bool mOnline;
    QElapsedTimer mUptime;

    // ---- 状态机 ----
    SendState mState;
    Entry mActive;
    QDateTime mNextAttempt;
    QNetworkReply *mActiveReply;
    bool mTimedOut;
    int mLastHttpStatus;

    // ---- 统计 ----
    quint64 mSentCount;
    quint64 mFailedCount;
    quint64 mRetryCount;
    QDateTime mLastSuccessTime;
    QString mLastError;
};

#endif // QSCADAMESCLIENT_H
