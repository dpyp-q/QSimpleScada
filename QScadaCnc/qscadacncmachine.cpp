#include "qscadacncmachine.h"

#include <QChar>
#include <QDebug>
#include <QHash>
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

bool hasText(const QVariant &value)
{
    if (!value.isValid() || value.isNull()) {
        return false;
    }
    return !value.toString().trimmed().isEmpty();
}

//! 机床自身字段的前缀。集中在这里，避免字面量散落各处后改一处漏一处。
const char *const MachinePrefix = "M.";

} // namespace

QStringList QScadaCncMachine::bindableFields()
{
    QStringList fields;
    fields << QStringLiteral("machineState")
           << QStringLiteral("currentTool")
           << QStringLiteral("workOffset")
           << QStringLiteral("feedRateOverrideGlobal")
           << QStringLiteral("activeAlarmCode")
           << QStringLiteral("activeAlarmText");
    return fields;
}

QScadaCncMachine::QScadaCncMachine(QObject *parent)
    : QScadaCncTagBinding(parent)
    , mState(QScadaCncEnums::MachineOffline)
    , mSpindle(nullptr)
    , mProgram(nullptr)
    , mCurrentTool(0)
    , mFeedRateOverrideGlobal(100.0)
    , mActiveAlarmCode(0)
{
    // 机床自己带一根主轴、一个程序上下文：绝大多数加工中心就是 1 主轴 + 1 程序，
    // 让它们默认存在，可以省掉上层到处判空的代码；多主轴机型再 addAxis/多实例扩展。
    setFieldPrefix(QString::fromLatin1(MachinePrefix));

    mSpindle = new QScadaCncSpindle(QStringLiteral("S"), this);
    mProgram = new QScadaCncProgram(this);

    connect(mSpindle, &QScadaCncSpindle::spindleChanged,
            this, &QScadaCncMachine::onSpindleChanged);
    connect(mSpindle, &QScadaCncTagBinding::tagValueApplied,
            this, &QScadaCncMachine::onTagApplied);
    connect(mProgram, &QScadaCncProgram::programChanged,
            this, &QScadaCncMachine::onProgramChanged);
    connect(mProgram, &QScadaCncTagBinding::tagValueApplied,
            this, &QScadaCncMachine::onTagApplied);
}

QScadaCncMachine::~QScadaCncMachine()
{
    // 子对象都以 this 为 parent，Qt 会负责析构；这里只清空指针列表，
    // 避免析构过程中还有人通过 axes() 访问已释放对象。
    mAxes.clear();
    mSpindle = nullptr;
    mProgram = nullptr;
}

// ---- 静态信息 ----

void QScadaCncMachine::setMachineName(const QString &name)
{
    const QString trimmed = name.trimmed();
    if (mMachineName == trimmed) {
        return;
    }

    mMachineName = trimmed;
    emit machineStateChanged(mState);
}

void QScadaCncMachine::setMachineModel(const QString &model)
{
    const QString trimmed = model.trimmed();
    if (mMachineModel == trimmed) {
        return;
    }

    mMachineModel = trimmed;
    emit machineStateChanged(mState);
}

void QScadaCncMachine::setState(QScadaCncEnums::MachineState state)
{
    if (mState == state) {
        return;
    }

    mState = state;
    emit machineStateChanged(mState);
}

void QScadaCncMachine::setOffline()
{
    // 断线时把整机置为离线（灰色），而不是保留最后一帧的"运行中"：
    // 看板上显示"运行中"但其实是断线，会让调度误判产能，比显示灰色更危险。
    setState(QScadaCncEnums::MachineOffline);
}

// ---- 轴管理 ----

QScadaCncAxis *QScadaCncMachine::addAxis(const QString &name)
{
    const QString upper = QScadaCncEnums::normalizeAxisName(name);
    if (upper.isEmpty()) {
        return nullptr;
    }

    QScadaCncAxis *existing = axis(upper);
    if (existing) {
        return existing; // 重复添加同一根轴时返回已有实例，避免出现两根 "X"
    }

    QScadaCncAxis *newAxis = new QScadaCncAxis(upper, this);
    mAxes.append(newAxis);
    connectAxis(newAxis);
    return newAxis;
}

QScadaCncAxis *QScadaCncMachine::axis(const QString &name) const
{
    const QString upper = QScadaCncEnums::normalizeAxisName(name);
    if (upper.isEmpty()) {
        return nullptr;
    }

    for (int i = 0; i < mAxes.size(); ++i) {
        if (mAxes.at(i) && mAxes.at(i)->name() == upper) {
            return mAxes.at(i);
        }
    }
    return nullptr;
}

