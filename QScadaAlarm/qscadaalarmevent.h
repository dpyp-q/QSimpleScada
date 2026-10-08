#ifndef QSCADAALARMEVENT_H
#define QSCADAALARMEVENT_H

#include <QString>
#include <QDateTime>
#include <QJsonObject>
#include <QMetaType>

#include "qscadaalarmlevel.h"

/*!
 * \brief 一条报警事件（值语义对象）。
 *
 * 一条事件记录报警从产生到关闭的完整生命周期：谁在什么时刻因为什么值越限而报警、
 * 什么时候恢复正常、谁在什么时候确认。历史库、报警表、MES 上报拿到的都是这个对象，
 * 因此它必须是可拷贝、可序列化的纯数据，不能持有指针或依赖引擎。
 *
 * ## 为什么"是否越限"和"是否确认"必须是两个独立维度
 *
 * 这是 ISA-18.2（报警管理标准）的核心，也是现场事故的直接来源。把两者合并成
 * 一个状态字段（例如"未确认→已确认→已恢复"的单链条）会立刻出现两个问题：
 *
 *  1. 确认 ≠ 故障消失。操作员按下"确认"只是表示"我知道了、我在处理"，
 *     设备可能仍然在超温。如果把确认直接推到"已关闭"，报警表里就查不到这条记录了，
 *     而图元还会变色——界面与报警表互相矛盾，这是最容易被审核挑出来的问题。
 *  2. 恢复 ≠ 已处理。值回到正常范围时若操作员没看到过这条报警，
 *     它必须继续留在报警表里（显示为"已恢复未确认"/RTN unacknowledged），
 *     否则一次短暂的超压可能整个班都没人知道。
 *
 * 因此这里用两个独立布尔量描述状态，状态枚举只是它们的组合投影：
 *
 * | active（仍越限） | acknowledged（已被确认） | state()        | 含义                     |
 * |------------------|--------------------------|----------------|--------------------------|
 * | true             | false                    | Active         | 越限中、未确认，必须处理  |
 * | true             | true                     | Acknowledged   | 越限中、已确认，仍在处理  |
 * | false            | false                    | Cleared        | 已恢复、未确认（RTN）     |
 * | false            | true                     | Closed         | 生命周期结束             |
 *
 * 引擎的"活动报警表"保留条件是 !isClosed()，也就是"仍在越限"或"恢复但没人确认过"
 * 的报警都还在表里，只有"既恢复又确认过"的才离开——这样操作员永远不会漏看一条报警。
 */
class QScadaAlarmEvent
{
public:
    //! 状态枚举：由 active/acknowledged 两个维度推导，不单独存储。
    enum AlarmState {
        Active = 0,     //!< 越限中、未确认
        Cleared,        //!< 已恢复、未确认（RTN unacknowledged，仍需操作员确认）
        Acknowledged,   //!< 越限中、已确认
        Closed          //!< 已恢复且已确认
    };

    QScadaAlarmEvent();

    // ---- 标识与内容 ----
    QString eventId() const { return mEventId; }
    void setEventId(const QString &id) { mEventId = id; }

    QString ruleId() const { return mRuleId; }
    void setRuleId(const QString &id) { mRuleId = id; }

    QString tagKey() const { return mTagKey; }
    void setTagKey(const QString &key) { mTagKey = key; }

    QScadaAlarmLevel level() const { return mLevel; }
    void setLevel(QScadaAlarmLevel level) { mLevel = level; }

    QString message() const { return mMessage; }
    void setMessage(const QString &text) { mMessage = text; }

    /*!
     * 触发值。限值类规则是越限时的测点值；变化率规则是 dv/dt（单位/秒，带正负号）。
     * 之所以变化率规则记速率而不是瞬时值：这条报警的判定依据就是速率，
     * 复盘时"当时值是多少"可以从同期历史数据里取，而速率只能在这里留下。
     */
    double triggerValue() const { return mTriggerValue; }
    void setTriggerValue(double v) { mTriggerValue = v; }

    //! 本次越限对应的阈值（区间外规则只记被撞到的那一侧）。
    double limitValue() const { return mLimitValue; }
    void setLimitValue(double v) { mLimitValue = v; }

