#include "qscadasimulationsource.h"

#include <QRandomGenerator>
#include <QTimer>
#include <QtGlobal>
#include <QtMath>

#include <cmath>

namespace {
//! 2*pi。不用 M_PI：MSVC 默认不定义它（要 _USE_MATH_DEFINES），自己写一个最省心。
const double kTwoPi = 6.283185307179586476925286766559;
//! 周期与时间常数的下限：0 会让波形除零、让一阶跟踪退化成别的东西。
const double kMinPeriodSeconds = 0.001;
const double kMinTimeConstantSeconds = 0.001;
//! 噪声比例上限（100%）：再大就不是测量噪声而是乱码，没有演示价值。
const double kMaxNoiseRatio = 1.0;
}

QScadaSimulationSource::QScadaSimulationSource(QObject *parent)
    : QScadaDataSource(parent)
    , mTimer(nullptr)
    , mLastTickMs(0)
    , mFaultInjection(false)
    , mDropRate(0.0)
    , mAutoConfigure(true)
    , mInjectedDrops(0)
    , mSampleCount(0)
{
    mTimer = new QTimer(this);
    // 采集定时器用粗精度：仿真源不需要毫秒级准点，而真实 dt 由 QElapsedTimer
    // 量出来，定时器抖动不会污染波形周期。
    mTimer->setTimerType(Qt::CoarseTimer);
    connect(mTimer, &QTimer::timeout, this, &QScadaSimulationSource::onTick);
}

QScadaSimulationSource::~QScadaSimulationSource()
{
    // 对象可能在没 close() 的情况下就被删掉（例如管理器析构时统一回收），
    // 这里兜底停表，避免定时器在析构过程中还触发回调。
    if (mTimer)
        mTimer->stop();
}

const QScadaSimulationSource::TagSimulation &QScadaSimulationSource::defaults()
{
    // C++11 起函数内静态对象的初始化是线程安全的。用它当"参数默认值"的唯一出处，
    // 避免同样的默认值在十几个 getter 里各抄一遍（抄漏一处就是难查的不一致）。
    static const TagSimulation s;
    return s;
}

const QScadaSimulationSource::TagSimulation *QScadaSimulationSource::findState(const QString &key) const
{
    const QHash<QString, TagSimulation>::const_iterator it = mStates.constFind(key);
    if (it == mStates.constEnd())
        return nullptr;
    return &it.value();
}

QScadaSimulationSource::TagSimulation &QScadaSimulationSource::state(const QString &key)
{
    // QHash::operator[] 在 key 不存在时插入一份默认参数，正好用来"按需建点"：
    // 点表里的点在第一次被采集时才产生状态，不预先占内存。
    TagSimulation &s = mStates[key];
    if (mAutoConfigure && !s.userConfigured && !s.autoApplied)
        applyAutoConfig(key, s);
    return s;
}

/*!
 * 波形与参数配置。
 *
 * 每个 setter 都先判空 key 再改状态，并且把 userConfigured 置位——自动波形
 * 一旦发现位号被显式配置过就永久退出，避免"我明明设了方波，跑了十分钟自己
 * 变回正弦"这种让人怀疑软件有 bug 的行为。
 */
void QScadaSimulationSource::setWaveform(const QString &key, Waveform waveform)
{
    if (key.isEmpty())
        return;

    TagSimulation &s = state(key);
    s.waveform = waveform;
    s.userConfigured = true;

    // 相位清零：否则从锯齿切到正弦会从一个随机相位开始，看起来像"曲线抽搐了一下"。
    s.phase = 0.0;

    if (waveform == Counter) {
        // 切到计数器时保住值的连续性：把当前值折算成计数初值，
        // 否则曲线会先掉回 offset 再往上爬，像设备重启了一样。
        s.counter = s.value - s.offset;
        if (s.counter < 0.0)
            s.counter = 0.0;
    }
}

