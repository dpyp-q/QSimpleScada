#ifndef QSCADAHISTORYSTORAGE_H
#define QSCADAHISTORYSTORAGE_H

#include <QObject>
#include <QString>
#include <QStringList>
#include <QDateTime>
#include <QList>
#include <QtGlobal>

#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QVariant>

#include "qscadahistoryrecord.h"
#include "qscadastorageconfig.h"

class QTimer;
class QThread;

/*!
 * \brief 基于 QtSql 的历史数据持久化实现（历史库 / 趋势库）。
 *
 * 职责：把采集层 QScadaDataSource::valueChanged(tagKey, value) 送上来的
 * 高频数据，缓冲、批量、带事务地写进关系库，并提供按位号+时间范围的
 * 查询、降采样与过期清理能力。
 *
 * 设计取舍（面试重点，逐条都有现场原因）：
 *
 * 1. **不在 append() 里写库**。append() 只是往内存 QList 里追加，
 *    真正的落库由 QTimer 每 flushIntervalMs 触发一次 flush()，
 *    或者缓冲区达到 maxBatchSize 时提前触发。原因：采集回调每秒可能被调用上千次，
 *    在里面做磁盘 IO / 网络往返会把采集节奏彻底带偏。
 *
 * 2. **必须批量 + 事务**，绝不逐条 INSERT。见 flush() 的详细注释。
 *
 * 3. **不使用预处理语句缓存（QSqlQuery::prepare 复用）跨批次**。
 *    每批重新 prepare 一次，多花的时间在毫秒级，但能避免不同驱动在
 *    "重新绑定 vs 重新 prepare"上的行为差异（QODBC 在这方面尤其挑剔），
 *    换库时不用重调。批量本身已经把往返次数压到很低。
 *
 * 4. **异步失败可观测**。定时 flush 是后台行为，调用方没有返回值可以看，
 *    所以失败一律通过 errorOccurred(QString) 上报、并计入 statistics()，
 *    库里绝不允许弹 QMessageBox（库代码弹窗在多线程下会直接崩，
 *    而且上位机是无人值守运行，弹窗没人点会卡住整个界面）。
 *
 * 5. **不依赖驱动特有细节**。Qt 6 里各驱动对 lastInsertId() 的支持不一致
 *    （有的返回空、有的报错），本模块从不调用它，主键一律由数据库自己生成；
 *    DDL 也不把数据库特有语法混进公共路径，而是按驱动分方言执行
 *    （见 schemaDialect()）。
 *
 * 6. 数据库连接在 open() 后**一直复用**，不在查询时临时开连接：
 *    建立 ODBC/TCP 连接是几十到几百毫秒级别的开销，趋势查询是频繁操作，
 *    每次重连会让界面明显卡顿。
 *
 * 线程模型：本对象及其 QSqlDatabase 连接**只能在创建它的线程使用**。
 * 推荐的接法是把它 moveToThread 到一个专门的历史库线程，
 * 采集线程通过信号（自动队列连接）投递记录，数据库 IO 就不会影响界面刷新。
 * 当前实现不内部跨线程，是为了避免在 C++11/Qt5.12 下自己管理
 * "连接只能在其创建线程使用"这条约束时出错。
 */
class QScadaHistoryStorage : public QObject
{
    Q_OBJECT
public:
    /*!
     * SQL 方言。为什么要在 C++ 里也维护一份 DDL：
     * sql/ 目录下的 .sql 脚本是给 DBA 手工建库、审阅、纳入版本管理用的；
     * 但上位机往往是"交付即部署"，现场没有人会去手跑脚本。
     * 所以两条路都要留：createSchema() 直接执行内嵌 DDL（可重复执行、幂等），
     * applySchemaFile() 则允许执行外部的 .sql 文件（预埋 SQL 需要 DBA 审核时用）。
     * 两边的表结构必须保持一致，改动时同步修改，脚本里有对应说明。
     */
    enum SchemaDialect {
        DialectUnknown = 0,
        DialectSqlite,   //!< QSQLITE
        DialectMssql,    //!< QODBC 指向 SQL Server
        DialectPostgres  //!< QPSQL（预留，本模块暂只提供 SQLite/SQL Server 两套脚本）
    };

