/*!
 * \file tst_qscadaalarm.cpp
 * \brief 报警引擎单元测试。
 *
 * 为什么测这三块（都是面试/现场最容易被追问、又最适合用测试锁住的行为）：
 *
 *  1. **规则判定与死区**——产生侧必须用配置的原阈值（点位验收的触发点要精确），
 *     恢复侧才让出死区宽度。这两条混在一起是报警系统最常见的实现错误，
 *     用测试锁死"触发点 = 阈值、恢复点 = 阈值 ∓ 死区"。
 *  2. **事件状态机**——ISA-18.2 的双维度状态（越限 / 确认）是审计与界面一致性的根基；
 *     状态迁移必须幂等（重复确认不能覆盖第一个操作人，重复恢复不能改写最早恢复时间）。
 *  3. **引擎判定路径**——延时确认的"计时不重启 + 到点复核"、系统关闭的
 *     僵尸报警治理、无效值不误判恢复、历史有界裁剪、变化率断线后只重建基线。
 *     这些是报警系统"上线一周不被操作员关掉"的工程底线。
 *
 * 测试全部基于 QtTest，不依赖任何硬件、GUI 或数据库，可在 CI 上跑。
 */

#include <QtTest>
#include <QSignalSpy>
#include <QJsonObject>
#include <QDateTime>

#include "../../QScadaAlarm/qscadaalarmengine.h"
#include "../../QScadaAlarm/qscadaalarmrule.h"
#include "../../QScadaAlarm/qscadaalarmevent.h"
#include "../../QScadaAlarm/qscadaalarmlevel.h"

class TestQScadaAlarm : public QObject
{
    Q_OBJECT

private slots:
    // ---------------- 规则：判定与死区 ----------------
    void ruleDefaultsAreSane();
    void ruleGreaterThanTriggersAtThreshold();
    void ruleEvaluateIgnoresDeadband();
    void ruleLessThanVariants();
    void ruleOutOfRangeReportsHitSide();
    void ruleRateOfChangeUsesMagnitudeWithSign();
    void ruleIsRecoveredAppliesDeadband();
    void ruleOutOfRangeRecoveryZone();
    void ruleNegativeDeadbandTreatedAsZero();
    void ruleValidityChecks();
    void ruleMessagePlaceholdersReplaced();
    void ruleMessageDescriptionReplacedLast();
    void ruleDefaultMessageTemplates();
    void ruleJsonRoundTrip();
    void ruleLevelAndComparisonNames();
    void ruleSetRuleIdGeneratesWhenEmpty();

    // ---------------- 事件：状态机 ----------------
    void eventStateProjectionMatrix();
    void eventClosedOnlyWhenBoth();
    void eventDefaultIsInvalid();
    void eventMarkClearedIdempotent();
    void eventMarkAcknowledgedKeepsFirst();
    void eventDurationRaisedToCleared();
    void eventDurationWhileActiveCountsToNow();
    void eventJsonRoundTrip();
    void eventJsonAcceptsStateOnly();
    void eventStateNameRoundTrip();

    // ---------------- 引擎：判定与生命周期 ----------------
    void engineRejectsInvalidRule();
    void engineDuplicateRuleIdRejected();
    void engineImmediateRaise();
    void engineNormalValueSilent();
    void engineRecoveryNeedsSafetyZone();
    void engineClearedUnacknowledgedStaysOpen();
    void engineAcknowledgeMarksAndIdempotent();
    void engineAcknowledgeAllCounts();
    void engineAcknowledgeTagScopes();
    void engineDelayRaiseAfterElapsed();
    void engineDelayCanceledOnRecovery();
    void engineNoDuplicateWhilePersistent();
    void engineRemoveRuleSystemCloses();
    void engineDisableRuleSystemCloses();
    void engineResetClosesAllOpen();
    void engineInvalidVariantIgnored();
    void engineHistoryBounded();
    void engineRateChangeRaiseAndRecover();
    void engineRateBaselineRebuildAfterGap();
    void engineNotificationsCoalesced();

private:
    // 便捷构造：默认启用、无死区、无延时的即时报警规则
    static QScadaAlarmRule makeRule(const QString &tagKey,
                                    QScadaAlarmRule::ComparisonType type,
                                    QScadaAlarmLevel level,
                                    double threshold)
    {
        QScadaAlarmRule rule(tagKey, type, level, threshold);
        return rule;
    }
};

// =====================================================================
// 规则：判定与死区
// =====================================================================

void TestQScadaAlarm::ruleDefaultsAreSane()
{
    const QScadaAlarmRule rule;
    QVERIFY(!rule.ruleId().isEmpty());          // 默认构造自动生成 id
    QCOMPARE(rule.comparison(), QScadaAlarmRule::GreaterThan);
    QCOMPARE(rule.level(), QScadaAlarmLevels::Warning);
    QVERIFY(rule.enabled());
    QCOMPARE(rule.delayMs(), 0);                // 默认立即报警
    QCOMPARE(rule.deadband(), 0.0);
    QVERIFY(rule.tagKey().isEmpty());
}

void TestQScadaAlarm::ruleGreaterThanTriggersAtThreshold()
{
    // 高限 85：严格大于才触发
    QScadaAlarmRule rule("T1", QScadaAlarmRule::GreaterThan,
                         QScadaAlarmLevels::High, 85.0);
    QVERIFY(!rule.evaluate(85.0).exceeded);
    QVERIFY(!rule.evaluate(84.99).exceeded);
    QVERIFY(rule.evaluate(85.01).exceeded);
    // 撞线时 limit 必须带上正确的阈值，供 {limit} 占位符与事件记录使用
    QCOMPARE(rule.evaluate(90.0).limit, 85.0);

    // 含等于的变体：85 直接触发
    QScadaAlarmRule ge("T2", QScadaAlarmRule::GreaterThanOrEqual,
                       QScadaAlarmLevels::High, 85.0);
    QVERIFY(ge.evaluate(85.0).exceeded);
}

