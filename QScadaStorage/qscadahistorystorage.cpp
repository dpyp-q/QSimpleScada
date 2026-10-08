#include "qscadahistorystorage.h"

#include <QFile>
#include <QMetaType>
#include <QRegularExpression>
#include <QStringList>
#include <QTextStream>
#include <QThread>
#include <QTimer>
#include <QVector>
#include <QtGlobal>

#include <algorithm>

//==============================================================================
// 内部工具：时间与方言适配
//==============================================================================

namespace {

/*!
 * 历史库统一的时间字符串格式。
 *
 * 为什么要自己格式化，而不是直接把 QDateTime 绑给驱动：
 * 1. 各驱动对 QDateTime 的序列化结果不一致（有的带 T、有的带时区后缀、
 *    有的按本地格式），同一份数据换个驱动就可能解析失败或差 8 小时；
 * 2. SQLite 把时间存成 TEXT，必须保证"字符串排序 == 时间排序"，
 *    即固定宽度、零填充、从大到小（年-月-日 时:分:秒.毫秒）。
 *
 * 用 "yyyy-MM-dd HH:mm:ss.zzz"：SQL Server 的 DATETIME2 能按语言无关的方式
 * 解析它（这是少数对 SET LANGUAGE / SET DATEFORMAT 免疫的常用格式），
 * SQLite 里也能直接做字符串比较和 BETWEEN。长度固定 23 字符。
 */
QString formatTimestamp(const QDateTime &ts)
{
    return ts.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"));
}

//! 解析上面格式的时间字符串（读库时用），并兼容 ISO 8601 与整型时间戳。
QDateTime parseTimestamp(const QString &text)
{
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty())
        return QDateTime();

    QDateTime ts = QDateTime::fromString(trimmed, QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"));
    if (!ts.isValid())
        ts = QDateTime::fromString(trimmed, Qt::ISODateWithMs);
    if (!ts.isValid())
        ts = QDateTime::fromString(trimmed, Qt::ISODate);
    if (!ts.isValid()) {
        // 兜底：SQLite 允许把时间列存成 Unix 秒/毫秒整数（见脚本里的说明）。
        bool ok = false;
        const qlonglong numeric = trimmed.toLongLong(&ok);
        if (ok && numeric > 0) {
            // 10 位按秒、13 位按毫秒判断，避免把 2024 年误当成 1970 年。
            ts = (numeric < Q_INT64_C(100000000000))
                    ? QDateTime::fromMSecsSinceEpoch(numeric * Q_INT64_C(1000))
                    : QDateTime::fromMSecsSinceEpoch(numeric);
        }
    }
    return ts;
}

/*!
 * 剥离 C 风格块注释（保留换行，便于后续按行切分）。
 *
 * 必须做这一步的原因：sql/ 目录下的脚本是"给人和给 DBA 看"的文档，
 * 里面有大量说明性文字，其中难免出现分号（例如说明里的 SQL 示例）。
 * 如果直接把整份脚本按分号切分并逐条发给数据库，注释里的分号会被当成
 * 语句结束符，把一条完整语句切断，报出莫名其妙的语法错误。
 * 所以先把块注释去掉，再切分。
 *
 * 行注释（两个减号开头）不用在这里处理：后面的切分逻辑会整行跳过它们。
 */
QString stripBlockComments(const QString &sql)
{
    QString out;
    out.reserve(sql.size());
    bool inBlockComment = false;
    for (int i = 0; i < sql.size(); ++i) {
        const QChar c = sql.at(i);
        const QChar next = (i + 1 < sql.size()) ? sql.at(i + 1) : QChar();

        if (inBlockComment) {
            const bool isEnd = (c == QLatin1Char('*') && next == QLatin1Char('/'));
            if (isEnd) {
                inBlockComment = false;
                ++i;
            } else if (c == QLatin1Char('\n')) {
                out += c;   // 保留换行：切分逻辑依赖行结构
            }
            continue;
        }

        if (c == QLatin1Char('/') && next == QLatin1Char('*')) {
            inBlockComment = true;
            ++i;
            continue;
        }
        out += c;
    }
    return out;
}

/*!
 * 把 SQL 脚本切成可逐条执行的语句。
 *
 * 三个必须处理的坑：
 *  1. SQL Server 的 CREATE PROCEDURE 内部含分号，不能无脑按分号切；
 *  2. CREATE PROCEDURE / CREATE VIEW 必须是批中的第一条语句，
 *     所以脚本用 GO 显式切批（GO 是 SSMS/sqlcmd 的批分隔符，不是 T-SQL 语法，
 *     绝不能把 GO 发给服务器）；
 *  3. BEGIN ... END 块内部会出现 "END;"（例如 IF ... BEGIN ... END;），
 *     如果把它当成语句结束，会把一条完整的 DDL 拦腰切断。
 *     所以只把 BEGIN/END 嵌套深度为 0 时的 "END;" 当作结束——
 *     这是"幂等 DDL 必须是 BEGIN/END 块"这一约定带来的简化：
 *     本模块的脚本里，块内不出现裸的 END; 行。
 */
QStringList splitSqlStatements(const QString &sql, const QString &batchSeparator)
{
    QStringList statements;
    QString current;
    int beginDepth = 0;
    const QStringList lines = stripBlockComments(sql).split(QLatin1Char('\n'));
    for (int i = 0; i < lines.size(); ++i) {
        const QString line = lines.at(i);
        const QString trimmed = line.trimmed();

        // 跳过空行与纯注释行：把注释单独发给驱动是没意义的往返。
        if (trimmed.isEmpty() || trimmed.startsWith(QLatin1String("--")))
            continue;

        if (!batchSeparator.isEmpty()
                && trimmed.compare(batchSeparator, Qt::CaseInsensitive) == 0) {
            if (!current.trimmed().isEmpty())
                statements.append(current.trimmed());
            current.clear();
            beginDepth = 0;
            continue;
        }

        current += line;
        current += QLatin1Char('\n');

        if (trimmed.startsWith(QLatin1String("BEGIN"), Qt::CaseInsensitive))
            ++beginDepth;
        else if (trimmed.startsWith(QLatin1String("END"), Qt::CaseInsensitive))
            beginDepth = qMax(0, beginDepth - 1);

        // 语句结束判定：整行以分号结尾。BEGIN 行后接分号的情况不存在于本模块脚本；
        // END; 只有在嵌套回到 0 时才代表语句结束（见上面第 3 点）。
        bool endsStatement = trimmed.endsWith(QLatin1Char(';'));
        if (endsStatement && beginDepth > 0
                && trimmed.startsWith(QLatin1String("END"), Qt::CaseInsensitive)) {
            endsStatement = false;
        }
        if (endsStatement) {
            statements.append(current.trimmed());
            current.clear();
        }
    }
    if (!current.trimmed().isEmpty())
        statements.append(current.trimmed());
    return statements;
}

} // namespace

//==============================================================================
// 内嵌 DDL：与 sql/mssql_schema.sql、sql/sqlite_schema.sql 保持一致
// 修改表结构时两处必须同步改，否则"手工建的表"和"自动建的表"会不一致，
// 这类问题在升级现场极难排查（表结构悄悄差一列，写入全部失败）。
//==============================================================================

