/* =============================================================================
 * QSimpleScada 历史数据持久化模块 —— SQLite 建表脚本
 * -----------------------------------------------------------------------------
 * 适用场景：单机上位机、点数不多（几百点以内）、不想额外装数据库服务的场合。
 *   SQLite 的最大优势是零部署：QSQLITE 驱动随 Qt 一起发布，
 *   数据库就是一个文件，拷走文件就是备份。
 *
 * 不适用场景（这几点面试常被问，务必清楚）：
 *   - 多台上位机共用一个历史库：SQLite 以文件锁实现并发控制，
 *     多个进程写同一个文件会频繁 "database is locked"，必须换 SQL Server；
 *   - 高频写入：单点 1 秒 1 条、1000 点的系统（约 3000 条/秒）已经接近 SQLite 上限，
 *     需要开启 WAL 并调大 busy_timeout 才能勉强跟上；
 *   - 数据量上亿行后，聚合查询（降采样）会明显变慢，需要换关系库 + 分区。
 *
 * 使用方式（二选一，建出来的表结构完全一致）：
 *   1. 手工执行（sqlite3 命令行）：
 *        sqlite3 D:/data/scada_history.db ".read sqlite_schema.sql"
 *   2. 由程序自动执行：QScadaHistoryStorage::open() 会执行与本文档等价的
 *      内嵌 DDL（同样幂等），也可以调用 applySchemaFile("sql/sqlite_schema.sql")。
 *
 * 幂等性：全部用 CREATE TABLE / CREATE INDEX IF NOT EXISTS，
 *   重复执行不会重建已有表、不会丢数据、不会报错。
 *
 * 建库：SQLite 不需要显式建库，连接时指定文件路径即可（文件不存在会自动创建）。
 * ========================================================================== */

/* 建议的连接级设置（由程序在 open() 时执行，这里列出以便手工建库时对齐）：
 *
 *   PRAGMA journal_mode = WAL;
 *       -- 默认的 rollback journal 模式在"边写边查"（历史写入 + 趋势查询同时进行）
 *       -- 时会立刻报 database is locked。WAL 模式允许读写并发，
 *       -- 是单机把 SQLite 当历史库用的必要前提。
 *   PRAGMA synchronous = NORMAL;
 *       -- WAL 下的推荐值。FULL 会把每次事务提交都 fsync 一次，写入速度掉一个数量级；
 *       -- NORMAL 在掉电时最多丢失最后一个事务（上位机场景可以接受，
 *       -- 因为最多丢 1 秒数据，而且历史数据不是安全联锁的一部分）。
 *   PRAGMA busy_timeout = 5000;
 *       -- 遇到锁时不立即失败，而是重试 5 秒（程序里通过连接选项
 *       -- QSQLITE_BUSY_TIMEOUT=5000 设置）。
 *   PRAGMA auto_vacuum = NONE;
 *       -- 不做自动 vacuum：自动整理会造成大量随机 IO，清理历史数据请用
 *       -- 程序里的 purge()，确有需要时手工执行一次 VACUUM。
 */

/* =============================================================================
 * 1. 历史数据表
 * -----------------------------------------------------------------------------
 * 与 SQL Server 版本的字段一一对应：
 *   id      INTEGER PRIMARY KEY AUTOINCREMENT  <->  BIGINT IDENTITY
 *   tag_key TEXT NOT NULL                       <->  NVARCHAR(128) NOT NULL
 *   value   REAL NOT NULL                       <->  FLOAT NOT NULL
 *   quality INTEGER NOT NULL                    <->  TINYINT NOT NULL
 *   ts      TEXT NOT NULL                       <->  DATETIME2(3) NOT NULL
 *   raw_text TEXT NULL                          <->  NVARCHAR(256) NULL
 *
 * 关于时间列的存储格式（本脚本最重要的一个设计决定）：
 *   这里用 TEXT 存 "yyyy-MM-dd HH:mm:ss.zzz"（固定 23 字符，ISO8601 的变体）。
 *   理由：
 *     - 可读：运维直接用 sqlite3 查库、或用 Excel 打开导出的 CSV 时一眼能看懂；
 *     - 可排序：固定宽度 + 零填充，字符串的字典序等于时间序，
 *       WHERE ts >= '2024-01-01 00:00:00.000' 这样的范围条件能走索引，
 *       不需要任何转换函数（在列上套函数会导致索引失效，这是常见的性能坑）；
 *     - 与 SQL Server 版本共用同一套 SQL 语句和同一套 C++ 代码路径，
 *       两种库的差异只体现在建表脚本上，业务代码零分支。
 *   代价是每行占 23 字节，比 INTEGER 存 Unix 毫秒（8 字节）多 15 字节。
 *   如果磁盘非常紧张、且不需要人工查库，可以改用整型时间戳：
 *     ts INTEGER NOT NULL   -- Unix epoch 毫秒
 *   此时本模块的代码无需改动：parseTimestamp() 已经兼容整型时间戳
 *   （10 位按秒、13 位按毫秒自动判断），写入时改成绑定 record.timestampMs() 即可。
 *   这里默认选 TEXT，是因为"现场能看懂"在工控项目里比省 15 字节重要得多。
 * ========================================================================== */
