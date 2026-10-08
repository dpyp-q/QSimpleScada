#ifndef QSCADASIMULATIONSOURCE_H
#define QSCADASIMULATIONSOURCE_H

#include "qscadadatasource.h"
#include "qscadatagvalue.h"

#include <QElapsedTimer>
#include <QHash>
#include <QString>
#include <QStringList>
#include <QVariant>

class QTimer;

/*!
 * \brief 仿真采集驱动：不接任何硬件，凭空产生"像真的一样"的数据。
 *
 * ## 为什么项目里必须有这么一个驱动
 *
 * 1. **现场设备不是随时可用的**。真机在客户车间，调试窗口常常只有几个小时；
 *    设备被别的调试人员占着、断电、网段不通都是常态。有了仿真源，采集层以上的
 *    每一层（数据中枢、历史库、报警、趋势、界面）都能在办公室里被完整地跑通、
 *    改坏、再改回来，不必每次改动都去抢设备。
 * 2. **评审者拿到的是一份"下载即可运行"的工程**。面试官或评审手头不会有 PLC，
 *    也不会去配一个 Modbus 从站；但只要有这个驱动，"打开软件 -> 看到趋势曲线
 *    在动 -> 手点一下设定值 -> 曲线平滑爬升 -> 注入故障 -> 报警因坏质量码触发"
 *    这条完整链路当场就能演示完。
 * 3. **它是单元测试与压测的数据发生器**。`addSyntheticTags()` 能凭空造出几百个
 *    位号，配合 100 ms 采集周期即可验证中枢吞吐、视图节流、历史库批量写入的
 *    性能。这类测试在真机上根本没法重复做——不可能让客户的生产线配合你压测。
 *
 * ## 它模拟了什么
 *
 *  · 波形：恒值 / 正弦 / 锯齿 / 方波 / 随机游走 / 计数，每个位号可独立配置；
 *  · 测量噪声：按幅度比例叠加，让曲线像现场数据而不是数学函数；
 *  · **一阶惯性跟踪**：调用 `setSetpoint()` 之后，该位号的值按时间常数 tau
 *    平滑趋近目标值。配合 `writeTag()` 就能演示"上位机下发设定值 -> 过程值
 *    平滑爬升"，这是工业上位机最典型的闭环交互，比"写进去瞬间跳变"真实得多
 *    （真机上数值瞬间跳变通常意味着传感器坏了）；
 *  · **故障注入**：按概率制造丢包，并让这些点的质量码变成 QualityUncertain。
 *    注意是"照常上报，但带坏质量码"，不是"不上报"——项目的质量码机制
 *    （历史曲线画虚线、报警区分"真值"与"通讯故障"）正是为这种场景准备的，
 *    用它刚好能验证下游对坏质量的处理是否正确。
 *
 * ## 三个必须说明的实现细节
 *
 *  · **用 QElapsedTimer 计算真实 dt，而不是假设 dt 等于采集周期**。QTimer 在系统
 *    繁忙时必然漂移，按标称周期积分会让波形周期越来越不准（演示十分钟就能看出
 *    正弦的实际周期比设定值长一截）。
 *  · **一阶跟踪的步长系数用 qBound 夹在 [0,1]**。dt 可能远大于 tau（断点调试、
 *    笔记本休眠后恢复），不夹住就会一步冲过目标值、来回振荡甚至发散；夹住之后
 *    最坏情况是"一步到位"，这是大 dt 下唯一合理的近似。
 *  · **上报前按位号的数据类型做一次强制转换**。真机读回来的 Int16 永远是整数、
 *    UInt16 永远不会是负数；如果仿真源一律发 double，界面上就会出现
 *    "产量 41.87 件"这种一眼假的数据，还会掩盖整数截断/溢出这类真实问题的表现。
 *
 * ## 质量码是怎么送出去的
 *
 * 基类信号 `valueChanged(QString, QVariant)` 里没有质量码的位置，而它是所有驱动
 * 共用的公共契约（不该为了仿真器的一个特性去改它）。所以本类额外声明
 * `valueChangedWithQuality(QString, QVariant, QScadaQuality)`：
 * `QScadaDataSourceManager` 检测到某个驱动带这个信号时会优先连它，坏质量码于是
 * 能一路送到中枢。**因此仿真源必须经管理器汇聚**；如果绕过管理器把
 * `valueChanged` 直接连到 `hub->publish()`，注入的 QualityUncertain 会被当成
 * QualityGood，故障注入就白做了。
 */