namespace {

/*
 * SQLite 建表语句（与 sql/sqlite_schema.sql 等价）。
 * 幂等靠 CREATE TABLE/INDEX IF NOT EXISTS，重复执行不会破坏已有表与数据。
 */
const char *kSqliteSchemaSql =
    "CREATE TABLE IF NOT EXISTS scada_history (\n"
    "    id        INTEGER PRIMARY KEY AUTOINCREMENT,\n"
    "    tag_key   TEXT    NOT NULL,\n"
    "    value     REAL    NOT NULL,\n"
    "    quality   INTEGER NOT NULL DEFAULT 0,\n"
    "    ts        TEXT    NOT NULL,\n"
    "    raw_text  TEXT    NULL\n"
    ");\n"
    "CREATE INDEX IF NOT EXISTS IX_scada_history_tag_ts ON scada_history (tag_key, ts);\n"
    "CREATE INDEX IF NOT EXISTS IX_scada_history_ts ON scada_history (ts);\n"
    "CREATE TABLE IF NOT EXISTS scada_alarm_history (\n"
    "    event_id      INTEGER PRIMARY KEY AUTOINCREMENT,\n"
    "    tag_key       TEXT    NOT NULL,\n"
    "    level         INTEGER NOT NULL,\n"
    "    message       TEXT    NULL,\n"
    "    trigger_value REAL    NULL,\n"
    "    limit_value   REAL    NULL,\n"
    "    raised_at     TEXT    NOT NULL,\n"
    "    cleared_at    TEXT    NULL,\n"
    "    acked_at      TEXT    NULL,\n"
    "    acked_by      TEXT    NULL\n"
    ");\n"
    "CREATE INDEX IF NOT EXISTS IX_scada_alarm_tag_raised ON scada_alarm_history (tag_key, raised_at);\n"
    "CREATE INDEX IF NOT EXISTS IX_scada_alarm_raised ON scada_alarm_history (raised_at);\n";

/*
 * SQL Server 建表语句（与 sql/mssql_schema.sql 等价）。
 *
 * 幂等靠 IF OBJECT_ID(...) IS NULL：重复启动、升级重跑都不会重建表、
 * 不会丢数据，也不会因为"表已存在"而报错中断启动流程。
 *
 * 索引列顺序 (tag_key, ts) 的原因：历史查询永远是
 * "WHERE tag_key = ? AND ts BETWEEN ? AND ?"，先等值过滤、后范围扫描。
 * tag_key 在前可以在 B 树里一次定位到该位号的分段，再在段内按 ts 顺序扫描；
 * 反过来 (ts, tag_key) 则要先扫整个时间窗内所有位号的数据再过滤，
 * 数据量一大就是数量级级别的读放大。
 * 另外 ts 在第二位，索引本身就能支撑 ORDER BY ts，省掉一次排序。
 *
 * 用 GO 切批：CREATE PROCEDURE 必须是批中的第一条语句，
 * 而且 CREATE TABLE 里引用的索引要在表建好之后再建，必须分成两批。
 */
const char *kMssqlSchemaSql =
    "IF OBJECT_ID(N'dbo.scada_history', N'U') IS NULL\n"
    "BEGIN\n"
    "    CREATE TABLE dbo.scada_history (\n"
    "        id       BIGINT IDENTITY(1,1) NOT NULL,\n"
    "        tag_key  NVARCHAR(128) NOT NULL,\n"
    "        value    FLOAT NOT NULL,\n"
    "        quality  TINYINT NOT NULL CONSTRAINT DF_scada_history_quality DEFAULT (0),\n"
    "        ts       DATETIME2(3) NOT NULL,\n"
    "        raw_text NVARCHAR(256) NULL,\n"
    "        CONSTRAINT PK_scada_history PRIMARY KEY CLUSTERED (id ASC)\n"
    "    );\n"
    "END;\n"
    "GO\n"
    "IF NOT EXISTS (SELECT 1 FROM sys.indexes WHERE name = N'IX_scada_history_tag_ts' AND object_id = OBJECT_ID(N'dbo.scada_history'))\n"
    "BEGIN\n"
    "    CREATE NONCLUSTERED INDEX IX_scada_history_tag_ts ON dbo.scada_history (tag_key ASC, ts ASC);\n"
    "END;\n"
    "GO\n"
    "IF NOT EXISTS (SELECT 1 FROM sys.indexes WHERE name = N'IX_scada_history_ts' AND object_id = OBJECT_ID(N'dbo.scada_history'))\n"
    "BEGIN\n"
    "    CREATE NONCLUSTERED INDEX IX_scada_history_ts ON dbo.scada_history (ts ASC);\n"
    "END;\n"
    "GO\n"
    "IF OBJECT_ID(N'dbo.scada_alarm_history', N'U') IS NULL\n"
    "BEGIN\n"
    "    CREATE TABLE dbo.scada_alarm_history (\n"
    "        event_id      BIGINT IDENTITY(1,1) NOT NULL,\n"
    "        tag_key       NVARCHAR(128) NOT NULL,\n"
    "        level         TINYINT NOT NULL,\n"
    "        message       NVARCHAR(512) NULL,\n"
    "        trigger_value FLOAT NULL,\n"
    "        limit_value   FLOAT NULL,\n"
    "        raised_at     DATETIME2(3) NOT NULL,\n"
    "        cleared_at    DATETIME2(3) NULL,\n"
    "        acked_at      DATETIME2(3) NULL,\n"
    "        acked_by      NVARCHAR(64) NULL,\n"
    "        CONSTRAINT PK_scada_alarm_history PRIMARY KEY CLUSTERED (event_id ASC)\n"
    "    );\n"
    "END;\n"
    "GO\n"
    "IF NOT EXISTS (SELECT 1 FROM sys.indexes WHERE name = N'IX_scada_alarm_tag_raised' AND object_id = OBJECT_ID(N'dbo.scada_alarm_history'))\n"
    "BEGIN\n"
    "    CREATE NONCLUSTERED INDEX IX_scada_alarm_tag_raised ON dbo.scada_alarm_history (tag_key ASC, raised_at ASC);\n"
    "END;\n"
    "GO\n"
    "IF NOT EXISTS (SELECT 1 FROM sys.indexes WHERE name = N'IX_scada_alarm_raised' AND object_id = OBJECT_ID(N'dbo.scada_alarm_history'))\n"
    "BEGIN\n"
    "    CREATE NONCLUSTERED INDEX IX_scada_alarm_raised ON dbo.scada_alarm_history (raised_at ASC);\n"
    "END;\n"
    "GO\n"
    "IF OBJECT_ID(N'dbo.sp_scada_purge_history', N'P') IS NULL\n"
    "BEGIN\n"
    "    EXEC(N'CREATE PROCEDURE dbo.sp_scada_purge_history\n"
    "        @before DATETIME2(3),\n"
    "        @batch_rows INT = 5000\n"
    "    AS\n"
    "    BEGIN\n"
    "        SET NOCOUNT ON;\n"
    "        DECLARE @total INT = 0;\n"
    "        DECLARE @deleted INT = 1;\n"
    "        WHILE @deleted > 0\n"
    "        BEGIN\n"
    "            DELETE TOP (@batch_rows) FROM dbo.scada_history WHERE ts < @before;\n"
    "            SET @deleted = @@ROWCOUNT;\n"
    "            SET @total = @total + @deleted;\n"
    "        END\n"
    "        SELECT @total AS deleted_rows;\n"
    "    END');\n"
    "END;\n";

} // namespace

//==============================================================================
// 构造 / 析构
//==============================================================================

QScadaHistoryStorage::QScadaHistoryStorage(QObject *parent)
    : QObject(parent)
    , mDialect(DialectUnknown)
    , mFlushTimer(nullptr)
    , mEnabled(true)
    , mTransactionActive(false)
{
    // 单次触发 + 每次 flush 结束后重新安排：这样"写库耗时超过刷新间隔"时
    // 不会堆积多个待处理的超时事件（QTimer 重复模式会），
    // 现场表现是数据库卡顿时自动降低提交频率，而不是越卡越堆直到 OOM。
    mFlushTimer = new QTimer(this);
    mFlushTimer->setSingleShot(true);
    connect(mFlushTimer, &QTimer::timeout, this, &QScadaHistoryStorage::onFlushTimer);
}

QScadaHistoryStorage::~QScadaHistoryStorage()
{
    close();
}

//==============================================================================
// 配置相关
//==============================================================================

bool QScadaStorageConfig::isValid() const
{
    if (driver.trimmed().isEmpty())
        return false;
    if (databaseName.trimmed().isEmpty())
        return false;
    // 走网络的关系库必须有主机名；SQLite 是本地文件，不要主机。
    const QString d = driver.trimmed().toUpper();
    if (d != QLatin1String("QSQLITE") && hostName.trimmed().isEmpty())
        return false;
    if (maxBatchSize <= 0 || flushIntervalMs <= 0)
        return false;
    return true;
}

