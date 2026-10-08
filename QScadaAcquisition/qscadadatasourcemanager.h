#ifndef QSCADADATASOURCEMANAGER_H
#define QSCADADATASOURCEMANAGER_H

#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariant>

#include <functional>

#include "qscadadatasource.h"
#include "qscadatagvalue.h"

class QScadaDataHub;
class QScadaRegisterMap;

/*!
 * \brief 采集管理器：按设备创建并持有驱动实例，把所有驱动的输出统一汇聚到数据中枢。
 *
 * ## 为什么需要这一层
 *
 * `QScadaDataSource` 是"一个驱动对一个设备"的抽象，但一台机器上会有多台设备
 * （主轴驱动器、PLC、温控仪、机器人……），每台设备的协议、地址、采集周期都可能
 * 不同。如果让界面或业务代码自己去 new 驱动、连信号、处理断线，那么"新增一台
 * 设备"就要改界面，"换一种协议"就要改一遍所有连信号的地方。
 * 这一层把那些事收敛到一处：**谁在哪里、用什么协议、采多久，全在这里决定；
 * 上层只看见中枢里的位号值**。
 *
 * ## 驱动工厂：本项目里"开闭原则"最具体的落点
 *
 * 管理器自己不认识任何具体协议类型，只持有一张 `名字 -> 工厂函数` 的表。
 * 于是以后要接 OPC UA / 西门子 S7 / MQTT / 串口自定义协议时，只需要：
 *
 * ```cpp
 * manager->registerDriver(QStringLiteral("opcua"), [](QObject *parent) -> QScadaDataSource * {
 *     return new QScadaOpcUaClient(parent);
 * });
 * ```
 *
 * 管理器、中枢、报警引擎、历史库、界面**一行都不用改**——它们从来就没见过
 * "Modbus"这个词。反过来，如果这里写成一串 `if (driverName == "modbus-tcp") ...`，
 * 每加一种协议都要重新编译、重新回归所有上层功能。
 * 工厂还有个测试上的好处：同名注册会覆盖，所以单元测试可以注入一个"永远连不上"
 * 或"只会发固定值"的测试替身来验证断线告警、质量码降级这些分支，
 * 不需要真的准备一台设备。
 *
 * ## 线程模型（重要）
 *
 * 连接驱动的信号时一律用默认的 `Qt::AutoConnection`：
 *
 *  · 驱动与管理器同线程时（当前默认），它等价于**直接函数调用**，没有事件队列、
 *    没有额外拷贝，是最快的路径；
 *  · 一旦将来把某个驱动 `moveToThread()` 到工作线程，Qt 会在 emit 的那一刻自动
 *    改走**排队连接**：数据经事件队列汇入中枢所属线程，于是所有订阅者
 *    （界面、报警、历史库）永远只在同一个线程里被通知，不需要给中枢加锁。
 *
 * 换句话说，"同线程直调 / 跨线程排队"这个切换不需要改一行业务代码——这正是
 * 用信号槽而不是回调函数的价值所在。
 * `QScadaDataHub` 的构造函数里已经 `qRegisterMetaType` 了 `QScadaTagValue` 与
 * `QScadaQuality`，跨线程信号不会因为元类型未注册而被静默丢弃（那种故障的表现
 * 是"数据时有时无"，在现场极难排查）。
 *
 * ## 可选的"带质量码"信号约定
 *
 * 基类信号 `valueChanged(QString, QVariant)` 没有质量码的位置。某些驱动能提供比
 * 裸值更丰富的信息（本项目的仿真源要上报 QualityUncertain；将来的 OPC UA 驱动
 * 有 StatusCode、S7 有诊断位），因此约定：
 *
 * ```cpp
 * signals:
 *     void valueChangedWithQuality(const QString &tagKey, const QVariant &value,
 *                                  QScadaQuality quality);
 * ```
 *
 * 驱动只要声明了这个信号（名字、参数个数、类型都对得上），管理器就会优先连它，
 * 并把质量码原样送进中枢；没有这个信号的驱动走普通的 valueChanged。
 * 用运行时探测而不是"继承一个接口类"，是因为 `QScadaDataSource` 是所有驱动
 * 共用的公共契约，为一个可选能力去改它的签名，等于逼着每个驱动都改一遍。
 */
class QScadaDataSourceManager : public QObject
{
    Q_OBJECT
public:
    /*!
     * 驱动工厂：给一个父对象，返回一个新的驱动实例。
     * 返回空指针表示创建失败，管理器会把它当成启动失败处理。
     */
    typedef std::function<QScadaDataSource *(QObject *)> DriverFactory;

    /*!
     * \param hub 数据中枢。所有采集值最终都汇入它。
     *            传空指针只用于不关心数据的单元测试（管理器仍可正常起停驱动）。
     */
    explicit QScadaDataSourceManager(QScadaDataHub *hub, QObject *parent = nullptr);
    ~QScadaDataSourceManager() override;

