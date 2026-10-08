#ifndef QSCADAMESOFFLINEQUEUE_H
#define QSCADAMESOFFLINEQUEUE_H

#include <QJsonObject>
#include <QJsonArray>
#include <QVector>
#include <QString>

/*!
 * \brief 有界离线上报队列（环形缓冲），用于缓存"还没成功送到 MES"的报文。
 *
 * 为什么必须有这个队列：工业现场的 MES 连接断掉是常态——交换机重启、
 * 网线被叉车压断、厂区做网络割接、MES 服务端升级。上位机不能因为"发不出去"
 * 就把数据扔了，否则这段产量在 MES 里就是永久缺失的，事后只能靠人工补录。
 *
 * 三个关键设计取舍：
 *
 * 1. **有界**。上限来自 QScadaMesConfig::maxQueueSize。一台设备按秒级上报，
 *    断网一天就是几万条报文，无界队列会吃光内存并把上位机主界面拖到卡死，
 *    那比丢数据更严重——数据丢了 MES 侧还能看出缺口，上位机卡死则是全线停产。
 *
 * 2. **满了丢最旧，不丢最新**。最新数据对生产决策更有价值：MES 关心的是
 *    "现在这条线在做什么、良率如何"，而几分钟前的历史产量已经可以由历史库
 *    补齐。反过来若丢最新，现场看到的现象会是"MES 上一直停在十分钟前"，
 *    运维会误判成采集故障。丢弃必须留痕可查（droppedCount()），
 *    否则数据缺了却没人知道，追溯时才发现对不上账。
 *
 * 3. **可选持久化**。软件升级、重启、断电后，内存里的队列会全部消失。
 *    saveToFile() / loadFromFile() 把队列连同序列号一起落盘，
 *    重启后能接着上报，MES 侧也不会看到序号回退。
 *
 * 线程约束：本类不做内部加锁，约定只在客户端所属线程使用（见 QScadaMesClient）。
 * 这样避免为"单线程访问"付出无谓的锁开销；若将来要跨线程共享，
 * 调用方需要在外面加锁，或改用 QMutex 包装。
 */
class QScadaMesOfflineQueue
{
public:
    //! 持久化文件的格式版本。将来报文结构变化时用它做兼容分支。
    static const int FileFormatVersion = 1;

    explicit QScadaMesOfflineQueue(int maxSize = 5000);

    /*!
     * 调整容量上限。缩容时立即丢弃最旧的若干条并计入 droppedCount()，
     * 保证 size() 永远不会超过 maxSize()——配置改小之后如果队列还保持着
     * 旧的大容量，下次断网照样会吃掉几万条的内存。
     */
    void setMaxSize(int maxSize);
    int maxSize() const { return mMaxSize; }

    /*!
     * 入队。队列已满时丢弃**最旧**的一条，再放入新报文，并累加 droppedCount()。
     * \return 是否发生了丢弃（便于上层立即刷新一次统计界面并告警）。
     */
    bool enqueue(const QJsonObject &payload);

    /*!
     * 插到队首，用于"发送失败后重新入队"。
     * 必须插回队首而不是队尾：否则一次失败会把这条报文挪到所有待发报文之后，
     * MES 侧就会看到产量报文的 sequenceNo 大范围乱序（虽然能靠序号重排，
     * 但没必要给对端增加负担）。
     * 若插回时队列已满，仍然丢弃最旧的一条以维持有界性——此时最旧的那条
     * 恰恰是等待时间最长、业务最过时的数据。
     */
    bool prepend(const QJsonObject &payload);

    //! 取队首报文。队列为空时返回空 QJsonObject（isEmpty() 返回的对象也是 false）。
    QJsonObject dequeue();

    //! 只看队首不消费，用于统计或日志，避免为了看一眼就 dequeue/再入队。
    QJsonObject peek() const;

    int size() const { return mCount; }
    bool isEmpty() const { return mCount == 0; }

    //! 清空队列并归零丢弃计数（只应在明确放弃历史数据时调用，例如人工干预）。
    void clear();

    /*!
     * 累计丢弃条数。这是必须暴露给运维的指标：
     * 队列丢数据意味着 MES 侧存在不可恢复的缺口，必须让界面能显示、能告警，
     * 而不是悄悄丢掉了事。
     */
    quint64 droppedCount() const { return mDroppedCount; }

    //! 累计入队条数（含之后被丢弃的），用于判断现场上报压力是否超出配置上限。
    quint64 enqueuedCount() const { return mEnqueuedCount; }

    /*!
     * 取用于持久化的序列号。
     * 队列里残留报文的 sequenceNo 可能大于全局计数器（例如从文件恢复后），
     * 存盘时取两者较大值，重启后 MES 侧才不会看到序号回退。
     */
    quint64 maxSequenceNo() const;

    /*!
     * 落盘为 JSON。结构：{"version":1, "sequenceNo":N, "items":[{...}, ...]}
     *
     * 为什么用"对象包数组"而不是直接写一个 JSON 数组：包一层才能带上
     * version 与 sequenceNo 这类元信息；元信息用数组下标去塞会很别扭，
     * 而且老版本客户端读到纯数组时会误解成报文列表。
     *
     * 写入采用"先写同目录临时文件、再原子替换"的方式，避免掉电或进程被杀
     * 时留下一个半截的 JSON 文件——下次启动解析失败，整个队列就废了。
     */
    bool saveToFile(const QString &path, QString *errorString = nullptr) const;

    /*!
     * 从文件恢复。文件不存在视为"首次运行"，返回 true 且不改动队列。
     * 文件损坏时返回 false 并保持队列原样，由调用方决定是忽略还是告警——
     * 库代码不弹对话框，也不擅自删用户文件。
     */
    bool loadFromFile(const QString &path, QString *errorString = nullptr);

private:
    //! 环形缓冲写入，不做容量检查（检查由 enqueue/prepend 负责）。
    void pushBack(const QJsonObject &payload);
    void pushFront(const QJsonObject &payload);
    //! 丢弃最旧的一条并计数。
    void dropOldest();

    QVector<QJsonObject> mBuffer;
    int mMaxSize;
    int mHead;      //!< 队首在 mBuffer 中的下标
    int mCount;     //!< 当前条数（不是 mBuffer.size()，后者是容量）
    quint64 mDroppedCount;
    quint64 mEnqueuedCount;
};

#endif // QSCADAMESOFFLINEQUEUE_H