QScadaSimulationSource::Waveform QScadaSimulationSource::waveform(const QString &key) const
{
    const TagSimulation *s = findState(key);
    return s ? s->waveform : defaults().waveform;
}

void QScadaSimulationSource::setAmplitude(const QString &key, double amplitude)
{
    if (key.isEmpty())
        return;
    TagSimulation &s = state(key);
    s.amplitude = amplitude; // 允许负值：等价于波形取反，没必要专门禁止
    s.userConfigured = true;
}

double QScadaSimulationSource::amplitude(const QString &key) const
{
    const TagSimulation *s = findState(key);
    return s ? s->amplitude : defaults().amplitude;
}

void QScadaSimulationSource::setOffset(const QString &key, double offset)
{
    if (key.isEmpty())
        return;
    TagSimulation &s = state(key);
    s.offset = offset;
    s.userConfigured = true;
}

double QScadaSimulationSource::offset(const QString &key) const
{
    const TagSimulation *s = findState(key);
    return s ? s->offset : defaults().offset;
}

void QScadaSimulationSource::setPeriod(const QString &key, double seconds)
{
    if (key.isEmpty())
        return;
    TagSimulation &s = state(key);
    s.period = qMax(kMinPeriodSeconds, seconds);
    s.userConfigured = true;
}

double QScadaSimulationSource::period(const QString &key) const
{
    const TagSimulation *s = findState(key);
    return s ? s->period : defaults().period;
}

void QScadaSimulationSource::setNoise(const QString &key, double ratio)
{
    if (key.isEmpty())
        return;
    TagSimulation &s = state(key);
    s.noise = qBound(0.0, ratio, kMaxNoiseRatio);
    s.userConfigured = true;
}

double QScadaSimulationSource::noise(const QString &key) const
{
    const TagSimulation *s = findState(key);
    return s ? s->noise : defaults().noise;
}

void QScadaSimulationSource::setConstantValue(const QString &key, double value)
{
    if (key.isEmpty())
        return;

    TagSimulation &s = state(key);
    s.constantValue = value;
    s.userConfigured = true;

    // 只有在恒值波形下才立刻改当前值：其它波形下 constantValue 只是个"备用的值"，
    // 现在改它不应该让正在摆动的曲线跳一下。
    if (s.waveform == Constant) {
        s.value = value;
        s.started = true;
    }
}

double QScadaSimulationSource::constantValue(const QString &key) const
{
    const TagSimulation *s = findState(key);
    return s ? s->constantValue : defaults().constantValue;
}

/*!
 * 设定值语义（一阶惯性跟踪）。
 *
 * 下发的目标值只写进 setpoint，不动当前值：过程值应当从当前工况平滑爬向目标，
 * 这正是要演示的"跟随"。真机上如果写进去就瞬间到位，通常意味着反馈通道断了
 * 或者传感器被强制了，反而是故障征兆。
 */
void QScadaSimulationSource::setSetpoint(const QString &key, double target)
{
    if (key.isEmpty())
        return;
    TagSimulation &s = state(key);
    s.setpoint = target;
    s.hasSetpoint = true;
    s.userConfigured = true;
}

double QScadaSimulationSource::setpoint(const QString &key) const
{
    const TagSimulation *s = findState(key);
    return s ? s->setpoint : defaults().setpoint;
}

bool QScadaSimulationSource::hasSetpoint(const QString &key) const
{
    const TagSimulation *s = findState(key);
    return s ? s->hasSetpoint : false;
}

void QScadaSimulationSource::clearSetpoint(const QString &key)
{
    // 用 find 而不是 operator[]：取消一个不存在的位号不该顺手把它创建出来。
    QHash<QString, TagSimulation>::iterator it = mStates.find(key);
    if (it == mStates.end())
        return;
    it.value().hasSetpoint = false;
    // 相位一并重置：重新按波形运动时从 0 相位开始，避免曲线从随机位置接上。
    it.value().phase = 0.0;
}