void TestQScadaAlarm::ruleEvaluateIgnoresDeadband()
{
    // 产生侧刻意不含死区：触发点必须等于配置阈值（点位验收的硬要求）
    QScadaAlarmRule rule("T1", QScadaAlarmRule::GreaterThan,
                         QScadaAlarmLevels::High, 85.0);
    rule.setDeadband(5.0);
    QVERIFY(!rule.evaluate(85.0).exceeded);
    QVERIFY(rule.evaluate(85.1).exceeded);
}

void TestQScadaAlarm::ruleLessThanVariants()
{
    QScadaAlarmRule lt("T1", QScadaAlarmRule::LessThan,
                       QScadaAlarmLevels::High, 10.0);
    QVERIFY(lt.evaluate(9.9).exceeded);
    QVERIFY(!lt.evaluate(10.0).exceeded);

    QScadaAlarmRule le("T2", QScadaAlarmRule::LessThanOrEqual,
                       QScadaAlarmLevels::High, 10.0);
    QVERIFY(le.evaluate(10.0).exceeded);
    QVERIFY(!le.evaluate(10.1).exceeded);
}

void TestQScadaAlarm::ruleOutOfRangeReportsHitSide()
{
    // 区间外规则有两个限值，必须报告"这次撞的是哪一侧"
    QScadaAlarmRule rule("T1", QScadaAlarmRule::OutOfRange,
                         QScadaAlarmLevels::High, 0.0);
    rule.setLowLimit(0.0);
    rule.setHighLimit(10.0);

    const QScadaAlarmRule::Evaluation low = rule.evaluate(-1.0);
    QVERIFY(low.exceeded);
    QCOMPARE(low.limit, 0.0);          // 撞下限报下限

    const QScadaAlarmRule::Evaluation high = rule.evaluate(11.0);
    QVERIFY(high.exceeded);
    QCOMPARE(high.limit, 10.0);        // 撞上限报上限

    QVERIFY(!rule.evaluate(5.0).exceeded);
}

void TestQScadaAlarm::ruleRateOfChangeUsesMagnitudeWithSign()
{
    QScadaAlarmRule rule("T1", QScadaAlarmRule::RateOfChange,
                         QScadaAlarmLevels::Warning, 1.0);

    // 判定用绝对值：上升和下降都算越限
    QVERIFY(rule.evaluate(0.0, 3.0).exceeded);
    QVERIFY(rule.evaluate(0.0, -3.0).exceeded);
    QVERIFY(!rule.evaluate(0.0, 0.5).exceeded);
    QVERIFY(!rule.evaluate(0.0, -0.5).exceeded);

    // 但 Evaluation.rate 保留符号：工艺上"快速升温"和"快速降温"是两类故障
    QCOMPARE(rule.evaluate(0.0, -3.0).rate, -3.0);
    // 变化率规则里参与比较的量是 dv/dt 而不是瞬时值
    QCOMPARE(rule.evaluate(42.0, -3.0).value, -3.0);
}

void TestQScadaAlarm::ruleIsRecoveredAppliesDeadband()
{
    // 恢复必须回落到"阈值 - 死区"以下，只退回阈值一点点不算恢复
    QScadaAlarmRule rule("T1", QScadaAlarmRule::GreaterThan,
                         QScadaAlarmLevels::High, 85.0);
    rule.setDeadband(5.0);

    QVERIFY(!rule.isRecovered(81.0));   // 81 > 80，仍在"抖动区"
    QVERIFY(rule.isRecovered(80.0));    // 80 == 85-5，回到安全区
    QVERIFY(rule.isRecovered(50.0));
}

void TestQScadaAlarm::ruleOutOfRangeRecoveryZone()
{
    QScadaAlarmRule rule("T1", QScadaAlarmRule::OutOfRange,
                         QScadaAlarmLevels::High, 0.0);
    rule.setLowLimit(0.0);
    rule.setHighLimit(10.0);
    rule.setDeadband(1.0);

    // 可恢复区是 [low+db, high-db] = [1, 9]
    QVERIFY(!rule.isRecovered(0.5));
    QVERIFY(rule.isRecovered(1.0));
    QVERIFY(rule.isRecovered(9.0));
    QVERIFY(!rule.isRecovered(9.5));
}

void TestQScadaAlarm::ruleNegativeDeadbandTreatedAsZero()
{
    // 死区负值按 0 处理（配置防呆）
    QScadaAlarmRule rule("T1", QScadaAlarmRule::GreaterThan,
                         QScadaAlarmLevels::High, 85.0);
    rule.setDeadband(-3.0);

    // 死区 0：恢复线 = 阈值本身
    QVERIFY(!rule.isRecovered(85.1));
    QVERIFY(rule.isRecovered(85.0));
}

void TestQScadaAlarm::ruleValidityChecks()
{
    // 空位号：无效
    QScadaAlarmRule noTag("", QScadaAlarmRule::GreaterThan,
                          QScadaAlarmLevels::High, 85.0);
    QVERIFY(!noTag.isValid());

    // 区间外规则：上限必须严格大于下限，否则区间为空、任何值都报警
    QScadaAlarmRule badRange("T1", QScadaAlarmRule::OutOfRange,
                             QScadaAlarmLevels::High, 0.0);
    badRange.setLowLimit(10.0);
    badRange.setHighLimit(5.0);
    QVERIFY(!badRange.isValid());

    // 变化率规则：阈值必须为正，配 0 会把任何噪声都算成越限
    QScadaAlarmRule zeroRate("T1", QScadaAlarmRule::RateOfChange,
                             QScadaAlarmLevels::High, 0.0);
    QVERIFY(!zeroRate.isValid());

    // 限值类规则允许阈值为 0（"液位不得低于 0"是合法配置）
    QScadaAlarmRule ok("T1", QScadaAlarmRule::GreaterThan,
                       QScadaAlarmLevels::High, 0.0);
    QVERIFY(ok.isValid());
}

