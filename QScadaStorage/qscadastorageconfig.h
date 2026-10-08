#ifndef QSCADASTORAGECONFIG_H
#define QSCADASTORAGECONFIG_H

#include <QString>
#include <QStringList>

/*!
 * \brief 历史库连接与写入策略配置。
 *
 * 为什么把"连接参数"和"写入策略"放在同一个结构里：
 * 这两类参数在现场是**一起调**的——换成 SQL Server 以后，批量条数和刷新间隔
 * 往往要跟着改（SQL Server 一次 INSERT 的固定开销比 SQLite 大得多，
 * 批量应该更大、提交应该更稀）。合成一个配置对象由界面"历史库设置"页一次性
 * 填完、一次性 open()，比拆成两个对象更符合现场使用习惯。
 *
 * 关于 SQL Server 的连接方式（简历里最容易被追问的点，这里写清楚）：
 *
 * 1. Qt 访问 SQL Server 走的是 **QODBC** 驱动，不是原生驱动。
 *    Qt 官方从 5.x 到 6.x 都没有提供 SQL Server 原生驱动（QSQLMSSQL 不存在），
 *    所以链路是：QtSql(QODBC 插件) -> 系统 ODBC 驱动管理器 ->
 *    "ODBC Driver 18 for SQL Server"（或老的 "SQL Server Native Client" / "SQL Server"）。
 *
 * 2. 因此 **QODBC 插件依赖系统已安装的 ODBC 驱动**：部署到工控机时必须先装
 *    msodbcsql.msi（或随 SQL Server 客户端一起安装），否则 open() 会直接报
 *    "Driver not loaded / Data source name not found"，代码再对也没用。
 *    这一点必须在部署文档里写明，现场最常见的"连不上"就是漏装 ODBC 驱动。
 *
 * 3. 两种连接写法都支持（见 databaseName 字段的说明）：
 *    - DSN 方式：databaseName = DSN 名称，需要在"ODBC 数据源管理器"里预先配好，
 *      账号密码也可以存在 DSN 里。适合运维统一维护、不想在配置文件里写密码的场合。
 *    - 无 DSN 连接串方式：databaseName = "DRIVER={ODBC Driver 18 for SQL Server};SERVER=..."
 *      优点是不依赖每台机器手工配 DSN，安装包开箱即用，推荐用于交付现场。
 *      connectionString() 会按本结构的字段自动拼出这条串。
 *
 * 4. 驱动 18 默认 Encrypt=yes，连老版本 SQL Server（自签证书）会报
 *    "SSL Provider: certificate verify failed"。所以 odbcOptions 默认带
 *    TrustServerCertificate=yes，这是内网工控环境的标准做法；
 *    若现场安全审计不允许，把它改成 Encrypt=no 或导入正式证书。
 */
struct QScadaStorageConfig
{
    QScadaStorageConfig()
        : port(1433)
        , maxBatchSize(500)
        , flushIntervalMs(1000)
        , retentionDays(90)
        , odbcOptions(QStringLiteral("TrustServerCertificate=yes"))
    {
    }

    /*!
     * Qt SQL 驱动名，例如 "QSQLITE" / "QODBC" / "QPSQL" / "QMYSQL"。
     * 用字符串而不是枚举，是因为业务层可能接入 Qt 支持的任何数据库，
     * 写死枚举会把"换库"变成"改代码重新编译"，而现场换库是常事。
     */
    QString driver;

    /*!
     * 数据库名，语义随驱动变化：
     * - QSQLITE：数据库文件路径（":memory:" 表示内存库，仅用于自测）
     * - QODBC ：DSN 名称，**或**一条完整 ODBC 连接串（含 "DRIVER=..." 即按连接串处理）
     * - QPSQL / QMYSQL：库名
     */
    QString databaseName;

    //! 主机名或实例名；SQLite 忽略。支持 "192.168.1.10" 或 "SERVER\\SQLEXPRESS"。
    QString hostName;

    //! 端口；SQL Server 默认 1433、PostgreSQL 5432、MySQL 3306。0 表示用驱动默认值。
    int port;

    QString userName;
    QString password;

    /*!
     * 连接名。
     * 为什么需要显式指定：一个进程里同时连"实时库 + 历史库 + 报表库"时，
     * QSqlDatabase 是按连接名区分的；不给名字就会落到 defaultConnection，
     * 多个模块互相踩连接（典型的坑：报表模块 query() 时把历史库连接给关了）。
     * 留空时由 QScadaHistoryStorage 自动生成唯一名字（见 effectiveConnectionName()）。
     */
    QString connectionName;