void QScadaSimulationSource::setTimeConstant(const QString &key, double tauSeconds)
{
    if (key.isEmpty())
        return;
    TagSimulation &s = state(key);
    s.tau = qMax(kMinTimeConstantSeconds, tauSeconds);
    s.userConfigured = true;
}

double QScadaSimulationSource::timeConstant(const QString &key) const
{
    const TagSimulation *s = findState(key);
    return s ? s->tau : defaults().tau;
}

void QScadaSimulationSource::setFaultInjection(bool enabled, double dropRate)
{
    mFaultInjection = enabled;
    mDropRate = qBound(0.0, dropRate, 1.0);
}

void QScadaSimulationSource::setAutoConfigure(bool enabled)
{
    mAutoConfigure = enabled;
    if (mAutoConfigure) {
        // 打开开关就立刻补齐之前没配置过的点，而不是等下一拍：
        // 调用方紧接着查参数时能立刻看到结果。
        autoConfigureTags();
    }
}

int QScadaSimulationSource::autoConfigureTags()
{
    int applied = 0;
    const QList<QScadaTagDefinition> definitions = tags();
    for (int i = 0; i < definitions.size(); ++i) {
        // 这里遍历的是点表（而不是 mStates），因此循环体里改哈希表是安全的。
        TagSimulation &s = mStates[definitions.at(i).key()];
        if (s.userConfigured || s.autoApplied)
            continue;
        applyAutoConfig(definitions.at(i).key(), s);
        ++applied;
    }
    return applied;
}

void QScadaSimulationSource::setWaveformForAll(Waveform waveform)
{
    const QList<QScadaTagDefinition> definitions = tags();
    for (int i = 0; i < definitions.size(); ++i)
        setWaveform(definitions.at(i).key(), waveform);

    // 不在点表里但已经建过状态的位号（写值临时建的、压测造的）也要跟上。
    // 先取 key 快照再遍历：setWaveform() 会写 mStates，边迭代边改哈希表是未定义行为。
    const QStringList knownKeys = mStates.keys();
    for (int i = 0; i < knownKeys.size(); ++i) {
        if (!hasTag(knownKeys.at(i)))
            setWaveform(knownKeys.at(i), waveform);
    }
}

QStringList QScadaSimulationSource::addSyntheticTags(int count, const QString &keyPrefix, const QString &deviceIp)
{
    QStringList created;
    if (count <= 0)
        return created;

    QList<QScadaTagDefinition> all = tags(); // 追加而不是替换：点表里原有的位号必须保留
    const QString prefix = keyPrefix.isEmpty() ? QStringLiteral("SIM") : keyPrefix;
    const int width = QString::number(count).size(); // 序号定长，字典序才等于数值序

    for (int i = 0; i < count; ++i) {
        const QString key = QStringLiteral("%1.%2")
                                .arg(prefix,
                                     QString::number(i + 1).rightJustified(width, QLatin1Char('0')));
        if (hasTag(key) || created.contains(key))
            continue; // 绝不覆盖已有点位

        QScadaTagDefinition definition;
        definition.setKey(key);
        definition.setDisplayName(QStringLiteral("仿真点 %1").arg(key));
        // 仿真源不用真实地址，但仍给一个稳定的值：万一有人把同一份点表切到 Modbus
        // 上调试，也不至于所有点都挤在地址 0 上。
        definition.setAddress(static_cast<quint16>(i % 4096));
        // 故意混用几种数据类型：压测与单元测试要覆盖整型/浮点/布尔在解码、质量码、
        // 界面显示上的不同分支，全用 float 就测不到。
        switch (i % 4) {
        case 0:
            definition.setDataType(QScadaTagDefinition::DataTypeFloat32);
            break;
        case 1:
            definition.setDataType(QScadaTagDefinition::DataTypeInt16);
            break;
        case 2:
            definition.setDataType(QScadaTagDefinition::DataTypeUInt16);
            break;
        default:
            definition.setDataType(QScadaTagDefinition::DataTypeBool);
            break;
        }
        if (i % 4 == 0)
            definition.setUnit(QStringLiteral("%"));
        definition.setDeviceIp(deviceIp);
        definition.setEnabled(true);

        all.append(definition);
        created.append(key);
    }

    setTags(all);

    // 新点位立刻套上自动波形：否则要等到第一次采集才配置，
    // 调用方紧接着读 waveform() 会拿到默认值，以为配置没生效。
    for (int i = 0; i < created.size(); ++i)
        state(created.at(i));

    return created;
}

