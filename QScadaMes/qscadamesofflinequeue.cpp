#include "qscadamesofflinequeue.h"
#include "qscadamespayload.h"

#include <QFile>
#include <QSaveFile>
#include <QJsonDocument>
#include <QJsonValue>
#include <QJsonParseError>

QScadaMesOfflineQueue::QScadaMesOfflineQueue(int maxSize)
    : mMaxSize(maxSize > 0 ? maxSize : 1)
    , mHead(0)
    , mCount(0)
    , mDroppedCount(0)
    , mEnqueuedCount(0)
{
    // 一次性预分配：环形缓冲的意义就在于稳态运行时不再有内存分配。
    // 上报循环运行在界面线程里，任何一次 realloc 都可能表现为界面掉帧，
    // 而定长分配在启动阶段完成，运行期不会再有这个抖动。
    mBuffer.resize(mMaxSize);
}

void QScadaMesOfflineQueue::setMaxSize(int maxSize)
{
    if (maxSize < 1)
        maxSize = 1;
    if (maxSize == mMaxSize)
        return;

    // 缩容时先丢弃多余的旧数据：它们最过时，且丢掉后队列立刻回到有界状态，
    // 不会出现"配置已改小、内存却还占着老容量"的假象。
    while (mCount > maxSize)
        dropOldest();

    // 把有效数据重排到新缓冲的下标 0 起始处。
    // 这一步不能省：环形缓冲的 mHead 可能大于新容量（例如容量从 5 改成 2，
    // 而当前 mHead 是 3），此时即便 mCount 已经合法，按下标访问也会越界。
    // 重排后恒有 mHead == 0，是唯一能保证后续取模运算自洽的做法。
    QVector<QJsonObject> compacted(maxSize);
    for (int i = 0; i < mCount; ++i)
        compacted[i] = mBuffer.at((mHead + i) % mMaxSize);

    mBuffer = compacted;
    mMaxSize = maxSize;
    mHead = 0;
}

void QScadaMesOfflineQueue::pushBack(const QJsonObject &payload)
{
    const int tail = (mHead + mCount) % mMaxSize;
    mBuffer[tail] = payload;
    ++mCount;
}

void QScadaMesOfflineQueue::pushFront(const QJsonObject &payload)
{
    // 环形缓冲里"前插"等价于把头指针往回退一格。
    mHead = mHead - 1;
    if (mHead < 0)
        mHead += mMaxSize;
    mBuffer[mHead] = payload;
    ++mCount;
}

void QScadaMesOfflineQueue::dropOldest()
{
    if (mCount <= 0)
        return;
    // 覆盖前先清掉被丢弃的对象：QJsonObject 是隐式共享的，不清的话那块数据
    // 会一直被引用着，等于队列实际占用的内存永远不下降。
    mBuffer[mHead] = QJsonObject();
    mHead = (mHead + 1) % mMaxSize;
    --mCount;
    ++mDroppedCount;
}

bool QScadaMesOfflineQueue::enqueue(const QJsonObject &payload)
{
    if (payload.isEmpty())
        return false; // 空报文发出去只会被 MES 拒收，不入队以免白占容量

    ++mEnqueuedCount;

    bool dropped = false;
    if (mCount >= mMaxSize) {
        dropOldest();
        dropped = true;
    }
    pushBack(payload);
    return dropped;
}

bool QScadaMesOfflineQueue::prepend(const QJsonObject &payload)
{
    if (payload.isEmpty())
        return false;

    ++mEnqueuedCount;

    bool dropped = false;
    if (mCount >= mMaxSize) {
        dropOldest();
        dropped = true;
    }
    pushFront(payload);
    return dropped;
}

QJsonObject QScadaMesOfflineQueue::dequeue()
{
    if (mCount <= 0)
        return QJsonObject();

    const QJsonObject payload = mBuffer.at(mHead);
    mBuffer[mHead] = QJsonObject();   // 及时释放引用，理由同 dropOldest()
    mHead = (mHead + 1) % mMaxSize;
    --mCount;
    return payload;
}

QJsonObject QScadaMesOfflineQueue::peek() const
{
    if (mCount <= 0)
        return QJsonObject();
    return mBuffer.at(mHead);
}

void QScadaMesOfflineQueue::clear()
{
    for (int i = 0; i < mMaxSize; ++i)
        mBuffer[i] = QJsonObject();
    mHead = 0;
    mCount = 0;
    mDroppedCount = 0;
    mEnqueuedCount = 0;
}

quint64 QScadaMesOfflineQueue::maxSequenceNo() const
{
    quint64 maxSeq = 0;
    for (int i = 0; i < mCount; ++i) {
        const QJsonObject &item = mBuffer.at((mHead + i) % mMaxSize);
        // QJsonValue 只有 toDouble()，序号在实际范围内 (< 2^53) 精度无损，
        // 这是 JSON 数字本身的限制，不是这里偷懒。
        const quint64 seq =
            static_cast<quint64>(item.value(QStringLiteral("sequenceNo")).toDouble(0.0));
        if (seq > maxSeq)
            maxSeq = seq;
    }
    return maxSeq;
}

