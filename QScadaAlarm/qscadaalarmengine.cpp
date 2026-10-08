#include "qscadaalarmengine.h"

#include <QTimer>
#include <QElapsedTimer>
#include <QStringList>
#include <QDebug>
#include <QtGlobal>

namespace {

//! 历史默认容量：够查"最近几天报了什么"，又不会让常年运行的上位机内存无限增长。
const int kDefaultHistoryLimit = 1000;

/*!
 * 变化率计算的最小时间窗（毫秒）。两次采样间隔小于它时不重新估算变化率：
 * 采集驱动重发、一次轮询补发多个点时，dt 会趋近 0，除以它会把正常的
 * 量化噪声（例如 0.1℃ 的分辨率）放大成"每秒几千度"的假变化率。
 */
const int kDefaultMinRateIntervalMs = 50;

/*!
 * 变化率基线的最大有效间隔（毫秒）。超过它说明中间断过（掉线、重启、点表重载），
 * 这时首尾两点之差代表的是"断线期间累计的变化量"，不是变化率，
 * 直接算会在通讯恢复的瞬间固定误报一次。
 */
const int kDefaultRateMaxGapMs = 30000;

//! 规则被删除/停用、引擎复位时，系统代操作员关闭报警所用的署名。
const char *const kSystemOperator = "system";

//! 确认时没填操作员名字（例如调试脚本调用）时的署名，避免历史里出现空白确认人。
const char *const kUnnamedOperator = "未署名操作员";

} // namespace

/*!
 * 每条规则的运行时状态。规则本身是纯配置（值语义、可随便拷贝），
 * 所有"会变的量"都集中在这个槽里，引擎重启一次判定逻辑就是干净状态。
 */
struct QScadaAlarmEngine::RuleSlot
{
    QScadaAlarmRule rule;

    QTimer *delayTimer;       //!< 延时确认定时器；只有 delayMs > 0 的规则才创建
    bool pendingBreach;       //!< 已越限、正在等延时到点（候选状态）
    double pendingValue;      //!< 候选状态的触发值（到点后复核用最新值）
    double pendingRate;       //!< 候选状态的变化率（变化率规则复核要用）
    QDateTime pendingSince;   //!< 候选状态起始时刻，可用于界面显示"还有多久确认"

    QString eventId;          //!< 该规则当前未关闭报警的 id，空表示没有
    bool inAlarm;             //!< 是否已产生报警（恢复判定要按死区走）

    bool hasSample;           //!< 变化率基线是否有效
    double lastValue;         //!< 上一个采样值
    double lastRate;          //!< 上一次算出的变化率（样本间隔过小时沿用）
    QElapsedTimer sampleClock;//!< 单调时钟：系统校时/人工改钟不会算出负的 dt

    RuleSlot()
        : delayTimer(nullptr)
        , pendingBreach(false)
        , pendingValue(0.0)
        , pendingRate(0.0)
        , inAlarm(false)
        , hasSample(false)
        , lastValue(0.0)
        , lastRate(0.0)
    {
    }
};

QScadaAlarmEngine::QScadaAlarmEngine(QObject *parent)
    : QObject(parent)
    , mHistoryLimit(kDefaultHistoryLimit)
    , mMinRateIntervalMs(kDefaultMinRateIntervalMs)
    , mRateMaxGapMs(kDefaultRateMaxGapMs)
    , mAlarmListDirty(false)
    , mHistoryDirty(false)
{
    /*!
     * 事件对象要按值随信号投递。跨线程（采集线程 → 界面线程）时 Qt 必须先知道
     * 怎么拷贝它，没注册的话编译期毫无提示，运行时只在控制台打印
     * "Cannot queue arguments of type 'QScadaAlarmEvent'"，报警就悄悄丢了。
     * 这类问题现场极难定位，所以注册放在构造函数里，谁创建引擎谁就自动带上。
     */
    qRegisterMetaType<QScadaAlarmEvent>("QScadaAlarmEvent");
    qRegisterMetaType<QScadaAlarmLevel>("QScadaAlarmLevel");
    qRegisterMetaType<QScadaAlarmRule>("QScadaAlarmRule");
}