double QScadaSimulationSource::simulatedValue(const QString &key) const
{
    const TagSimulation *s = findState(key);
    return s ? s->value : defaults().value;
}

QStringList QScadaSimulationSource::configuredKeys() const
{
    return mStates.keys();
}

QString QScadaSimulationSource::waveformName(Waveform waveform)
{
    switch (waveform) {
    case Constant:   return QStringLiteral("恒值");
    case Sine:       return QStringLiteral("正弦");
    case Sawtooth:   return QStringLiteral("锯齿");
    case Square:     return QStringLiteral("方波");
    case RandomWalk: return QStringLiteral("随机游走");
    case Counter:    return QStringLiteral("计数");
    }
    return QStringLiteral("未知");
}

/*!
 * 自动波形：让"下载即可运行"成立的关键一步。
 *
 * 判据只用点表里本来就必填的元数据（数据类型、单位、名称），不要求现场工程师
 * 额外填任何东西。目的不是"物理正确"，而是让任何一个陌生点表接上仿真源后，
 * 界面上的曲线立刻就是"像现场数据"的样子：开关量在 0/1 跳、温度在室温附近慢漂、
 * 负载率在 50% 上下波动、产量单调上涨。
 */
void QScadaSimulationSource::applyAutoConfig(const QString &key, TagSimulation &s)
{
    s.autoApplied = true;

    const QScadaTagDefinition definition = tag(key);
    const QString name = definition.displayName().isEmpty() ? key : definition.displayName();
    const QString probe = (key + QLatin1Char(' ') + name + QLatin1Char(' ') + definition.unit()).toLower();

    // 兜底：缓慢随机游走。温度、压力、流量这类过程量本来就都在慢漂，
    // 比一律用正弦更像真实数据。
    s.waveform = RandomWalk;
    s.offset = 0.0;
    s.amplitude = 1.0;
    s.period = 20.0;
    s.noise = 0.005; // 0.5% 仪表噪声：曲线只有带点毛刺才不像"数学函数"

    if (definition.dataType() == QScadaTagDefinition::DataTypeBool) {
        // 开关量：方波 0/1 交替，模拟"运行/停止""门开/关"。
        s.waveform = Square;
        s.offset = 0.0;
        s.amplitude = 1.0;
        s.period = 15.0;
        s.noise = 0.0; // 开关量没有测量噪声
    } else if (probe.contains(QStringLiteral("count")) || probe.contains(QStringLiteral("qty"))
               || probe.contains(QStringLiteral("total")) || probe.contains(QStringLiteral("产量"))
               || probe.contains(QStringLiteral("计数"))) {
        // 产量/计数：每周期 +1 的台阶上升。amplitude 兼作步长。
        s.waveform = Counter;
        s.offset = 0.0;
        s.amplitude = 1.0;
        s.noise = 0.0;
    } else if (probe.contains(QStringLiteral("temp")) || probe.contains(QStringLiteral("温度"))) {
        // 温度：室温附近缓慢正弦，周期两分钟——和真实的热惯性是一个量级。
        s.waveform = Sine;
        s.offset = 25.0;
        s.amplitude = 8.0;
        s.period = 120.0;
    } else if (probe.contains(QStringLiteral("press")) || probe.contains(QStringLiteral("压力"))
               || probe.contains(QStringLiteral("flow")) || probe.contains(QStringLiteral("流量"))
               || probe.contains(QStringLiteral("speed")) || probe.contains(QStringLiteral("转速"))
               || probe.contains(QStringLiteral("current")) || probe.contains(QStringLiteral("电流"))
               || probe.contains(QStringLiteral("%"))) {
        // 压力/流量/转速/负载率：围绕额定值（取 50）波动，周期 30 s。
        s.waveform = Sine;
        s.offset = 50.0;
        s.amplitude = 20.0;
        s.period = 30.0;
    }

    // 启动值同样取波形基线：见 onTick() 里的说明（避免启动瞬间的假报警）。
    s.value = (s.waveform == Constant) ? s.constantValue : s.offset;
    s.started = true;
}