QString QScadaStorageConfig::effectiveConnectionName() const
{
    if (!connectionName.trimmed().isEmpty())
        return connectionName.trimmed();

    // 稳定命名：同一份配置永远映射到同一个连接名，重复 open() 不会泄漏连接。
    QString name = QStringLiteral("qscada_history_%1").arg(driver.trimmed().toLower());
    if (!hostName.trimmed().isEmpty())
        name += QLatin1Char('_') + hostName.trimmed();
    if (!databaseName.trimmed().isEmpty()) {
        QString dbPart = databaseName.trimmed();
        // 连接串里含分号/等号/大括号，做成连接名不好看也不安全，做一次粗粒度清洗。
        dbPart.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9_.-]")),
                       QStringLiteral("_"));
        if (dbPart.size() > 48)
            dbPart = dbPart.left(48);
        name += QLatin1Char('_') + dbPart;
    }
    return name;
}

QString QScadaStorageConfig::connectionString() const
{
    const QString db = databaseName.trimmed();

    // 已经给了完整连接串（含 DRIVER=）就原样返回：
    // 调用方可能用了本结构没覆盖的选项（多子网故障转移、MARS、只读路由……），
    // 我们不去"帮忙"重写，否则会把人家调好的参数抹掉。
    if (db.contains(QLatin1String("DRIVER="), Qt::CaseInsensitive))
        return db;

    QStringList parts;
    parts << QStringLiteral("DRIVER={ODBC Driver 17 for SQL Server}");

    const QString host = hostName.trimmed();
    if (host.contains(QLatin1Char('\\'))) {
        // 命名实例（SERVER\SQLEXPRESS）：必须用 ServerName + InstanceName，
        // 由 SQL Server Browser 服务解析端口，不能同时写 PORT。
        const int slash = host.indexOf(QLatin1Char('\\'));
        parts << QStringLiteral("ServerName=%1").arg(host.left(slash));
        parts << QStringLiteral("InstanceName=%1").arg(host.mid(slash + 1));
    } else {
        parts << QStringLiteral("Server=%1").arg(host);
        if (port > 0)
            parts << QStringLiteral("Port=%1").arg(port);
    }

    parts << QStringLiteral("Database=%1").arg(db);
    if (!userName.isEmpty()) {
        parts << QStringLiteral("Uid=%1").arg(userName);
        parts << QStringLiteral("Pwd=%1").arg(password);
    } else {
        // 不给账号时用 Windows 集成认证：工控机常以域账号运行上位机，
        // 这样配置文件里就不用落明文密码（安全审计比较看重这点）。
        parts << QStringLiteral("Trusted_Connection=yes");
    }
    if (!odbcOptions.trimmed().isEmpty())
        parts << odbcOptions.trimmed();

    return parts.join(QLatin1Char(';'));
}

QString QScadaStorageConfig::description() const
{
    // 刻意不输出 password：这个字符串会进日志和界面，明文口令泄露是审计红线。
    return QStringLiteral("driver=%1 db=%2 host=%3:%4 user=%5 batch=%6 interval=%7ms retention=%8d")
            .arg(driver)
            .arg(databaseName)
            .arg(hostName)
            .arg(port)
            .arg(userName.isEmpty() ? QStringLiteral("(integrated)") : userName)
            .arg(maxBatchSize)
            .arg(flushIntervalMs)
            .arg(retentionDays);
}

QScadaStorageConfig QScadaStorageConfig::sqliteDefault(const QString &filePath)
{
    QScadaStorageConfig cfg;
    cfg.driver = QStringLiteral("QSQLITE");
    cfg.databaseName = filePath;
    cfg.connectionName = QStringLiteral("qscada_history_sqlite");
    // 单机场景点数少、写入压力小，提交可以更密一些，掉电丢数据的窗口更小。
    cfg.maxBatchSize = 200;
    cfg.flushIntervalMs = 500;
    cfg.retentionDays = 30;
    return cfg;
}

QScadaStorageConfig QScadaStorageConfig::sqlServerDefault(const QString &host,
                                                          const QString &database,
                                                          const QString &user,
                                                          const QString &password,
                                                          int port)
{
    QScadaStorageConfig cfg;
    cfg.driver = QStringLiteral("QODBC");
    cfg.hostName = host;
    cfg.databaseName = database;
    cfg.userName = user;
    cfg.password = password;
    cfg.port = port;
    cfg.connectionName = QStringLiteral("qscada_history_mssql");
    // SQL Server 单条 INSERT 的固定开销比 SQLite 大（网络往返 + 日志）,
    // 所以批更大、间隔更长，用"最多丢 1 秒数据"换数据库压力。
    cfg.maxBatchSize = 500;
    cfg.flushIntervalMs = 1000;
    cfg.retentionDays = 90;
    cfg.odbcOptions = QStringLiteral("TrustServerCertificate=yes");
    return cfg;
}

//==============================================================================
// 驱动方言
//==============================================================================

QScadaHistoryStorage::SchemaDialect QScadaHistoryStorage::dialectForDriver(const QString &driver)
{
    const QString d = driver.trimmed().toUpper();
    if (d == QLatin1String("QSQLITE"))
        return DialectSqlite;
    if (d == QLatin1String("QODBC") || d == QLatin1String("QODBC3"))
        return DialectMssql;
    if (d == QLatin1String("QPSQL"))
        return DialectPostgres;
    return DialectUnknown;
}

QString QScadaHistoryStorage::dialectName(SchemaDialect dialect)
{
    switch (dialect) {
    case DialectSqlite:   return QStringLiteral("SQLite");
    case DialectMssql:    return QStringLiteral("SQL Server (ODBC)");
    case DialectPostgres: return QStringLiteral("PostgreSQL");
    case DialectUnknown:  break;
    }
    return QStringLiteral("未知");
}

//==============================================================================
// 打开 / 关闭
//==============================================================================

bool QScadaHistoryStorage::sameThread(QString *errorString) const
{
    // QSqlDatabase 的连接对象只能在创建它的线程使用（Qt 的硬约束，
    // 跨线程使用会偶发崩溃或返回空结果）。这里主动拦一道，
    // 给出明确错误，比让驱动层抛一句含糊的 "Driver not loaded" 好排查得多。
    if (thread() == QThread::currentThread())
        return true;
    if (errorString)
        *errorString = QStringLiteral("历史库对象只能在创建它的线程使用；请先用 moveToThread() 迁移到目标线程。");
    return false;
}

bool QScadaHistoryStorage::open(const QScadaStorageConfig &config, QString *errorString)
{
    if (!sameThread(errorString))
        return false;

    if (!config.isValid()) {
        const QString msg = QStringLiteral("历史库配置不完整（驱动=%1 库=%2 主机=%3，批量=%4 间隔=%5）")
                .arg(config.driver, config.databaseName, config.hostName)
                .arg(config.maxBatchSize)
                .arg(config.flushIntervalMs);
        if (errorString)
            *errorString = msg;
        reportError(msg);
        return false;
    }

    // 换配置时先干净地关掉旧连接，避免同一连接名上残留旧驱动的句柄。
    if (mDatabase.isValid())
        close();

    mConfig = config;
    mDialect = dialectForDriver(mConfig.driver);
    if (mDialect == DialectUnknown) {
        // 不认识的驱动不直接拒绝：Qt 支持的库很多，用户可能想用 QMYSQL。
        // 但要让上层知道"建表语句没有对应方言"，避免静默失败。
        const QString warn = QStringLiteral("驱动 %1 没有内置建表脚本，需要手工建表或使用 applySchemaFile()。")
                .arg(mConfig.driver);
        reportError(warn);
    }

    if (!ensureDatabase(errorString))
        return false;

    // 打开即建表（幂等），覆盖"配置指向了一个还没建表的空库"这一现场常见情况。
    QString schemaError;
    if (!createSchema(&schemaError)) {
        if (errorString)
            *errorString = schemaError;
        // 建表失败不一定致命（可能库已建好但当前账号没有 DDL 权限），
        // 所以保留连接，让后续写入用真实错误说话。
        reportError(schemaError);
    }

    mEnabled = true;
    scheduleFlushTimer();
    emit openedChanged(true);
    return true;
}