QScadaAlarmEngine::~QScadaAlarmEngine()
{
    destroyAllSlots();
}

// ---------------------------------------------------------------------------
// 规则管理
// ---------------------------------------------------------------------------

bool QScadaAlarmEngine::addRule(const QScadaAlarmRule &rule)
{
    if (!rule.isValid()) {
        qWarning() << "[QScadaAlarm] 规则配置无效，已忽略. ruleId:" << rule.ruleId()
                   << "tag:" << rule.tagKey();
        return false;
    }
    if (mSlots.contains(rule.ruleId())) {
        qWarning() << "[QScadaAlarm] 规则 id 已存在，请用 updateRule. ruleId:" << rule.ruleId();
        return false;
    }

    RuleSlot *slot = new RuleSlot;
    slot->rule = rule;
    mSlots.insert(rule.ruleId(), slot);
    return true;
}

bool QScadaAlarmEngine::updateRule(const QScadaAlarmRule &rule)
{
    RuleSlot *slot = findSlot(rule.ruleId());
    if (slot == nullptr) {
        qWarning() << "[QScadaAlarm] 要更新的规则不存在. ruleId:" << rule.ruleId();
        return false;
    }
    if (!rule.isValid()) {
        qWarning() << "[QScadaAlarm] 规则配置无效，保留原配置. ruleId:" << rule.ruleId();
        return false;
    }

    // 换了目标位号等于换了判据：旧报警再也没有恢复的依据，
    // 只能按"配置变更"由系统关闭，否则它会永远挂在报警表上。
    if (slot->rule.tagKey() != rule.tagKey())
        closeEventsOfRule(rule.ruleId());

    slot->rule = rule;

    // 配置变了就丢掉候选状态与变化率基线：拿旧阈值算出来的候选值继续计时，
    // 会按新配置产生一条本不该存在的报警。
    cancelPending(slot);
    slot->hasSample = false;
    slot->lastValue = 0.0;
    slot->lastRate = 0.0;
    slot->sampleClock.invalidate();

    flushNotifications();
    return true;
}

bool QScadaAlarmEngine::removeRule(const QString &ruleId)
{
    RuleSlot *slot = findSlot(ruleId);
    if (slot == nullptr)
        return false;

    closeEventsOfRule(ruleId);
    destroySlot(slot);
    mSlots.remove(ruleId);
    flushNotifications();
    return true;
}

bool QScadaAlarmEngine::setRuleEnabled(const QString &ruleId, bool enabled)
{
    RuleSlot *slot = findSlot(ruleId);
    if (slot == nullptr)
        return false;
    if (slot->rule.enabled() == enabled)
        return true;

    if (!enabled) {
        // 停用后该规则不再判定，未关闭的报警失去依据 → 由系统关闭（记入历史）。
        // 若留在表里就会变成永远无法关闭的僵尸报警。
        cancelPending(slot);
        closeEventsOfRule(ruleId);
    } else {
        // 重新启用时基线早已过期（中间可能隔了几个班），从这里重新积累，
        // 免得启用瞬间拿旧值算出一个巨大的变化率。
        slot->hasSample = false;
        slot->lastRate = 0.0;
        slot->sampleClock.invalidate();
    }

    slot->rule.setEnabled(enabled);
    flushNotifications();
    return true;
}

QScadaAlarmRule QScadaAlarmEngine::rule(const QString &ruleId) const
{
    RuleSlot *slot = findSlot(ruleId);
    return (slot != nullptr) ? slot->rule : QScadaAlarmRule();
}