bool QScadaMesOfflineQueue::saveToFile(const QString &path, QString *errorString) const
{
    if (path.trimmed().isEmpty()) {
        if (errorString)
            *errorString = QStringLiteral("离线队列持久化路径为空");
        return false;
    }

    QJsonArray items;
    for (int i = 0; i < mCount; ++i)
        items.append(mBuffer.at((mHead + i) % mMaxSize));

    QJsonObject root;
    root.insert(QStringLiteral("version"), FileFormatVersion);
    // 把序列号一起落盘：重启后接着用，否则 MES 侧会看到序号从 1 重新开始，
    // 误判成"历史报文重放"。
    root.insert(QStringLiteral("sequenceNo"), static_cast<double>(maxSequenceNo()));
    root.insert(QStringLiteral("items"), items);

    // QSaveFile 写同目录临时文件再原子替换，避免掉电时留下半截 JSON——
    // 那种文件下次启动解析失败，整个离线队列就永久废掉了。
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (errorString)
            *errorString = QStringLiteral("无法打开离线队列文件: %1").arg(file.errorString());
        return false;
    }

    const QJsonDocument doc(root);
    const QByteArray data = doc.toJson(QJsonDocument::Indented);
    if (file.write(data) != data.size()) {
        if (errorString)
            *errorString = QStringLiteral("写入离线队列文件失败: %1").arg(file.errorString());
        file.cancelWriting();
        return false;
    }
    if (!file.commit()) {
        if (errorString)
            *errorString = QStringLiteral("提交离线队列文件失败: %1").arg(file.errorString());
        return false;
    }
    return true;
}

bool QScadaMesOfflineQueue::loadFromFile(const QString &path, QString *errorString)
{
    if (path.trimmed().isEmpty()) {
        if (errorString)
            *errorString = QStringLiteral("离线队列持久化路径为空");
        return false;
    }

    // 文件不存在属于首次运行的正常情况，不是错误：直接返回成功且不改动队列。
    if (!QFile::exists(path))
        return true;

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (errorString)
            *errorString = QStringLiteral("无法打开离线队列文件: %1").arg(file.errorString());
        return false;
    }
    const QByteArray data = file.readAll();
    file.close();

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(data, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        if (errorString)
            *errorString = QStringLiteral("离线队列文件解析失败(偏移 %1): %2")
                               .arg(parseError.offset)
                               .arg(parseError.errorString());
        return false;
    }

    // 兼容两种落盘形态：当前的对象包裹格式，以及早期版本直接写的纯数组。
    QJsonArray items;
    quint64 sequenceNo = 0;
    if (doc.isObject()) {
        const QJsonObject root = doc.object();
        if (root.value(QStringLiteral("version")).toInt(0) > FileFormatVersion) {
            if (errorString)
                *errorString = QStringLiteral("离线队列文件版本(%1)高于当前程序支持的版本(%2)")
                                   .arg(root.value(QStringLiteral("version")).toInt(0))
                                   .arg(FileFormatVersion);
            return false;
        }
        sequenceNo = static_cast<quint64>(
            root.value(QStringLiteral("sequenceNo")).toDouble(0.0));
        const QJsonValue itemsValue = root.value(QStringLiteral("items"));
        if (!itemsValue.isArray()) {
            if (errorString)
                *errorString = QStringLiteral("离线队列文件缺少 items 数组");
            return false;
        }
        items = itemsValue.toArray();
    } else if (doc.isArray()) {
        items = doc.array();
    } else {
        if (errorString)
            *errorString = QStringLiteral("离线队列文件格式无法识别");
        return false;
    }

    // 先整体替换内存状态。之所以按"最新优先"截断：队列可能有界变小了，
    // 此时保留靠近文件尾部的报文（也就是最近产生的），并把因此丢掉的条数
    // 计入 droppedCount，让缺口在统计里依然可见。
    clear();

    int skipped = 0;
    if (items.size() > mMaxSize)
        skipped = items.size() - mMaxSize;

    for (int i = skipped; i < items.size(); ++i) {
        const QJsonValue value = items.at(i);
        if (value.isObject())
            pushBack(value.toObject());
    }
    mDroppedCount = static_cast<quint64>(skipped);

    // 恢复序列号：取文件中的值与本次运行已用过的值里较大者，
    // 保证新报文序号一定大于队列里残留的旧报文。
    const quint64 resumeFrom = qMax(sequenceNo, QScadaMesPayload::nextSequenceNo());
    QScadaMesPayload::resetSequenceNo(resumeFrom);

    return true;
}