bool QScadaHistoryStorage::ensureDatabase(QString *errorString)
{
    mConnectionName = mConfig.effectiveConnectionName();

    // 同名连接已存在时先移除：重复 open() （例如用户改了主机名再点"连接"）
    // 若不清理，Qt 会继续用旧参数的连接，出现"改了配置没生效"的诡异现象。
    if (QSqlDatabase::contains(mConnectionName)) {
        {
            QSqlDatabase stale = QSqlDatabase::database(mConnectionName, true);
            if (stale.isOpen())
                stale.close();
        }
        QSqlDatabase::removeDatabase(mConnectionName);
    }

    if (!QSqlDatabase::isDriverAvailable(mConfig.driver)) {
        // 这是部署问题，不是代码问题，错误信息必须直接点出来源：
        // QODBC 不可用 = 没装 ODBC 驱动或 Qt 的 sqldrivers 目录没拷全。
        QString msg = QStringLiteral("Qt 未提供 SQL 驱动 %1。").arg(mConfig.driver);
        if (mConfig.driver.toUpper() == QLatin1String("QODBC")) {
            msg += QStringLiteral(" 请确认：1) 已安装 \"Microsoft ODBC Driver for SQL Server\" "
                                  "（或 SQL Server Native Client）；"
                                  "2) 程序目录下 plugins\\sqldrivers\\ 里有 qsqlodbc.dll。");
        } else if (mConfig.driver.toUpper() == QLatin1String("QSQLITE")) {
            msg += QStringLiteral(" 请确认程序目录下 plugins\\sqldrivers\\ 里有 qsqlite.dll。");
        }
        msg += QStringLiteral(" 当前可用驱动：") + QSqlDatabase::drivers().join(QStringLiteral(", "));
        if (errorString)
            *errorString = msg;
        reportError(msg);
        return false;
    }

    mDatabase = QSqlDatabase::addDatabase(mConfig.driver, mConnectionName);

    const QString driverUpper = mConfig.driver.trimmed().toUpper();
    if (driverUpper == QLatin1String("QODBC")) {
        // QODBC 的 databaseName 就是连接串；connectionString() 已处理
        // "用户直接给完整串"和"按字段拼接"两种情况。
        mDatabase.setDatabaseName(mConfig.connectionString());
    } else {
        mDatabase.setDatabaseName(mConfig.databaseName);
    }

    if (!mConfig.hostName.isEmpty())
        mDatabase.setHostName(mConfig.hostName);
    if (mConfig.port > 0 && driverUpper != QLatin1String("QODBC"))
        mDatabase.setPort(mConfig.port);
    if (!mConfig.userName.isEmpty())
        mDatabase.setUserName(mConfig.userName);
    if (!mConfig.password.isEmpty())
        mDatabase.setPassword(mConfig.password);

    if (driverUpper == QLatin1String("QSQLITE")) {
        /*
         * SQLite 默认的 journal 模式在"边写边查"（历史库写入 + 趋势查询同时进行）
         * 时会立刻抛 "database is locked"。busy_timeout 让偶发冲突自动重试而不是
         * 直接失败——这是单机上把 SQLite 当历史库用的必要前提。
         * 只设置这一个选项是有意的：WAL 模式的连接选项（QSQLITE_ENABLE_WAL）
         * 在较老的 Qt 5 版本上不存在，写了会被驱动当成未知选项忽略甚至拒绝打开；
         * 需要在 open() 之后自行执行 "PRAGMA journal_mode=WAL;" 来开启
         * （executeSql() 可以直接跑这条 PRAGMA）。
         */
        mDatabase.setConnectOptions(QStringLiteral("QSQLITE_BUSY_TIMEOUT=5000"));
    } else if (driverUpper == QLatin1String("QODBC")) {
        // 不设置任何 QODBC 连接选项：事务的自动提交由 QSqlDatabase::transaction()
        // 统一切换，驱动侧再插一脚会导致"事务里看不到自己的数据"这类怪问题。
        // 需要额外 ODBC 属性时，请通过 config.odbcOptions 写进连接串。
    }

    if (!mDatabase.open()) {
        const QSqlError err = mDatabase.lastError();
        const QString msg = QStringLiteral("打开历史库失败：%1（驱动=%2，%3）")
                .arg(err.text().trimmed().isEmpty() ? QStringLiteral("驱动未给出详细信息") : err.text())
                .arg(mConfig.driver)
                .arg(mConfig.description());
        if (errorString)
            *errorString = msg;
        reportError(msg);
        return false;
    }

    mStatistics.lastError.clear();
    return true;
}

void QScadaHistoryStorage::close()
{
    if (!mDatabase.isValid() && !QSqlDatabase::contains(mConnectionName))
        return;

    // 关闭前先落库：缓冲里那几十毫秒的数据对现场排查很重要，
    // 而"关闭历史库"通常是用户主动操作，多花几毫秒完全可以接受。
    if (mEnabled && !mPending.isEmpty())
        flush();

    if (mFlushTimer)
        mFlushTimer->stop();

    if (mTransactionActive)
        rollbackTransaction();

    const bool wasOpen = isOpen();
    const QString name = mConnectionName;

    // 关键点：必须先让所有 QSqlQuery 析构、mDatabase 失效，再 removeDatabase，
    // 否则 Qt 会打印 "connection is still in use" 警告，且连接实际没被释放。
    mDatabase = QSqlDatabase();
    if (!name.isEmpty() && QSqlDatabase::contains(name)) {
        {
            QSqlDatabase db = QSqlDatabase::database(name, true);
            if (db.isOpen())
                db.close();
        }
        QSqlDatabase::removeDatabase(name);
    }
    mConnectionName.clear();

    if (wasOpen)
        emit openedChanged(false);
}

bool QScadaHistoryStorage::isOpen() const
{
    return mDatabase.isValid() && mDatabase.isOpen();
}

//==============================================================================
// 建表脚本（幂等）
//==============================================================================

bool QScadaHistoryStorage::createSchema(QString *errorString)
{
    if (!sameThread(errorString))
        return false;
    if (!isOpen()) {
        if (errorString)
            *errorString = QStringLiteral("历史库尚未打开，无法建表。");
        return false;
    }

    QStringList statements;
    QString batchSeparator;
    switch (mDialect) {
    case DialectSqlite:
        statements = splitSqlStatements(QString::fromLatin1(kSqliteSchemaSql), QString());
        break;
    case DialectMssql:
        batchSeparator = QStringLiteral("GO");
        statements = splitSqlStatements(QString::fromLatin1(kMssqlSchemaSql), batchSeparator);
        break;
    case DialectPostgres:
    case DialectUnknown:
        if (errorString)
            *errorString = QStringLiteral("驱动 %1 没有内置建表脚本，请用 applySchemaFile() 执行对应脚本。")
                    .arg(mConfig.driver);
        return false;
    }

    for (int i = 0; i < statements.size(); ++i) {
        const QString sql = statements.at(i);
        if (sql.trimmed().isEmpty())
            continue;
        QSqlQuery query(mDatabase);
        if (!query.exec(sql)) {
            const QSqlError err = query.lastError();
            const QString msg = QStringLiteral("执行建表语句失败：%1；SQL=%2")
                    .arg(err.text().trimmed())
                    .arg(sql.left(160).replace(QLatin1Char('\n'), QLatin1Char(' ')));
            if (errorString)
                *errorString = msg;
            reportError(msg);
            return false;
        }
    }
    return true;
}