class QScadaSimulationSource : public QScadaDataSource
{
    Q_OBJECT
public:
    //! 波形种类。默认 Constant，未配置的位号在首次采集时会按点表元数据自动套用。
    enum Waveform {
        Constant = 0,  //!< 恒值：值等于 constantValue
        Sine,          //!< 正弦：offset + amplitude * sin(2*pi*t/period)
        Sawtooth,      //!< 锯齿：在 offset ± amplitude 之间线性上升后回跳
        Square,        //!< 方波：在 offset ± amplitude 之间跳变（常用来演 0/1 开关量）
        RandomWalk,    //!< 有界随机游走：围绕 offset 缓漂，反射回量程内
        Counter        //!< 计数器：每个采集周期自增，形成台阶上升的产量曲线
    };
    Q_ENUM(Waveform)

    explicit QScadaSimulationSource(QObject *parent = nullptr);
    ~QScadaSimulationSource() override;

    QString driverName() const override { return QStringLiteral("simulator"); }

    /*!
     * 波形与参数（每个位号独立）。
     *
     * 所有随时间的波形都以 offset 为直流分量 / 中心线；Constant 是唯一的例外：
     * 它直接取 constantValue，不再叠加 offset —— 否则"设了恒值 25 结果读回 30"
     * 这种歧义会让人怀疑是驱动算错了。
     *
     * 这些 setter 可以在运行中随时调用，下一个采集周期即生效（无需重开驱动）。
     * 另外：任何一次显式配置都会关掉该位号的"自动波形"（见 setAutoConfigure）。
     */
    void setWaveform(const QString &key, Waveform waveform);
    Waveform waveform(const QString &key) const;
    void setAmplitude(const QString &key, double amplitude);
    double amplitude(const QString &key) const;
    void setOffset(const QString &key, double offset);
    double offset(const QString &key) const;
    void setPeriod(const QString &key, double seconds);
    double period(const QString &key) const;
    //! 测量噪声幅度，按 amplitude 的比例给（0.01 = 1%）。
    void setNoise(const QString &key, double ratio);
    double noise(const QString &key) const;
    void setConstantValue(const QString &key, double value);
    double constantValue(const QString &key) const;

    /*!
     * 把一个位号设成"设定值语义"：写入的值作为目标，过程值按一阶惯性趋近它。
     * tau 是时间常数（秒），约 3~5 个 tau 之后基本到位——和现场调节回路的
     * 响应时间是一个量级，演示时能明显看出"跟随"而不是"跳变"。
     */
    void setSetpoint(const QString &key, double target);
    double setpoint(const QString &key) const;
    bool hasSetpoint(const QString &key) const;
    //! 取消设定值语义，该位号重新按波形运动。
    void clearSetpoint(const QString &key);
    void setTimeConstant(const QString &key, double tauSeconds);
    double timeConstant(const QString &key) const;

    /*!
     * 故障注入：按 dropRate 的概率让某些点本次"读不回来"。
     * 被命中的点上报的是上一次成功读到的值 + QualityUncertain 质量码，
     * 模拟"通讯丢了、值停留在上一拍"这一现场最常见的坏数据形态。
     * dropRate 被夹在 [0, 1]；默认为 0.1（10% 丢包）。
     */
    void setFaultInjection(bool enabled, double dropRate = 0.1);
    bool faultInjectionEnabled() const { return mFaultInjection; }
    double dropRate() const { return mDropRate; }
    quint64 injectedDropCount() const { return mInjectedDrops; }

    /*!
     * 自动波形（默认开启）：点表里新加一个位号时，按它的数据类型 / 单位 / 名称
     * 自动编一段像样的波形，省掉"每个点都手工配一遍"的体力活。
     * 一旦对某个位号调用过上面任何一个 setter，该位号的自动配置就不再改动它。
     */
    void setAutoConfigure(bool enabled);
    bool autoConfigure() const { return mAutoConfigure; }
    //! 立刻对当前已知的全部位号套用自动波形，返回被配置的位号个数。
    int autoConfigureTags();

