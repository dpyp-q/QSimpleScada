#include "qscadaalarmrule.h"

#include <QJsonValue>
#include <QRegularExpression>
#include <QUuid>
#include <QtGlobal>

/*!
 * 数值格式化：报警文本里出现的数不应该带一串无意义的小数（"12.000 ℃"），
 * 也不应该把 0.25 显示成 0（"值 0 超过高限 0"这种记录事后根本没法复盘）。
 * 因此整数值按整数显示，其余保留 3 位并去掉末尾的 0。
 */
static QString formatNumber(double value)
{
    if (!qIsFinite(value))
        return QStringLiteral("-");

    if (qAbs(value) < 1.0e15) {
        const double rounded = static_cast<double>(qRound64(value));
        if (qAbs(value - rounded) < 1.0e-9)
            return QString::number(rounded, 'f', 0);
    }

    QString text = QString::number(value, 'f', 3);
    while (text.endsWith(QLatin1Char('0')))
        text.chop(1);
    if (text.endsWith(QLatin1Char('.')))
        text.chop(1);
    return text;
}

QScadaAlarmRule::QScadaAlarmRule()
    : mLevel(QScadaAlarmLevels::Warning)
    , mEnabled(true)
    , mComparison(GreaterThan)
    , mThreshold(0.0)
    , mLowLimit(0.0)
    , mHighLimit(0.0)
    , mDeadband(0.0)
    , mDelayMs(0)
{
    mRuleId = newRuleId();
}

QScadaAlarmRule::QScadaAlarmRule(const QString &tagKey, ComparisonType type,
                                 QScadaAlarmLevel level, double threshold)
    : mTagKey(tagKey)
    , mLevel(level)
    , mEnabled(true)
    , mComparison(type)
    , mThreshold(threshold)
    , mLowLimit(0.0)
    , mHighLimit(0.0)
    , mDeadband(0.0)
    , mDelayMs(0)
{
    mRuleId = newRuleId();
}

void QScadaAlarmRule::setRuleId(const QString &id)
{
    // 空 id 会让"按 id 找规则/找事件"全部失效，所以宁可自动补一个也不留空。
    mRuleId = id.trimmed().isEmpty() ? newRuleId() : id.trimmed();
}

QScadaAlarmRule::Evaluation QScadaAlarmRule::evaluate(double value, double ratePerSecond) const
{
    Evaluation result;
    result.value = value;
    result.rate = ratePerSecond;

    switch (mComparison) {
    case GreaterThan:
        result.limit = mThreshold;
        result.exceeded = (value > mThreshold);
        break;
    case GreaterThanOrEqual:
        result.limit = mThreshold;
        result.exceeded = (value >= mThreshold);
        break;
    case LessThan:
        result.limit = mThreshold;
        result.exceeded = (value < mThreshold);
        break;
    case LessThanOrEqual:
        result.limit = mThreshold;
        result.exceeded = (value <= mThreshold);
        break;
    case OutOfRange:
        // 两个限值里只报被撞到的那一个，{limit} 与 limitValue 才有意义。
        if (value < mLowLimit) {
            result.exceeded = true;
            result.limit = mLowLimit;
        } else if (value > mHighLimit) {
            result.exceeded = true;
            result.limit = mHighLimit;
        } else {
            result.limit = mHighLimit;
        }
        break;
    case RateOfChange:
        result.limit = mThreshold;
        // 变化率规则里"参与比较的量"是 dv/dt 而不是瞬时值：
        // 触发值、{value} 占位符、事件记录必须体现"速率"这一判定依据，
        // 否则报警文本会显示"变化率 200/s"，而 200 其实是当时的测点值。
        result.value = ratePerSecond;
        // 判定用绝对值，但保留正负号：工艺上"温度快速上升"和"快速下降"
        // 是两种完全不同的故障，报警记录里丢掉方向就失去了诊断价值。
        result.exceeded = (qAbs(ratePerSecond) > mThreshold);
        break;
    }

    return result;
}