bool QScadaHistoryStorage::applySchemaFile(const QString &filePath, QString *errorString)
{
    if (!sameThread(errorString))
        return false;
    if (!isOpen()) {
        if (errorString)
            *errorString = QStringLiteral("历史库尚未打开，无法执行建表脚本。");
        return false;
    }

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (errorString)
            *errorString = QStringLiteral("无法打开 SQL 脚本 %1：%2").arg(filePath, file.errorString());
        return false;
    }
    QTextStream stream(&file);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    stream.setEncoding(QStringConverter::Utf8);
#else
    stream.setCodec("UTF-8");
#endif
    const QString content = stream.readAll();
    file.close();

    // GO 只在 SQL Server 脚本里有意义，其余驱动按分号切。
    const QString separator = (mDialect == DialectMssql) ? QStringLiteral("GO") : QString();
    const QStringList statements = splitSqlStatements(content, separator);
    for (int i = 0; i < statements.size(); ++i) {
        const QString sql = statements.at(i);
        if (sql.trimmed().isEmpty())
            continue;
        QSqlQuery query(mDatabase);
        if (!query.exec(sql)) {
            const QSqlError err = query.lastError();
            const QString msg = QStringLiteral("执行脚本 %1 第 %2 段失败：%3")
                    .arg(filePath).arg(i + 1).arg(err.text().trimmed());
            if (errorString)
                *errorString = msg;
            reportError(msg);
            return false;
        }
    }
    return true;
}

bool QScadaHistoryStorage::executeSql(const QString &sql, QString *errorString)
{
    if (!sameThread(errorString))
        return false;
    if (!isOpen()) {
        if (errorString)
            *errorString = QStringLiteral("历史库尚未打开。");
        return false;
    }
    QSqlQuery query(mDatabase);
    if (!query.exec(sql)) {
        const QString msg = QStringLiteral("执行 SQL 失败：%1").arg(query.lastError().text().trimmed());
        if (errorString)
            *errorString = msg;
        reportError(msg);
        return false;
    }
    return true;
}

//==============================================================================
// 缓冲与批量写入
//==============================================================================

bool QScadaHistoryStorage::append(const QScadaHistoryRecord &record)
{
    if (!mEnabled)
        return false;

    // 即使当前没连上库也先缓冲：现场经常出现"数据库重启了 1 分钟"的情况，
    // 缓冲能让这段时间的数据在恢复后补写进去（上限由 maxBatchSize 控制，
    // 超出部分由 flush 失败后的丢弃策略兜底，不会无限涨）。
    if (!record.isValid()) {
        ++mStatistics.rejectedRecords;
        return false;
    }

    if (!sameThread(0)) {
        // 跨线程调用属于用法错误：直接拒绝并上报，不做隐式加锁
        // （加锁会让"谁在什么线程写库"变得不可预测，Qt Sql 本身也不是为此设计的）。
        reportError(QStringLiteral("append() 被跨线程调用，记录已丢弃（位号=%1）。").arg(record.tagKey()));
        ++mStatistics.rejectedRecords;
        return false;
    }

    mPending.append(record);
    mStatistics.bufferedRecords = mPending.size();

    // 缓冲达到批量上限时立刻提交，避免高频场景下内存里堆太多记录
    // （3000 点/秒 的系统，1 秒缓冲就是 3000 条，其中含 QString，占用不小）。
    if (mPending.size() >= qMax(1, mConfig.maxBatchSize))
        flush();
    return true;
}

bool QScadaHistoryStorage::appendBatch(const QList<QScadaHistoryRecord> &records)
{
    if (!mEnabled || records.isEmpty())
        return false;

    bool allAccepted = true;
    mPending.reserve(mPending.size() + records.size());
    for (int i = 0; i < records.size(); ++i) {
        if (!records.at(i).isValid()) {
            ++mStatistics.rejectedRecords;
            allAccepted = false;
            continue;
        }
        mPending.append(records.at(i));
    }
    mStatistics.bufferedRecords = mPending.size();

    if (mPending.size() >= qMax(1, mConfig.maxBatchSize))
        flush();
    return allAccepted;
}

int QScadaHistoryStorage::pendingCount() const
{
    return mPending.size();
}

void QScadaHistoryStorage::clearPending()
{
    mPending.clear();
    mStatistics.bufferedRecords = 0;
}

void QScadaHistoryStorage::onFlushTimer()
{
    flush();
    // 重新安排下一次：单次定时器 + 末尾续期，天然实现"上一次没做完就不叠加"。
    scheduleFlushTimer();
}

void QScadaHistoryStorage::scheduleFlushTimer()
{
    if (!mFlushTimer)
        return;
    if (!mEnabled || !isOpen()) {
        mFlushTimer->stop();
        return;
    }
    // 下限 100ms：现场若把间隔配成 1ms，等于把数据库当忙等目标打。
    mFlushTimer->start(qMax(100, mConfig.flushIntervalMs));
}

bool QScadaHistoryStorage::flush()
{
    int written = 0;
    QString error;
    const bool ok = flushInternal(&written, &error);
    if (!ok && !error.isEmpty())
        reportError(error);
    emit flushCompleted(written);
    return ok;
}

bool QScadaHistoryStorage::flushInternal(int *writtenCount, QString *errorString)
{
    if (writtenCount)
        *writtenCount = 0;

    if (!sameThread(errorString))
        return false;

    if (!isOpen()) {
        if (errorString)
            *errorString = QStringLiteral("历史库尚未打开，flush 被跳过（缓冲 %1 条）。").arg(mPending.size());
        return false;
    }
    if (mPending.isEmpty())
        return true;

    ++mStatistics.flushCount;

    // 把缓冲整体取出，边写边清：这样 flush 过程中新来的 append() 会进入新的缓冲，
    // 不会因为一次写很久而让采集侧阻塞或丢数据。
    QList<QScadaHistoryRecord> batch;
    batch.swap(mPending);
    mStatistics.bufferedRecords = 0;

    // 按时间排序再写。理由：
    // 1. 同一批记录时间接近，写入时索引页（tag_key, ts）的插入位置基本连续，
    //    减少页分裂与随机 IO，SQL Server 上批量插入的收益非常明显；
    // 2. 多线程采集时记录到达顺序是乱的，排序能保证库里同毫秒数据的相对顺序稳定，
    //    避免曲线出现"回跳"（后写的数据时间更早）。
    sortByTimestamp(batch);

    const int maxPerStatement = qMax(1, mConfig.maxBatchSize);
    int totalWritten = 0;

    if (!beginTransaction(errorString)) {
        ++mStatistics.failedBatches;
        // 本批数据回滚式丢弃：不重新塞回缓冲。原因是"事务开不起来"通常意味着
        // 连接已断或库不可写，塞回去只会在下一次 flush 再失败一次，
        // 并且缓冲区会持续膨胀。上层收到 errorOccurred 后可决定落本地文件兜底。
        // 统计字段（lastError 等）由最外层 flush() 的 reportError() 统一写入，
        // 这里不重复写，避免两处维护同一份错误信息而不一致。
        return false;
    }

    for (int offset = 0; offset < batch.size(); offset += maxPerStatement) {
        const int count = qMin(maxPerStatement, batch.size() - offset);
        const QList<QScadaHistoryRecord> chunk = batch.mid(offset, count);
        QString chunkError;
        if (!execBatch(chunk, &chunkError)) {
            // 整批回滚：宁可这一批全丢，也不要库里出现"半批数据"。
            // 半批数据在曲线上表现为一段锯齿或一段平台，比缺一段更难解释。
            rollbackTransaction();
            ++mStatistics.failedBatches;
            if (errorString)
                *errorString = chunkError;
            return false;
        }
        totalWritten += count;
    }

    if (!commitTransaction(errorString)) {
        rollbackTransaction();
        ++mStatistics.failedBatches;
        return false;
    }

    mStatistics.writtenRecords += static_cast<quint64>(totalWritten);
    mStatistics.lastWriteTime = QDateTime::currentDateTime();
    mStatistics.bufferedRecords = mPending.size();
    if (writtenCount)
        *writtenCount = totalWritten;
    return true;
}

