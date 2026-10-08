#include "qscadaviewbridge.h"

#include <QTimer>
#include <QtGlobal>

#include "../QScadaBoard/qscadaboardcontroller.h"
#include "qscadadatahub.h"

namespace {
//! 默认界面刷新上限：10 Hz。人眼对数字跳变与指针位置的分辨能力远低于此，
//! 再快只是白白占用 GUI 线程（而 GUI 线程同时也是界面响应输入的那条线程）。
const int kDefaultMaxUpdateRateHz = 10;
//! 图元寻址键的分隔符：用不可打印的 0x1F（Unit Separator）。
const char kViewKeySeparator = '\x1f';
}

QScadaViewBridge::QScadaViewBridge(QScadaDataHub *hub, QScadaBoardController *controller,
                                   QObject *parent)
    : QObject(parent)
    , mHub(hub)
    , mController(controller)
    , mFlushTimer(nullptr)
    , mMaxUpdateRate(kDefaultMaxUpdateRateHz)
    , mAppliedCount(0)
    , mCoalescedCount(0)
    , mFilteredCount(0)
    , mShowBadQuality(true)
{
    mFlushTimer = new QTimer(this);
    // 粗精度即可：这是"最多等 100 ms"的合流节拍，不是采样时钟。
    mFlushTimer->setTimerType(Qt::CoarseTimer);
    connect(mFlushTimer, &QTimer::timeout, this, &QScadaViewBridge::flushPending);
    mFlushTimer->start(refreshIntervalMs(mMaxUpdateRate));

    if (mHub) {
        // 中枢承诺所有订阅者在同一个线程里被通知，因此这里用默认的 AutoConnection
        // （同线程即直接调用）即可，既不需要排队也不需要加锁。
        connect(mHub, &QScadaDataHub::tagValueChanged,
                this, &QScadaViewBridge::onTagValueChanged);
    }
}

QScadaViewBridge::~QScadaViewBridge()
{
    // 只停表：定时器是子对象，会随本对象一起销毁。这里刻意不做"最后再刷一次界面"
    // 之类的收尾动作——桥被销毁时界面往往正在关闭，再去动图元只会给关闭流程添乱。
    if (mFlushTimer)
        mFlushTimer->stop();
}

void QScadaViewBridge::setController(QScadaBoardController *controller)
{
    mController = controller;
    // 待刷新的值继续留着：新控制器挂上之后的第一个节拍就会把它们推下去，
    // 不会出现"切画面时刚好丢一拍数据"的空窗。
}

void QScadaViewBridge::bindTag(const QString &tagKey, const QString &deviceIp, int boardId,
                               int objectId)
{
    if (tagKey.isEmpty())
        return;

    Binding binding;
    binding.deviceIp = deviceIp;
    binding.boardId = boardId;
    binding.objectId = objectId;
    // 寻址键在这里（绑定阶段）算一次，数据通路上直接复用：
    // 采集周期 100 ms、几百个位号时，每个值都拼一次字符串就是每秒几千次
    // 无谓的内存分配。
    binding.viewKey = makeViewKey(deviceIp, boardId, objectId);

    mBindings.insert(tagKey, binding);
}

void QScadaViewBridge::bindTags(const QList<QScadaTagDefinition> &definitions)
{
    for (int i = 0; i < definitions.size(); ++i) {
        const QScadaTagDefinition &definition = definitions.at(i);
        if (definition.isNull() || !definition.enabled())
            continue; // 点表里临时停用的点不该刷界面
        if (definition.deviceIp().isEmpty())
            continue; // 没有设备归属就没法视图寻址，绑了也刷不动

        bindTag(definition.key(), definition.deviceIp(),
                definition.boardId(), definition.objectId());
    }
}

void QScadaViewBridge::unbindTag(const QString &key)
{
    const QHash<QString, Binding>::const_iterator it = mBindings.constFind(key);
    if (it == mBindings.constEnd())
        return;

    const QString viewKey = it.value().viewKey;

    // 同一个图元可能还绑着别的位号（例如一个表头同时显示设定值与实际值）：
    // 只有确认没有别人再用这个寻址键，才清掉待刷新值，否则会顺手把另一个位号
    // 这一拍的值一起丢掉。
    bool stillUsed = false;
    QHash<QString, Binding>::const_iterator scan = mBindings.constBegin();
    for (; scan != mBindings.constEnd(); ++scan) {
        if (scan.key() != key && scan.value().viewKey == viewKey) {
            stillUsed = true;
            break;
        }
    }
    if (!stillUsed)
        mPending.remove(viewKey);

    mBindings.remove(key);
}