void TestQScadaAlarm::ruleMessagePlaceholdersReplaced()
{
    QScadaAlarmRule rule("T1", QScadaAlarmRule::GreaterThan,
                         QScadaAlarmLevels::High, 85.0);
    rule.setUnit(QStringLiteral("℃"));
    rule.setDescription(QStringLiteral("主轴过热"));
    rule.setMessageTemplate(QStringLiteral("TAG={tag} VAL={value} LIM={limit} "
                                           "UNIT={unit} LVL={level} DESC={description}"));

    const QString message = rule.buildMessage(88.5, 85.0);
    QVERIFY(message.contains(QStringLiteral("TAG=T1")));
    QVERIFY(message.contains(QStringLiteral("VAL=88.5")));
    QVERIFY(message.contains(QStringLiteral("LIM=85")));
    QVERIFY(message.contains(QStringLiteral("UNIT=℃")));
    QVERIFY(message.contains(QStringLiteral("LVL=高限")));
    QVERIFY(message.contains(QStringLiteral("DESC=主轴过热")));
}

void TestQScadaAlarm::ruleMessageDescriptionReplacedLast()
{
    // 占位符替换顺序：{description} 最后替换，描述文本里写的别的占位符
    // 不会被二次替换——否则一条带 "{tag}" 字样的描述会变成真实位号，误导复盘。
    QScadaAlarmRule rule("T1", QScadaAlarmRule::GreaterThan,
                         QScadaAlarmLevels::High, 85.0);
    rule.setDescription(QStringLiteral("参考位号 {tag} 的说明"));
    rule.setMessageTemplate(QStringLiteral("D={description}"));

    const QString message = rule.buildMessage(88.5, 85.0);
    QCOMPARE(message, QStringLiteral("D=参考位号 {tag} 的说明"));
}

void TestQScadaAlarm::ruleDefaultMessageTemplates()
{
    // 默认模板要自带位号与限值，现场少配一项也能看懂报警内容
    QScadaAlarmRule rule("T1", QScadaAlarmRule::GreaterThan,
                         QScadaAlarmLevels::High, 85.0);
    rule.setUnit(QStringLiteral("℃"));
    const QString message = rule.buildMessage(88.5, 85.0);
    QVERIFY(message.contains(QStringLiteral("T1")));
    QVERIFY(message.contains(QStringLiteral("85")));
    QVERIFY(message.contains(QStringLiteral("超过高限")));
}

void TestQScadaAlarm::ruleJsonRoundTrip()
{
    QScadaAlarmRule rule("T1", QScadaAlarmRule::OutOfRange,
                         QScadaAlarmLevels::Critical, 0.0);
    rule.setRuleId(QStringLiteral("rule-abc"));
    rule.setDescription(QStringLiteral("区间报警"));
    rule.setLowLimit(0.0);
    rule.setHighLimit(10.0);
    rule.setDeadband(1.5);
    rule.setDelayMs(200);
    rule.setUnit(QStringLiteral("A"));
    rule.setEnabled(false);
    rule.setMessageTemplate(QStringLiteral("自定义 {tag}"));

    const QScadaAlarmRule restored = QScadaAlarmRule::fromJson(rule.toJson());
    QCOMPARE(restored.ruleId(), rule.ruleId());
    QCOMPARE(restored.tagKey(), rule.tagKey());
    QCOMPARE(restored.description(), rule.description());
    QCOMPARE(restored.level(), rule.level());
    QCOMPARE(restored.enabled(), rule.enabled());
    QCOMPARE(restored.comparison(), rule.comparison());
    QCOMPARE(restored.threshold(), rule.threshold());
    QCOMPARE(restored.lowLimit(), rule.lowLimit());
    QCOMPARE(restored.highLimit(), rule.highLimit());
    QCOMPARE(restored.deadband(), rule.deadband());
    QCOMPARE(restored.delayMs(), rule.delayMs());
    QCOMPARE(restored.unit(), rule.unit());
    QCOMPARE(restored.messageTemplate(), rule.messageTemplate());
}

void TestQScadaAlarm::ruleLevelAndComparisonNames()
{
    // 稳定英文标识（存配置/数据库）与中文显示名（界面）分离
    QCOMPARE(QScadaAlarmLevels::levelName(QScadaAlarmLevels::Critical),
             QStringLiteral("critical"));
    QCOMPARE(QScadaAlarmLevels::levelDisplayName(QScadaAlarmLevels::High),
             QStringLiteral("高限"));

    // 解析兼容大小写、别名与中文，且忽略首尾空格——点表经常是人手写的
    QCOMPARE(QScadaAlarmLevels::levelFromName(QStringLiteral("Critical")),
             QScadaAlarmLevels::Critical);
    QCOMPARE(QScadaAlarmLevels::levelFromName(QStringLiteral(" 严重 ")),
             QScadaAlarmLevels::Critical);
    QCOMPARE(QScadaAlarmLevels::levelFromName(QStringLiteral("warn")),
             QScadaAlarmLevels::Warning);
    bool ok = true;
    QCOMPARE(QScadaAlarmLevels::levelFromName(QStringLiteral("未知"), &ok),
             QScadaAlarmLevels::Info);
    QVERIFY(!ok);   // 识别失败必须上报，不能悄悄退回默认值

    // 比较类型双向映射
    QCOMPARE(QScadaAlarmRule::comparisonName(QScadaAlarmRule::RateOfChange),
             QStringLiteral("rateOfChange"));
    QCOMPARE(QScadaAlarmRule::comparisonFromName(QStringLiteral("outOfRange")),
             QScadaAlarmRule::OutOfRange);
}

void TestQScadaAlarm::ruleSetRuleIdGeneratesWhenEmpty()
{
    QScadaAlarmRule rule;
    const QString original = rule.ruleId();
    // 空 / 全空白 id 会被自动替换，保证"按 id 查找"不会拿着空键去查
    rule.setRuleId(QStringLiteral("   "));
    QVERIFY(!rule.ruleId().isEmpty());
    QVERIFY(rule.ruleId() != original);
}