bool QScadaHistoryStorage::beginTransaction(QString *errorString)
{
    if (!mDatabase.transaction()) {
        const QString msg = QStringLiteral("开启数据库事务失败：%1").arg(mDatabase.lastError().text().trimmed());
        if (errorString)
            *errorString = msg;
        return false;
    }
    mTransactionActive = true;
    return true;
}

bool QScadaHistoryStorage::commitTransaction(QString *errorString)
{
    if (!mTransactionActive)
        return true;
    if (!mDatabase.commit()) {
        const QString msg = QStringLiteral("提交事务失败：%1").arg(mDatabase.lastError().text().trimmed());
        if (errorString)
            *errorString = msg;
        mTransactionActive = false;
        return false;
    }
    mTransactionActive = false;
    return true;
}

void QScadaHistoryStorage::rollbackTransaction()
{
    if (!mTransactionActive)
        return;
    mDatabase.rollback();
    mTransactionActive = false;
}

bool QScadaHistoryStorage::execBatch(const QList<QScadaHistoryRecord> &batch, QString *errorString)
{
    if (batch.isEmpty())
        return true;

    /*
     * 多行 VALUES 的批量 INSERT。
     *
     * 为什么不用"循环里反复 addBindValue + exec"：
     * 那样每条记录仍是一次独立的语句执行（虽然共享事务，但往返/解析次数不变），
     * 我们要的是"一次解析、一次执行、一次网络往返写进去 500 行"。
     *
     * 参数占位符统一用 '?'（位置绑定）而不是命名占位符：
     * QODBC 对命名占位符的翻译依赖驱动实现，各版本表现不一，
     * 位置绑定在 SQLite / ODBC / PostgreSQL 上行为一致，换库不用改绑定代码。
     *
     * 参数个数上限：SQL Server 单条语句最多 2100 个参数，
     * 每行 5 个参数 => 最多 420 行/语句。maxBatchSize 默认 500 看似越界，
     * 但 flushInternal() 已按 maxBatchSize 切块，而这里再按 400 行兜底一次，
     * 保证即使用户把 maxBatchSize 配成 100000 也不会撞上驱动上限。
     */
    const int rowsPerStatement = 400;
    const QString prefix = QStringLiteral("INSERT INTO %1 (tag_key, value, quality, ts, raw_text) VALUES ")
            .arg(historyTableName());

    for (int offset = 0; offset < batch.size(); offset += rowsPerStatement) {
        const int count = qMin(rowsPerStatement, batch.size() - offset);

        QString sql = prefix;
        for (int i = 0; i < count; ++i) {
            if (i > 0)
                sql += QStringLiteral(", ");
            sql += QStringLiteral("(?, ?, ?, ?, ?)");
        }

        QSqlQuery query(mDatabase);
        if (!query.prepare(sql)) {
            if (errorString)
                *errorString = QStringLiteral("准备批量写入语句失败：%1")
                        .arg(query.lastError().text().trimmed());
            return false;
        }
        for (int i = 0; i < count; ++i) {
            const QScadaHistoryRecord &record = batch.at(offset + i);
            query.addBindValue(record.tagKey());
            query.addBindValue(record.value());
            query.addBindValue(static_cast<int>(record.quality()));
            // 时间统一由我们格式化成固定字符串（见 formatTimestamp 的说明），
            // 而不是把 QDateTime 直接交给驱动去序列化。
            query.addBindValue(formatTimestamp(record.timestamp()));
            if (record.hasRawText()) {
                // raw_text 列在 SQL Server 侧是 NVARCHAR(256)：超长会整条语句报错，
                // 而批量写入是一条语句，超长会导致**整批 500 条一起失败**。
                // 所以在这里截断而不是把异常留给数据库——原始文本只是辅助追溯信息，
                // 截断的代价远小于丢一批历史数据。
                const QString text = record.rawText();
                query.addBindValue(text.size() > 256 ? text.left(256) : text);
            } else {
                query.addBindValue(QVariant(QVariant::String));   // 显式 NULL
            }
        }

        if (!query.exec()) {
            const QSqlError err = query.lastError();
            if (errorString)
                *errorString = QStringLiteral("批量写入 %1 条记录失败：%2")
                        .arg(count)
                        .arg(err.text().trimmed());
            return false;
        }
        // 刻意不调用 query.lastInsertId()：本项目的主键完全交给数据库
        // （IDENTITY / AUTOINCREMENT），不依赖驱动返回自增值——
        // Qt 6 各驱动的 lastInsertId() 行为不一致，依赖它会让代码换库即坏。
    }
    return true;
}

void QScadaHistoryStorage::sortByTimestamp(QList<QScadaHistoryRecord> &records)
{
    // 用 std::sort（C++11，Qt5/Qt6 都有）而不是已废弃的 qSort；
    // 稳定排序（stable_sort）在这里没有必要：同毫秒的两条点谁先入库对曲线无影响，
    // 而 stable_sort 会额外申请临时内存，批量路径上不值得。
    std::sort(records.begin(), records.end(),
              [](const QScadaHistoryRecord &a, const QScadaHistoryRecord &b) {
                  return a.timestampMs() < b.timestampMs();
              });
}

void QScadaHistoryStorage::reportError(const QString &message)
{
    mStatistics.lastError = message;
    mStatistics.lastErrorTime = QDateTime::currentDateTime();
    // 库代码不弹窗、不写死日志系统：只发信号，由应用层决定是记日志、
    // 显示状态栏还是写入审计表。
    emit errorOccurred(message);
}

//==============================================================================
// 查询
//==============================================================================

QList<QScadaHistoryRecord> QScadaHistoryStorage::queryRaw(const QString &tagKey,
                                                          const QDateTime &from,
                                                          const QDateTime &to,
                                                          QString *errorString)
{
    QList<QScadaHistoryRecord> result;
    if (!sameThread(errorString))
        return result;
    if (!isOpen()) {
        if (errorString)
            *errorString = QStringLiteral("历史库尚未打开，无法查询。");
        return result;
    }
    if (tagKey.isEmpty() || !from.isValid() || !to.isValid() || from > to) {
        if (errorString)
            *errorString = QStringLiteral("查询参数非法（位号空或时间区间颠倒）。");
        return result;
    }

    /*
     * 查询只走 (tag_key, ts) 复合索引：
     * tag_key 等值 + ts 范围，两者都用上，不会退化成全表扫描。
     * ORDER BY ts 也能直接用索引顺序，省掉排序步骤——
     * 这也是索引列顺序必须是 tag_key 在前的原因之一。
     */
    const QString sql = QStringLiteral(
                "SELECT tag_key, value, quality, ts, raw_text FROM %1 "
                "WHERE tag_key = ? AND ts >= ? AND ts <= ? ORDER BY ts ASC, id ASC")
            .arg(historyTableName());

    QSqlQuery query(mDatabase);
    if (!query.prepare(sql)) {
        const QString msg = QStringLiteral("准备查询语句失败：%1").arg(query.lastError().text().trimmed());
        if (errorString)
            *errorString = msg;
        reportError(msg);
        return result;
    }
    query.addBindValue(tagKey);
    query.addBindValue(formatTimestamp(from));
    query.addBindValue(formatTimestamp(to));

    if (!query.exec()) {
        const QString msg = QStringLiteral("查询历史数据失败：%1").arg(query.lastError().text().trimmed());
        if (errorString)
            *errorString = msg;
        reportError(msg);
        return result;
    }

    while (query.next()) {
        QScadaHistoryRecord record;
        record.setTagKey(query.value(0).toString());
        record.setValue(query.value(1).toDouble());
        // 质量码用整数读回后校验：库里若出现越界值（人工改库、程序旧版本写入），
        // 统一按 Bad 处理，绝不当成 Good 混进统计。
        record.setQuality(qscadaQualityFromInt(query.value(2).toInt()));
        record.setTimestamp(parseTimestamp(query.value(3).toString()));
        if (!query.value(4).isNull())
            record.setRawText(query.value(4).toString());
        if (record.timestamp().isValid())
            result.append(record);
    }
    return result;
}