QStringList QScadaCncMachine::axisNames() const
{
    QStringList names;
    for (int i = 0; i < mAxes.size(); ++i) {
        if (mAxes.at(i)) {
            names.append(mAxes.at(i)->name());
        }
    }
    names.sort();
    return names;
}

void QScadaCncMachine::connectAxis(QScadaCncAxis *axis)
{
    if (!axis) {
        return;
    }

    connect(axis, &QScadaCncAxis::axisChanged,
            this, &QScadaCncMachine::onAxisChanged);
    connect(axis, &QScadaCncAxis::stateChanged,
            this, &QScadaCncMachine::onAxisStateChanged);
    connect(axis, &QScadaCncTagBinding::tagValueApplied,
            this, &QScadaCncMachine::onTagApplied);
}

// ---- 机床级字段 ----

QString QScadaCncMachine::currentToolText() const
{
    if (mCurrentTool <= 0) {
        return QStringLiteral("-");
    }
    return QStringLiteral("T%1").arg(mCurrentTool, 2, 10, QLatin1Char('0'));
}

QString QScadaCncMachine::activeAlarmText() const
{
    if (!mActiveAlarmText.isEmpty()) {
        return mActiveAlarmText;
    }
    if (mActiveAlarmCode == 0) {
        return QString();
    }
    return QScadaCncEnums::defaultAlarmText(mActiveAlarmCode);
}

void QScadaCncMachine::setFeedRateOverrideGlobal(double percent)
{
    double value = percent;
    if (value < 0.0) {
        value = 0.0;
    } else if (value > 200.0) {
        value = 200.0;
    }

    if (nearlyEqual(mFeedRateOverrideGlobal, value)) {
        return;
    }

    mFeedRateOverrideGlobal = value;
    emit machineStateChanged(mState);
}

void QScadaCncMachine::setActiveAlarmCode(int code, const QString &text)
{
    const int previousCode = mActiveAlarmCode;
    const bool wasAlarm = (previousCode != 0);
    const bool isAlarm = (code != 0);

    mActiveAlarmCode = code;
    mActiveAlarmText = text.trimmed();

    if (isAlarm && !wasAlarm) {
        const QString message = activeAlarmText();
        emit anyAlarmRaised(QStringLiteral("MACHINE"), code, message);
    } else if (!isAlarm && wasAlarm) {
        // 注意这里报的是 previousCode：报警已经清掉了，界面需要知道
        // "刚才是哪一条报警消失了"才能把它从报警列表里移除。
        emit alarmCleared(QStringLiteral("MACHINE"), previousCode);
    }

    if (isAlarm) {
        setState(QScadaCncEnums::MachineAlarm);
    }
}

// ---- 状态映射 ----

QScadaStatus QScadaCncMachine::toScadaStatus() const
{
    // 报警优先：只要有一个轴报警，整机就必须是红的，不能被"运行中"盖过去。
    if (hasAlarm()) {
        return QScadaStatusRed;
    }

    switch (mState) {
    case QScadaCncEnums::MachineRunning:
        return QScadaStatusGreen;
    case QScadaCncEnums::MachinePaused:
    case QScadaCncEnums::MachineHold:
    case QScadaCncEnums::MachineStopped:
    case QScadaCncEnums::MachineIdle:
        // 黄色含义是"有情况但还不致命"：待机久了要催料，暂停太久要问原因。
        return QScadaStatusYellow;
    case QScadaCncEnums::MachineAlarm:
        return QScadaStatusRed;
    case QScadaCncEnums::MachineOffline:
    case QScadaCncEnums::MachineMaintenance:
    default:
        return QScadaStatusGray;
    }
}

bool QScadaCncMachine::hasAlarm() const
{
    if (mState == QScadaCncEnums::MachineAlarm || mActiveAlarmCode != 0) {
        return true;
    }

    for (int i = 0; i < mAxes.size(); ++i) {
        if (mAxes.at(i) && mAxes.at(i)->isAlarmActive()) {
            return true;
        }
    }
    if (mSpindle && mSpindle->isAlarmActive()) {
        return true;
    }
    return false;
}

bool QScadaCncMachine::anyAxisMoving() const
{
    for (int i = 0; i < mAxes.size(); ++i) {
        if (mAxes.at(i) && mAxes.at(i)->isMoving()) {
            return true;
        }
    }
    return false;
}

