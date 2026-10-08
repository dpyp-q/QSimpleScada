#include "qscadadatasourcemanager.h"

#include <QDebug>
#include <QMetaMethod>
#include <QMetaObject>
#include <QMetaType>
#include <QtGlobal>

#include <algorithm>

#include "qscadadatahub.h"
#include "qscadamodbustcpclient.h"
#include "qscadaregistermap.h"
#include "qscadasimulationsource.h"

namespace {
//! 点表里只有设备 IP（没有端口字段）；Modbus TCP 的标准端口是 502。
const quint16 kDefaultModbusPort = 502;
//! 默认采集周期：常规点位 1 s 足够。高频演示与压测可以调小（仿真源能跑 100 ms）。
const int kDefaultPollIntervalMs = 1000;
//! 管理器识别的"带质量码"扩展信号名与参数个数（约定见头文件里的说明）。
const char kQualitySignalName[] = "valueChangedWithQuality";
const int kQualitySignalParamCount = 3;
}

QScadaDataSourceManager::QScadaDataSourceManager(QScadaDataHub *hub, QObject *parent)
    : QObject(parent)
    , mHub(hub)
    , mDefaultPort(kDefaultModbusPort)
    , mDefaultPollInterval(kDefaultPollIntervalMs)
{
    // 跨线程排队投递要求参数类型已注册到元类型系统，否则信号会被静默丢弃。
    // 中枢的构造函数里也注册过一次；这里再注册是防御性的——单元测试里可能
    // 根本不建中枢，而管理器照样要能把驱动丢到工作线程上去跑。
    qRegisterMetaType<QScadaQuality>("QScadaQuality");
    qRegisterMetaType<QScadaTagValue>("QScadaTagValue");

    // 内置驱动在这里注册。注意管理器本身不认识这两个具体类型之外的一切：
    // 它只认识"名字 -> 工厂函数"，所以新增协议不需要改这个类的任何逻辑分支。
    registerDriver(QStringLiteral("modbus-tcp"), [](QObject *parent) -> QScadaDataSource * {
        return new QScadaModbusTcpClient(parent);
    });
    registerDriver(QStringLiteral("simulator"), [](QObject *parent) -> QScadaDataSource * {
        return new QScadaSimulationSource(parent);
    });
}

QScadaDataSourceManager::~QScadaDataSourceManager()
{
    // 驱动是本管理器的子对象，父对象析构时 Qt 也会删掉它们；这里先显式停一遍，
    // 是为了让每个驱动都能收到 close()（停掉定时器、断开连接、置为未连接状态），
    // 而不是在事件循环里"突然消失"。
    stopAll();
}

void QScadaDataSourceManager::registerDriver(const QString &name, DriverFactory factory)
{
    if (name.isEmpty() || !factory) {
        qWarning() << "[QScadaDataSourceManager] 忽略非法的驱动注册，名字:" << name;
        return;
    }

    // 同名覆盖是刻意保留的：单元测试可以用它把内置驱动换成"永远连不上"或
    // "只发固定值"的测试替身，从而在没有硬件的情况下验证断线告警、质量码降级
    // 这些分支，不必为测试单独开后门。
    mFactories.insert(name, factory);
}

QStringList QScadaDataSourceManager::availableDrivers() const
{
    QStringList names = mFactories.keys();
    // 哈希表的顺序是不确定的，排序后界面下拉框的位置才不会每次启动都变
    // （现场操作员是按肌肉记忆点菜单的）。
    std::sort(names.begin(), names.end());
    return names;
}

bool QScadaDataSourceManager::hasDriver(const QString &name) const
{
    return mFactories.contains(name);
}

void QScadaDataSourceManager::setDefaultPollInterval(int ms)
{
    // 下限保护与 QScadaDataSource::setPollInterval 一致：把周期设成 0 会把设备打爆。
    mDefaultPollInterval = qMax(10, ms);
}