    // ---- 时间戳与人 ----
    QDateTime raisedAt() const { return mRaisedAt; }
    void setRaisedAt(const QDateTime &when) { mRaisedAt = when; }

    QDateTime clearedAt() const { return mClearedAt; }
    void setClearedAt(const QDateTime &when) { mClearedAt = when; }

    QDateTime ackedAt() const { return mAckedAt; }
    void setAckedAt(const QDateTime &when) { mAckedAt = when; }

    QString ackedBy() const { return mAckedBy; }
    void setAckedBy(const QString &who) { mAckedBy = who; }

    // ---- 两个独立维度 ----
    bool active() const { return mActive; }
    //! 直接置位只给引擎与历史反序列化用；界面与报表请走 acknowledge 流程。
    void setActive(bool on) { mActive = on; }

    bool acknowledged() const { return mAcknowledged; }
    void setAcknowledged(bool on) { mAcknowledged = on; }

    //! 两个维度投影出来的状态，见类注释里的状态表。
    AlarmState state() const;
    //! 当前状态的稳定英文标识（active/cleared/acknowledged/closed），用于存储。
    QString stateText() const { return stateName(state()); }
    //! 当前状态的中文文案，用于界面显示。
    QString stateDisplayText() const;

    //! 已经走完生命周期（既恢复又确认），可以从活动报警表里移除。
    bool isClosed() const { return !mActive && mAcknowledged; }
    //! 还在活动报警表里（未关闭）。
    bool isOpen() const { return !isClosed(); }
    bool isUnacknowledged() const { return !mAcknowledged; }
    //! 已恢复但没人确认过，界面需要特殊标记（例如"恢复未确认"闪烁）。
    bool isReturnedToNormalUnacknowledged() const { return (!mActive && !mAcknowledged); }

    /*!
     * 事件对象是否是一条真实记录。默认构造出来的对象 eventId 为空，
     * 它的 active/acknowledged 没有业务含义，调用方用这个函数区分"没查到"和"查到了"。
     */
    bool isValid() const { return !mEventId.isEmpty(); }

    // ---- 状态迁移（幂等，重复调用不会破坏已有记录）----
    /*!
     * 标记为已恢复。只在第一次有效时记录 clearedAt：
     * 报警反复越限/恢复时，这条事件只会被关闭一次，时间戳必须是最早那次恢复的时间，
     * 否则历史的持续时间会被后来的抖动不断改写。
     */
    void markCleared(const QDateTime &when);
    //! 标记为已确认。重复确认不会覆盖第一个确认人和确认时间（审计要求）。
    void markAcknowledged(const QString &operatorName, const QDateTime &when);

    //! 持续时长（毫秒）：已恢复的按 raisedAt→clearedAt 算，仍在越限的算到当前时刻。
    qint64 durationMs() const;
    //! 时长的可读文本，例如 "1 分 20 秒"。
    QString durationText() const;

    // ---- 序列化（历史库 / MES 上报 / 报警记录导出）----
    QJsonObject toJson() const;
    static QScadaAlarmEvent fromJson(const QJsonObject &obj);

    static QString stateName(AlarmState state);
    static AlarmState stateFromName(const QString &name);

    //! 生成新的事件 id（UUID，去花括号），保证跨重启、跨机器都不重复。
    static QString newEventId();

private:
    QString mEventId;
    QString mRuleId;
    QString mTagKey;
    QScadaAlarmLevel mLevel;
    QString mMessage;
    double mTriggerValue;
    double mLimitValue;
    QDateTime mRaisedAt;
    QDateTime mClearedAt;
    QDateTime mAckedAt;
    QString mAckedBy;

    // 状态判定的唯一依据是这两个布尔量，而不是"clearedAt 是否有效"之类的推断：
    // 时间戳要经过时区转换、数据库存取、外部补录，一旦被写成 1970 或空值，
    // 整个报警表的状态就会跟着错乱。时间戳只负责记录，不负责表达状态。
    bool mActive;
    bool mAcknowledged;
};

Q_DECLARE_METATYPE(QScadaAlarmEvent)

#endif // QSCADAALARMEVENT_H