// =====================================================================
// 事件：状态机
// =====================================================================

void TestQScadaAlarm::eventStateProjectionMatrix()
{
    // ISA-18.2 双维度投影：两个独立布尔量组合出四态
    QScadaAlarmEvent e;

    e.setActive(true);
    e.setAcknowledged(false);
    QCOMPARE(e.state(), QScadaAlarmEvent::Active);

    e.setActive(true);
    e.setAcknowledged(true);
    QCOMPARE(e.state(), QScadaAlarmEvent::Acknowledged);

    e.setActive(false);
    e.setAcknowledged(false);
    QCOMPARE(e.state(), QScadaAlarmEvent::Cleared);

    e.setActive(false);
    e.setAcknowledged(true);
    QCOMPARE(e.state(), QScadaAlarmEvent::Closed);
}

void TestQScadaAlarm::eventClosedOnlyWhenBoth()
{
    QScadaAlarmEvent e;
    e.setActive(false);
    e.setAcknowledged(true);

    QVERIFY(e.isClosed());
    QVERIFY(!e.isOpen());
    // "已恢复未确认"（RTN unacknowledged）必须在报警表里单独标记
    QScadaAlarmEvent rtn;
    rtn.setActive(false);
    rtn.setAcknowledged(false);
    QVERIFY(rtn.isOpen());
    QVERIFY(rtn.isReturnedToNormalUnacknowledged());
    QVERIFY(rtn.isUnacknowledged());
}

void TestQScadaAlarm::eventDefaultIsInvalid()
{
    // 默认构造对象 eventId 为空：调用方用 isValid 区分"没查到"和"查到了"
    const QScadaAlarmEvent e;
    QVERIFY(!e.isValid());
    QScadaAlarmEvent real;
    real.setEventId(QStringLiteral("evt-1"));
    QVERIFY(real.isValid());
}

void TestQScadaAlarm::eventMarkClearedIdempotent()
{
    QScadaAlarmEvent e;
    e.setEventId(QStringLiteral("evt-1"));
    e.setRaisedAt(QDateTime::fromMSecsSinceEpoch(1000));
    e.setActive(true);

    const QDateTime cleared = QDateTime::fromMSecsSinceEpoch(2000);
    e.markCleared(cleared);
    QCOMPARE(e.clearedAt(), cleared);
    QVERIFY(!e.active());

    // 反复越限/恢复时只应记录最早那次恢复时间，后续抖动不能改写历史持续时间
    e.markCleared(QDateTime::fromMSecsSinceEpoch(9000));
    QCOMPARE(e.clearedAt(), cleared);
}

void TestQScadaAlarm::eventMarkAcknowledgedKeepsFirst()
{
    QScadaAlarmEvent e;
    e.setEventId(QStringLiteral("evt-1"));

    const QDateTime first = QDateTime::fromMSecsSinceEpoch(3000);
    e.markAcknowledged(QStringLiteral("张三"), first);
    QCOMPARE(e.ackedBy(), QStringLiteral("张三"));
    QCOMPARE(e.ackedAt(), first);

    // 重复确认不覆盖第一个确认人与时间（审计要求）
    e.markAcknowledged(QStringLiteral("李四"), QDateTime::fromMSecsSinceEpoch(5000));
    QCOMPARE(e.ackedBy(), QStringLiteral("张三"));
    QCOMPARE(e.ackedAt(), first);
}

void TestQScadaAlarm::eventDurationRaisedToCleared()
{
    QScadaAlarmEvent e;
    e.setRaisedAt(QDateTime::fromMSecsSinceEpoch(1000));
    e.setClearedAt(QDateTime::fromMSecsSinceEpoch(4000));
    QCOMPARE(e.durationMs(), qint64(3000));
    QCOMPARE(e.durationText(), QStringLiteral("3 秒"));
}

void TestQScadaAlarm::eventDurationWhileActiveCountsToNow()
{
    // 仍在越限的事件：持续时长算到当前时刻
    QScadaAlarmEvent e;
    e.setRaisedAt(QDateTime::currentDateTime().addMSecs(-100));
    QVERIFY(e.durationMs() >= 100);

    e.setRaisedAt(QDateTime::currentDateTime().addMSecs(-200));
    QVERIFY(e.durationMs() >= 200);
}

void TestQScadaAlarm::eventJsonRoundTrip()
{
    QScadaAlarmEvent e;
    e.setEventId(QStringLiteral("evt-1"));
    e.setRuleId(QStringLiteral("rule-1"));
    e.setTagKey(QStringLiteral("T1"));
    e.setLevel(QScadaAlarmLevels::High);
    e.setMessage(QStringLiteral("位号 T1 超限"));
    e.setTriggerValue(88.5);
    e.setLimitValue(85.0);
    e.setRaisedAt(QDateTime::fromMSecsSinceEpoch(1000));
    e.setClearedAt(QDateTime::fromMSecsSinceEpoch(2000));
    e.setAckedAt(QDateTime::fromMSecsSinceEpoch(3000));
    e.setAckedBy(QStringLiteral("张三"));
    e.setActive(false);
    e.setAcknowledged(true);

    const QScadaAlarmEvent restored = QScadaAlarmEvent::fromJson(e.toJson());
    QCOMPARE(restored.eventId(), e.eventId());
    QCOMPARE(restored.ruleId(), e.ruleId());
    QCOMPARE(restored.tagKey(), e.tagKey());
    QCOMPARE(restored.level(), e.level());
    QCOMPARE(restored.message(), e.message());
    QCOMPARE(restored.triggerValue(), e.triggerValue());
    QCOMPARE(restored.limitValue(), e.limitValue());
    QCOMPARE(restored.raisedAt(), e.raisedAt());
    QCOMPARE(restored.clearedAt(), e.clearedAt());
    QCOMPARE(restored.ackedAt(), e.ackedAt());
    QCOMPARE(restored.ackedBy(), e.ackedBy());
    QCOMPARE(restored.active(), e.active());
    QCOMPARE(restored.acknowledged(), e.acknowledged());
    QCOMPARE(restored.state(), e.state());
}