QScadaDataSource *QScadaDataSourceManager::createSource(const QString &deviceIp,
                                                        const QString &driverName,
                                                        const QString &host, quint16 port,
                                                        int pollIntervalMs, QString *errorString)
{
    if (errorString)
        errorString->clear();

    if (deviceIp.isEmpty()) {
        if (errorString)
            *errorString = QStringLiteral("设备标识(deviceIp)不能为空");
        return nullptr;
    }

    if (mSources.contains(deviceIp)) {
        if (errorString)
            *errorString = QStringLiteral("设备 %1 已在采集中，请先 stopDevice() 再重新启动")
                               .arg(deviceIp);
        return nullptr;
    }

    const DriverFactory factory = mFactories.value(driverName, DriverFactory());
    if (!factory) {
        if (errorString)
            *errorString = QStringLiteral("未知驱动 \"%1\"；已注册的驱动: %2")
                               .arg(driverName, availableDrivers().join(QStringLiteral(", ")));
        return nullptr;
    }

    QScadaDataSource *source = factory(this);
    if (!source) {
        if (errorString)
            *errorString = QStringLiteral("驱动工厂 \"%1\" 返回了空指针").arg(driverName);
        return nullptr;
    }

    // 父子关系兜底：驱动由管理器创建，也由管理器回收，不会出现"设备停了但驱动
    // 对象还挂在事件循环上"的泄漏。如果工厂返回的对象已经有父对象，就不动它
    // （硬抢父子关系会导致对象被删两次）。
    if (!source->parent())
        source->setParent(this);

    source->setTarget(host, port);
    source->setPollInterval(pollIntervalMs > 0 ? pollIntervalMs : mDefaultPollInterval);

    // 登记必须在连接信号之前：槽函数里要用 sender() 反查 deviceIp。
    mSources.insert(deviceIp, source);

    connectSource(source, deviceIp);

    // 先把设备登记为"未连接"，等驱动 open() 成功、stateChanged(Connected) 到达时
    // 再提升为 QualityGood。顺序反过来的话，一台连不上的设备会短暂地显示为正常，
    // 而那几秒恰好是操作员最需要看到"没连上"的时候。
    if (mHub)
        mHub->setDeviceQuality(deviceIp, QualityNotConnected);

    return source;
}

bool QScadaDataSourceManager::connectQualitySignal(QScadaDataSource *source)
{
    // 按约定在运行时探测驱动是否声明了带质量码的扩展信号。
    //
    // 为什么不用继承一个"质量感知接口"：QScadaDataSource 是所有驱动（内置的
    // Modbus、以后的 OPC UA/S7、以及第三方的自定义驱动）共用的公共契约，
    // 为某一个驱动的额外能力去改它的签名，等于逼着所有驱动都改一遍。
    // "可选能力 + 运行时探测"则两全：有富信息的驱动（OPC UA 的 StatusCode、
    // S7 的诊断位）自己多声明一个信号，就能把质量码送进中枢，
    // 管理器与上层一行都不用改。
    //
    // 代价是约定必须写死签名，所以名字、参数个数、以及连接是否成功都要检查；
    // 任何一项不满足就退回普通的 valueChanged，绝不因为一个不规范的自定义驱动
    // 而让整条采集链路失效。
    const QMetaObject *meta = source->metaObject();
    for (int i = 0; i < meta->methodCount(); ++i) {
        const QMetaMethod method = meta->method(i);
        if (method.methodType() != QMetaMethod::Signal)
            continue;
        if (method.name() != QByteArray(kQualitySignalName))
            continue;
        if (method.parameterCount() != kQualitySignalParamCount)
            continue;

        // 用信号自己的规范化签名做字符串连接："2" 是 SIGNAL() 宏展开时加的前缀，
        // 这样即使参数类型的写法与约定完全一致，也不必在这里手抄一遍签名。
        QByteArray signature = method.methodSignature();
        signature.prepend('2');
        // 连接是否成功要用 if (connection) 判断，不能直接赋给 bool：
        // QMetaObject::Connection 的 operator bool 是 explicit 的。
        const QMetaObject::Connection connection =
            QObject::connect(source, signature.constData(),
                             this, SLOT(onSourceValueWithQuality(QString,QVariant,QScadaQuality)));
        if (connection)
            return true;
    }
    return false;
}