    QScadaDataHub *hub() const { return mHub; }

    /*!
     * 注册一个驱动工厂。内置已注册 `"modbus-tcp"` 与 `"simulator"`。
     * 同名重复注册会覆盖（便于测试替身替换内置驱动）。
     */
    void registerDriver(const QString &name, DriverFactory factory);
    //! 已注册的驱动名（按字典序，便于界面下拉框稳定显示）。
    QStringList availableDrivers() const;
    bool hasDriver(const QString &name) const;

    /*!
     * 点表里只有设备 IP，没有端口与周期字段，因此这里给出"没有另行指定时"的默认值。
     * Modbus TCP 的标准端口是 502；非标端口（网关映射、串口服务器）用 setDefaultPort 改。
     */
    void setDefaultPort(quint16 port) { mDefaultPort = port; }
    quint16 defaultPort() const { return mDefaultPort; }
    void setDefaultPollInterval(int ms);
    int defaultPollInterval() const { return mDefaultPollInterval; }

    /*!
     * 为某台设备创建驱动并开始采集。
     * \param deviceIp 设备标识，同时是位号归属与界面寻址用的键（不一定是真实 IP，
     *                 例如仿真设备可以叫 "SIM-01"）。
     * \param host     驱动要连的地址；仿真源不需要，可以传空串。
     * \param errorString 失败原因（未注册的驱动、设备已在运行、工厂返回空……）。
     * \return 启动成功（不代表已经连上设备——连接是异步的，状态看 stateChanged）。
     */
    bool startDevice(const QString &deviceIp, const QString &driverName, const QString &host,
                     quint16 port, int pollIntervalMs, QString *errorString = nullptr);

    void stopDevice(const QString &deviceIp);
    void stopAll();

    QScadaDataSource *source(const QString &deviceIp) const;
    QStringList activeDevices() const;
    int activeDeviceCount() const;
    //! 某个位号当前由哪个驱动采集（反向控制时要按位号找到持有它的设备）。
    QScadaDataSource *sourceForTag(const QString &tagKey) const;

    /*!
     * 按位号写值：先查点表定义找到所属设备，再路由到对应驱动的 writeTag()。
     * 界面上的"下发设定值"按钮只需要知道位号，不需要知道它挂在哪个驱动上。
     *
     * 返回 true 只表示"已交给驱动下发"，不代表设备已经执行：
     * Modbus 这类协议的写确认是异步的，真正的结果由 errorOccurred ->
     * deviceError 信号给出（写成功则能从下一轮采集值上看出效果）。
     */
    bool writeTag(const QString &tagKey, const QVariant &value, QString *errorString = nullptr);

    /*!
     * 按采集点表批量起设备：
     *  · 先把整套点表元数据登记到中枢（中枢靠"位号 -> 设备"的归属关系给每个值
     *    盖上 deviceIp，断线时才能把它名下所有位号降级为 NotConnected）；
     *  · 再遍历点表里出现过的设备 IP，为每台设备建一个驱动、灌入该设备的位号、启动；
     *  · **已在运行的设备会跳过**，因此可以先启动一部分设备、稍后再调用一次补上其余设备。
     * \param errors 收集每台设备的失败原因（可为空）。返回实际启动成功的设备数。
     */
    int startFromRegisterMap(const QScadaRegisterMap &map, const QString &defaultDriver,
                             QStringList *errors = nullptr);

signals:
    void deviceStarted(QString deviceIp, QString driverName);
    void deviceStopped(QString deviceIp);
    //! 驱动报错（连不上、超时、写被拒……）。管理器只转发，不弹窗、不写日志文件。
    void deviceError(QString deviceIp, QString message);

private slots:
    void onSourceValueChanged(const QString &tagKey, const QVariant &value);
    //! 带质量码的扩展信号对应的槽（约定见类注释，签名必须与约定完全一致）。
    void onSourceValueWithQuality(const QString &tagKey, const QVariant &value, QScadaQuality quality);
    void onSourceStateChanged(QScadaDataSource::ConnectionState state);
    void onSourceError(const QString &message);

private:
    QScadaDataSource *createSource(const QString &deviceIp, const QString &driverName,
                                   const QString &host, quint16 port, int pollIntervalMs,
                                   QString *errorString);
    void connectSource(QScadaDataSource *source, const QString &deviceIp);
    //! 尝试连接驱动上"带质量码"的可选信号；成功返回 true。
    bool connectQualitySignal(QScadaDataSource *source);
    void openSource(QScadaDataSource *source, const QString &deviceIp, const QString &driverName);
    QString deviceIpForSource(const QScadaDataSource *source) const;

    QScadaDataHub *mHub;
    QHash<QString, QScadaDataSource *> mSources; //!< deviceIp -> 驱动实例
    QHash<QString, DriverFactory> mFactories;    //!< 驱动名 -> 工厂函数
    quint16 mDefaultPort;
    int mDefaultPollInterval;
};

#endif // QSCADADATASOURCEMANAGER_H