double QScadaSimulationSource::advance(TagSimulation &s, double dt)
{
    // 一阶惯性跟踪优先：配了设定值语义的位号，值按时间常数趋近目标，
    // 此时波形参数不再参与——闭环演示要的是"下发后平滑跟随"，
    // 而不是一边跟随一边还继续按正弦摆动。
    if (s.hasSetpoint) {
        const double tau = qMax(kMinTimeConstantSeconds, s.tau);
        // 严格按 v += (target - v) * dt / tau，但把步长系数夹在 [0,1]：
        // dt 远大于 tau 时（断点调试、笔记本休眠后恢复）不夹住会一步冲过目标、
        // 来回振荡甚至发散，现场表现就是"设定值下发之后数值乱跳"。
        // 夹住之后最坏情况是"一步到位"，这是大 dt 下唯一合理的近似。
        const double alpha = qBound(0.0, dt / tau, 1.0);
        s.value += (s.setpoint - s.value) * alpha;
        return s.value;
    }

    const double period = qMax(kMinPeriodSeconds, s.period);

    switch (s.waveform) {
    case Constant:
        s.value = s.constantValue;
        break;

    case Sine:
        s.phase += dt;
        s.value = s.offset + s.amplitude * qSin(kTwoPi * s.phase / period);
        break;

    case Sawtooth: {
        s.phase += dt;
        double frac = std::fmod(s.phase / period, 1.0);
        if (frac < 0.0)
            frac += 1.0;
        s.value = s.offset + s.amplitude * (2.0 * frac - 1.0);
        break;
    }

    case Square: {
        s.phase += dt;
        double frac = std::fmod(s.phase / period, 1.0);
        if (frac < 0.0)
            frac += 1.0;
        s.value = s.offset + (frac < 0.5 ? s.amplitude : -s.amplitude);
        break;
    }

    case RandomWalk: {
        // 步长按 dt 缩放（"每秒漂移 amplitude/period"）。用固定步长的话，
        // 把采集周期从 1 s 改成 100 ms，曲线变化速度会跟着变成十分之一，
        // 演示时看起来就像"改了周期以后数据不动了"。
        const double span = qAbs(s.amplitude);
        const double step = (QRandomGenerator::global()->generateDouble() * 2.0 - 1.0)
                            * span * dt / period;
        s.value = reflectIntoBand(s.offset, span, s.value + step);
        break;
    }

    case Counter:
        // amplitude 兼作每周期步长：默认 1 就是"每周期一件"的产量计数，
        // 设成 60 就得到"每周期 60 件"的台阶曲线。
        s.value = s.offset + s.counter;
        s.counter += qMax(1.0, qAbs(s.amplitude));
        break;
    }

    return s.value;
}

double QScadaSimulationSource::reflectIntoBand(double center, double halfSpan, double value)
{
    // 反射而不是截断：截断会让曲线"贴"在量程边界上不动（看起来像死值），
    // 反射则保持随机游走的统计特性。这里用取模处理"一步跨过整个量程"的情况
    // ——笔记本休眠几小时后恢复，dt 可能有几千秒，单次反射是不够的。
    const double span = qMax(qAbs(halfSpan), 1e-9);
    const double period4 = 4.0 * span;
    double p = std::fmod((value - center) + span, period4);
    if (p < 0.0)
        p += period4;
    const double folded = (p <= 2.0 * span) ? (p - span) : (3.0 * span - p);
    return center + folded;
}