void TestQScadaAlarm::eventJsonAcceptsStateOnly()
{
    // 兼容只存了 state 字段的外部记录（例如别的系统写进 MES 的数据），
    // 按状态表反推出两个维度，避免老数据加载后状态全变成"越限中"。
    QJsonObject active;
    active.insert(QStringLiteral("state"), QStringLiteral("active"));
    QScadaAlarmEvent fromActive = QScadaAlarmEvent::fromJson(active);
    QVERIFY(fromActive.active());
    QVERIFY(!fromActive.acknowledged());
    QCOMPARE(fromActive.state(), QScadaAlarmEvent::Active);

    QJsonObject closed;
    closed.insert(QStringLiteral("state"), QStringLiteral("closed"));
    QScadaAlarmEvent fromClosed = QScadaAlarmEvent::fromJson(closed);
    QVERIFY(!fromClosed.active());
    QVERIFY(fromClosed.acknowledged());
    QCOMPARE(fromClosed.state(), QScadaAlarmEvent::Closed);
}

void TestQScadaAlarm::eventStateNameRoundTrip()
{
    QCOMPARE(QScadaAlarmEvent::stateName(QScadaAlarmEvent::Cleared),
             QStringLiteral("cleared"));
    QCOMPARE(QScadaAlarmEvent::stateFromName(QStringLiteral("acknowledged")),
             QScadaAlarmEvent::Acknowledged);
}

// =====================================================================
// 引擎：判定与生命周期
// =====================================================================

void TestQScadaAlarm::engineRejectsInvalidRule()
{
    QScadaAlarmEngine engine;
    QSignalSpy raised(&engine, &QScadaAlarmEngine::alarmRaised);

    // 空位号规则必须被拒绝，而不是产生一堆空报警
    QVERIFY(!engine.addRule(makeRule(QString(), QScadaAlarmRule::GreaterThan,
                                     QScadaAlarmLevels::High, 85.0)));
    // 变化率阈值 0 同样拒绝
    QVERIFY(!engine.addRule(makeRule(QStringLiteral("T1"),
                                     QScadaAlarmRule::RateOfChange,
                                     QScadaAlarmLevels::High, 0.0)));
    QCOMPARE(engine.ruleCount(), 0);
    QCOMPARE(raised.count(), 0);
}

void TestQScadaAlarm::engineDuplicateRuleIdRejected()
{
    QScadaAlarmEngine engine;
    QScadaAlarmRule a = makeRule(QStringLiteral("T1"),
                                 QScadaAlarmRule::GreaterThan,
                                 QScadaAlarmLevels::High, 85.0);
    QVERIFY(engine.addRule(a));

    // 相同 ruleId 再添加必须拒绝，且不能破坏已有配置
    QScadaAlarmRule b = a;
    b.setTagKey(QStringLiteral("T2"));
    QVERIFY(!engine.addRule(b));
    QCOMPARE(engine.ruleCount(), 1);
    QCOMPARE(engine.rules().first().tagKey(), QStringLiteral("T1"));
}

void TestQScadaAlarm::engineImmediateRaise()
{
    QScadaAlarmEngine engine;
    QSignalSpy raised(&engine, &QScadaAlarmEngine::alarmRaised);

    QVERIFY(engine.addRule(makeRule(QStringLiteral("T1"),
                                    QScadaAlarmRule::GreaterThan,
                                    QScadaAlarmLevels::High, 85.0)));

    engine.onValueChanged(QStringLiteral("T1"), 88.5);

    QCOMPARE(raised.count(), 1);
    const QScadaAlarmEvent event =
            raised.at(0).at(0).value<QScadaAlarmEvent>();
    QCOMPARE(event.tagKey(), QStringLiteral("T1"));
    QCOMPARE(event.level(), QScadaAlarmLevels::High);
    QCOMPARE(event.triggerValue(), 88.5);
    QCOMPARE(event.limitValue(), 85.0);
    QVERIFY(event.active());
    QVERIFY(!event.acknowledged());

    QCOMPARE(engine.openCount(), 1);
    QCOMPARE(engine.activeCount(), 1);
    QCOMPARE(engine.unacknowledgedCount(), 1);
    QCOMPARE(engine.historyCount(), 1);   // 产生即入历史
}

void TestQScadaAlarm::engineNormalValueSilent()
{
    QScadaAlarmEngine engine;
    QSignalSpy raised(&engine, &QScadaAlarmEngine::alarmRaised);

    QVERIFY(engine.addRule(makeRule(QStringLiteral("T1"),
                                    QScadaAlarmRule::GreaterThan,
                                    QScadaAlarmLevels::High, 85.0)));
    engine.onValueChanged(QStringLiteral("T1"), 50.0);
    engine.onValueChanged(QStringLiteral("T1"), 84.99);

    QCOMPARE(raised.count(), 0);
    QCOMPARE(engine.openCount(), 0);
}

void TestQScadaAlarm::engineRecoveryNeedsSafetyZone()
{
    QScadaAlarmEngine engine;
    QSignalSpy raised(&engine, &QScadaAlarmEngine::alarmRaised);
    QSignalSpy cleared(&engine, &QScadaAlarmEngine::alarmCleared);

    QScadaAlarmRule rule = makeRule(QStringLiteral("T1"),
                                    QScadaAlarmRule::GreaterThan,
                                    QScadaAlarmLevels::High, 85.0);
    rule.setDeadband(5.0);
    QVERIFY(engine.addRule(rule));

    engine.onValueChanged(QStringLiteral("T1"), 95.0);   // 报警
    QCOMPARE(raised.count(), 1);

    // 82 已回到阈值以下，但还在抖动区（>85-5=80），不许恢复
    engine.onValueChanged(QStringLiteral("T1"), 82.0);
    QCOMPARE(cleared.count(), 0);
    QCOMPARE(engine.activeCount(), 1);

    // 80 进入安全区，恢复
    engine.onValueChanged(QStringLiteral("T1"), 80.0);
    QCOMPARE(cleared.count(), 1);
    QCOMPARE(engine.activeCount(), 0);
}