void QScadaViewBridge::clearBindings()
{
    mBindings.clear();
    // 待刷新队列一并清空：解绑之后不该再有一次"迟到的刷新"打到旧图元上。
    mPending.clear();
}

void QScadaViewBridge::setMaxUpdateRate(int hz)
{
    // 上限 1000 Hz 只是防呆：比采集周期还快的刷新没有任何意义。
    mMaxUpdateRate = qBound(1, hz, 1000);
    if (mFlushTimer)
        mFlushTimer->start(refreshIntervalMs(mMaxUpdateRate));
}

void QScadaViewBridge::setShowBadQuality(bool on)
{
    mShowBadQuality = on;
    if (on)
        return;

    // 策略改成"只刷好值"之后，把还在排队等待刷新的坏质量值丢掉，
    // 否则它们会在下一个节拍里违背新策略打上去。
    QHash<QString, PendingUpdate>::iterator it = mPending.begin();
    while (it != mPending.end()) {
        if (it.value().quality != QualityGood)
            it = mPending.erase(it);
        else
            ++it;
    }
}

void QScadaViewBridge::onTagValueChanged(const QScadaTagValue &tagValue)
{
    const QHash<QString, Binding>::const_iterator it = mBindings.constFind(tagValue.key);
    if (it == mBindings.constEnd())
        return; // 没绑定到图元的位号在这里就被丢掉：本类只服务视图刷新

    // 质量不好时默认照样刷新（让操作员看见异常），具体策略见 setShowBadQuality()。
    if (tagValue.quality != QualityGood && !mShowBadQuality) {
        ++mFilteredCount;
        return;
    }

    // 连值都没有的情况（设备从来没采集成功过、或解码失败）必须挡掉：
    // 把一个空 QVariant 推给图元，QML 侧会变成 undefined、属性会变空，
    // 比"保持上一次的值"更糟，也更容易让现场以为是软件挂了。
    if (!tagValue.value.isValid()) {
        ++mFilteredCount;
        return;
    }

    const Binding &binding = it.value();

    PendingUpdate update;
    update.deviceIp = binding.deviceIp;
    update.boardId = binding.boardId;
    update.objectId = binding.objectId;
    update.value = tagValue.value;
    update.quality = tagValue.quality;

    // 覆盖写：同一个图元在两次刷新之间来了 N 个值，只有最后一个是有意义的。
    // 被覆盖掉的那些记在 coalescedCount() 里，用来量化节流到底省了多少次界面刷新。
    QHash<QString, PendingUpdate>::iterator pending = mPending.find(binding.viewKey);
    if (pending == mPending.end()) {
        mPending.insert(binding.viewKey, update);
    } else {
        ++mCoalescedCount;
        pending.value() = update;
    }
}

void QScadaViewBridge::flushPending()
{
    if (mPending.isEmpty())
        return;

    if (!mController) {
        // 控制器还没挂上（画面还没创建）：待刷新的值保留着，等 setController()
        // 之后的第一个节拍再推。合并表的容量由绑定数决定，等再久也不会膨胀。
        return;
    }

    // 先整批取出再清空，然后才去刷界面：刷新会触发绘图，图元的属性变化有可能
    // 反向触发应用层代码并重入 onTagValueChanged()，边遍历边改哈希表就是未定义行为。
    const QList<PendingUpdate> batch = mPending.values();
    mPending.clear();

    for (int i = 0; i < batch.size(); ++i) {
        const PendingUpdate &update = batch.at(i);
        // 视图层唯一的入口，原框架一行都不用改。
        mController->updateValue(update.deviceIp, update.boardId, update.objectId, update.value);
        ++mAppliedCount;
        emit viewUpdated(update.deviceIp, update.boardId, update.objectId, update.value);
    }
}

int QScadaViewBridge::refreshIntervalMs(int hz)
{
    const int rate = qBound(1, hz, 1000);
    return qMax(1, 1000 / rate);
}

QString QScadaViewBridge::makeViewKey(const QString &deviceIp, int boardId, int objectId)
{
    // 分隔符用不可打印的 0x1F（Unit Separator）而不是 '|' 之类：设备标识是人为
    // 配置的字符串，用可打印字符分隔就有撞键的风险（例如设备名里真的带了竖线），
    // 一旦撞键，两个图元会互相覆盖刷新值——这种 bug 极难定位。
    // 分隔符只在这里构造一次（本函数在绑定时调用），不进数据通路。
    static const QString separator = QString::fromLatin1("\x1f");
    return deviceIp + separator + QString::number(boardId)
           + separator + QString::number(objectId);
}