    /*!
     * 查询返回的一行。
     * 正常（未降采样）时只有 value 有效、sampleCount 为 1，
     * min/max/avg 不参与展示；降采样后每个桶返回三条包络线，
     * 由调用方分别画到 min 曲线、max 曲线、avg 曲线上。
     */
    struct Row
    {
        Row()
            : sequence(-1)
            , value(0.0)
            , minValue(0.0)
            , maxValue(0.0)
            , avgValue(0.0)
            , sampleCount(1)
            , quality(Good)
        {
        }

        qint64 sequence;          //!< 时间序；用于同毫秒内的稳定排序
        QDateTime timestamp;      //!< 原始点的时间 / 降采样桶的起始时间
        double value;             //!< 原始值（未降采样时有意义）
        double minValue;          //!< 桶内最小值
        double maxValue;          //!< 桶内最大值
        double avgValue;          //!< 桶内平均值
        int sampleCount;          //!< 桶内原始点条数；为 0 表示该桶没有任何数据
        QScadaDataQuality quality;//!< 原始质量码 / 桶内最差质量码
        QString rawText;          //!< 可选的原始文本
    };

    /*!
     * 运行统计，用于界面"历史库状态"面板与现场排障。
     * 关键用法：现场报"历史曲线断了一段"时，先看 failedBatches 和 lastError——
     * 如果失败次数在涨，说明是写库出了问题（磁盘满、连接断、权限不足）；
     * 如果失败为 0，那问题在采集侧，看采集驱动的失败计数即可。
     * 这个区分能省掉大量现场扯皮。
     */
    struct Statistics
    {
        Statistics()
            : writtenRecords(0)
            , failedBatches(0)
            , rejectedRecords(0)
            , bufferedRecords(0)
            , flushCount(0)
        {
        }

        quint64 writtenRecords;   //!< 累计成功写入库的记录条数
        quint64 failedBatches;    //!< 累计失败的批量事务次数
        quint64 rejectedRecords;  //!< 因 isValid() 不通过被丢弃的条数（脏数据统计）
        int bufferedRecords;      //!< 当前仍在内存缓冲里、尚未落库的条数
        quint64 flushCount;       //!< 累计 flush 次数（含空 flush）
        QDateTime lastWriteTime;  //!< 最后一次写库成功的时间
        QDateTime lastErrorTime;  //!< 最后一次写库失败的时间
        QString lastError;        //!< 最后一次错误描述
    };

    explicit QScadaHistoryStorage(QObject *parent = nullptr);
    ~QScadaHistoryStorage() override;

    /*!
     * \brief 打开数据库连接并按配置建表（幂等）。
     * \param errorString 失败时输出可直接显示给现场人员的错误描述（可为 nullptr）
     *
     * 成功时会：注册/复用命名连接、打开数据库、执行一次 createSchema()。
     * 之所以 open() 里顺手建表：现场经常出现"配置文件指向了一个还没建库的空
     * SQL Server 实例"，如果要求用户手工跑脚本，百分之百会漏。
     */
    bool open(const QScadaStorageConfig &config, QString *errorString = nullptr);

    //! 关闭连接：会先 flush()，避免缓冲里的数据被丢掉。重复调用安全。
    void close();

    bool isOpen() const;

    //! 当前生效的配置。
    QScadaStorageConfig config() const { return mConfig; }

    /*!
     * \brief 按当前驱动执行对应的建表语句，可重复执行不报错（幂等）。
     *
     * 幂等靠数据库自己的能力实现，而不是靠 try/catch 忽略错误：
     * - SQL Server：IF OBJECT_ID(N'dbo.scada_history', N'U') IS NULL + GO 分批
     * - SQLite    ：CREATE TABLE IF NOT EXISTS / CREATE INDEX IF NOT EXISTS
     * 这样重复启动、升级重跑都不会中断，也不会把已有数据表重建掉。
     */
    bool createSchema(QString *errorString = nullptr);

    /*!
     * \brief 执行外部 .sql 脚本文件（按 ; 与 GO 分批）。
     * 给"DBA 要求先审 SQL 再上线"的项目用；脚本内容与内嵌 DDL 等价。
     */
    bool applySchemaFile(const QString &filePath, QString *errorString = nullptr);

    //! 直接执行一段 SQL（工具/测试用；批量写入请用 append/flush）。
    bool executeSql(const QString &sql, QString *errorString = nullptr);