void TestQScadaAlarm::engineClearedUnacknowledgedStaysOpen()
{
    QScadaAlarmEngine engine;
    QSignalSpy acked(&engine, &QScadaAlarmEngine::alarmAcknowledged);

    QVERIFY(engine.addRule(makeRule(QStringLiteral("T1"),
                                    QScadaAlarmRule::GreaterThan,
                                    QScadaAlarmLevels::High, 85.0)));
    engine.onValueChanged(QStringLiteral("T1"), 95.0);   // 报警
    engine.onValueChanged(QStringLiteral("T1"), 50.0);   // 恢复

    // 已恢复但没人确认：必须留在报警表里（ISA-18.2），状态为 Cleared
    QCOMPARE(engine.openCount(), 1);
    bool found = false;
    const QScadaAlarmEvent event = engine.findEvent(
                engine.openAlarms().first().eventId(), &found);
    QVERIFY(found);
    QCOMPARE(event.state(), QScadaAlarmEvent::Cleared);
    QVERIFY(event.isReturnedToNormalUnacknowledged());

    // 确认后生命周期结束，离开报警表
    const int ackedCount = engine.acknowledge(event.eventId(), QStringLiteral("张三"));
    QCOMPARE(ackedCount, 1);
    QCOMPARE(acked.count(), 1);
    QCOMPARE(engine.openCount(), 0);
    // 历史里保留的是更新后的完整生命周期（Closed + 确认人）
    QCOMPARE(engine.history().first().state(), QScadaAlarmEvent::Closed);
    QCOMPARE(engine.history().first().ackedBy(), QStringLiteral("张三"));
}

void TestQScadaAlarm::engineAcknowledgeMarksAndIdempotent()
{
    QScadaAlarmEngine engine;
    QSignalSpy acked(&engine, &QScadaAlarmEngine::alarmAcknowledged);

    QVERIFY(engine.addRule(makeRule(QStringLiteral("T1"),
                                    QScadaAlarmRule::GreaterThan,
                                    QScadaAlarmLevels::High, 85.0)));
    engine.onValueChanged(QStringLiteral("T1"), 95.0);
    const QString eventId = engine.openAlarms().first().eventId();

    QVERIFY(engine.acknowledge(eventId, QStringLiteral("张三")));
    QCOMPARE(acked.count(), 1);
    QCOMPARE(engine.openAlarms().first().ackedBy(), QStringLiteral("张三"));
    QCOMPARE(engine.openAlarms().first().state(), QScadaAlarmEvent::Acknowledged);

    // 确认不等于故障消失：仍在越限时确认后继续留在报警表
    QCOMPARE(engine.openCount(), 1);

    // 重复确认返回 false，不覆盖第一个确认人
    QVERIFY(!engine.acknowledge(eventId, QStringLiteral("李四")));
    QCOMPARE(engine.openAlarms().first().ackedBy(), QStringLiteral("张三"));
}

void TestQScadaAlarm::engineAcknowledgeAllCounts()
{
    QScadaAlarmEngine engine;
    QVERIFY(engine.addRule(makeRule(QStringLiteral("T1"),
                                    QScadaAlarmRule::GreaterThan,
                                    QScadaAlarmLevels::High, 85.0)));
    QVERIFY(engine.addRule(makeRule(QStringLiteral("T2"),
                                    QScadaAlarmRule::LessThan,
                                    QScadaAlarmLevels::High, 10.0)));
    engine.onValueChanged(QStringLiteral("T1"), 95.0);
    engine.onValueChanged(QStringLiteral("T2"), 1.0);

    QCOMPARE(engine.openCount(), 2);
    QCOMPARE(engine.acknowledgeAll(QStringLiteral("张三")), 2);
    QCOMPARE(engine.unacknowledgedCount(), 0);
    QCOMPARE(engine.acknowledgeAll(QStringLiteral("张三")), 0);   // 无未确认可确认
}

void TestQScadaAlarm::engineAcknowledgeTagScopes()
{
    QScadaAlarmEngine engine;
    QVERIFY(engine.addRule(makeRule(QStringLiteral("TA"),
                                    QScadaAlarmRule::GreaterThan,
                                    QScadaAlarmLevels::High, 85.0)));
    QVERIFY(engine.addRule(makeRule(QStringLiteral("TB"),
                                    QScadaAlarmRule::GreaterThan,
                                    QScadaAlarmLevels::High, 85.0)));
    engine.onValueChanged(QStringLiteral("TA"), 95.0);
    engine.onValueChanged(QStringLiteral("TB"), 95.0);
    QCOMPARE(engine.openCount(), 2);

    // 只确认指定工段的位号
    QCOMPARE(engine.acknowledgeTag(QStringLiteral("TA"), QStringLiteral("张三")), 1);
    QVERIFY(engine.alarmsOfTag(QStringLiteral("TA")).first().acknowledged());
    QVERIFY(!engine.alarmsOfTag(QStringLiteral("TB")).first().acknowledged());
}

void TestQScadaAlarm::engineDelayRaiseAfterElapsed()
{
    QScadaAlarmEngine engine;
    QSignalSpy raised(&engine, &QScadaAlarmEngine::alarmRaised);

    QScadaAlarmRule rule = makeRule(QStringLiteral("T1"),
                                    QScadaAlarmRule::GreaterThan,
                                    QScadaAlarmLevels::High, 85.0);
    rule.setDelayMs(100);
    QVERIFY(engine.addRule(rule));

    // 越限瞬间不报警（延时确认中）
    engine.onValueChanged(QStringLiteral("T1"), 95.0);
    QCOMPARE(raised.count(), 0);
    QCOMPARE(engine.openCount(), 0);

    // 延时到点后仍越限 → 产生报警
    QVERIFY(raised.wait(1500));
    QCOMPARE(raised.count(), 1);
    QCOMPARE(engine.openCount(), 1);
}