double QScadaSimulationSource::noiseOffset(const TagSimulation &s) const
{
    // 噪声按幅度比例给："1% 噪声"用在 ±20 的压力上就是 ±0.2。
    if (s.noise <= 0.0)
        return 0.0;
    const double span = qAbs(s.amplitude);
    if (span <= 0.0)
        return 0.0;
    return (QRandomGenerator::global()->generateDouble() * 2.0 - 1.0) * s.noise * span;
}

QVariant QScadaSimulationSource::coerce(const QScadaTagDefinition &definition, double value) const
{
    // 为什么上报前要按位号的数据类型整形：真机读回来的 Int16 永远是整数，
    // UInt16 永远不会是负数，Float32 只有 7 位有效数字。仿真源如果一律发 double，
    // 界面上就会出现"产量 41.87 件""计数器 -12"这种一眼假的数据，
    // 还会掩盖整数截断/溢出这类真实问题在下游的表现。
    switch (definition.dataType()) {
    case QScadaTagDefinition::DataTypeBool:
        return QVariant(value != 0.0);
    case QScadaTagDefinition::DataTypeInt16:
        return QVariant(static_cast<int>(qRound(qBound(-32768.0, value, 32767.0))));
    case QScadaTagDefinition::DataTypeUInt16:
        return QVariant(static_cast<int>(qRound(qBound(0.0, value, 65535.0))));
    case QScadaTagDefinition::DataTypeInt32:
        return QVariant(static_cast<int>(qBound(-2147483648.0, value, 2147483647.0)));
    case QScadaTagDefinition::DataTypeUInt32:
        return QVariant(static_cast<uint>(qBound(0.0, value, 4294967295.0)));
    case QScadaTagDefinition::DataTypeFloat32:
        // 真机的 32 位浮点回读值必然带量化误差，这里照样截成 float，
        // 免得演示曲线"过于干净"而不像现场数据。
        return QVariant(static_cast<double>(static_cast<float>(value)));
    case QScadaTagDefinition::DataTypeFloat64:
        break;
    }
    return QVariant(value);
}

void QScadaSimulationSource::publishSample(const QString &key, const QVariant &value, QScadaQuality quality)
{
    ++mSampleCount;

    // 两条信号都发，各有各的用处：
    //  · valueChanged 是基类契约，任何按基类写的消费者（调试打印、
    //    QScadaCncTagBinding、只关心数值的单元测试）都还能继续用；
    //  · valueChangedWithQuality 多带一个质量码。管理器检测到它之后只连这一条，
    //    因此中枢既不会收到两份，也不会丢掉坏质量码。
    emit valueChanged(key, value);
    emit valueChangedWithQuality(key, value, quality);
}

void QScadaSimulationSource::open()
{
    if (mTimer->isActive())
        return;

    // 仿真源没有握手过程：定时器起来就算"已连接"。
    // 必须先置状态再出数据——管理器把 Connected 映射成 QualityGood，而中枢在
    // 设备质量不是 Good 时会把收到的值一律降级为 NotConnected。顺序反了，
    // 第一批数据就会被打上"未连接"的标记（现场表现是"刚启动时曲线是灰的"）。
    setState(Connected);

    mClock.start();
    mLastTickMs = 0;
    mTimer->start(pollInterval());

    // 立刻出第一帧（推迟到事件循环空闲时执行）：切到画面就能看到初值，
    // 与 Modbus 驱动"连上后先采一次"的行为一致；同时避免在 open() 的调用栈里
    // 就把数据回调给上层（那时调用方可能还没接好信号）。
    QTimer::singleShot(0, this, &QScadaSimulationSource::onTick);
}

void QScadaSimulationSource::close()
{
    mTimer->stop();
    setState(Disconnected);
}