    /*!
     * \brief 追加一条记录到内存缓冲。
     * 不落库、不阻塞；缓冲达到 maxBatchSize 时会立即触发一次 flush()。
     * 返回 false 有三种情况：持久化已被 setEnabled(false) 关闭、
     * 记录没有通过 isValid() 校验、或从非本对象所属线程调用。
     * 三种情况都会计入 statistics().rejectedRecords 并按需上报 errorOccurred。
     */
    bool append(const QScadaHistoryRecord &record);

    //! 批量追加；语义同 append()，只是减少函数调用次数（采集侧一次投递一批点更划算）。
    bool appendBatch(const QList<QScadaHistoryRecord> &records);

    /*!
     * \brief 把缓冲中的数据批量写入数据库（事务提交）。
     *
     * **为什么绝对不能逐条 INSERT（现场问题的核心）**：
     * 一个中等规模的站，3000 个位号按 1 秒采集 = 3000 条/秒。
     * 逐条 INSERT 意味着每秒 3000 次独立事务，每次事务 SQL Server 都要写
     * 事务日志（log flush，受磁盘 fsync 限制，机械盘约几百次/秒）、
     * 还要申请/释放闩锁、维护日志序列号。结果是：
     *   - 日志文件暴涨：每条记录都留下 begin/insert/commit 三段日志，
     *     1000 点的历史库一天能写出几十 GB 日志，日志盘先满；
     *   - 磁盘 IO 打满：随机写 + 每次 fsync，SSD 也扛不住，更别说现场还在跑
     *     其他 IO；数据库一慢，写入线程排队，内存缓冲涨到 OOM；
     *   - 加剧锁竞争：高频小事务不断申请行锁/页锁，和趋势查询互相阻塞，
     *     典型现象是"界面一开历史曲线，采集数据就开始丢"。
     * 改成"一个事务 + 多行 VALUES 的批量 INSERT"以后，事务数与 fsync 次数
     * 按批大小成比例下降（比如 500 条/批，1 秒 3000 点 = 每秒 6 个事务，
     * 数量级直接降到 1/500），日志量、锁次数同步下降。
     * 再叠加"按时间排序写入"（同一批记录时间接近，索引页命中率高、
     * 页分裂少），批量写入是历史库能跑起来的前提，不是优化项。
     *
     * 失败处理：本批整体回滚（保证不出现半批数据的"锯齿"），
     * 已丢弃的这批**不自动重试**——重试在磁盘满/连接断的场景下只会反复失败并放大
     * 内存占用；改为上报 errorOccurred 让上层决定（现场做法是转存本地文件兜底）。
     */
    bool flush();

    //! 缓冲中尚未落库的条数。
    int pendingCount() const;

    //! 丢弃缓冲（例如切换位号表、确认数据无价值时用它，避免脏数据入待写队列）。
    void clearPending();

    /*!
     * \brief 查询某位号在 [from, to] 区间内的历史数据（含降采样）。
     * \param maxPoints 0 表示不降采样，返回全部原始点；>0 表示降采样到不超过该点数。
     *
     * 返回结果按时间升序；降采样时每个桶的 MIN / MAX / AVG 分别放在
     * minValue / maxValue / avgValue 里（sampleCount 为桶内原始点数），
     * 由调用方画成包络曲线 + 均值中线。
     *
     * 质量码：Bad 的点也会被查出来（历史事实要如实呈现），
     * 由界面决定是灰显还是断线；桶内质量取最差的一条（保守原则）。
     */
    QList<Row> query(const QString &tagKey,
                     const QDateTime &from,
                     const QDateTime &to,
                     int maxPoints = 0,
                     QString *errorString = nullptr);

    /*!
     * 查询原始记录（不降采样、不做任何聚合）。
     * 用途：数据导出/CSV 报表，以及需要对原始点做自定义计算（例如累计流量积分）的场合。
     * 点数很大时不要直接喂给界面绘图——那正是 query(maxPoints) 要解决的问题。
     */
    QList<QScadaHistoryRecord> queryRaw(const QString &tagKey,
                                        const QDateTime &from,
                                        const QDateTime &to,
                                        QString *errorString = nullptr);