void TestQScadaAlarm::engineDelayCanceledOnRecovery()
{
    QScadaAlarmEngine engine;
    QSignalSpy raised(&engine, &QScadaAlarmEngine::alarmRaised);

    QScadaAlarmRule rule = makeRule(QStringLiteral("T1"),
                                    QScadaAlarmRule::GreaterThan,
                                    QScadaAlarmLevels::High, 85.0);
    rule.setDelayMs(100);
    QVERIFY(engine.addRule(rule));

    engine.onValueChanged(QStringLiteral("T1"), 95.0);   // 开始延时计时
    QTest::qWait(30);
    engine.onValueChanged(QStringLiteral("T1"), 50.0);   // 恢复 → 整段作废

    QTest::qWait(300);                                   // 远超延时也不报警
    QCOMPARE(raised.count(), 0);
    QCOMPARE(engine.openCount(), 0);
}

void TestQScadaAlarm::engineNoDuplicateWhilePersistent()
{
    QScadaAlarmEngine engine;
    QSignalSpy raised(&engine, &QScadaAlarmEngine::alarmRaised);

    QVERIFY(engine.addRule(makeRule(QStringLiteral("T1"),
                                    QScadaAlarmRule::GreaterThan,
                                    QScadaAlarmLevels::High, 85.0)));
    engine.onValueChanged(QStringLiteral("T1"), 95.0);
    engine.onValueChanged(QStringLiteral("T1"), 96.0);   // 持续恶化
    engine.onValueChanged(QStringLiteral("T1"), 97.0);

    // 一条规则同时最多一条未关闭报警：持续超温不刷屏
    QCOMPARE(raised.count(), 1);
    QCOMPARE(engine.openCount(), 1);
}

void TestQScadaAlarm::engineRemoveRuleSystemCloses()
{
    QScadaAlarmEngine engine;
    QSignalSpy cleared(&engine, &QScadaAlarmEngine::alarmCleared);

    QScadaAlarmRule ruleA = makeRule(QStringLiteral("TA"),
                                     QScadaAlarmRule::GreaterThan,
                                     QScadaAlarmLevels::High, 85.0);
    QScadaAlarmRule ruleB = makeRule(QStringLiteral("TB"),
                                     QScadaAlarmRule::GreaterThan,
                                     QScadaAlarmLevels::High, 85.0);
    QVERIFY(engine.addRule(ruleA));
    QVERIFY(engine.addRule(ruleB));
    engine.onValueChanged(QStringLiteral("TA"), 95.0);
    engine.onValueChanged(QStringLiteral("TB"), 95.0);
    QCOMPARE(engine.openCount(), 2);

    // 删除规则：其名下未关闭报警失去判定依据，由系统关闭（署名 system）
    QVERIFY(engine.removeRule(ruleA.ruleId()));
    QCOMPARE(cleared.count(), 1);
    QCOMPARE(engine.openCount(), 1);
    QVERIFY(engine.openAlarms().first().tagKey() == QStringLiteral("TB"));

    const QList<QScadaAlarmEvent> taHistory = engine.historyForTag(QStringLiteral("TA"));
    QCOMPARE(taHistory.size(), 1);
    QCOMPARE(taHistory.first().ackedBy(), QStringLiteral("system"));
    QCOMPARE(taHistory.first().state(), QScadaAlarmEvent::Closed);
}

void TestQScadaAlarm::engineDisableRuleSystemCloses()
{
    QScadaAlarmEngine engine;
    QSignalSpy cleared(&engine, &QScadaAlarmEngine::alarmCleared);

    QScadaAlarmRule rule = makeRule(QStringLiteral("T1"),
                                    QScadaAlarmRule::GreaterThan,
                                    QScadaAlarmLevels::High, 85.0);
    QVERIFY(engine.addRule(rule));
    engine.onValueChanged(QStringLiteral("T1"), 95.0);
    QCOMPARE(engine.openCount(), 1);

    // 停用规则：未关闭报警同样系统关闭，不留下僵尸报警
    QVERIFY(engine.setRuleEnabled(rule.ruleId(), false));
    QCOMPARE(cleared.count(), 1);
    QCOMPARE(engine.openCount(), 0);
    QCOMPARE(engine.history().first().ackedBy(), QStringLiteral("system"));

    // 重新启用后不再有旧事件，可再次报警
    QVERIFY(engine.setRuleEnabled(rule.ruleId(), true));
    engine.onValueChanged(QStringLiteral("T1"), 95.0);
    QCOMPARE(engine.openCount(), 1);
}

void TestQScadaAlarm::engineResetClosesAllOpen()
{
    QScadaAlarmEngine engine;
    QSignalSpy cleared(&engine, &QScadaAlarmEngine::alarmCleared);

    QVERIFY(engine.addRule(makeRule(QStringLiteral("T1"),
                                    QScadaAlarmRule::GreaterThan,
                                    QScadaAlarmLevels::High, 85.0)));
    QVERIFY(engine.addRule(makeRule(QStringLiteral("T2"),
                                    QScadaAlarmRule::GreaterThan,
                                    QScadaAlarmLevels::High, 85.0)));
    engine.onValueChanged(QStringLiteral("T1"), 95.0);
    engine.onValueChanged(QStringLiteral("T2"), 95.0);
    QCOMPARE(engine.openCount(), 2);

    engine.reset();

    // 全部未关闭报警按系统关闭处理，规则与历史保留
    QCOMPARE(cleared.count(), 2);
    QCOMPARE(engine.openCount(), 0);
    QCOMPARE(engine.ruleCount(), 2);
    QCOMPARE(engine.historyCount(), 2);
}

