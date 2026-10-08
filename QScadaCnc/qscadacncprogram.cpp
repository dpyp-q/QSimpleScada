#include "qscadacncprogram.h"

#include <QChar>
#include <QString>

namespace {

bool nearlyEqual(double a, double b)
{
    if (a == b) {
        return true;
    }
    const double diff = a - b;
    const double absDiff = (diff < 0.0) ? -diff : diff;
    return absDiff < 1e-6;
}

//! 剩余时间未知时的占位文本，界面统一用它，避免各处写死不同的"未知"。
const char *const UnknownTimeText = "--:--:--";

} // namespace

QStringList QScadaCncProgram::bindableFields()
{
    QStringList fields;
    fields << QStringLiteral("programName")
           << QStringLiteral("programNumber")
           << QStringLiteral("currentLine")
           << QStringLiteral("totalLines")
           << QStringLiteral("currentBlock")
           << QStringLiteral("mode")
           << QStringLiteral("feedOverride")
           << QStringLiteral("rapidOverride")
           << QStringLiteral("cycleTimeMs")
           << QStringLiteral("partsCompleted")
           << QStringLiteral("partsTarget");
    return fields;
}

QScadaCncProgram::QScadaCncProgram(QObject *parent)
    : QScadaCncTagBinding(parent)
    , mProgramNumber(0)
    , mCurrentLine(0)
    , mTotalLines(0)
    , mMode(QScadaCncEnums::InterpNone)
    , mFeedOverride(100.0)
    , mRapidOverride(100.0)
    , mCycleTimeMs(0)
    , mPartsCompleted(0)
    , mPartsTarget(0)
{
    // 程序字段统一加 "O." 前缀（O 号是数控程序号的惯用前缀），
    // 这样机床模型里 "O.currentLine" 不会和任何轴的字段撞名。
    setFieldPrefix(QStringLiteral("O."));
    setFeedOverride(100.0);
    setRapidOverride(100.0);
}

QScadaCncProgram::~QScadaCncProgram()
{
}

QString QScadaCncProgram::programNumberText() const
{
    if (mProgramNumber <= 0) {
        return QStringLiteral("-");
    }
    return QStringLiteral("O%1").arg(mProgramNumber);
}

QString QScadaCncProgram::modeText() const
{
    return QScadaCncEnums::interpolationModeName(mMode);
}

QString QScadaCncProgram::modeGCodeText() const
{
    const int code = QScadaCncEnums::interpolationModeGCode(mMode);
    if (code < 0) {
        return QString();
    }
    return QStringLiteral("G%1").arg(code, 2, 10, QLatin1Char('0'));
}

QString QScadaCncProgram::cycleTimeText() const
{
    return formatDuration(mCycleTimeMs);
}

QString QScadaCncProgram::partsText() const
{
    if (mPartsTarget > 0) {
        return QStringLiteral("%1 / %2").arg(mPartsCompleted).arg(mPartsTarget);
    }
    return QString::number(mPartsCompleted);
}

double QScadaCncProgram::progressPercent() const
{
    // 程序还没开始跑（总行数为 0）时直接返回 0：这里是最容易写出除零的地方，
    // 一旦在采集线程里除零，整个上位机会静默崩溃，现场表现为"软件自己关了"。
    if (mTotalLines <= 0 || mCurrentLine <= 0) {
        return 0.0;
    }

    double percent = static_cast<double>(mCurrentLine) * 100.0
                     / static_cast<double>(mTotalLines);
    if (percent > 100.0) {
        percent = 100.0; // 子程序调用会让行号超出主程序总行数，夹紧避免进度条溢出
    }
    return percent;
}

bool QScadaCncProgram::hasProgram() const
{
    return mProgramNumber > 0 || !mProgramName.isEmpty();
}

QString QScadaCncProgram::remainingTimeText() const
{
    const double percent = progressPercent();
    if (percent <= 0.0 || mCycleTimeMs <= 0) {
        return QString::fromLatin1(UnknownTimeText);
    }

    if (percent >= 100.0) {
        return formatDuration(0);
    }

    const double elapsed = static_cast<double>(mCycleTimeMs);
    const double total = elapsed * 100.0 / percent;
    double remain = total - elapsed;
    if (remain < 0.0) {
        remain = 0.0;
    }
    return formatDuration(static_cast<qint64>(remain));
}

QString QScadaCncProgram::formatDuration(qint64 ms)
{
    if (ms < 0) {
        ms = 0;
    }

    const qint64 totalSeconds = ms / 1000;
    const qint64 hours = totalSeconds / 3600;
    const qint64 minutes = (totalSeconds % 3600) / 60;
    const qint64 seconds = totalSeconds % 60;

    // 小时不取模：连续加工超过 24 小时（大批量小零件很常见）时，
    // 显示 "25:03:11" 比回绕成 "01:03:11" 更有意义。
    return QStringLiteral("%1:%2:%3")
            .arg(hours, 2, 10, QLatin1Char('0'))
            .arg(minutes, 2, 10, QLatin1Char('0'))
            .arg(seconds, 2, 10, QLatin1Char('0'));
}

// ---- setter ----

void QScadaCncProgram::setProgramName(const QString &name)
{
    const QString trimmed = name.trimmed();
    if (mProgramName == trimmed) {
        return;
    }

    mProgramName = trimmed;
    emit programChanged();
}

void QScadaCncProgram::setProgramNumber(int number)
{
    if (mProgramNumber == number) {
        return;
    }

    mProgramNumber = number;
    emit programChanged();
}