QString QScadaCncMachine::statusSummary() const
{
    QStringList parts;
    parts << QScadaCncEnums::machineStateName(mState);

    // 轴坐标：有采集值才显示数字，没读到就显示 "--"。
    // 显示 0.000 会让操作工以为轴真的在原点，是上位机最容易被投诉的一类错误。
    QStringList axisTexts;
    for (int i = 0; i < mAxes.size(); ++i) {
        QScadaCncAxis *a = mAxes.at(i);
        if (!a) {
            continue;
        }
        if (a->hasPosition()) {
            axisTexts << QStringLiteral("%1:%2")
                         .arg(a->name())
                         .arg(a->machinePosition(), 0, 'f', 3);
        } else {
            axisTexts << QStringLiteral("%1:--").arg(a->name());
        }
    }
    if (!axisTexts.isEmpty()) {
        // 用 Latin1String 而不是 QChar 作分隔符：QStringList::join(QChar)
        // 要到 Qt 5.14 才有，这里必须兼容到 Qt 5.12。
        parts << axisTexts.join(QLatin1String(" "));
    }

    if (mSpindle) {
        if (mSpindle->speed() > 0.0) {
            parts << QStringLiteral("S:%1rpm").arg(mSpindle->speed(), 0, 'f', 0);
        } else {
            parts << QStringLiteral("S:0rpm");
        }
    }

    if (mProgram) {
        if (mProgram->hasProgram()) {
            QString programText = mProgram->programNumberText();
            if (mProgram->totalLines() > 0) {
                programText += QStringLiteral(" L%1/%2")
                               .arg(mProgram->currentLine())
                               .arg(mProgram->totalLines());
            }
            parts << programText;
        }
        parts << QStringLiteral("已加工 %1 件").arg(mProgram->partsCompleted());
    }

    return parts.join(QLatin1String(" | "));
}

// ---- 绑定快捷方式 ----

void QScadaCncMachine::bindAxisTag(const QString &axisName, const QString &field,
                                   const QString &tagKey)
{
    QScadaCncAxis *a = axis(axisName);
    if (a) {
        a->bindTag(field, tagKey);
        return;
    }
    qWarning() << "[QScadaCnc] bindAxisTag: 未定义的轴" << axisName
               << "字段" << field << "位号" << tagKey;
}

QString QScadaCncMachine::axisTag(const QString &axisName, const QString &field) const
{
    QScadaCncAxis *a = axis(axisName);
    return a ? a->tagFor(field) : QString();
}

void QScadaCncMachine::bindSpindleTag(const QString &field, const QString &tagKey)
{
    if (mSpindle) {
        mSpindle->bindTag(field, tagKey);
    }
}

QString QScadaCncMachine::spindleTag(const QString &field) const
{
    return mSpindle ? mSpindle->tagFor(field) : QString();
}

void QScadaCncMachine::bindProgramTag(const QString &field, const QString &tagKey)
{
    if (mProgram) {
        mProgram->bindTag(field, tagKey);
    }
}

QString QScadaCncMachine::programTag(const QString &field) const
{
    return mProgram ? mProgram->tagFor(field) : QString();
}

QHash<QString, QString> QScadaCncMachine::fieldTagMap() const
{
    QHash<QString, QString> map = mFieldToTag;

    for (int i = 0; i < mAxes.size(); ++i) {
        QScadaCncAxis *a = mAxes.at(i);
        if (!a) {
            continue;
        }
        const QStringList fields = a->boundFields();
        for (int j = 0; j < fields.size(); ++j) {
            map.insert(fields.at(j), a->tagFor(fields.at(j)));
        }
    }

    if (mSpindle) {
        const QStringList fields = mSpindle->boundFields();
        for (int j = 0; j < fields.size(); ++j) {
            map.insert(fields.at(j), mSpindle->tagFor(fields.at(j)));
        }
    }

    if (mProgram) {
        const QStringList fields = mProgram->boundFields();
        for (int j = 0; j < fields.size(); ++j) {
            map.insert(fields.at(j), mProgram->tagFor(fields.at(j)));
        }
    }

    return map;
}

// ---- 采集值分发 ----