bool QScadaAlarmRule::isRecovered(double value, double ratePerSecond) const
{
    const double db = (mDeadband > 0.0) ? mDeadband : 0.0;

    switch (mComparison) {
    case GreaterThan:
    case GreaterThanOrEqual:
        // 必须回落到"阈值 - 死区"以下才算恢复，只退回阈值一点点不算，
        // 否则信号在阈值上下来回抖动时会反复产生/恢复报警。
        return (value <= (mThreshold - db));
    case LessThan:
    case LessThanOrEqual:
        return (value >= (mThreshold + db));
    case OutOfRange:
        // 上下限各自让出一个死区宽度，可恢复区是 [low+db, high-db]。
        // 若死区大于半个区间宽度，可恢复区为空，报警会一直保持到操作员确认——
        // 这是配置问题，但某些高噪声测点确实故意这么配（只想要一个"确认即可"的提示）。
        return (value >= (mLowLimit + db)) && (value <= (mHighLimit - db));
    case RateOfChange:
        // 变化率是瞬时量，正常情况下下一拍就回落，所以死区要小；
        // 真正需要"压住"的场景应该配 delayMs 而不是靠死区。
        return (qAbs(ratePerSecond) <= (mThreshold - db));
    }

    return true;
}

bool QScadaAlarmRule::isValid() const
{
    if (mTagKey.trimmed().isEmpty())
        return false;

    switch (mComparison) {
    case OutOfRange:
        // 上限必须严格大于下限，否则区间为空、任何值都报警。
        return (mHighLimit > mLowLimit);
    case RateOfChange:
        // 变化率阈值必须为正；配 0 会把任何一点噪声都算成越限。
        return (mThreshold > 0.0);
    default:
        break;
    }

    // 限值类规则允许阈值为 0（例如"液位不得低于 0"是合法配置）。
    return true;
}

QString QScadaAlarmRule::buildMessage(double value, double limit) const
{
    QString text = mMessageTemplate;
    if (text.trimmed().isEmpty())
        text = defaultMessageTemplate();

    // 用 QRegularExpression 做占位符替换：模板里可能同时出现多个占位符，
    // 用固定模式一次性表达比链式 replace 更清楚，也不会误伤模板里的普通文本。
    // 注意替换顺序：{description} 放在最后，避免描述文本里恰好写了别的占位符时被二次替换。
    text.replace(QRegularExpression(QStringLiteral("\\{tag\\}")), mTagKey);
    text.replace(QRegularExpression(QStringLiteral("\\{value\\}")), formatNumber(value));
    text.replace(QRegularExpression(QStringLiteral("\\{limit\\}")), formatNumber(limit));
    text.replace(QRegularExpression(QStringLiteral("\\{unit\\}")), mUnit);
    text.replace(QRegularExpression(QStringLiteral("\\{level\\}")),
                 QScadaAlarmLevels::levelDisplayName(mLevel));
    text.replace(QRegularExpression(QStringLiteral("\\{description\\}")), mDescription);
    return text;
}

QString QScadaAlarmRule::defaultMessageTemplate() const
{
    // 单位在各处重复出现，先拼成后缀再插入，避免模板里到处写 {unit}。
    const QString u = mUnit;

    switch (mComparison) {
    case GreaterThan:
        return QStringLiteral("位号 {tag} 当前值 {value}%1 超过高限 {limit}%1").arg(u);
    case GreaterThanOrEqual:
        return QStringLiteral("位号 {tag} 当前值 {value}%1 达到或超过高限 {limit}%1").arg(u);
    case LessThan:
        return QStringLiteral("位号 {tag} 当前值 {value}%1 低于低限 {limit}%1").arg(u);
    case LessThanOrEqual:
        return QStringLiteral("位号 {tag} 当前值 {value}%1 达到或低于低限 {limit}%1").arg(u);
    case OutOfRange:
        // 区间外规则把允许区间直接写进默认文本，操作员不用回去翻点表。
        return QStringLiteral("位号 {tag} 当前值 {value}%1 超出允许区间 [%2 ~ %3]%4")
                .arg(u)
                .arg(formatNumber(mLowLimit))
                .arg(formatNumber(mHighLimit))
                .arg(u.isEmpty() ? QString() : QStringLiteral(" ") + u);
    case RateOfChange:
        return QStringLiteral("位号 {tag} 变化率 {value}%1/s 超过允许值 {limit}%1/s").arg(u);
    }

    return QStringLiteral("位号 {tag} 报警：当前值 {value}，限值 {limit}");
}