void QScadaSimulationSource::onTick()
{
    // close() 之后可能还有一个"第一帧"的 singleShot 在路上，这里挡掉。
    if (!mTimer->isActive())
        return;

    // 用真实经过的时间积分，而不是假设它等于 pollInterval()：
    // QTimer 在系统繁忙时必然漂移，按标称值积分会让波形周期越来越不准。
    const qint64 nowMs = mClock.isValid() ? mClock.elapsed() : 0;
    double dt = static_cast<double>(nowMs - mLastTickMs) / 1000.0;
    mLastTickMs = nowMs;
    if (dt < 0.0)
        dt = 0.0; // QElapsedTimer 只会前进，这里纯粹是防御

    const QList<QScadaTagDefinition> definitions = tags();
    bool droppedThisCycle = false;

    for (int i = 0; i < definitions.size(); ++i) {
        const QScadaTagDefinition definition = definitions.at(i);
        if (!definition.enabled())
            continue; // 点表里临时停用的点不参与仿真

        TagSimulation &s = state(definition.key());

        if (!s.started) {
            // 启动值取波形基线而不是 0：温度点表的零点常常是 25℃、压力是 0.4 MPa，
            // 从 0 起步会在启动瞬间制造一串假报警（"温度低于下限"）。
            s.value = (s.waveform == Constant) ? s.constantValue : s.offset;
            s.started = true;
        }

        const double processValue = advance(s, dt);

        if (mFaultInjection && QRandomGenerator::global()->generateDouble() < mDropRate) {
            // 丢包：这一次"没读回来"。注意工艺过程不会因为通讯丢包而停下来，
            // 所以内部状态照常推进（上面刚 advance 过），上报的却是上一拍的旧值
            // ——这正是现场最常见的坏数据形态：值看着正常，其实已经不动了。
            ++mInjectedDrops;
            droppedThisCycle = true;
            const double stale = s.hasLastValue ? s.lastValue : processValue;
            publishSample(definition.key(), coerce(definition, stale), QualityUncertain);
            continue;
        }

        const double measured = processValue + noiseOffset(s);
        s.lastValue = measured;
        s.hasLastValue = true;
        ++s.samples;
        publishSample(definition.key(), coerce(definition, measured), QualityGood);
    }

    // 通讯统计按"采集周期"记账而不是按点记账：一轮里哪怕只丢了一个点，
    // 这一轮就算失败。这样注入 10% 丢包时，界面上的成功率会落在 90% 附近，
    // 数字与注入参数对得上，演示和排查都直观。
    if (droppedThisCycle)
        countFailure();
    else
        countSuccess();
}

void QScadaSimulationSource::writeTag(const QString &key, const QVariant &value)
{
    if (key.isEmpty()) {
        reportError(QStringLiteral("写值失败：位号不能为空"));
        return;
    }

    bool ok = false;
    const double target = value.toDouble(&ok);
    if (!ok) {
        reportError(QStringLiteral("写值失败：仿真源只接受数值，位号 %1 收到 %2")
                        .arg(key, value.toString()));
        return;
    }

    TagSimulation &s = state(key);

    if (s.hasSetpoint) {
        // 设定值语义的位号：写入的是目标值，过程值按一阶惯性平滑跟随。
        // 这就是工业上位机最典型的闭环交互——"下发设定值 → 过程值爬升"。
        s.setpoint = target;
    } else {
        // 其余位号直接跳变，并把波形切成恒值：如果还按原波形运动，
        // 下一个采集周期就会把刚写入的值覆盖掉，操作员会看到
        // "刚下发的值自己跳回去了"——现场最忌讳这种不确定的反馈。
        s.waveform = Constant;
        s.constantValue = target;
        s.value = target;
        s.started = true;
    }

    // 无论是哪种语义都立刻回发一次当前值：真机写值后要等下一轮采集才能看到反馈，
    // 仿真源可以让界面立刻响应，演示时手感更接近"操作有反应"。
    const QScadaTagDefinition definition = tag(key);
    s.lastValue = s.value;
    s.hasLastValue = true;
    publishSample(key, coerce(definition, s.value), QualityGood);
    countSuccess();
}