    //! 查询记录数（画曲线前先估算数据量、或做分页时用）。失败返回 -1。
    int countRecords(const QString &tagKey,
                     const QDateTime &from,
                     const QDateTime &to,
                     QString *errorString = nullptr);

    //! 列出库中出现过的所有位号（给趋势界面的位号选择树用）。
    QStringList distinctTagKeys(QString *errorString = nullptr);

    /*!
     * \brief 删除 timestamp 早于 before 的数据，返回删除行数；失败返回 -1。
     *
     * 删除本身也是一次长事务，所以要分批删（每次最多 5000 行），
     * 否则一次删几千万行会把日志撑爆、并长时间持有锁把实时写入堵死
     * （现场表现是"凌晨自动清理时历史曲线出空洞"）。
     */
    int purge(const QDateTime &before);

    //! 当前生效的保留天数（来自配置）。
    int retentionDays() const { return mConfig.retentionDays; }

    //! 立即按保留期清理一次（供"立即清理"按钮或启动时调用）。返回删除行数，失败 -1。
    int purgeExpired();

    Statistics statistics() const { return mStatistics; }

    //! 当前配置对应的 SQL 方言。
    SchemaDialect schemaDialect() const { return mDialect; }
    static QString dialectName(SchemaDialect dialect);

    //! 由驱动名推断方言（"QODBC" 统一按 SQL Server 处理）。
    static SchemaDialect dialectForDriver(const QString &driver);

    //! 内部使用的表名常量，界面/报表模块可以引用，避免各处硬编码字符串。
    static QString historyTableName() { return QStringLiteral("scada_history"); }
    static QString alarmTableName() { return QStringLiteral("scada_alarm_history"); }

public slots:
    /*!
     * 启用/停用持久化。
     * 停用时：停止定时器、丢弃还在内存里的缓冲（这些数据既然没落库，
     * 说明用户正在维护历史表或磁盘已满，强行写入只会制造错误日志）。
     */
    void setEnabled(bool enabled);
    bool isEnabled() const { return mEnabled; }

    /*!
     * 清空统计计数（不清数据）。
     * 注意不清 lastError：现场希望"错误信息一直留到最后一次成功写入"，
     * 便于事后翻记录。
     */
    void resetStatistics();

    /*!
     * 便利槽：直接接到 QScadaDataSource::valueChanged(tagKey, value) 上。
     * 采集驱动无需知道历史库的存在——这是采集层与存储层解耦的关键，
     * 也是"新增一种协议不用改历史库"的原因。
     * 驱动侧的连接状态变化建议由上层转成 Bad / NotConnected 质量码后写入，
     * 因为 valueChanged 只带值和位号，不带质量信息。
     */
    void onValueChanged(const QString &tagKey, const QVariant &value);

signals:
    //! 任何内部错误（打开失败、写入失败、查询失败）统一从这里出去，库内不弹窗。
    void errorOccurred(const QString &message);
    //! 一次 flush 完成；writtenCount 为本批成功写入的条数（含 0）。
    void flushCompleted(int writtenCount);
    //! 连接打开/关闭状态变化（界面据此显示"历史库：已连接/断开"）。
    void openedChanged(bool opened);

private slots:
    void onFlushTimer();

private:
    // 内部实现细节，见 .cpp。放在私有区是为了让头文件只暴露稳定接口。
    bool ensureDatabase(QString *errorString);
    void scheduleFlushTimer();
    void reportError(const QString &message);
    bool flushInternal(int *writtenCount, QString *errorString);
    bool execBatch(const QList<QScadaHistoryRecord> &batch, QString *errorString);
    bool beginTransaction(QString *errorString);
    bool commitTransaction(QString *errorString);
    void rollbackTransaction();
    bool sameThread(QString *errorString) const;

    static void sortByTimestamp(QList<QScadaHistoryRecord> &records);
    static QList<Row> downsample(const QList<QScadaHistoryRecord> &records,
                                 const QDateTime &from, const QDateTime &to,
                                 int maxPoints);

    QScadaStorageConfig mConfig;
    QSqlDatabase mDatabase;
    SchemaDialect mDialect;
    QTimer *mFlushTimer;
    QList<QScadaHistoryRecord> mPending;
    bool mEnabled;
    bool mTransactionActive;
    Statistics mStatistics;
    QString mConnectionName;
};

#endif // QSCADAHISTORYSTORAGE_H