    /*!
     * 单次批量写入的最大记录条数。
     * 现场一个 3000 点的系统、1 秒采集一次，就是 3000 条/秒；逐条 INSERT 必然追不上。
     * 批量条数不是越大越好：太大则单条 SQL 文本过长（SQL Server 有 2100 个参数的硬上限，
     * 每行 4~5 个参数，所以 500 行/批是留足余量的安全值），
     * 而且一次事务持有锁的时间变长，会顶住其他查询。500 是一个实测比较稳的折中。
     * 超过该值的缓冲会在一次 flush 里拆成多条语句，但仍然在同一个事务内提交。
     */
    int maxBatchSize;

    /*!
     * 定时 flush 间隔（毫秒）。
     * 为什么不在 append() 里直接写库：采集回调是高频路径（每秒上千次），
     * 在里面做磁盘 IO 会把采集线程拖死，甚至造成采集周期抖动丢点。
     * 所以 append() 只往内存缓冲里放，由定时器统一批量落库。
     * 代价是"最近 flushIntervalMs 毫秒内的数据还在内存里"，
     * 上位机被强杀（任务管理器结束进程）会丢这一小段——这是有意接受的取舍：
     * 换取的是采集实时性不受数据库抖动影响（数据库卡一下不能反过来卡住采集）。
     */
    int flushIntervalMs;

    /*!
     * 历史数据保留天数，<= 0 表示不自动清理。
     * 现场必须能自动清理：单点 1 秒 1 条，一年就是 3150 万条；
     * 1000 点的系统一年 300 亿条，不清理磁盘迟早写满，
     * 而历史库所在分区写满会连带影响 SQL Server 整实例（连 tempdb 都写不进去，
     * 结果是整个上位机所有功能一起挂）。所以保留期是安全阀，不是可选功能。
     */
    int retentionDays;

    /*!
     * 追加到 ODBC 连接串尾部的选项（仅 QODBC 使用）。
     * 默认 TrustServerCertificate=yes，用于绕开驱动 18 对自签证书的强校验。
     */
    QString odbcOptions;

    //! 只返回连接配置是否完整（不判断是否真的连得上）。
    bool isValid() const;

    /*!
     * 规范化的连接名：connectionName 为空时按"驱动+库名+主机"生成一个稳定名字。
     * 之所以要"稳定"而不是加随机数：同一个配置重复 open() 时应当复用同一个
     * 连接名，否则 QSqlDatabase 会不断产生新连接直到触达驱动连接上限。
     */
    QString effectiveConnectionName() const;

    /*!
     * 拼出 ODBC 连接串（仅对 QODBC 有意义）。
     * 逻辑：
     * - databaseName 里已经含 "DRIVER=" 时，认为调用方直接给了完整连接串，原样返回
     *   （不覆盖，因为调用方可能用了我们不认识的选项）；
     * - 否则按 hostName/port/databaseName/userName/password 拼无 DSN 连接串；
     * - hostName 里含 '\' 时按"命名实例"处理（SERVER\\SQLEXPRESS），
     *   此时不能同时指定端口——SQL Server Browser 服务负责把实例名解析成端口。
     */
    QString connectionString() const;

    //! 给界面/日志用的可读描述（不含密码，避免日志泄露口令）。
    QString description() const;

    /*!
     * 便利构造：SQLite 单文件历史库。
     * 选它的场合：单机上位机、点数不多、不想额外装数据库服务。
     * SQLite 是 Qt 自带的（QSQLITE 插件随 Qt 一起发布），部署成本为零，
     * 适合演示与小型项目；但**不支持多客户端并发写**，
     * 多台上位机共用一个历史库时必须换 SQL Server。
     */
    static QScadaStorageConfig sqliteDefault(const QString &filePath);

    /*!
     * 便利构造：SQL Server（走 QODBC + 无 DSN 连接串）。
     * 注意两点（见类头注释更详细的说明）：
     * - 依赖系统安装 ODBC 驱动；未安装时 open() 会失败并给出明确 errorString。
     * - 默认端口 1433；命名实例请写成 sqlServerDefault("SERVER\\SQLEXPRESS", ...)，
     *   并把 port 置 0。
     */
    static QScadaStorageConfig sqlServerDefault(const QString &host,
                                                const QString &database,
                                                const QString &user,
                                                const QString &password,
                                                int port = 1433);
};

#endif // QSCADASTORAGECONFIG_H