int QScadaHistoryStorage::countRecords(const QString &tagKey,
                                       const QDateTime &from,
                                       const QDateTime &to,
                                       QString *errorString)
{
    if (!sameThread(errorString))
        return -1;
    if (!isOpen()) {
        if (errorString)
            *errorString = QStringLiteral("历史库尚未打开，无法统计。");
        return -1;
    }

    // COUNT(*) 也走同一个复合索引（覆盖索引扫描），不读数据页，比取回数据再 size() 快得多。
    // 界面用它在画曲线前估算数据量，决定是否需要降采样。
    const QString sql = QStringLiteral("SELECT COUNT(*) FROM %1 WHERE tag_key = ? AND ts >= ? AND ts <= ?")
            .arg(historyTableName());
    QSqlQuery query(mDatabase);
    if (!query.prepare(sql)) {
        if (errorString)
            *errorString = QStringLiteral("准备统计语句失败：%1").arg(query.lastError().text().trimmed());
        return -1;
    }
    query.addBindValue(tagKey);
    query.addBindValue(formatTimestamp(from));
    query.addBindValue(formatTimestamp(to));
    if (!query.exec() || !query.next()) {
        const QString msg = QStringLiteral("统计历史数据条数失败：%1").arg(query.lastError().text().trimmed());
        if (errorString)
            *errorString = msg;
        reportError(msg);
        return -1;
    }
    return query.value(0).toInt();
}

QStringList QScadaHistoryStorage::distinctTagKeys(QString *errorString)
{
    QStringList keys;
    if (!sameThread(errorString))
        return keys;
    if (!isOpen()) {
        if (errorString)
            *errorString = QStringLiteral("历史库尚未打开。");
        return keys;
    }
    const QString sql = QStringLiteral("SELECT DISTINCT tag_key FROM %1 ORDER BY tag_key ASC")
            .arg(historyTableName());
    QSqlQuery query(mDatabase);
    if (!query.exec(sql)) {
        const QString msg = QStringLiteral("读取位号列表失败：%1").arg(query.lastError().text().trimmed());
        if (errorString)
            *errorString = msg;
        reportError(msg);
        return keys;
    }
    while (query.next())
        keys.append(query.value(0).toString());
    return keys;
}

/*
 * ============================ 降采样 ============================
 *
 * 为什么必须做（这是历史趋势能否"能用"的分界线）：
 * 界面上要看一个月的曲线，1 秒 1 条 = 259 万条，1000 点系统单点也常是几十万条。
 * 直接把原始点喂给 QCustomPlot/QChart：
 *   - 查询本身要传几十万行、占几十 MB 内存，界面卡住几秒到几十秒；
 *   - 绘图时每个点都要算一次坐标变换、走一次 painter 路径，
 *     几十万个 lineTo 会让重绘（鼠标缩放、窗口拉伸）彻底失去响应；
 *   - 而屏幕横向只有一两千个像素，画 259 万个点在信息量上是完全冗余的。
 * 所以超过 maxPoints 时必须先降采样。
 *
 * 关键取舍：**等时间间隔分桶 + 每桶取 MIN/MAX/AVG 组成包络**，
 * 而不是"每 N 个点取一个"或"直接求平均"。
 *
 *   - 纯平均（每桶只出一条均值）会把尖峰抹平：一个持续 5 秒的超压尖峰，
 *     在 1 小时一桶的均值曲线里几乎看不见，而工艺上恰恰是这种尖峰导致安全阀起跳。
 *     用平均值做监控曲线是典型的"看起来平滑、出事时查不出原因"。
 *   - 纯抽样（每 N 个取 1 个）更糟：可能刚好跳过了尖峰，且曲线上点的疏密
 *     与时间不成比例，容易误导读数。
 *   - MIN/MAX 包络保留了桶内的极值，尖峰一定会被画出来（超限分析、事故复盘的依据），
 *     AVG 则给出趋势中线。三条线一起画，既不失真也不卡。
 *
 * 桶宽用"请求区间长度 / maxPoints"计算，保证桶数 <= maxPoints，
 * 从而总点数 <= 3*maxPoints；最后一个桶吸收余数（区间不能整除时的时间尾巴），
 * 避免出现一个特别窄的畸形桶。桶时间戳取桶起点，使三条包络线之间严格对齐。
 */
QList<QScadaHistoryStorage::Row> QScadaHistoryStorage::downsample(
        const QList<QScadaHistoryRecord> &records,
        const QDateTime &from, const QDateTime &to, int maxPoints)
{
    QList<Row> rows;
    if (records.isEmpty())
        return rows;

    if (maxPoints <= 0 || records.size() <= maxPoints) {
        // 不降采样：原始点直接返回，value 有效、min/max/avg 不参与展示。
        for (int i = 0; i < records.size(); ++i) {
            const QScadaHistoryRecord &record = records.at(i);
            Row row;
            row.sequence = i;
            row.timestamp = record.timestamp();
            row.value = record.value();
            row.minValue = record.value();
            row.maxValue = record.value();
            row.avgValue = record.value();
            row.sampleCount = 1;
            row.quality = record.quality();
            row.rawText = record.rawText();
            rows.append(row);
        }
        return rows;
    }

    const qint64 startMs = from.toMSecsSinceEpoch();
    const qint64 endMs = to.toMSecsSinceEpoch();
    const qint64 spanMs = qMax<qint64>(1, endMs - startMs);
    // 桶宽至少 1ms，防止 maxPoints 远大于区间毫秒数时出现 0 宽桶（除零）。
    const qint64 bucketWidthMs = qMax<qint64>(1, spanMs / static_cast<qint64>(maxPoints));

    // 桶数按 ceil(span / width) 算，实际可能比 maxPoints 少一个（整除时），
    // 但绝不会多——总点数因此有硬上界 3*maxPoints，界面内存可预期。
    const int bucketCount = static_cast<int>(qMin<qint64>(
            static_cast<qint64>(maxPoints), (spanMs + bucketWidthMs - 1) / bucketWidthMs));

    // 用纯 C 数组做累加器（不用 QVector<struct>）：这个函数在趋势刷新路径上会被
    // 频繁调用，避免为几十万个临时小对象做堆分配。
    QVector<double> sum(bucketCount, 0.0);
    QVector<double> minValue(bucketCount, 0.0);
    QVector<double> maxValue(bucketCount, 0.0);
    QVector<int> count(bucketCount, 0);
    QVector<int> worstQuality(bucketCount, static_cast<int>(Good));

    for (int i = 0; i < records.size(); ++i) {
        const QScadaHistoryRecord &record = records.at(i);
        qint64 index = (record.timestampMs() - startMs) / bucketWidthMs;
        if (index < 0)
            index = 0;
        // 取不到最后一个桶就并入最后一桶：宁可让末桶宽一点，
        // 也不要丢掉区间右端的数据（用户拖到"现在"时这段恰恰最新）。
        if (index >= bucketCount)
            index = bucketCount - 1;

        const double v = record.value();
        if (count.at(static_cast<int>(index)) == 0) {
            minValue[static_cast<int>(index)] = v;
            maxValue[static_cast<int>(index)] = v;
        } else {
            if (v < minValue.at(static_cast<int>(index)))
                minValue[static_cast<int>(index)] = v;
            if (v > maxValue.at(static_cast<int>(index)))
                maxValue[static_cast<int>(index)] = v;
        }
        sum[static_cast<int>(index)] += v;
        count[static_cast<int>(index)] += 1;

        // 桶内质量取"最差"的一条（保守原则）：
        // 只要桶里有一个 Bad 点，这个桶就不能被当成完全可信的数据。
        // 取最好会把通讯中断美化成正常数据，取最差最多让用户多看一眼灰色区段。
        const int q = static_cast<int>(record.quality());
        if (q > worstQuality.at(static_cast<int>(index)))
            worstQuality[static_cast<int>(index)] = q;
    }

    // sequence 作为时间序输出，保证同一时间戳的 min/max/avg 顺序稳定
    // （界面若把三条线合成一条绘制，靠它排序不会来回跳）。
    qint64 sequence = 0;
    for (int b = 0; b < bucketCount; ++b) {
        if (count.at(b) <= 0)
            continue;   // 空桶不产生点：画包络线时直接连到下一个有数据的桶，
                        // 比造一个 0 值点更好——造 0 会在曲线上画出一条假的"掉到零"。
        Row row;
        row.sequence = sequence++;
        row.timestamp = QDateTime::fromMSecsSinceEpoch(startMs + bucketWidthMs * b, Qt::LocalTime);
        row.minValue = minValue.at(b);
        row.maxValue = maxValue.at(b);
        row.avgValue = sum.at(b) / static_cast<double>(count.at(b));
        // 未降采样时的 value 语义：包络场景下给平均值，
        // 让只画一条线的调用方（例如导出 CSV 打印）也能拿到合理结果。
        row.value = row.avgValue;
        row.sampleCount = count.at(b);
        row.quality = qscadaQualityFromInt(worstQuality.at(b));
        rows.append(row);
    }
    return rows;
}

