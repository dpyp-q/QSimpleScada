#ifndef QSCADAALARMENGINE_H
#define QSCADAALARMENGINE_H

#include <QObject>
#include <QString>
#include <QVariant>
#include <QList>
#include <QMap>
#include <QDateTime>
#include <QMetaType>

#include "qscadaalarmrule.h"
#include "qscadaalarmevent.h"

class QTimer;

/*!
 * \brief 报警判定引擎：把采集层的值变化翻译成报警的产生 / 恢复 / 确认。
 *
 * ## 定位
 * 引擎是纯逻辑层，不认识 Modbus、也不认识界面。它与采集层的唯一耦合点就是
 * 槽 onValueChanged(QString, QVariant)，签名与 QScadaDataSource::valueChanged
 * 完全一致，因此直接 connect 即可；换协议、换驱动都不需要改这里一行代码。
 *
 * ## 线程模型（很重要）
 * 引擎是单线程对象：规则表、活动报警表、历史都在引擎所属线程里被读写，不加锁。
 * 采集驱动在自己的线程里发信号，Qt 在跨线程时自动改用队列连接，
 * 于是所有判定都排在引擎线程的事件循环里串行执行——不存在竞态。
 * 反过来说，界面不能拿另一个线程去直接调 rules() / acknowledge()，
 * 需要跨线程时用 QMetaObject::invokeMethod 投递到引擎线程。
 *
 * ## 两条判定路径（每条规则独立）
 *  - 产生路径：值越限 → （delayMs > 0 时启动单次 QTimer 计时）→ 计时到点仍越限才产生报警。
 *    计时期间值回到正常范围就取消计时（整段作废），这是滤掉瞬时尖峰的关键；
 *    已计时期间再次越限绝不重启计时器，否则持续抖动的信号永远等不到确认。
 *  - 恢复路径：已产生报警 → 值回到"阈值 ∓ 死区"的安全区才算恢复。
 *    恢复但未确认的报警仍留在活动报警表里（状态 Cleared），直到有人确认才关闭。
 *
 * 每条规则同时最多只有一条未关闭的报警：值继续恶化不会重复产生新事件，
 * 否则一个持续超温的点会在一分钟内刷出几百条记录，报警系统就废了。
 */
class QScadaAlarmEngine : public QObject
{
    Q_OBJECT
public:
    explicit QScadaAlarmEngine(QObject *parent = nullptr);
    ~QScadaAlarmEngine();

    // ---- 规则管理 ----
    /*!
     * 添加规则。规则无效（见 QScadaAlarmRule::isValid）或 id 已存在时返回 false，
     * 并保持原有配置不变——现场改错一个阈值不应该把已有报警规则弄丢。
     */
    bool addRule(const QScadaAlarmRule &rule);
    /*!
     * 更新已有规则（界面改阈值走这里）。
     * 已产生的报警不会被改写：报警记录是审计依据，事后改配置不能篡改历史。
     * 但如果换了目标位号，该规则未关闭的报警会由系统关闭（判定依据已经不存在了）。
     */
    bool updateRule(const QScadaAlarmRule &rule);
    /*!
     * 删除规则。规则本身消失后其报警再无判定依据，因此未关闭的报警由系统关闭
     * （记入历史、署名 system），避免报警表里留下永远无法关闭的僵尸报警。
     */
    bool removeRule(const QString &ruleId);
    bool setRuleEnabled(const QString &ruleId, bool enabled);
    QScadaAlarmRule rule(const QString &ruleId) const;
    QList<QScadaAlarmRule> rules() const;
    QList<QScadaAlarmRule> rulesForTag(const QString &tagKey) const;
    int ruleCount() const;
    //! 清空全部规则（同样会由系统关闭这些规则未关闭的报警）。
    void clearRules();

    // ---- 活动报警表 ----
    /*!
     * 未关闭的报警：仍在越限的 + 已恢复但未确认的。
     * 报警条、报警表用这一份数据，操作员因此不会漏看"闪一下就恢复"的报警。
     */
    QList<QScadaAlarmEvent> openAlarms() const;
    //! 仅仍在越限的报警（true == active()），用于"当前工况"类统计。
    QList<QScadaAlarmEvent> activeAlarms() const;
    QList<QScadaAlarmEvent> unacknowledgedAlarms() const;
    QList<QScadaAlarmEvent> alarmsOfTag(const QString &tagKey) const;

    //! 按事件 id 查未关闭的报警；未找到时 found 置 false。
    QScadaAlarmEvent findEvent(const QString &eventId, bool *found = nullptr) const;

    int openCount() const;
    int activeCount() const;
    int unacknowledgedCount() const;
    //! 统计某一等级未关闭的报警数；onlyActive 为 true 时只数仍在越限的。
    int countByLevel(QScadaAlarmLevel level, bool onlyActive = false) const;
    bool hasOpenAlarm(const QString &tagKey) const;

    // ---- 报警历史 ----
    /*!
     * 历史容量上限，默认 1000 条。超限后从最旧的开始丢弃。
     * 为什么用"上限 + 裁剪"而不是全存：上位机常年在现场无人值守运行，
     * 内存必须有界；需要长期留档的场景应该由上层把事件写进数据库（MES / SQL Server），
     * 引擎只保证"最近发生了什么"可以随时查到。
     */
    void setHistoryLimit(int maxEvents);
    int historyLimit() const;

