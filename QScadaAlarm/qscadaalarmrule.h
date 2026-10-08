#ifndef QSCADAALARMRULE_H
#define QSCADAALARMRULE_H

#include <QString>
#include <QJsonObject>
#include <QMetaType>

#include "qscadaalarmlevel.h"

/*!
 * \brief 一条报警规则：把某个位号的数值按某种比较方式判定成报警。
 *
 * 职责边界：规则只描述"怎么判"，不保存"现在判到哪了"。是否已越限、
 * 延时计时走到哪、变化率基线是多少，全部由 QScadaAlarmEngine 运行时维护。
 * 这样做的好处是规则对象可以随便拷贝——配置对话框改一半、JSON 读写、
 * 界面列表显示都直接用值语义，不会出现"两个地方各存一份状态对不上"。
 *
 * 两个现场必备的滤抖手段（不做这两个，报警系统上线一周就会被操作员关掉）：
 *  - deadband（死区）：恢复判定用的是"阈值 ∓ 死区"，而不是同一根阈值线。
 *    信号在阈值附近抖动时会反复产生/恢复，报警表疯狂刷屏。
 *  - delayMs（延时确认）：越限必须持续满 delayMs 才真正产生报警，
 *    用来滤掉电机启动电流冲击、阀门换向瞬间的压力波动这类瞬时尖峰。
 */
class QScadaAlarmRule
{
public:
    /*!
     * 比较类型。
     * GreaterThan/LessThan 系列用 threshold；OutOfRange 用 lowLimit/highLimit；
     * RateOfChange 用 threshold，含义是"每秒允许的最大变化量"。
     */
    enum ComparisonType {
        GreaterThan = 0,      //!< 高限：value > threshold
        GreaterThanOrEqual,   //!< 高限（含等于）
        LessThan,             //!< 低限：value < threshold
        LessThanOrEqual,      //!< 低限（含等于）
        OutOfRange,           //!< 区间外：value < lowLimit 或 value > highLimit
        RateOfChange          //!< 变化率：|dv/dt| > threshold（单位/秒）
    };

    /*!
     * 一次判定的结果。
     * 把"是否越限"和"越的是哪一条限"一起返回，是为了让 {limit} 占位符、
     * 事件里的 limitValue 都能拿到正确的阈值：OutOfRange 规则有两个限，
     * 只有判定函数自己知道这次是撞了上限还是下限。
     */
    struct Evaluation {
        bool exceeded;   //!< 是否越限（不含死区，死区只用于恢复判定）
        double limit;    //!< 本次越限对应的阈值
        double value;    //!< 参与比较的量：一般规则是测点值，变化率规则是 dv/dt
        double rate;     //!< 变化率（单位/秒），非变化率规则也会算出来备用

        Evaluation()
            : exceeded(false), limit(0.0), value(0.0), rate(0.0) {}
    };

    QScadaAlarmRule();
    QScadaAlarmRule(const QString &tagKey, ComparisonType type,
                    QScadaAlarmLevel level, double threshold);

    // ---- 标识 ----
    QString ruleId() const { return mRuleId; }
    //! 传空字符串表示"自动生成一个"；从配置文件恢复时应设置成文件里的原值。
    void setRuleId(const QString &id);

    QString tagKey() const { return mTagKey; }
    void setTagKey(const QString &key) { mTagKey = key; }

    QString description() const { return mDescription; }
    void setDescription(const QString &text) { mDescription = text; }

    // ---- 判定条件 ----
    QScadaAlarmLevel level() const { return mLevel; }
    void setLevel(QScadaAlarmLevel level) { mLevel = level; }

    bool enabled() const { return mEnabled; }
    void setEnabled(bool on) { mEnabled = on; }

    ComparisonType comparison() const { return mComparison; }
    void setComparison(ComparisonType type) { mComparison = type; }

    double threshold() const { return mThreshold; }
    void setThreshold(double v) { mThreshold = v; }

    double lowLimit() const { return mLowLimit; }
    void setLowLimit(double v) { mLowLimit = v; }

    double highLimit() const { return mHighLimit; }
    void setHighLimit(double v) { mHighLimit = v; }

    //! 死区（回差），必须非负；负值会被当成 0 使用。
    double deadband() const { return mDeadband; }
    void setDeadband(double v) { mDeadband = v; }

    //! 延时确认时间（毫秒），0 表示立即报警。
    int delayMs() const { return mDelayMs; }
    void setDelayMs(int ms) { mDelayMs = ms; }

    // ---- 文本 ----
    //! 报警文本模板，支持 {tag} {value} {limit} {unit} {level} {description} 占位符。
    QString messageTemplate() const { return mMessageTemplate; }
    void setMessageTemplate(const QString &text) { mMessageTemplate = text; }

    //! 工程单位（如 "℃"、"A"），只用于拼报警文本；留空则不显示单位。
    QString unit() const { return mUnit; }
    void setUnit(const QString &u) { mUnit = u; }

    // ---- 判定逻辑（纯函数，不修改自身）----

    /*!
     * 判定当前是否越限。刻意不含死区：死区只参与"恢复"判定，
     * 如果把死区加到产生侧，报警的实际触发点就会偏离配置的阈值，
     * 点位验收时会被判定为不合格。
     * \param ratePerSecond 变化率（单位/秒），非变化率规则可传 0。
     */
    Evaluation evaluate(double value, double ratePerSecond = 0.0) const;

    /*!
     * 越限后是否已经回到"可以恢复"的区域（已应用死区）。
     * 只有它返回 true 才允许恢复报警；返回 false 期间报警一直保持。
     */
    bool isRecovered(double value, double ratePerSecond = 0.0) const;

    //! 该规则是否需要变化率（引擎据此决定要不要维护差分基线）。
    bool needsRate() const { return mComparison == RateOfChange; }

    /*!
     * 配置是否可用。无效规则被 addRule 拒绝而不是产生一堆空报警：
     * 例如区间外规则把上限配得比下限还低、高限规则忘了填阈值。
     */
    bool isValid() const;

    //! 按模板生成报警文本；模板为空时用 defaultMessageTemplate()。
    QString buildMessage(double value, double limit) const;

    //! 各种比较类型对应的默认中文模板（现场少配一项也能看懂报警内容）。
    QString defaultMessageTemplate() const;

    // ---- 序列化 ----
    QJsonObject toJson() const;
    static QScadaAlarmRule fromJson(const QJsonObject &obj);

    //! 比较类型与稳定字符串的互转（用于 JSON 与界面下拉框）。
    static QString comparisonName(ComparisonType type);
    static ComparisonType comparisonFromName(const QString &name);

    //! 生成一个新的规则 id（UUID，去花括号）。
    static QString newRuleId();

private:
    QString mRuleId;
    QString mTagKey;
    QString mDescription;
    QScadaAlarmLevel mLevel;
    bool mEnabled;
    ComparisonType mComparison;
    double mThreshold;
    double mLowLimit;
    double mHighLimit;
    double mDeadband;
    int mDelayMs;
    QString mMessageTemplate;
    QString mUnit;
};

Q_DECLARE_METATYPE(QScadaAlarmRule)

#endif // QSCADAALARMRULE_H