void QScadaCncMachine::onTagValue(const QString &tagKey, const QVariant &value)
{
    const QString key = tagKey.trimmed();
    if (key.isEmpty()) {
        return;
    }

    const QStringList fields = fieldsForTag(key);

    if (fields.isEmpty()) {
        // 位号没绑到任何字段。这通常意味着"点表加了一个点但没建绑定"，
        // 采集层会一直把值送过来而界面永远不动，是最难自己发现的一类问题，
        // 因此这里必须出声（写日志，不弹窗——库代码弹窗会阻塞采集线程）。
        qWarning() << "[QScadaCnc] 未绑定的位号，值被丢弃:" << key;
        return;
    }

    for (int i = 0; i < fields.size(); ++i) {
        const QString &field = fields.at(i);

        // 先把值交给字段所属的对象。注意各子对象是用自己的反向索引处理的，
        // 它们并不知道机床的这张表，所以必须由机床显式转发。
        if (field.startsWith(QString::fromLatin1(MachinePrefix))) {
            if (updateFromTag(field, value)) {
                emit tagValueApplied(field, key);
            }
            continue;
        }
        if (dispatchToAxis(field, value)) {
            continue;
        }
        if (dispatchToSpindleOrProgram(field, value)) {
            continue;
        }

        qWarning() << "[QScadaCnc] 字段无人处理（绑定表与模型不匹配）:"
                   << field << "位号" << key;
    }

    // 无论有多少个字段被更新，一个位号只报一次 tagObserved，
    // 界面按位号刷新一次即可，不必按字段刷新多次。
    emit tagObserved(key);
}

bool QScadaCncMachine::dispatchToAxis(const QString &field, const QVariant &value)
{
    // 轴字段的前缀就是"轴名 + 点"（"X." / "X1."），因此取出点号前面的部分
    // 当轴名即可，不需要为每根轴准备一张前缀表。
    const int dot = field.indexOf(QLatin1Char('.'));
    if (dot <= 0) {
        return false;
    }

    QScadaCncAxis *a = axis(field.left(dot));
    if (a) {
        return a->applyTagValue(a->tagFor(field), value);
    }
    return false;
}

bool QScadaCncMachine::dispatchToSpindleOrProgram(const QString &field, const QVariant &value)
{
    if (mSpindle && mSpindle->applyTagValue(mSpindle->tagFor(field), value)) {
        return true;
    }
    if (mProgram && mProgram->applyTagValue(mProgram->tagFor(field), value)) {
        return true;
    }
    return false;
}

bool QScadaCncMachine::updateFromTag(const QString &field, const QVariant &value)
{
    const QString f = qualify(field);

    if (f == QStringLiteral("M.machineState")) {
        QScadaCncEnums::MachineState state = mState;
        if (QScadaCncEnums::machineStateFromValue(value, &state)) {
            setState(state);
        } else if (value.isValid() && !value.isNull()) {
            bool ok = false;
            const int raw = value.toInt(&ok);
            if (ok) {
                setState(static_cast<QScadaCncEnums::MachineState>(raw));
            }
        }
        return true;
    }
    if (f == QStringLiteral("M.currentTool")) {
        const int tool = value.toInt();
        if (mCurrentTool != tool) {
            mCurrentTool = tool;
            emit machineStateChanged(mState);
        }
        return true;
    }
    if (f == QStringLiteral("M.workOffset")) {
        // G54/G55 这类工件坐标系，系统给出的可能是编号（1~6）也可能是字符串。
        // 编号要转成 "G5x"（G54 起，所以是 53+n），否则界面上会出现孤零零的 "1"。
        QString offset = value.toString().trimmed();
        if (!offset.isEmpty() && !offset.startsWith(QLatin1Char('G'), Qt::CaseInsensitive)) {
            bool ok = false;
            const int index = offset.toInt(&ok);
            if (ok && index > 0 && index <= 9) {
                offset = QStringLiteral("G%1").arg(53 + index);
            }
        }
        if (mWorkOffset != offset) {
            mWorkOffset = offset;
            emit machineStateChanged(mState);
        }
        return true;
    }
    if (f == QStringLiteral("M.feedRateOverrideGlobal")) {
        setFeedRateOverrideGlobal(value.toDouble());
        return true;
    }
    if (f == QStringLiteral("M.activeAlarmCode")) {
        setActiveAlarmCode(value.toInt(), mActiveAlarmText);
        return true;
    }
    if (f == QStringLiteral("M.activeAlarmText")) {
        // 报警文本位号通常比报警号晚到（文本往往要后处理去查表），
        // 因此文本更新时若报警号仍是 0，也要补发一次报警，否则操作工
        // 只看到一个红块却没有任何说明。
        const QString text = hasText(value) ? value.toString().trimmed() : QString();
        const bool wasAlarm = (mActiveAlarmCode != 0);
        mActiveAlarmText = text;

        if (!text.isEmpty() && !wasAlarm) {
            setActiveAlarmCode(-1, text); // -1：报警号未知，用文本报警
        } else if (text.isEmpty() && wasAlarm && mActiveAlarmCode == -1) {
            setActiveAlarmCode(0, QString());
        } else if (!text.isEmpty()) {
            setState(QScadaCncEnums::MachineAlarm);
        }
        return true;
    }

    return false;
}