void TestQScadaAlarm::engineInvalidVariantIgnored()
{
    QScadaAlarmEngine engine;
    QSignalSpy raised(&engine, &QScadaAlarmEngine::alarmRaised);
    QSignalSpy cleared(&engine, &QScadaAlarmEngine::alarmCleared);

    QVERIFY(engine.addRule(makeRule(QStringLiteral("T1"),
                                    QScadaAlarmRule::GreaterThan,
                                    QScadaAlarmLevels::High, 85.0)));

    // 通讯中断时驱动发无效 QVariant：不能当成 0 也不当正常值
    engine.onValueChanged(QStringLiteral("T1"), QVariant());
    QCOMPARE(raised.count(), 0);
    QCOMPARE(engine.openCount(), 0);

    // 关键场景：报警中通讯中断 → 保持报警状态，绝不误判恢复
    engine.onValueChanged(QStringLiteral("T1"), 95.0);
    QCOMPARE(raised.count(), 1);
    engine.onValueChanged(QStringLiteral("T1"), QVariant());
    QCOMPARE(cleared.count(), 0);
    QCOMPARE(engine.activeCount(), 1);   // 仍是"越限中"，等通讯恢复后由真实值判定
}

void TestQScadaAlarm::engineHistoryBounded()
{
    QScadaAlarmEngine engine;
    engine.setHistoryLimit(2);

    QVERIFY(engine.addRule(makeRule(QStringLiteral("TA"),
                                    QScadaAlarmRule::GreaterThan,
                                    QScadaAlarmLevels::High, 85.0)));
    QVERIFY(engine.addRule(makeRule(QStringLiteral("TB"),
                                    QScadaAlarmRule::GreaterThan,
                                    QScadaAlarmLevels::High, 85.0)));
    QVERIFY(engine.addRule(makeRule(QStringLiteral("TC"),
                                    QScadaAlarmRule::GreaterThan,
                                    QScadaAlarmLevels::High, 85.0)));
    engine.onValueChanged(QStringLiteral("TA"), 95.0);
    engine.onValueChanged(QStringLiteral("TB"), 95.0);
    engine.onValueChanged(QStringLiteral("TC"), 95.0);

    // 上限 + 裁剪：超限从最旧的开始丢
    QCOMPARE(engine.historyCount(), 2);
    QVERIFY(engine.history().first().tagKey() == QStringLiteral("TB"));
    QVERIFY(engine.history().last().tagKey() == QStringLiteral("TC"));
}

void TestQScadaAlarm::engineRateChangeRaiseAndRecover()
{
    QScadaAlarmEngine engine;
    QSignalSpy raised(&engine, &QScadaAlarmEngine::alarmRaised);
    QSignalSpy cleared(&engine, &QScadaAlarmEngine::alarmCleared);

    QVERIFY(engine.addRule(makeRule(QStringLiteral("RATE"),
                                    QScadaAlarmRule::RateOfChange,
                                    QScadaAlarmLevels::Warning, 1.0)));

    engine.onValueChanged(QStringLiteral("RATE"), 0.0);   // 第一次采样：建立基线
    QTest::qWait(80);                                     // 超过最小时间窗
    engine.onValueChanged(QStringLiteral("RATE"), 200.0); // 跳变 → 变化率巨大 → 报警

    QCOMPARE(raised.count(), 1);
    const QScadaAlarmEvent event = raised.at(0).at(0).value<QScadaAlarmEvent>();
    // triggerValue 记的是变化率（带方向），不是瞬时值
    QVERIFY(event.triggerValue() > 1000.0);

    // 值稳住后变化率回落 → 恢复
    QTest::qWait(80);
    engine.onValueChanged(QStringLiteral("RATE"), 200.0);
    QCOMPARE(cleared.count(), 1);
}

void TestQScadaAlarm::engineRateBaselineRebuildAfterGap()
{
    QScadaAlarmEngine engine;
    QSignalSpy raised(&engine, &QScadaAlarmEngine::alarmRaised);

    QScadaAlarmRule rule = makeRule(QStringLiteral("RATE2"),
                                    QScadaAlarmRule::RateOfChange,
                                    QScadaAlarmLevels::Warning, 1.0);
    QVERIFY(engine.addRule(rule));
    engine.setRateMaxGapMs(100);

    engine.onValueChanged(QStringLiteral("RATE2"), 0.0);   // 基线
    QTest::qWait(150);                                     // 间隔超过最大有效间隔（模拟断线）
    engine.onValueChanged(QStringLiteral("RATE2"), 500.0); // 断线后的第一次刷新

    // 首尾之差反映的是"断线期间累计的变化"，不是变化率 → 只重建基线，不误报
    QCOMPARE(raised.count(), 0);

    QTest::qWait(80);
    engine.onValueChanged(QStringLiteral("RATE2"), 505.0); // 正常采样：变化率大 → 报警
    QCOMPARE(raised.count(), 1);
}

void TestQScadaAlarm::engineNotificationsCoalesced()
{
    QScadaAlarmEngine engine;
    QSignalSpy raised(&engine, &QScadaAlarmEngine::alarmRaised);
    QSignalSpy changed(&engine, &QScadaAlarmEngine::activeAlarmsChanged);

    // 同一测点挂两条规则（高高限 / 高限）
    QScadaAlarmRule high = makeRule(QStringLiteral("T1"),
                                    QScadaAlarmRule::GreaterThan,
                                    QScadaAlarmLevels::High, 85.0);
    QScadaAlarmRule critical = makeRule(QStringLiteral("T1"),
                                        QScadaAlarmRule::GreaterThan,
                                        QScadaAlarmLevels::Critical, 90.0);
    QVERIFY(engine.addRule(high));
    QVERIFY(engine.addRule(critical));

    // 一条报文触发两条报警：alarmRaised 逐条发，activeAlarmsChanged 只发一次
    engine.onValueChanged(QStringLiteral("T1"), 95.0);
    QCOMPARE(raised.count(), 2);
    QCOMPARE(changed.count(), 1);
    QCOMPARE(engine.openCount(), 2);
}

QTEST_MAIN(TestQScadaAlarm)
#include "tst_qscadaalarm.moc"