CREATE TABLE IF NOT EXISTS scada_history
(
    id       INTEGER PRIMARY KEY AUTOINCREMENT,
    tag_key  TEXT    NOT NULL,
    value    REAL    NOT NULL,
    quality  INTEGER NOT NULL DEFAULT 0,
    ts       TEXT    NOT NULL,
    raw_text TEXT    NULL
);

/* =============================================================================
 * 2. 核心查询索引
 * -----------------------------------------------------------------------------
 * 索引列顺序必须是 (tag_key, ts)，不能反过来——原因与 SQL Server 版本相同：
 *   趋势查询永远是 WHERE tag_key = ? AND ts BETWEEN ? AND ?，
 *   最左列放 tag_key 才能先定位到单个位号的连续区间再按时间范围扫描；
 *   反过来 (ts, tag_key) 会先扫整个时间窗内所有位号的数据再过滤，
 *   读放大可能达到几个数量级。
 *
 * 与 SQL Server 的差异：SQLite 的普通索引默认会带上 rowid 作为"行定位符"，
 *   也就是天然具备覆盖"回表"的能力，没有 SQL Server 那种
 *   "非聚集索引 + INCLUDE 列"的写法；也不需要指定文件组/填充因子。
 * ========================================================================== */
CREATE INDEX IF NOT EXISTS IX_scada_history_tag_ts
    ON scada_history (tag_key, ts);

/* 单独的 ts 索引：给"清理过期数据"（WHERE ts < ?，不带 tag_key）
 * 和"跨位号的时段统计"用。 */
CREATE INDEX IF NOT EXISTS IX_scada_history_ts
    ON scada_history (ts);

/* =============================================================================
 * 3. 报警历史表
 * -----------------------------------------------------------------------------
 * 字段含义与 SQL Server 版本完全一致，详见 mssql_schema.sql 里的说明。
 * 差别只在类型映射：
 *   TEXT    <-> NVARCHAR(n)   （SQLite 不限制长度，长度约束由应用层保证）
 *   INTEGER <-> TINYINT / BIGINT
 *   REAL    <-> FLOAT
 *
 * 时间列同样用 TEXT 存固定格式字符串，NULL 表示"未恢复 / 未确认"：
 *   cleared_at IS NULL 就是"当前仍未恢复的报警"，
 *   这是报警页面最常用的一个查询，所以要为 raised_at 建索引。
 * ========================================================================== */
CREATE TABLE IF NOT EXISTS scada_alarm_history
(
    event_id      INTEGER PRIMARY KEY AUTOINCREMENT,
    tag_key       TEXT    NOT NULL,
    level         INTEGER NOT NULL,
    message       TEXT    NULL,
    trigger_value REAL    NULL,
    limit_value   REAL    NULL,
    raised_at     TEXT    NOT NULL,
    cleared_at    TEXT    NULL,
    acked_at      TEXT    NULL,
    acked_by      TEXT    NULL
);

/* level 取值约定（与报警引擎枚举对应）：0=提示 1=一般 2=重要 3=紧急。 */

CREATE INDEX IF NOT EXISTS IX_scada_alarm_tag_raised
    ON scada_alarm_history (tag_key, raised_at);

CREATE INDEX IF NOT EXISTS IX_scada_alarm_raised
    ON scada_alarm_history (raised_at);

/* =============================================================================
 * 4. 过期数据清理
 * -----------------------------------------------------------------------------
 * SQLite 没有存储过程，清理逻辑放在程序里：
 *   QScadaHistoryStorage::purge(before) 会循环执行
 *       DELETE FROM scada_history WHERE id IN
 *           (SELECT id FROM scada_history WHERE ts < ? LIMIT 5000);
 *   分批删的理由与 SQL Server 相同：一次删几百万行会让事务日志（WAL 文件）
 *   暴涨，并长时间持锁把实时写入堵死。
 *
 * 手工清理（比如迁移前清历史数据）可以直接在 sqlite3 里执行：
 *   DELETE FROM scada_history WHERE ts < '2024-01-01 00:00:00.000';
 *   DELETE FROM scada_alarm_history WHERE raised_at < '2024-01-01 00:00:00.000';
 *   PRAGMA wal_checkpoint(TRUNCATE);   -- 把 WAL 文件截断，真正把空间还给操作系统
 *
 * 为什么删完数据文件没变小：SQLite 删除行只是把页标记为空闲，文件不会自动缩小。
 *   需要真正回收空间时执行一次 VACUUM（会重建整个文件，
 *   期间需要相当于数据库大小的临时空间，且会阻塞写入，请在停机窗口做）。
 * ========================================================================== */

/* =============================================================================
 * 5. 可选：周期性做一次统计信息更新
 * -----------------------------------------------------------------------------
 * SQLite 的查询计划器依赖 sqlite_stat1 里的统计信息。历史表数据量变化很大时
 *   （例如刚清理过 90 天数据），执行一次 ANALYZE 能让计划器重新选择合适的索引：
 *       ANALYZE scada_history;
 *   程序没有自动调用它：ANALYZE 会全表扫描一遍，在写入高峰期执行反而有害，
 *   建议由运维在低峰期手工执行或加入维护脚本。
 * ========================================================================== */