QList<QScadaAlarmRule> QScadaAlarmEngine::rules() const
{
    QList<QScadaAlarmRule> list;
    QMap<QString, RuleSlot *>::const_iterator it;
    for (it = mSlots.constBegin(); it != mSlots.constEnd(); ++it) {
        if (it.value() != nullptr)
            list.append(it.value()->rule);
    }
    return list;
}

QList<QScadaAlarmRule> QScadaAlarmEngine::rulesForTag(const QString &tagKey) const
{
    QList<QScadaAlarmRule> list;
    QMap<QString, RuleSlot *>::const_iterator it;
    for (it = mSlots.constBegin(); it != mSlots.constEnd(); ++it) {
        if (it.value() != nullptr && it.value()->rule.tagKey() == tagKey)
            list.append(it.value()->rule);
    }
    return list;
}

int QScadaAlarmEngine::ruleCount() const
{
    return mSlots.size();
}

void QScadaAlarmEngine::clearRules()
{
    destroyAllSlots();
    // 规则全没了，所有未关闭报警都失去判定依据 → 由系统关闭并记入历史，
    // 操作员在报警表里看不到"来历不明且永远关不掉"的条目。
    closeAllOpenEvents();
    flushNotifications();
}

// ---------------------------------------------------------------------------
// 查询
// ---------------------------------------------------------------------------

QList<QScadaAlarmEvent> QScadaAlarmEngine::openAlarms() const
{
    return mOpenEvents;
}

QList<QScadaAlarmEvent> QScadaAlarmEngine::activeAlarms() const
{
    QList<QScadaAlarmEvent> list;
    for (int i = 0; i < mOpenEvents.size(); ++i) {
        if (mOpenEvents.at(i).active())
            list.append(mOpenEvents.at(i));
    }
    return list;
}

QList<QScadaAlarmEvent> QScadaAlarmEngine::unacknowledgedAlarms() const
{
    QList<QScadaAlarmEvent> list;
    for (int i = 0; i < mOpenEvents.size(); ++i) {
        if (!mOpenEvents.at(i).acknowledged())
            list.append(mOpenEvents.at(i));
    }
    return list;
}

QList<QScadaAlarmEvent> QScadaAlarmEngine::alarmsOfTag(const QString &tagKey) const
{
    QList<QScadaAlarmEvent> list;
    for (int i = 0; i < mOpenEvents.size(); ++i) {
        if (mOpenEvents.at(i).tagKey() == tagKey)
            list.append(mOpenEvents.at(i));
    }
    return list;
}

QScadaAlarmEvent QScadaAlarmEngine::findEvent(const QString &eventId, bool *found) const
{
    const int index = indexOfOpenEvent(eventId);
    if (found != nullptr)
        *found = (index >= 0);
    return (index >= 0) ? mOpenEvents.at(index) : QScadaAlarmEvent();
}

int QScadaAlarmEngine::openCount() const
{
    return mOpenEvents.size();
}

int QScadaAlarmEngine::activeCount() const
{
    int count = 0;
    for (int i = 0; i < mOpenEvents.size(); ++i) {
        if (mOpenEvents.at(i).active())
            ++count;
    }
    return count;
}

int QScadaAlarmEngine::unacknowledgedCount() const
{
    int count = 0;
    for (int i = 0; i < mOpenEvents.size(); ++i) {
        if (!mOpenEvents.at(i).acknowledged())
            ++count;
    }
    return count;
}

int QScadaAlarmEngine::countByLevel(QScadaAlarmLevel level, bool onlyActive) const
{
    int count = 0;
    for (int i = 0; i < mOpenEvents.size(); ++i) {
        const QScadaAlarmEvent &event = mOpenEvents.at(i);
        if (event.level() != level)
            continue;
        if (onlyActive && !event.active())
            continue;
        ++count;
    }
    return count;
}