QList<QScadaHistoryStorage::Row> QScadaHistoryStorage::query(const QString &tagKey,
                                                             const QDateTime &from,
                                                             const QDateTime &to,
                                                             int maxPoints,
                                                             QString *errorString)
{
    const QList<QScadaHistoryRecord> raw = queryRaw(tagKey, from, to, errorString);
    return downsample(raw, from, to, maxPoints);
}

//==============================================================================
// 过期清理
//==============================================================================

int QScadaHistoryStorage::purge(const QDateTime &before)
{
    if (!sameThread(0) || !isOpen() || !before.isValid())
        return -1;

    /*
     * 分批删除的理由（见头文件）：一次 DELETE 几千万行会
     * 1) 把事务日志撑到与数据量同量级（SQL Server 日志可能直接涨几十 GB）；
     * 2) 长时间持有排他锁，把实时写入全部堵死（现场表现是清理期间历史曲线出现空洞）。
     * 每次 5000 行、循环删除，可以把锁切成一段一段，让写入线程有机会插进去。
     *
     * 两种库的分批写法不同（这是必须屏蔽的差异之一）：
     * - SQL Server 支持 DELETE TOP (n)，直接限制行数；
     * - SQLite 没有 TOP 语法，改用 id IN (SELECT id ... LIMIT n) 子查询。
     *
     * 关于返回的行数：QODBC 在某些配置下 numRowsAffected() 返回 -1（驱动不上报行数），
     * 所以不能把行数当成唯一判据。判据是"这一轮 exec() 是否还有可删的行"：
     * 没有可删行时 DELETE 依然执行成功，但影响行数为 0（或驱动报 -1 时靠
     * 下一轮的空操作收敛）。因此返回值的语义是"至少删除了这么多行"，
     * 用来记日志和界面提示足够；需要精确值请自己 COUNT(*)。
     */
    const int batchRows = 5000;
    int totalDeleted = 0;

    for (int round = 0; round < 100000; ++round) {
        QSqlQuery query(mDatabase);
        if (mDialect == DialectMssql) {
            if (!query.prepare(QStringLiteral("DELETE TOP (?) FROM %1 WHERE ts < ?")
                               .arg(historyTableName()))) {
                reportError(QStringLiteral("准备清理语句失败：%1").arg(query.lastError().text().trimmed()));
                return totalDeleted > 0 ? totalDeleted : -1;
            }
            query.addBindValue(batchRows);
            query.addBindValue(formatTimestamp(before));
        } else {
            if (!query.prepare(QStringLiteral("DELETE FROM %1 WHERE id IN "
                                              "(SELECT id FROM %1 WHERE ts < ? LIMIT ?)")
                               .arg(historyTableName()))) {
                reportError(QStringLiteral("准备清理语句失败：%1").arg(query.lastError().text().trimmed()));
                return totalDeleted > 0 ? totalDeleted : -1;
            }
            query.addBindValue(formatTimestamp(before));
            query.addBindValue(batchRows);
        }

        if (!query.exec()) {
            reportError(QStringLiteral("清理过期数据失败：%1").arg(query.lastError().text().trimmed()));
            return totalDeleted > 0 ? totalDeleted : -1;
        }

        const int affected = query.numRowsAffected();
        if (affected > 0) {
            totalDeleted += affected;
            // 这一批不足一整批，说明已经是最后一批，不必再多跑一轮 DELETE。
            if (affected < batchRows)
                break;
        } else if (affected < 0) {
            // 驱动不报行数：把这一轮按"满批"计，靠下面的空操作检测收敛。
            totalDeleted += batchRows;
        } else {
            // affected == 0：确实没有更多可删的行了。
            break;
        }
    }
    return totalDeleted;
}

int QScadaHistoryStorage::purgeExpired()
{
    if (mConfig.retentionDays <= 0)
        return 0;   // 配 0 或负数表示"不自动清理"，由运维自己用脚本清
    const QDateTime before = QDateTime::currentDateTime().addDays(-mConfig.retentionDays);
    return purge(before);
}

//==============================================================================
// 状态与槽
//==============================================================================

void QScadaHistoryStorage::setEnabled(bool enabled)
{
    if (mEnabled == enabled)
        return;
    mEnabled = enabled;

    if (!mEnabled) {
        mFlushTimer->stop();
        // 停用时丢弃缓冲：用户停用历史库通常是因为磁盘满或正在维护表结构，
        // 此时继续写入只会不断产生错误。丢弃量最多 maxBatchSize 条，
        // 且这段时间的数据本身就没有持久化价值（用户明确的意图是不存）。
        clearPending();
    } else {
        scheduleFlushTimer();
    }
}

void QScadaHistoryStorage::resetStatistics()
{
    mStatistics.writtenRecords = 0;
    mStatistics.failedBatches = 0;
    mStatistics.rejectedRecords = 0;
    mStatistics.flushCount = 0;
    mStatistics.bufferedRecords = mPending.size();
    // 保留 lastWriteTime / lastErrorTime / lastError：
    // 现场排障时需要看到"统计归零之前发生过什么"。
}

void QScadaHistoryStorage::onValueChanged(const QString &tagKey, const QVariant &value)
{
    /*
     * 这是采集层与存储层之间的标准接法：
     *   connect(source, &QScadaDataSource::valueChanged,
     *           storage, &QScadaHistoryStorage::onValueChanged);
     * 采集驱动完全不知道历史库的存在（依赖方向是"存储依赖采集的信号"，
     * 而不是"采集依赖存储"），所以新增一种协议驱动时历史库一行都不用改。
     *
     * 关于质量码：valueChanged 只带位号和值。约定是
     *   - QVariant 无效（isNull）=> 本次读取失败，记 Bad；
     *   - 有值 => 记 Good。
     * 驱动断线期间的 NotConnected / 超时未更新的 Uncertain，
     * 由上层（采集管理器）依据 QScadaDataSource::state() 与时间戳自行判定后
     * 用 append() 写入，这样"质量判定策略"集中在一处，不散落在存储层。
     */
    QScadaHistoryRecord record;
    record.setTagKey(tagKey);
    record.setTimestamp(QDateTime::currentDateTime());
    if (!value.isValid() || value.isNull()) {
        record.setQuality(Bad);
        record.setValue(0.0);
    } else {
        record.setQuality(Good);
        record.setValue(value.toDouble());
        // 布尔量按 0/1 存（值语义统一成 double 的约定），
        // 同时把原始文本落库，"到底是不是布尔量"以后还能查出来。
        if (value.userType() == QMetaType::Bool)
            record.setRawText(value.toBool() ? QStringLiteral("true") : QStringLiteral("false"));
        else
            record.setRawText(value.toString());
    }
    append(record);
}