QJsonObject QScadaAlarmRule::toJson() const
{
    QJsonObject obj;
    obj.insert(QStringLiteral("ruleId"), mRuleId);
    obj.insert(QStringLiteral("tagKey"), mTagKey);
    obj.insert(QStringLiteral("description"), mDescription);
    // 等级与比较类型存稳定英文标识：以后改界面文案不会让老配置文件失效。
    obj.insert(QStringLiteral("level"), QScadaAlarmLevels::levelName(mLevel));
    obj.insert(QStringLiteral("enabled"), mEnabled);
    obj.insert(QStringLiteral("comparison"), comparisonName(mComparison));
    obj.insert(QStringLiteral("threshold"), mThreshold);
    obj.insert(QStringLiteral("lowLimit"), mLowLimit);
    obj.insert(QStringLiteral("highLimit"), mHighLimit);
    obj.insert(QStringLiteral("deadband"), mDeadband);
    obj.insert(QStringLiteral("delayMs"), mDelayMs);
    obj.insert(QStringLiteral("messageTemplate"), mMessageTemplate);
    obj.insert(QStringLiteral("unit"), mUnit);
    return obj;
}

QScadaAlarmRule QScadaAlarmRule::fromJson(const QJsonObject &obj)
{
    QScadaAlarmRule rule;
    // setRuleId 内部会把空值换成新 UUID：配置文件里少了 id 也能正常加载。
    rule.setRuleId(obj.value(QStringLiteral("ruleId")).toString());
    rule.mTagKey = obj.value(QStringLiteral("tagKey")).toString();
    rule.mDescription = obj.value(QStringLiteral("description")).toString();
    rule.mLevel = QScadaAlarmLevels::levelFromName(obj.value(QStringLiteral("level")).toString());
    rule.mEnabled = obj.value(QStringLiteral("enabled")).toBool(true);
    rule.mComparison = comparisonFromName(obj.value(QStringLiteral("comparison")).toString());
    rule.mThreshold = obj.value(QStringLiteral("threshold")).toDouble(0.0);
    rule.mLowLimit = obj.value(QStringLiteral("lowLimit")).toDouble(0.0);
    rule.mHighLimit = obj.value(QStringLiteral("highLimit")).toDouble(0.0);
    rule.mDeadband = obj.value(QStringLiteral("deadband")).toDouble(0.0);
    rule.mDelayMs = obj.value(QStringLiteral("delayMs")).toInt(0);
    rule.mMessageTemplate = obj.value(QStringLiteral("messageTemplate")).toString();
    rule.mUnit = obj.value(QStringLiteral("unit")).toString();
    return rule;
}

QString QScadaAlarmRule::comparisonName(ComparisonType type)
{
    switch (type) {
    case GreaterThan:        return QStringLiteral("greaterThan");
    case GreaterThanOrEqual: return QStringLiteral("greaterThanOrEqual");
    case LessThan:           return QStringLiteral("lessThan");
    case LessThanOrEqual:    return QStringLiteral("lessThanOrEqual");
    case OutOfRange:         return QStringLiteral("outOfRange");
    case RateOfChange:       return QStringLiteral("rateOfChange");
    }
    return QStringLiteral("greaterThan");
}

QScadaAlarmRule::ComparisonType QScadaAlarmRule::comparisonFromName(const QString &name)
{
    // 忽略大小写并接受几个现场常用的别名，避免人手写点表时因为一个大小写
    // 就静默退回"高限"，那是很难查的配置事故。
    const QString n = name.trimmed().toLower();

    if (n == QLatin1String("greaterthan") || n == QLatin1String("gt") || n == QLatin1String("high"))
        return GreaterThan;
    if (n == QLatin1String("greaterthanorequal") || n == QLatin1String("ge") || n == QLatin1String("gte"))
        return GreaterThanOrEqual;
    if (n == QLatin1String("lessthan") || n == QLatin1String("lt") || n == QLatin1String("low"))
        return LessThan;
    if (n == QLatin1String("lessthanorequal") || n == QLatin1String("le") || n == QLatin1String("lte"))
        return LessThanOrEqual;
    if (n == QLatin1String("outofrange") || n == QLatin1String("range") || n == QLatin1String("outside"))
        return OutOfRange;
    if (n == QLatin1String("rateofchange") || n == QLatin1String("rate") || n == QLatin1String("roc"))
        return RateOfChange;

    return GreaterThan;
}

QString QScadaAlarmRule::newRuleId()
{
    // 用 UUID 而不是自增序号：规则清单要导出/导入、还要和历史库里的记录对应，
    // 自增序号在两台机器或两次导入之间必然冲突。
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}