void QScadaDataSourceManager::connectSource(QScadaDataSource *source, const QString &deviceIp)
{
    Q_UNUSED(deviceIp)

    // ---- 1) 采集值：优先用带质量码的扩展信号 ----
    //
    // 连接类型一律用默认的 Qt::AutoConnection：
    //  · 驱动与管理器同线程时它等价于直接函数调用，没有任何排队与拷贝开销；
    //  · 驱动被 moveToThread() 到工作线程后，Qt 会在 emit 时自动改走排队连接，
    //    数据经事件队列汇入中枢所属线程，订阅者永远只在单一线程里被通知，
    //    中枢与界面层因此完全不需要加锁。
    if (!connectQualitySignal(source)) {
        // 普通驱动（例如 Modbus TCP）：只有裸值。质量码由设备连接状态决定，
        // 由中枢在 publish()/setDeviceQuality() 里统一处理。
        connect(source, &QScadaDataSource::valueChanged,
                this, &QScadaDataSourceManager::onSourceValueChanged);
    }

    // ---- 2) 连接状态：映射成设备级质量码 ----
    connect(source, &QScadaDataSource::stateChanged,
            this, &QScadaDataSourceManager::onSourceStateChanged);

    // ---- 3) 错误：转发给上层（日志栏、运维通知、MES 报警）----
    connect(source, &QScadaDataSource::errorOccurred,
            this, &QScadaDataSourceManager::onSourceError);
}

void QScadaDataSourceManager::openSource(QScadaDataSource *source, const QString &deviceIp,
                                         const QString &driverName)
{
    source->open();

    // open() 是异步的（Modbus 只是发起了 connectToHost），因此 started 的语义是
    // "采集已启动、正在连设备"，不代表已经连上；连不上会有 deviceError 跟上。
    emit deviceStarted(deviceIp, driverName);
}

bool QScadaDataSourceManager::startDevice(const QString &deviceIp, const QString &driverName,
                                          const QString &host, quint16 port, int pollIntervalMs,
                                          QString *errorString)
{
    QString error;
    QScadaDataSource *source = createSource(deviceIp, driverName, host, port,
                                           pollIntervalMs, &error);
    if (!source) {
        if (errorString)
            *errorString = error;
        return false;
    }

    openSource(source, deviceIp, driverName);
    return true;
}

int QScadaDataSourceManager::startFromRegisterMap(const QScadaRegisterMap &map,
                                                  const QString &defaultDriver,
                                                  QStringList *errors)
{
    // 中枢必须先拿到整套点表元数据：publish() 靠"位号 -> 设备"的归属关系给每个值
    // 盖上 deviceIp，设备断线时才能把它名下所有位号降级为 NotConnected，
    // 快照(snapshotForDevice)与界面按设备过滤也都依赖这个归属关系。
    if (mHub)
        mHub->registerTags(map.tags());

    // 先检查一遍"没有设备归属"的位号：它们不会被任何驱动采集，但点表里看不出来，
    // 是现场最常见的"点表抄漏了一列"的故障，值得单独报出来。
    if (errors) {
        const QList<QScadaTagDefinition> all = map.enabledTags();
        for (int i = 0; i < all.size(); ++i) {
            if (all.at(i).deviceIp().isEmpty()) {
                errors->append(QStringLiteral("位号 %1 未配置设备 IP，无法采集")
                                   .arg(all.at(i).key()));
            }
        }
    }

    int started = 0;
    const QStringList deviceIps = map.deviceIps();
    for (int i = 0; i < deviceIps.size(); ++i) {
        const QString deviceIp = deviceIps.at(i);

        // 已在运行的设备直接跳过：这样"先启动仿真设备演示、稍后补上真机点表"
        // 或者"分批上电"都不会把已经跑起来的驱动踢掉。
        if (mSources.contains(deviceIp))
            continue;

        const QList<QScadaTagDefinition> tags = map.tagsForDevice(deviceIp);
        if (tags.isEmpty())
            continue; // 该设备名下的位号都被停用了

        QString error;
        QScadaDataSource *source = createSource(deviceIp, defaultDriver, deviceIp,
                                                mDefaultPort, mDefaultPollInterval, &error);
        if (!source) {
            if (errors)
                errors->append(QStringLiteral("设备 %1 启动失败: %2").arg(deviceIp, error));
            continue;
        }

        // 位号必须在 open() 之前灌进去：Modbus 驱动在 open()/首次轮询时按地址合并
        // 读请求块，先连接后灌点会白跑一轮采集（界面上表现为"刚启动时值是空的"）。
        source->setTags(tags);
        openSource(source, deviceIp, defaultDriver);
        ++started;
    }

    return started;
}

