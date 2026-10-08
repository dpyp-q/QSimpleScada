#include "qscadacncenums.h"

#include <QChar>
#include <QStringList>

namespace QScadaCncEnums {
namespace {

/*!
 * 状态名的查找表。
 * 用表驱动而不是一长串 if-else：枚举以后一定会加值（比如客户要求区分
 * "刀具寿命到期"），表驱动只要加一行，忘了加也不会编译通过（case 少一个
 * 编译器会提醒，而 if-else 忘了加会静默走到 default）。
 */
struct NameKeyPair {
    int value;
    const char *name; //!< 中文名，给操作工看
    const char *key;  //!< 英文键，给日志/数据库/配置看
};

const NameKeyPair AxisStateTable[] = {
    { AxisDisabled, "未使能", "disabled" },
    { AxisReady,    "就绪",   "ready"    },
    { AxisHoming,   "回零中", "homing"   },
    { AxisHomed,    "已回零", "homed"    },
    { AxisMoving,   "运动中", "moving"   },
    { AxisHolding,  "保持",   "holding"  },
    { AxisFault,    "故障",   "fault"    }
};

const NameKeyPair MachineStateTable[] = {
    { MachineOffline,     "离线",   "offline"     },
    { MachineIdle,        "待机",   "idle"        },
    { MachineRunning,     "运行中", "running"     },
    { MachinePaused,      "暂停",   "paused"      },
    { MachineHold,        "保持",   "hold"        },
    { MachineStopped,     "停止",   "stopped"     },
    { MachineAlarm,       "报警",   "alarm"       },
    { MachineMaintenance, "维护",   "maintenance" }
};

const NameKeyPair SpindleStateTable[] = {
    { SpindleStopped,   "停止",   "stopped"   },
    { SpindleRunning,   "旋转中", "running"   },
    { SpindleOrienting, "定向中", "orienting" },
    { SpindleFault,     "故障",   "fault"     }
};

const NameKeyPair InterpolationModeTable[] = {
    { InterpNone,        "无",           "none"        },
    { InterpRapid,       "快速定位",     "rapid"       },
    { InterpLinear,      "直线插补",     "linear"      },
    { InterpCircularCW,  "顺时针圆弧",   "circular_cw" },
    { InterpCircularCCW, "逆时针圆弧",   "circular_ccw"},
    { InterpThreading,   "螺纹切削",     "threading"   }
};

const NameKeyPair ToolOffsetModeTable[] = {
    { ToolOffsetNone,   "无补偿",     "none"         },
    { ToolRadiusComp,   "刀具半径补偿", "radius"     },
    { ToolLengthComp,   "刀具长度补偿", "length"     },
    { CutterCompLeft,   "左刀补",     "cutter_left"  },
    { CutterCompRight,  "右刀补",     "cutter_right" }
};

const int AxisStateTableSize = static_cast<int>(sizeof(AxisStateTable) / sizeof(AxisStateTable[0]));
const int MachineStateTableSize = static_cast<int>(sizeof(MachineStateTable) / sizeof(MachineStateTable[0]));
const int SpindleStateTableSize = static_cast<int>(sizeof(SpindleStateTable) / sizeof(SpindleStateTable[0]));
const int InterpolationModeTableSize = static_cast<int>(sizeof(InterpolationModeTable) / sizeof(InterpolationModeTable[0]));
const int ToolOffsetModeTableSize = static_cast<int>(sizeof(ToolOffsetModeTable) / sizeof(ToolOffsetModeTable[0]));

QString lookupName(const NameKeyPair *table, int size, int value)
{
    for (int i = 0; i < size; ++i) {
        if (table[i].value == value) {
            return QString::fromUtf8(table[i].name);
        }
    }
    return QString();
}

QString lookupKey(const NameKeyPair *table, int size, int value)
{
    for (int i = 0; i < size; ++i) {
        if (table[i].value == value) {
            return QString::fromLatin1(table[i].key);
        }
    }
    return QString();
}

//! 反查：既接受英文键（配置文件里写的），也接受中文名（操作工手工填的）。
int lookupValue(const NameKeyPair *table, int size, const QString &name)
{
    const QString text = name.trimmed().toLower();
    if (text.isEmpty()) {
        return -1;
    }

    for (int i = 0; i < size; ++i) {
        if (text == QString::fromLatin1(table[i].key).toLower()) {
            return table[i].value;
        }
        if (name.trimmed() == QString::fromUtf8(table[i].name)) {
            return table[i].value;
        }
    }
    return -1;
}

QStringList keysOf(const NameKeyPair *table, int size)
{
    QStringList keys;
    for (int i = 0; i < size; ++i) {
        keys << QString::fromLatin1(table[i].key);
    }
    return keys;
}

/*!
 * 通用解析：先按文本查表（状态字是枚举名或中文的情况），
 * 再按数值处理（状态字是整数的情况）。
 * 顺序不能颠倒：很多系统用 1 表示 "running"，而 running 的枚举值恰好也是 1，
 * 先查文本能保证 "running" 这种字符串永远不会被 QVariant::toInt 误判成 0。
 */
bool resolveValue(const NameKeyPair *table, int size, const QVariant &value, int *out)
{
    if (!out || !value.isValid() || value.isNull()) {
        return false;
    }

    const QString text = value.toString().trimmed();
    if (!text.isEmpty()) {
        const int byName = lookupValue(table, size, text);
        if (byName >= 0) {
            *out = byName;
            return true;
        }
    }

    bool ok = false;
    const int raw = value.toInt(&ok);
    if (!ok || raw < 0 || raw >= size) {
        // 注意这里的上界用的是"表的元素个数"。各枚举的值都是从 0 开始连续编号，
        // 因此表长恰好等于合法取值个数，用它当上界既能挡住越界值，又不必额外
        // 维护一份"最大值"常量（那种常量最容易在加枚举值时忘记同步）。
        // 越界值直接判为无效：宁可保持上一状态，也不要把机床显示成"未知状态"。
        return false;
    }

    *out = raw;
    return true;
}

} // namespace

// ---- AxisState ----

QString axisStateName(AxisState s)
{
    const QString name = lookupName(AxisStateTable, AxisStateTableSize, static_cast<int>(s));
    if (name.isEmpty()) {
        return QStringLiteral("未知");
    }
    return name;
}

QString axisStateKey(AxisState s)
{
    const QString key = lookupKey(AxisStateTable, AxisStateTableSize, static_cast<int>(s));
    if (key.isEmpty()) {
        return QStringLiteral("unknown");
    }
    return key;
}

AxisState axisStateFromName(const QString &name)
{
    const int value = lookupValue(AxisStateTable, AxisStateTableSize, name);
    if (value < 0) {
        return AxisDisabled;
    }
    return static_cast<AxisState>(value);
}

bool axisStateFromValue(const QVariant &value, AxisState *out)
{
    int raw = 0;
    if (!resolveValue(AxisStateTable, AxisStateTableSize, value, &raw)) {
        return false;
    }
    if (out) {
        *out = static_cast<AxisState>(raw);
    }
    return true;
}

// ---- MachineState ----

QString machineStateName(MachineState s)
{
    const QString name = lookupName(MachineStateTable, MachineStateTableSize, static_cast<int>(s));
    if (name.isEmpty()) {
        return QStringLiteral("未知");
    }
    return name;
}

QString machineStateKey(MachineState s)
{
    const QString key = lookupKey(MachineStateTable, MachineStateTableSize, static_cast<int>(s));
    if (key.isEmpty()) {
        return QStringLiteral("unknown");
    }
    return key;
}

MachineState machineStateFromName(const QString &name)
{
    const int value = lookupValue(MachineStateTable, MachineStateTableSize, name);
    if (value < 0) {
        return MachineOffline;
    }
    return static_cast<MachineState>(value);
}

bool machineStateFromValue(const QVariant &value, MachineState *out)
{
    int raw = 0;
    if (!resolveValue(MachineStateTable, MachineStateTableSize, value, &raw)) {
        return false;
    }
    if (out) {
        *out = static_cast<MachineState>(raw);
    }
    return true;
}

// ---- SpindleState ----

QString spindleStateName(SpindleState s)
{
    const QString name = lookupName(SpindleStateTable, SpindleStateTableSize, static_cast<int>(s));
    if (name.isEmpty()) {
        return QStringLiteral("未知");
    }
    return name;
}

QString spindleStateKey(SpindleState s)
{
    const QString key = lookupKey(SpindleStateTable, SpindleStateTableSize, static_cast<int>(s));
    if (key.isEmpty()) {
        return QStringLiteral("unknown");
    }
    return key;
}

SpindleState spindleStateFromName(const QString &name)
{
    const int value = lookupValue(SpindleStateTable, SpindleStateTableSize, name);
    if (value < 0) {
        return SpindleStopped;
    }
    return static_cast<SpindleState>(value);
}

bool spindleStateFromValue(const QVariant &value, SpindleState *out)
{
    int raw = 0;
    if (!resolveValue(SpindleStateTable, SpindleStateTableSize, value, &raw)) {
        return false;
    }
    if (out) {
        *out = static_cast<SpindleState>(raw);
    }
    return true;
}

// ---- InterpolationMode ----

QString interpolationModeName(InterpolationMode m)
{
    const QString name = lookupName(InterpolationModeTable, InterpolationModeTableSize,
                                    static_cast<int>(m));
    if (name.isEmpty()) {
        return QStringLiteral("未知");
    }
    return name;
}

QString interpolationModeKey(InterpolationMode m)
{
    const QString key = lookupKey(InterpolationModeTable, InterpolationModeTableSize,
                                  static_cast<int>(m));
    if (key.isEmpty()) {
        return QStringLiteral("unknown");
    }
    return key;
}

InterpolationMode interpolationModeFromName(const QString &name)
{
    const int value = lookupValue(InterpolationModeTable, InterpolationModeTableSize, name);
    if (value < 0) {
        return InterpNone;
    }
    return static_cast<InterpolationMode>(value);
}

bool interpolationModeFromValue(const QVariant &value, InterpolationMode *out)
{
    int raw = 0;
    if (!resolveValue(InterpolationModeTable, InterpolationModeTableSize, value, &raw)) {
        return false;
    }
    if (out) {
        *out = static_cast<InterpolationMode>(raw);
    }
    return true;
}

int interpolationModeGCode(InterpolationMode m)
{
    switch (m) {
    case InterpRapid:       return 0;
    case InterpLinear:      return 1;
    case InterpCircularCW:  return 2;
    case InterpCircularCCW: return 3;
    case InterpThreading:   return 33;
    case InterpNone:
    default:                return -1;
    }
}

// ---- ToolOffsetMode ----

QString toolOffsetModeName(ToolOffsetMode m)
{
    const QString name = lookupName(ToolOffsetModeTable, ToolOffsetModeTableSize,
                                    static_cast<int>(m));
    if (name.isEmpty()) {
        return QStringLiteral("未知");
    }
    return name;
}

QString toolOffsetModeKey(ToolOffsetMode m)
{
    const QString key = lookupKey(ToolOffsetModeTable, ToolOffsetModeTableSize,
                                  static_cast<int>(m));
    if (key.isEmpty()) {
        return QStringLiteral("unknown");
    }
    return key;
}

ToolOffsetMode toolOffsetModeFromName(const QString &name)
{
    const int value = lookupValue(ToolOffsetModeTable, ToolOffsetModeTableSize, name);
    if (value < 0) {
        return ToolOffsetNone;
    }
    return static_cast<ToolOffsetMode>(value);
}

bool toolOffsetModeFromValue(const QVariant &value, ToolOffsetMode *out)
{
    int raw = 0;
    if (!resolveValue(ToolOffsetModeTable, ToolOffsetModeTableSize, value, &raw)) {
        return false;
    }
    if (out) {
        *out = static_cast<ToolOffsetMode>(raw);
    }
    return true;
}

// ---- 全集 ----

QStringList axisStateKeys()
{
    return keysOf(AxisStateTable, AxisStateTableSize);
}

QStringList machineStateKeys()
{
    return keysOf(MachineStateTable, MachineStateTableSize);
}

QStringList spindleStateKeys()
{
    return keysOf(SpindleStateTable, SpindleStateTableSize);
}

QStringList interpolationModeKeys()
{
    return keysOf(InterpolationModeTable, InterpolationModeTableSize);
}

QStringList toolOffsetModeKeys()
{
    return keysOf(ToolOffsetModeTable, ToolOffsetModeTableSize);
}

// ---- 报警文本兜底 ----

QString defaultAlarmText(int alarmCode)
{
    // 覆盖数控系统里出现频率最高的通用报警号。各厂家编号并不统一
    // （同一号码在西门子和发那科上含义不同），因此这只是一层兜底：
    // 只要后处理/点表能提供报警文本位号，就应以设备给的文本为准。
    switch (alarmCode) {
    case 0:
        return QString();
    case 1:
        return QStringLiteral("急停被按下");
    case 2:
        return QStringLiteral("伺服过载");
    case 3:
        return QStringLiteral("超程（行程极限）");
    case 4:
        return QStringLiteral("驱动器未就绪");
    case 5:
        return QStringLiteral("编码器反馈异常");
    case 6:
        return QStringLiteral("跟随误差超限");
    case 7:
        return QStringLiteral("主轴报警");
    case 8:
        return QStringLiteral("刀库/换刀异常");
    case 9:
        return QStringLiteral("程序语法错误");
    case 10:
        return QStringLiteral("气压不足");
    case 11:
        return QStringLiteral("润滑不足");
    case 12:
        return QStringLiteral("冷却液不足");
    case 13:
        return QStringLiteral("导轨/丝杠过热");
    case 14:
        return QStringLiteral("通讯中断");
    default:
        // 未知报警号也必须给出一句话，界面上只显示数字等于没显示。
        return QStringLiteral("机床报警 %1（未在通用报警表中定义）").arg(alarmCode);
    }
}

// ---- 轴名规范化 ----

QString normalizeAxisName(const QString &name)
{
    QString text = name.trimmed().toUpper();

    // 现场点表里常见 "X轴" / "X AXIS" / "X-轴" 等写法，统统收敛成 "X"，
    // 否则机床模型里会凭空多出 "X轴" 这根不存在的轴。
    if (text.endsWith(QChar(0x8F74))) { // '轴'
        text.chop(1);
    }
    if (text.endsWith(QStringLiteral("AXIS"))) {
        text.chop(4);
    }

    text = text.trimmed();
    while (!text.isEmpty()) {
        const QChar last = text.at(text.size() - 1);
        if (last.isLetterOrNumber()) {
            break;
        }
        text.chop(1);
    }

    return text;
}

} // namespace QScadaCncEnums