bool QScadaAlarmEngine::hasOpenAlarm(const QString &tagKey) const
{
    for (int i = 0; i < mOpenEvents.size(); ++i) {
        if (mOpenEvents.at(i).tagKey() == tagKey)
            return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// 历史
// ---------------------------------------------------------------------------

void QScadaAlarmEngine::setHistoryLimit(int maxEvents)
{
    mHistoryLimit = (maxEvents > 0) ? maxEvents : 0;
    pruneHistory();
    flushNotifications();
}

int QScadaAlarmEngine::historyLimit() const
{
    return mHistoryLimit;
}

QList<QScadaAlarmEvent> QScadaAlarmEngine::history() const
{
    return mHistory;
}

QList<QScadaAlarmEvent> QScadaAlarmEngine::recentHistory(int maxEvents) const
{
    QList<QScadaAlarmEvent> list;
    if (maxEvents <= 0)
        return list;

    // 历史是时间正序追加的，从尾部往回取就是"最新在前"，界面表格不用再排序。
    for (int i = mHistory.size() - 1; i >= 0 && list.size() < maxEvents; --i)
        list.append(mHistory.at(i));
    return list;
}

QList<QScadaAlarmEvent> QScadaAlarmEngine::historyForTag(const QString &tagKey) const
{
    QList<QScadaAlarmEvent> list;
    for (int i = 0; i < mHistory.size(); ++i) {
        if (mHistory.at(i).tagKey() == tagKey)
            list.append(mHistory.at(i));
    }
    return list;
}

int QScadaAlarmEngine::historyCount() const
{
    return mHistory.size();
}

void QScadaAlarmEngine::clearHistory()
{
    if (mHistory.isEmpty())
        return;
    mHistory.clear();
    mHistoryDirty = true;
    flushNotifications();
}

void QScadaAlarmEngine::setMinRateIntervalMs(int ms)
{
    mMinRateIntervalMs = (ms > 0) ? ms : 1;
}

int QScadaAlarmEngine::minRateIntervalMs() const
{
    return mMinRateIntervalMs;
}

void QScadaAlarmEngine::setRateMaxGapMs(int ms)
{
    mRateMaxGapMs = (ms > 0) ? ms : 1;
}

int QScadaAlarmEngine::rateMaxGapMs() const
{
    return mRateMaxGapMs;
}

// ---------------------------------------------------------------------------
// 采集入口与判定
// ---------------------------------------------------------------------------

void QScadaAlarmEngine::onValueChanged(const QString &tagKey, const QVariant &value)
{
    if (tagKey.isEmpty() || mSlots.isEmpty())
        return;

    /*!
     * 采集失败时驱动会发无效 QVariant。这里必须"什么都不做"，
     * 而不是当成 0、也不能当成正常值：通讯中断期间若把所有报警判定为恢复，
     * 操作员会看到满屏绿色，以为工况已正常——这是报警系统最危险的一类误判，
     * 正确做法是让报警保持在中断前的状态，并在界面上单独提示"通讯中断"。
     */
    if (!value.isValid())
        return;

    bool ok = false;
    const double numeric = value.toDouble(&ok);
    if (!ok)
        return; // 非数值量（字符串、结构体）不参与报警判定

    const QDateTime now = QDateTime::currentDateTime();

    // 同一测点可能挂了多条规则（高高限 / 高限 / 低限 / 变化率），各自独立判定；
    // 通知合并到本次处理末尾统一发一次，界面不会因为一条报文里的多条报警重排多次。
    QMap<QString, RuleSlot *>::const_iterator it;
    for (it = mSlots.constBegin(); it != mSlots.constEnd(); ++it) {
        RuleSlot *slot = it.value();
        if (slot == nullptr || !slot->rule.enabled())
            continue;
        if (slot->rule.tagKey() != tagKey)
            continue;
        evaluateSlot(slot, numeric, now);
    }

    flushNotifications();
}

double QScadaAlarmEngine::updateRate(RuleSlot *slot, double value)
{
    if (!slot->hasSample) {
        slot->hasSample = true;
        slot->lastValue = value;
        slot->lastRate = 0.0;
        slot->sampleClock.start();
        return 0.0;
    }

    const qint64 elapsedMs = slot->sampleClock.elapsed();

    // 时间窗太短：不重新估算，沿用上一次的有效估计。
    if (elapsedMs < mMinRateIntervalMs)
        return slot->lastRate;

    // 间隔过长：中间断过（掉线、停电、软件重启），首尾之差不能当变化率用，
    // 只重建基线。否则"通讯恢复的第一次刷新"会稳定地误报一次变化率报警。
    if (elapsedMs > mRateMaxGapMs) {
        slot->lastValue = value;
        slot->lastRate = 0.0;
        slot->sampleClock.restart();
        return 0.0;
    }

    const double seconds = static_cast<double>(elapsedMs) / 1000.0;
    const double rate = (value - slot->lastValue) / seconds;

    slot->lastValue = value;
    slot->lastRate = rate;
    slot->sampleClock.restart();
    return rate;
}

void QScadaAlarmEngine::evaluateSlot(RuleSlot *slot, double value, const QDateTime &now)
{
    // 变化率对所有规则都算：一条规则随时可能被改成"变化率"类型，
    // 基线必须一直在积累，否则改完类型的头几拍会用 0 或旧值去判定。
    const double rate = updateRate(slot, value);
    const QScadaAlarmRule::Evaluation evaluation = slot->rule.evaluate(value, rate);

    if (slot->inAlarm) {
        // 已经报过警：只关心是否回到安全区（含死区回差）。
        // 值继续恶化不重复产生事件，否则一个持续超温的点一分钟能刷出几百条记录。
        if (slot->rule.isRecovered(value, rate))
            clearEventOfSlot(slot, now);
        return;
    }

    if (!evaluation.exceeded) {
        // 未越限：如果之前正在延时计时，整段作废。
        // "必须持续越限满 delayMs"是延时确认的全部意义，中途恢复不能累计计时。
        cancelPending(slot);
        return;
    }

    if (slot->rule.delayMs() <= 0) {
        // 没配延时（例如变化率报警、联锁类快变量）直接产生。
        raiseEvent(slot, evaluation, now);
        return;
    }

    if (slot->pendingBreach) {
        // 已经在计时中：只刷新候选值，绝不重启计时器。
        // 如果每次越限都重启，信号在阈值附近来回跳时就永远等不到到点，
        // 报警要么永远不来、要么被迫改成"立即报"——两种都失去了滤抖的意义。
        slot->pendingValue = evaluation.value;
        slot->pendingRate = evaluation.rate;
        return;
    }

    slot->pendingBreach = true;
    slot->pendingValue = evaluation.value;
    slot->pendingRate = evaluation.rate;
    slot->pendingSince = now;
    startDelayTimer(slot);
}

void QScadaAlarmEngine::startDelayTimer(RuleSlot *slot)
{
    if (slot->delayTimer == nullptr) {
        // 只给配了延时的规则创建定时器：点表里可能上千条规则，
        // 绝大多数是即时报警，没必要每条都挂一个 QObject。
        slot->delayTimer = new QTimer(this);
        slot->delayTimer->setSingleShot(true);

        const QString ruleId = slot->rule.ruleId();
        // 回调只带规则 id，不带裸指针：计时期间规则被删除时查表自然失败，
        // 不会出现"定时器里拿着已释放的槽"这种悬空指针问题。
        connect(slot->delayTimer, &QTimer::timeout, this, [this, ruleId]() {
            onDelayElapsed(ruleId);
        });
    }

    slot->delayTimer->setInterval(qMax(1, slot->rule.delayMs()));
    slot->delayTimer->start();
}

void QScadaAlarmEngine::cancelPending(RuleSlot *slot)
{
    if (slot->delayTimer != nullptr)
        slot->delayTimer->stop();
    slot->pendingBreach = false;
    slot->pendingValue = 0.0;
    slot->pendingRate = 0.0;
    slot->pendingSince = QDateTime();
}

void QScadaAlarmEngine::onDelayElapsed(const QString &ruleId)
{
    RuleSlot *slot = findSlot(ruleId);
    if (slot == nullptr || !slot->pendingBreach)
        return; // 规则已删除，或计时期间已恢复正常（候选状态已被取消）

    // 到点后用最后一个候选值复核一次：定时器到点与最后一次采样之间值可能已回落，
    // 不复核就会出现"最后一拍已经正常却仍然报警"。
    const QScadaAlarmRule::Evaluation evaluation =
            slot->rule.evaluate(slot->pendingValue, slot->pendingRate);

    if (!evaluation.exceeded) {
        cancelPending(slot);
        return;
    }

    slot->pendingBreach = false;
    raiseEvent(slot, evaluation, QDateTime::currentDateTime());
    flushNotifications();
}

void QScadaAlarmEngine::raiseEvent(RuleSlot *slot,
                                   const QScadaAlarmRule::Evaluation &evaluation,
                                   const QDateTime &now)
{
    if (slot->inAlarm)
        return; // 双保险：一条规则同时只允许存在一条未关闭的报警

    QScadaAlarmEvent event;
    event.setEventId(QScadaAlarmEvent::newEventId());
    event.setRuleId(slot->rule.ruleId());
    event.setTagKey(slot->rule.tagKey());
    event.setLevel(slot->rule.level());
    event.setTriggerValue(evaluation.value);
    event.setLimitValue(evaluation.limit);
    event.setMessage(slot->rule.buildMessage(evaluation.value, evaluation.limit));
    event.setRaisedAt(now);
    event.setActive(true);
    event.setAcknowledged(false);

    slot->eventId = event.eventId();
    slot->inAlarm = true;
    cancelPending(slot); // 延时已经完成（或本来就是即时报警），候选状态不再需要

    mOpenEvents.append(event);
    appendHistory(event);
    mAlarmListDirty = true;

    // 报警必须留日志：现场争议"当时到底报没报"时，日志比界面截图可靠。
    qWarning() << "[QScadaAlarm] 报警产生:" << event.tagKey() << event.message();

    emit alarmRaised(event);
}

void QScadaAlarmEngine::clearEventOfSlot(RuleSlot *slot, const QDateTime &now)
{
    const int index = indexOfOpenEvent(slot->eventId);
    slot->inAlarm = false;
    slot->eventId.clear();

    if (index < 0)
        return;

    QScadaAlarmEvent event = mOpenEvents.at(index);
    event.markCleared(now);

    if (event.isClosed()) {
        // 已经确认过的报警：恢复即闭环，离开活动报警表。
        mOpenEvents.removeAt(index);
    } else {
        // 没人确认过的报警：留在表里显示为"已恢复未确认"，直到操作员确认。
        // 这是 ISA-18.2 的硬要求，也是"闪一下就恢复的报警"不被漏看的唯一办法。
        mOpenEvents[index] = event;
    }

    updateHistory(event);
    mAlarmListDirty = true;

    qWarning() << "[QScadaAlarm] 报警恢复:" << event.tagKey();
    emit alarmCleared(event);
}

void QScadaAlarmEngine::closeEventsOfRule(const QString &ruleId)
{
    if (ruleId.isEmpty())
        return;

    QStringList eventIds;
    for (int i = 0; i < mOpenEvents.size(); ++i) {
        if (mOpenEvents.at(i).ruleId() == ruleId)
            eventIds.append(mOpenEvents.at(i).eventId());
    }

    for (int i = 0; i < eventIds.size(); ++i)
        systemCloseEvent(eventIds.at(i));

    // 规则槽里记录的"当前事件"也要一起清掉，否则规则再次启用时
    // inAlarm 还是 true，新报警永远产生不出来。
    RuleSlot *slot = findSlot(ruleId);
    if (slot != nullptr) {
        slot->eventId.clear();
        slot->inAlarm = false;
    }
}

void QScadaAlarmEngine::closeAllOpenEvents()
{
    QStringList eventIds;
    for (int i = 0; i < mOpenEvents.size(); ++i)
        eventIds.append(mOpenEvents.at(i).eventId());

    for (int i = 0; i < eventIds.size(); ++i)
        systemCloseEvent(eventIds.at(i));
}

void QScadaAlarmEngine::systemCloseEvent(const QString &eventId)
{
    const int index = indexOfOpenEvent(eventId);
    if (index < 0)
        return;

    const QDateTime now = QDateTime::currentDateTime();
    QScadaAlarmEvent event = mOpenEvents.at(index);
    event.markCleared(now);
    event.markAcknowledged(QString::fromLatin1(kSystemOperator), now);

    mOpenEvents.removeAt(index);
    updateHistory(event);
    mAlarmListDirty = true;

    qWarning() << "[QScadaAlarm] 报警因规则变更或引擎复位由系统关闭:" << event.tagKey()
               << event.eventId();

    // 只发 cleared：确认动作是系统代做的，不应触发"操作员已确认"的声音/提示逻辑。
    emit alarmCleared(event);
}

// ---------------------------------------------------------------------------
// 确认
// ---------------------------------------------------------------------------

bool QScadaAlarmEngine::acknowledge(const QString &eventId, const QString &operatorName)
{
    const bool ok = acknowledgeEvent(eventId, operatorName);
    flushNotifications();
    return ok;
}

int QScadaAlarmEngine::acknowledgeAll(const QString &operatorName)
{
    QStringList eventIds;
    for (int i = 0; i < mOpenEvents.size(); ++i) {
        if (!mOpenEvents.at(i).acknowledged())
            eventIds.append(mOpenEvents.at(i).eventId());
    }

    int count = 0;
    for (int i = 0; i < eventIds.size(); ++i) {
        if (acknowledgeEvent(eventIds.at(i), operatorName))
            ++count;
    }

    flushNotifications();
    return count;
}

int QScadaAlarmEngine::acknowledgeTag(const QString &tagKey, const QString &operatorName)
{
    QStringList eventIds;
    for (int i = 0; i < mOpenEvents.size(); ++i) {
        if (mOpenEvents.at(i).tagKey() == tagKey && !mOpenEvents.at(i).acknowledged())
            eventIds.append(mOpenEvents.at(i).eventId());
    }

    int count = 0;
    for (int i = 0; i < eventIds.size(); ++i) {
        if (acknowledgeEvent(eventIds.at(i), operatorName))
            ++count;
    }

    flushNotifications();
    return count;
}

bool QScadaAlarmEngine::acknowledgeEvent(const QString &eventId, const QString &operatorName)
{
    const int index = indexOfOpenEvent(eventId);
    if (index < 0)
        return false;

    QScadaAlarmEvent event = mOpenEvents.at(index);
    if (event.acknowledged())
        return false; // 已确认过：不覆盖第一个确认人和确认时间（审计要求）

    const QString who = operatorName.trimmed().isEmpty()
            ? QString::fromLatin1(kUnnamedOperator)
            : operatorName.trimmed();
    event.markAcknowledged(who, QDateTime::currentDateTime());

    if (event.isClosed()) {
        // 已恢复的报警被确认 → 生命周期结束，离开活动报警表。
        mOpenEvents.removeAt(index);
    } else {
        // 仍在越限的报警被确认 → 只表示"我已经知道了"，值还是越限的，
        // 必须继续留在表里（状态 Acknowledged）直到值真正恢复。
        // 否则操作员一按确认，报警表就空了，而设备还在超温。
        mOpenEvents[index] = event;
    }

    updateHistory(event);
    mAlarmListDirty = true;

    qWarning() << "[QScadaAlarm] 报警确认:" << event.tagKey() << "by" << who;

    emit alarmAcknowledged(event);
    return true;
}

void QScadaAlarmEngine::reset()
{
    // 运行时状态全部作废。不这样做的话，点表/规则重载后的第一次采样会拿
    // "上一份配置下的旧值"去算变化率，直接产生一条假报警。
    QMap<QString, RuleSlot *>::const_iterator it;
    for (it = mSlots.constBegin(); it != mSlots.constEnd(); ++it) {
        RuleSlot *slot = it.value();
        if (slot == nullptr)
            continue;
        cancelPending(slot);
        slot->hasSample = false;
        slot->lastValue = 0.0;
        slot->lastRate = 0.0;
        slot->sampleClock.invalidate();
        slot->eventId.clear();
        slot->inAlarm = false;
    }

    // 所有未关闭报警的判定依据都随复位消失了，由系统补上恢复与确认并关闭；
    // 历史保留（reset 不清历史）——报警记录是要留档的，不能因为一次复位就抹掉。
    closeAllOpenEvents();
    flushNotifications();
}

// ---------------------------------------------------------------------------
// 内部工具
// ---------------------------------------------------------------------------

QScadaAlarmEngine::RuleSlot *QScadaAlarmEngine::findSlot(const QString &ruleId) const
{
    return mSlots.value(ruleId, nullptr);
}

void QScadaAlarmEngine::destroySlot(RuleSlot *slot)
{
    if (slot == nullptr)
        return;

    // 定时器是引擎的子对象；只删槽而不删定时器，删掉的规则还会在到点时被触发一次。
    delete slot->delayTimer;
    delete slot;
}

void QScadaAlarmEngine::destroyAllSlots()
{
    QMap<QString, RuleSlot *>::const_iterator it;
    for (it = mSlots.constBegin(); it != mSlots.constEnd(); ++it)
        destroySlot(it.value());
    mSlots.clear();
}

int QScadaAlarmEngine::indexOfOpenEvent(const QString &eventId) const
{
    if (eventId.isEmpty())
        return -1;

    // 活动报警表通常只有几条到几十条，线性查找足够快；
    // 再维护一张 id→下标 的哈希表，每次删除/裁剪都要同步，
    // 一旦漏同步就是"确认错记录"这种最难查的 bug，不值得。
    for (int i = 0; i < mOpenEvents.size(); ++i) {
        if (mOpenEvents.at(i).eventId() == eventId)
            return i;
    }
    return -1;
}

void QScadaAlarmEngine::appendHistory(const QScadaAlarmEvent &event)
{
    mHistory.append(event);
    pruneHistory();
    mHistoryDirty = true;
}

void QScadaAlarmEngine::updateHistory(const QScadaAlarmEvent &event)
{
    // 从尾部往前找：刚发生状态变化的记录总在末尾附近（历史按时间正序追加）。
    for (int i = mHistory.size() - 1; i >= 0; --i) {
        if (mHistory.at(i).eventId() != event.eventId())
            continue;
        mHistory[i] = event;
        mHistoryDirty = true;
        return;
    }

    // 找不到说明这条记录已经被容量上限裁掉了：不补写，
    // 补写会把新记录插到历史中间、打乱时间顺序。要长期留档应由上层写数据库。
}

void QScadaAlarmEngine::pruneHistory()
{
    if (mHistoryLimit <= 0) {
        if (!mHistory.isEmpty()) {
            mHistory.clear();
            mHistoryDirty = true;
        }
        return;
    }

    if (mHistory.size() <= mHistoryLimit)
        return;

    const int drop = mHistory.size() - mHistoryLimit;
    for (int i = 0; i < drop; ++i)
        mHistory.removeFirst();
    mHistoryDirty = true;
}

void QScadaAlarmEngine::flushNotifications()
{
    if (mAlarmListDirty) {
        mAlarmListDirty = false;
        emit activeAlarmsChanged();
    }
    if (mHistoryDirty) {
        mHistoryDirty = false;
        emit historyChanged();
    }
}