// ---- 子对象事件汇总 ----

void QScadaCncMachine::onAxisChanged(const QString &axisName)
{
    emit axisChanged(axisName);

    // 轴的故障位从有到无（复位报警）时，也要把整机的报警状态撤掉，
    // 否则界面会一直停在红色，操作工只能重启软件。
    clearAlarmStateIfRecovered();
}

void QScadaCncMachine::onAxisStateChanged(const QString &axisName,
                                          QScadaCncEnums::AxisState state)
{
    QString source = axisName;
    if (source.isEmpty()) {
        source = QStringLiteral("-");
    }

    if (state == QScadaCncEnums::AxisFault) {
        QScadaCncAxis *a = axis(source);
        const int code = a ? a->alarmCode() : 0;
        QString text = a ? a->alarmText() : QString();
        if (text.isEmpty()) {
            text = QStringLiteral("轴 %1 伺服/超程故障").arg(source);
        }
        raiseAlarm(source, code, text);
        return;
    }

    if (state == QScadaCncEnums::AxisDisabled) {
        // 轴被断开使能（急停、门开、伺服下电）不一定是故障，但整机绝不可能
        // 还在自动运行，这里降级为停止，避免看板上继续显示绿色"运行中"。
        if (mState == QScadaCncEnums::MachineRunning) {
            setState(QScadaCncEnums::MachineStopped);
        }
    }

    emit axisChanged(source);
    clearAlarmStateIfRecovered();
}

void QScadaCncMachine::onSpindleChanged()
{
    // 先看报警再看运行状态：主轴故障是比"转速变了"严重得多的事件，
    // 而且主轴故障位号常常和转速位号在同一个采集批次里到达。
    if (mSpindle && mSpindle->state() == QScadaCncEnums::SpindleFault) {
        int code = mSpindle->alarmCode();
        if (code == 0) {
            code = -1; // 主轴只给了故障位、没给报警号，用 -1 表示"有故障无编号"
        }
        QString text = mSpindle->alarmText();
        if (text.isEmpty()) {
            text = QStringLiteral("主轴故障");
        }
        raiseAlarm(mSpindle->name(), code, text);
        return;
    }

    // 主轴转速/负载变化很快，这里不改变整机状态，只转告界面。
    emit machineStateChanged(mState);

    if (mState == QScadaCncEnums::MachineAlarm) {
        clearAlarmStateIfRecovered();
    }
}

void QScadaCncMachine::onProgramChanged()
{
    emit programChanged();
}

void QScadaCncMachine::onTagApplied(const QString &field, const QString &tagKey)
{
    Q_UNUSED(field)
    emit tagObserved(tagKey);

    // 子对象的报警号被清 0 时，没人会主动通知机床，只能在这里检查一次。
    // 注意这里必须放在子对象已经改完自己成员之后：本槽是由子对象的
    // tagValueApplied 信号触发的，那时子对象的新状态已经生效，
    // 若提前检查就会看到"报警还在"的旧状态，导致红块永远不消失。
    if (mState == QScadaCncEnums::MachineAlarm) {
        clearAlarmStateIfRecovered();
    }
}

void QScadaCncMachine::raiseAlarm(const QString &source, int code, const QString &message)
{
    if (mState != QScadaCncEnums::MachineAlarm) {
        // 首次进入报警：通知界面。报警列表由上层（报警库）维护，
        // 模型只在"报警事件发生"时说话，不做周期性的重复通知。
        emit anyAlarmRaised(source, code, message);
    }
    // 已经处于报警中则不再重复发信号，但依然要保证整机状态是 Alarm：
    // 一条报警还没复位时又来了第二条（很常见），状态不能被后续的
    // "运行中"等位号覆盖回去。报警号本身由各子对象自己保留，
    // 上层需要完整列表时去读各轴/主轴的 alarmCode()。
    setState(QScadaCncEnums::MachineAlarm);
}

void QScadaCncMachine::clearAlarmStateIfRecovered()
{
    if (mState != QScadaCncEnums::MachineAlarm) {
        return;
    }
    if (hasAlarm()) {
        return;
    }

    // 报警全部消失后回到待机，而不是回到"运行中"：复位之后机床不会自己
    // 接着跑，必须人工按循环启动，显示运行中会让调度以为还在出活。
    setState(QScadaCncEnums::MachineIdle);
}