void QScadaDataSourceManager::stopDevice(const QString &deviceIp)
{
    QScadaDataSource *source = mSources.value(deviceIp, nullptr);
    if (!source)
        return;

    // 先从表里摘掉：close() 会同步发出 stateChanged，槽函数要能正确处理
    // "这台设备已经不在管理之中"的情况，而不是又把它当成活动设备。
    mSources.remove(deviceIp);

    source->close();

    // 断开与管理器的全部连接：删除过程中驱动可能还会发 stateChanged/errorOccurred，
    // 那些信号已经没有意义，也不该再回调到可能正在销毁的管理器上。
    source->disconnect(this);

    if (mHub)
        mHub->setDeviceQuality(deviceIp, QualityNotConnected);

    // 用 deleteLater() 而不是 delete：close() 触发的信号可能还在调用栈上，
    // 立刻 delete 会让栈上的代码访问已析构对象（现场表现就是"偶尔崩一下"，
    // 且只在停设备时复现）。驱动同时是管理器的子对象，即使事件循环没来得及
    // 处理延迟删除，父对象析构时也会兜底回收，不会泄漏。
    source->deleteLater();

    emit deviceStopped(deviceIp);
}

void QScadaDataSourceManager::stopAll()
{
    // 先取 key 快照：stopDevice() 会修改 mSources，边迭代边删表会破坏迭代器。
    const QStringList deviceIps = mSources.keys();
    for (int i = 0; i < deviceIps.size(); ++i)
        stopDevice(deviceIps.at(i));
}

QScadaDataSource *QScadaDataSourceManager::source(const QString &deviceIp) const
{
    return mSources.value(deviceIp, nullptr);
}

QStringList QScadaDataSourceManager::activeDevices() const
{
    return mSources.keys();
}

int QScadaDataSourceManager::activeDeviceCount() const
{
    return mSources.size();
}

QScadaDataSource *QScadaDataSourceManager::sourceForTag(const QString &tagKey) const
{
    // 首选点表定义里的归属关系：O(1)，而且它是"权威"的——
    // 中枢与视图寻址用的都是同一个 deviceIp。
    if (mHub) {
        const QScadaTagDefinition definition = mHub->definition(tagKey);
        if (!definition.isNull() && !definition.deviceIp().isEmpty()) {
            QScadaDataSource *source = mSources.value(definition.deviceIp(), nullptr);
            if (source)
                return source;
        }
    }

    // 退路：逐个驱动查它的点表。用于单元测试这种直接 setTags()、没有建中枢的场合。
    // 设备数量是十位数量级，而且这条路径只在"人点了一下按钮"时才走，不在数据通路上。
    QHash<QString, QScadaDataSource *>::const_iterator it = mSources.constBegin();
    for (; it != mSources.constEnd(); ++it) {
        if (it.value()->hasTag(tagKey))
            return it.value();
    }
    return nullptr;
}