    //! 把所有位号（含压测临时建的位号）的波形统一改成同一种。
    void setWaveformForAll(Waveform waveform);

    /*!
     * 造一批合成位号并追加到现有点表后面，返回新建位号的 key 列表。
     * 供单元测试与压测使用：不必手写几百行的点表 JSON。
     */
    QStringList addSyntheticTags(int count,
                                 const QString &keyPrefix = QStringLiteral("SIM"),
                                 const QString &deviceIp = QString());

    //! 当前仿真值（不含测量噪声），供界面/测试观察内部状态。
    double simulatedValue(const QString &key) const;
    //! 已经建立仿真状态的位号 key（点表里的点在被采过一次后才出现在这里）。
    QStringList configuredKeys() const;
    //! 累计上报的点数，用来和 QScadaDataHub::pointsPerSecond() 互相印证。
    quint64 sampleCount() const { return mSampleCount; }

    static QString waveformName(Waveform waveform);

public slots:
    void open() override;
    void close() override;
    /*!
     * 写值：配了设定值语义的位号把它当成新的目标值（随后平滑跟随）；
     * 其余位号直接跳变，并把波形切成恒值——否则下一个周期波形会立刻把写入值
     * 覆盖掉，操作员会看到"刚下发的值自己跳回去了"。
     * 两种情况都会立即回发一次当前值，让界面马上有反馈。
     */
    void writeTag(const QString &key, const QVariant &value) override;

signals:
    /*!
     * 带质量码的扩展信号（基类 valueChanged 之外的可选能力，约定见类注释）。
     * 参数类型必须与 `QScadaQuality` 完全一致，管理器的字符串连接才能匹配上。
     */
    void valueChangedWithQuality(const QString &tagKey, const QVariant &value, QScadaQuality quality);

private slots:
    void onTick();

private:
    //! 单个位号的仿真参数与运行状态。
    struct TagSimulation {
        // —— 配置（可由用户修改）——
        Waveform waveform = Constant;
        double amplitude = 1.0;
        double offset = 0.0;
        double period = 10.0;
        double noise = 0.0;
        double constantValue = 0.0;
        double tau = 2.0;
        double setpoint = 0.0;
        bool hasSetpoint = false;
        // —— 运行状态 ——
        double value = 0.0;        //!< 当前过程值（不含测量噪声）
        double phase = 0.0;        //!< 波形相位（秒），按真实 dt 累加
        double counter = 0.0;      //!< Counter 波形的累计值
        double lastValue = 0.0;    //!< 上一次成功上报的值（丢包时上报它）
        bool hasLastValue = false;
        bool started = false;      //!< 是否已经用初值初始化过
        bool userConfigured = false; //!< 用户显式配置过，自动波形不再插手
        bool autoApplied = false;    //!< 已经套过自动波形
        quint64 samples = 0;         //!< 该位号累计上报次数
    };

    static const TagSimulation &defaults();
    const TagSimulation *findState(const QString &key) const;
    //! 取（必要时创建并自动配置）某个位号的仿真状态。
    TagSimulation &state(const QString &key);
    void applyAutoConfig(const QString &key, TagSimulation &s);
    //! 按 dt 推进一个位号的仿真值，返回推进后的过程值。
    double advance(TagSimulation &s, double dt);
    //! 相对 offset 的测量噪声。
    double noiseOffset(const TagSimulation &s) const;
    //! 把随机游走反射回 [center-span, center+span]。
    static double reflectIntoBand(double center, double halfSpan, double value);
    //! 按位号的数据类型把仿真值整形成"真机读回来会是什么样"。
    QVariant coerce(const QScadaTagDefinition &definition, double value) const;
    void publishSample(const QString &key, const QVariant &value, QScadaQuality quality);

    QHash<QString, TagSimulation> mStates;
    QTimer *mTimer;
    QElapsedTimer mClock;
    qint64 mLastTickMs;
    bool mFaultInjection;
    double mDropRate;
    bool mAutoConfigure;
    quint64 mInjectedDrops;
    quint64 mSampleCount;
};

#endif // QSCADASIMULATIONSOURCE_H