    //! 全部历史，按时间正序（最旧在前）。已恢复/已确认的记录是更新后的完整生命周期。
    QList<QScadaAlarmEvent> history() const;
    //! 最近的 maxEvents 条，按时间倒序（最新在前），给界面表格直接用。
    QList<QScadaAlarmEvent> recentHistory(int maxEvents) const;
    QList<QScadaAlarmEvent> historyForTag(const QString &tagKey) const;
    int historyCount() const;
    void clearHistory();

    /*!
     * 变化率计算的最小时间窗，默认 50 ms。
     * 两次采样间隔小于它时不更新变化率估计（沿用上一次），否则 dt→0 会把
     * 毫伏级噪声放大成"每秒几千"的假变化率。采集周期很短的测试台可以调小。
     */
    void setMinRateIntervalMs(int ms);
    int minRateIntervalMs() const;

    /*!
     * 变化率基线的最大有效间隔，默认 30000 ms。
     * 超过它说明中间断过（通讯中断、软件重启、点表重载），首尾两点之差反映的是
     * "断线期间累计的变化"而不是变化率，此时只重建基线、不产生变化率报警。
     */
    void setRateMaxGapMs(int ms);
    int rateMaxGapMs() const;

public slots:
    //! 采集层入口，签名与 QScadaDataSource::valueChanged 一致，可直接连接。
    void onValueChanged(const QString &tagKey, const QVariant &value);

    /*!
     * 确认一条报警。已确认过的返回 false（不覆盖第一个确认人与时间）。
     * 确认不等于故障消失：仍在越限的报警确认后状态变成 Acknowledged，
     * 依旧留在活动报警表里；已恢复的报警确认后状态变成 Closed 并离开报警表。
     */
    bool acknowledge(const QString &eventId, const QString &operatorName);
    //! 确认全部未确认报警，返回确认成功的条数。
    int acknowledgeAll(const QString &operatorName);
    //! 按位号确认（例如只处理某个工段的报警），返回确认成功的条数。
    int acknowledgeTag(const QString &tagKey, const QString &operatorName);

    /*!
     * 复位运行时状态：停掉所有延时计时、清空候选值与变化率基线，
     * 并把所有未关闭报警按"系统关闭"处理（记入历史、署名 system）。
     * 使用场景：点表/规则整体重载、通讯长时间中断后重新建立基线、
     * 或者调试完把报警表清干净。
     * 规则与历史都不会被清空——报警记录是要留档的，不能因为一次复位就抹掉。
     */
    void reset();

signals:
    /*!
     * 报警产生 / 恢复 / 被确认。事件对象按值传递，跨线程队列连接也能安全投递
     * （构造函数里已注册元类型，否则运行时会报 "Cannot queue arguments of type"）。
     */
    void alarmRaised(const QScadaAlarmEvent &event);
    void alarmCleared(const QScadaAlarmEvent &event);
    void alarmAcknowledged(const QScadaAlarmEvent &event);

    /*!
     * 活动报警表发生变化（产生、恢复、确认、复位）。
     * 一次采集可能同时产生/恢复多条报警，因此引擎把通知合并到本次处理的末尾再发一次，
     * 界面不会因为一条采集报文里的 5 条报警而重排 5 次表格。
     */
    void activeAlarmsChanged();
    //! 历史新增或被更新（界面上的历史表格据此刷新）。
    void historyChanged();

private:
    struct RuleSlot;

    RuleSlot *findSlot(const QString &ruleId) const;
    void destroySlot(RuleSlot *slot);
    void destroyAllSlots();

    //! 维护该规则的变化率基线并返回本次的变化率（单位/秒）。
    double updateRate(RuleSlot *slot, double value);
    void evaluateSlot(RuleSlot *slot, double value, const QDateTime &now);

    void startDelayTimer(RuleSlot *slot);
    void cancelPending(RuleSlot *slot);
    void onDelayElapsed(const QString &ruleId);

    void raiseEvent(RuleSlot *slot, const QScadaAlarmRule::Evaluation &evaluation,
                    const QDateTime &now);
    void clearEventOfSlot(RuleSlot *slot, const QDateTime &now);

    //! 确认一条报警但不发合并通知（批量确认时用，避免 N 次 activeAlarmsChanged）。
    bool acknowledgeEvent(const QString &eventId, const QString &operatorName);
    //! 系统代操作员关闭一条报警：补上恢复与确认，署名 system，并记入历史。
    void systemCloseEvent(const QString &eventId);
    //! 规则被删除/停用/改点位时，把它的未关闭报警按系统关闭处理。
    void closeEventsOfRule(const QString &ruleId);
    void closeAllOpenEvents();

    int indexOfOpenEvent(const QString &eventId) const;
    void appendHistory(const QScadaAlarmEvent &event);
    void updateHistory(const QScadaAlarmEvent &event);
    void pruneHistory();
    void flushNotifications();

    QMap<QString, RuleSlot *> mSlots;          //!< ruleId -> 该规则的运行时状态
    QList<QScadaAlarmEvent> mOpenEvents;       //!< 活动报警表（未关闭）
    QList<QScadaAlarmEvent> mHistory;          //!< 报警历史，时间正序，超限裁剪
    int mHistoryLimit;
    int mMinRateIntervalMs;
    int mRateMaxGapMs;
    bool mAlarmListDirty;
    bool mHistoryDirty;
};

#endif // QSCADAALARMENGINE_H