bool QScadaDataSourceManager::writeTag(const QString &tagKey, const QVariant &value,
                                       QString *errorString)
{
    if (errorString)
        errorString->clear();

    if (tagKey.isEmpty()) {
        if (errorString)
            *errorString = QStringLiteral("位号不能为空");
        return false;
    }

    QScadaDataSource *source = sourceForTag(tagKey);
    if (!source) {
        if (errorString)
            *errorString = QStringLiteral("没有正在采集位号 %1 的驱动（设备未启动或点表里没有这个位号）")
                               .arg(tagKey);
        return false;
    }

    // 只负责把值交给驱动。写确认是异步的：失败会经 errorOccurred -> deviceError
    // 报出来，成功则体现在下一轮采集值上。上层界面不要因为这里返回 true 就
    // 把输入框锁死或显示"下发成功"，否则会掩盖设备端的异常码。
    source->writeTag(tagKey, value);
    return true;
}

/*!
 * 驱动状态 -> 设备级质量码。
 *
 * 映射刻意做得很粗：只有明确的 Connected 才算 Good，
 * Connecting / Reconnecting / Faulted / Disconnected 一律按未连接处理。
 * 宁可让操作员看到"未连接"，也不要让他盯着一个看起来很正常的旧值做决策——
 * 在工业现场，这个区别可能意味着几万元的废品。
 */
void QScadaDataSourceManager::onSourceStateChanged(QScadaDataSource::ConnectionState state)
{
    QScadaDataSource *source = qobject_cast<QScadaDataSource *>(sender());
    if (!source || !mHub)
        return;

    const QString deviceIp = deviceIpForSource(source);
    if (deviceIp.isEmpty())
        return;

    mHub->setDeviceQuality(deviceIp, state == QScadaDataSource::Connected
                                        ? QualityGood
                                        : QualityNotConnected);
}

void QScadaDataSourceManager::onSourceValueChanged(const QString &tagKey, const QVariant &value)
{
    if (!mHub)
        return;

    // 数据通路上只做一次调用，不做任何查找：deviceIp 由中枢按点表定义自己解析。
    // 这是刻意的设计——采集周期 100 ms、几百个位号时，每个值多一次哈希查找
    // 就是每秒几千次无谓开销。
    mHub->publish(tagKey, value, QualityGood);
}

void QScadaDataSourceManager::onSourceValueWithQuality(const QString &tagKey, const QVariant &value,
                                                       QScadaQuality quality)
{
    if (!mHub)
        return;

    // 质量码原样送进中枢。中枢还会做一次兜底：如果该位号所属设备当前是断线状态，
    // 驱动报上来的 Good 也会被降级为 NotConnected，避免陈旧值混进历史库。
    mHub->publish(tagKey, value, quality);
}

void QScadaDataSourceManager::onSourceError(const QString &message)
{
    QScadaDataSource *source = qobject_cast<QScadaDataSource *>(sender());
    const QString deviceIp = source ? deviceIpForSource(source) : QString();

    // 只转发。库代码不该决定"错误要弹窗、写文件还是发短信"，
    // 那是应用层（界面日志栏、运维告警）的策略。
    emit deviceError(deviceIp, message);
}

/*!
 * 由驱动对象反查设备标识。
 *
 * 线性查找是有意的：设备数量是十位数量级，而且这条路径只在状态变化、报错、
 * 停止设备时走一次，**不在数据通路上**——真正高频的值到达路径完全不查表
 * （见 onSourceValueChanged）。
 */
QString QScadaDataSourceManager::deviceIpForSource(const QScadaDataSource *source) const
{
    if (!source)
        return QString();

    QHash<QString, QScadaDataSource *>::const_iterator it = mSources.constBegin();
    for (; it != mSources.constEnd(); ++it) {
        if (it.value() == source)
            return it.key();
    }
    return QString();
}