void QScadaCncProgram::setCurrentLine(int line)
{
    if (line < 0) {
        line = 0;
    }
    if (mCurrentLine == line) {
        return;
    }

    mCurrentLine = line;
    emit programChanged();
}

void QScadaCncProgram::setTotalLines(int lines)
{
    if (lines < 0) {
        lines = 0;
    }
    if (mTotalLines == lines) {
        return;
    }

    mTotalLines = lines;
    emit programChanged();
}

void QScadaCncProgram::setCurrentBlock(const QString &block)
{
    const QString trimmed = block.trimmed();
    if (mCurrentBlock == trimmed) {
        return;
    }

    mCurrentBlock = trimmed;
    emit blockChanged(mCurrentBlock);
    emit programChanged();
}

void QScadaCncProgram::setMode(QScadaCncEnums::InterpolationMode mode)
{
    if (mMode == mode) {
        return;
    }

    mMode = mode;
    emit programChanged();
}

void QScadaCncProgram::setFeedOverride(double percent)
{
    double value = percent;
    if (value < 0.0) {
        value = 0.0;
    } else if (value > 200.0) {
        value = 200.0;
    }

    if (nearlyEqual(mFeedOverride, value)) {
        return;
    }

    mFeedOverride = value;
    emit programChanged();
}

void QScadaCncProgram::setRapidOverride(double percent)
{
    double value = percent;
    if (value < 0.0) {
        value = 0.0;
    } else if (value > 100.0) {
        // 多数系统的快移倍率只允许往低降（F0 / 25% / 50% / 100%），
        // 允许高于 100% 会让"机床不动"的排查方向跑偏。
        value = 100.0;
    }

    if (nearlyEqual(mRapidOverride, value)) {
        return;
    }

    mRapidOverride = value;
    emit programChanged();
}

void QScadaCncProgram::setCycleTimeMs(qint64 ms)
{
    if (ms < 0) {
        ms = 0;
    }
    if (mCycleTimeMs == ms) {
        return;
    }

    mCycleTimeMs = ms;
    emit programChanged();
}

void QScadaCncProgram::addCycleTimeMs(qint64 deltaMs)
{
    if (deltaMs <= 0) {
        return;
    }

    mCycleTimeMs += deltaMs;
    emit programChanged();
}

void QScadaCncProgram::setPartsCompleted(int parts)
{
    if (parts < 0) {
        parts = 0;
    }
    if (mPartsCompleted == parts) {
        return;
    }

    mPartsCompleted = parts;
    emit programChanged();
}

void QScadaCncProgram::markPartCompleted()
{
    ++mPartsCompleted;
    mCycleTimeMs = 0;
    emit programChanged();
}

void QScadaCncProgram::setPartsTarget(int target)
{
    if (target < 0) {
        target = 0;
    }
    if (mPartsTarget == target) {
        return;
    }

    mPartsTarget = target;
    emit programChanged();
}

// ---- 位号分发 ----

bool QScadaCncProgram::updateFromTag(const QString &field, const QVariant &value)
{
    const QString f = qualify(field);

    if (f == QStringLiteral("programName")) {
        setProgramName(value.toString());
        return true;
    }
    if (f == QStringLiteral("programNumber")) {
        // 位号可能是字符串 "O1234"（有些系统寄存器里放的是 ASCII 程序名），
        // QVariant::toInt 对 "O1234" 会失败，这里先把非数字前缀去掉。
        QString text = value.toString().trimmed();
        if (!text.isEmpty() && !text.at(0).isDigit()) {
            int pos = 0;
            while (pos < text.size() && !text.at(pos).isDigit()) {
                ++pos;
            }
            text = text.mid(pos);
        }
        bool ok = false;
        const int number = text.toInt(&ok);
        setProgramNumber(ok ? number : 0);
        return true;
    }
    if (f == QStringLiteral("currentLine")) {
        setCurrentLine(value.toInt());
        return true;
    }
    if (f == QStringLiteral("totalLines")) {
        setTotalLines(value.toInt());
        return true;
    }
    if (f == QStringLiteral("currentBlock")) {
        setCurrentBlock(value.toString());
        return true;
    }
    if (f == QStringLiteral("mode")) {
        QScadaCncEnums::InterpolationMode mode = mMode;
        if (QScadaCncEnums::interpolationModeFromValue(value, &mode)) {
            setMode(mode);
        } else if (value.isValid() && !value.isNull()) {
            bool ok = false;
            const int raw = value.toInt(&ok);
            if (ok) {
                setMode(static_cast<QScadaCncEnums::InterpolationMode>(raw));
            }
        }
        return true;
    }
    if (f == QStringLiteral("feedOverride")) {
        setFeedOverride(value.toDouble());
        return true;
    }
    if (f == QStringLiteral("rapidOverride")) {
        setRapidOverride(value.toDouble());
        return true;
    }
    if (f == QStringLiteral("cycleTimeMs")) {
        // 有的系统给的是秒，有的是毫秒。点表里用 scale 把单位统一到毫秒
        // （scale=1000 即可），模型只认毫秒，避免两处单位换算互相打架。
        setCycleTimeMs(static_cast<qint64>(value.toLongLong()));
        return true;
    }
    if (f == QStringLiteral("partsCompleted")) {
        setPartsCompleted(value.toInt());
        return true;
    }
    if (f == QStringLiteral("partsTarget")) {
        setPartsTarget(value.toInt());
        return true;
    }

    return false;
}
