/* =============================================================================
 * QSimpleScada 历史数据持久化模块 —— SQL Server 建表脚本
 * -----------------------------------------------------------------------------
 * 适用版本：SQL Server 2012 及以上（用到 DATETIME2、DELETE TOP(n)、
 *           sys.indexes 目录视图；SQL Server 2008 需把 DATETIME2 改成 DATETIME）。
 *
 * 使用方式（二选一，两种方式建出来的表结构完全一致）：
 *   1. 由 DBA 手工执行（推荐，DDL 纳入变更管理）：
 *        sqlcmd -S 服务器名 -d 目标库名 -U 用户 -P 密码 -i mssql_schema.sql
 *      或在 SSMS 里先选中目标数据库，再打开本文件整体执行。
 *      注意：GO 是 SSMS/sqlcmd 的批分隔符（不是 T-SQL 语法），必须保留。
 *   2. 由程序自动执行：QScadaHistoryStorage::open() 会执行与本文档等价的
 *      内嵌 DDL（同样幂等），也可以调用 applySchemaFile("sql/mssql_schema.sql")。
 *
 * 幂等性：本脚本可以重复执行任意次，不会重建已有表、不会丢数据、不会报错中断。
 *   所有 DDL 都用 IF OBJECT_ID(...) IS NULL 或 IF NOT EXISTS 包住。
 *   这是现场升级的硬要求——上位机每次启动都会调一次 createSchema()，
 *   脚本不幂等的话第二次启动就会因为"表已存在"而失败。
 *
 * 关于"当前数据库"：脚本刻意不写 USE 语句。原因是程序自动执行本脚本时，
 *   连接串里已经指定了 Database；若脚本再写死一个 USE，一旦现场库名不同
 *   就会切换到不存在的库而整段失败。手工执行时请自行在 SSMS 里选中目标库。
 *
 * 建库语句（按需手工执行）：程序不会自动建库，因为建库需要实例级权限，
 *   而现场给上位机的账号通常只有某个库的读写权限。
 *     IF DB_ID(N'ScadaHistory') IS NULL CREATE DATABASE ScadaHistory;
 *   生产环境建议同时限制日志增长，避免日志文件无限膨胀：
 *     ALTER DATABASE ScadaHistory SET RECOVERY SIMPLE;
 *     ALTER DATABASE ScadaHistory MODIFY FILE (NAME = N'ScadaHistory_log', MAXSIZE = 20GB);
 *
 * 与 Qt 侧代码的对应关系：表名与列名被 QScadaHistoryStorage 直接引用
 *   （tag_key / value / quality / ts / raw_text），改列名必须同步改代码。
 * ========================================================================== */

/* =============================================================================
 * 1. 历史数据表
 * -----------------------------------------------------------------------------
 * 设计说明：
 *  - id BIGINT IDENTITY：自增主键，同时充当聚集索引。用 BIGINT 而不是 INT，
 *    因为 1000 点、1 秒 1 条的系统一天就 8640 万行，INT 上限 21 亿只够 24 天，
 *    一旦溢出只能重建表（生产事故级操作）。BIGINT 一次到位。
 *  - tag_key NVARCHAR(128)：用 NVARCHAR 而不是 VARCHAR，是为了支持中文位号
 *    （现场点表里出现"1#炉给水泵电流"这种位号很常见）；128 字符足够表达
 *    "装置.单元.设备.测点"这类层级命名。
 *  - value FLOAT：所有量统一存 double。布尔量存 0/1，32 位整数也能无损装下
 *    （见 qscadahistoryrecord.h 的说明），这样一条聚合 SQL 就能覆盖所有位号，
 *    不必为每种数据类型写一套降采样查询。
 *  - quality TINYINT：质量码（0=Good 1=Bad 2=Uncertain 3=NotConnected）。
 *    只占 1 字节，却能让"通讯中断时的 0"和"真实测量的 0"在曲线上区分开，
 *    避免工艺人员误判（详见 qscadadataquality.h）。
 *  - ts DATETIME2(3)：毫秒精度。不用 DATETIME（精度只有 3.33ms 且会四舍五入），
 *    也不用 DATETIME2(7)（100ns 精度对工控采集没有意义，反而多占空间）。
 *    统一按本地时间写入（字符串格式 yyyy-MM-dd HH:mm:ss.zzz），
 *    全厂只允许一种时间口径，否则跨时区或夏令时会让曲线错位。
 *  - raw_text NVARCHAR(256) 可选：保存驱动侧原始文本（"RUN"/"STOP"、
 *    未换算的原始寄存器值），用于事后追溯"当时到底读到了什么"。
 * ========================================================================== */
IF OBJECT_ID(N'dbo.scada_history', N'U') IS NULL
BEGIN
    CREATE TABLE dbo.scada_history
    (
        id       BIGINT        IDENTITY(1,1) NOT NULL,
        tag_key  NVARCHAR(128) NOT NULL,
        value    FLOAT         NOT NULL,
        quality  TINYINT       NOT NULL CONSTRAINT DF_scada_history_quality DEFAULT (0),
        ts       DATETIME2(3)  NOT NULL,
        raw_text NVARCHAR(256) NULL,
        CONSTRAINT PK_scada_history PRIMARY KEY CLUSTERED (id ASC)
    );

    PRINT N'已创建表 dbo.scada_history';
END
ELSE
BEGIN
    PRINT N'表 dbo.scada_history 已存在，跳过创建（保留原有数据）';
END
GO

/* =============================================================================
 * 2. 核心查询索引
 * -----------------------------------------------------------------------------
 * 为什么索引列顺序必须是 (tag_key, ts) 而不是 (ts, tag_key)：
 *
 *   历史趋势查询永远是"先定位一个位号、再取一段时间"，即
 *       WHERE tag_key = ? AND ts BETWEEN ? AND ?
 *   复合索引遵循最左前缀原则：
 *     - (tag_key, ts)：用 tag_key 等值定位到该位号的连续区间，
 *       再在区间内按 ts 顺序做范围扫描，读多少行就是多少行；
 *     - (ts, tag_key)：先按时间范围把**所有位号**在该时段的数据全扫一遍，
 *       再逐行过滤 tag_key。查一个位号却要读全厂 1000 个位号的数据，
 *       读放大可能达到三个数量级——这是历史曲线卡顿最典型的原因。
 *
 *   附带好处：ORDER BY ts 能直接利用索引顺序，省掉一次排序（Sort 算子）。
 *   在几十万行的结果集上，这一步的代价往往比扫描本身还大。
 *
 * 关于 INCLUDE 覆盖列：本脚本没有加 INCLUDE (value, quality)，
 *   因为那样非聚集索引要额外保存这些列的副本，索引体积接近翻倍，
 *   批量写入的维护成本随之上升。是否值得取决于"查询量 vs 写入量"：
 *   以只读报表为主、写入压力小的系统，可以解开下面一行注释改成覆盖索引：
 *     CREATE NONCLUSTERED INDEX IX_scada_history_tag_ts
 *         ON dbo.scada_history (tag_key, ts) INCLUDE (value, quality);
 * ========================================================================== */
IF NOT EXISTS (SELECT 1 FROM sys.indexes
               WHERE name = N'IX_scada_history_tag_ts'
                 AND object_id = OBJECT_ID(N'dbo.scada_history'))
BEGIN
    CREATE NONCLUSTERED INDEX IX_scada_history_tag_ts
        ON dbo.scada_history (tag_key ASC, ts ASC);

    PRINT N'已创建索引 IX_scada_history_tag_ts (tag_key, ts)';
END
GO

/* 单独的 ts 索引：给"清理过期数据"和"跨位号的时段统计"用。
 * 清理语句是 WHERE ts < @before（不带 tag_key），
 * 上面那个复合索引因为最左列是 tag_key，对纯时间条件用不上。 */
IF NOT EXISTS (SELECT 1 FROM sys.indexes
               WHERE name = N'IX_scada_history_ts'
                 AND object_id = OBJECT_ID(N'dbo.scada_history'))
BEGIN
    CREATE NONCLUSTERED INDEX IX_scada_history_ts
        ON dbo.scada_history (ts ASC);

    PRINT N'已创建索引 IX_scada_history_ts (ts)';
END
GO

/* =============================================================================
 * 3. 报警历史表
 * -----------------------------------------------------------------------------
 * 报警是 SCADA 里唯一"不能只活在内存里"的数据：
 *   - 事故复盘要按时间倒查"当时报了什么警、值是多少、限值是多少"；
 *   - 合规审计（电力、石化）要求报警记录保存 1~3 年，且必须有确认人。
 * 所以报警事件必须在产生的瞬间落库，而不是等用户打开报警页面时才查。
 *
 * 字段与报警引擎的事件对象一一对应：
 *   raised_at  报警产生时间（必填，索引列）。
 *   cleared_at 报警恢复时间。NULL 表示至今未恢复——这本身就是重要信息，
 *              "当前未恢复的报警"就是 WHERE cleared_at IS NULL。
 *   acked_at / acked_by 确认时间与确认人（操作员账号）。
 *              为什么不把它们拼进 message 字符串：审计要按人统计，
 *              而且"谁确认的"是责任划分的依据，必须结构化成独立的列。
 *   trigger_value 触发时的实际值；limit_value 触发时的限值快照。
 *              两个值都要存：限值在运行中可能被人改过，事后只看当前限值
 *              会得出"根本没超限"的错误结论。
 * ========================================================================== */
IF OBJECT_ID(N'dbo.scada_alarm_history', N'U') IS NULL
BEGIN
    CREATE TABLE dbo.scada_alarm_history
    (
        event_id      BIGINT        IDENTITY(1,1) NOT NULL,
        tag_key       NVARCHAR(128) NOT NULL,
        level         TINYINT       NOT NULL,
        message       NVARCHAR(512) NULL,
        trigger_value FLOAT         NULL,
        limit_value   FLOAT         NULL,
        raised_at     DATETIME2(3)  NOT NULL,
        cleared_at    DATETIME2(3)  NULL,
        acked_at      DATETIME2(3)  NULL,
        acked_by      NVARCHAR(64)  NULL,
        CONSTRAINT PK_scada_alarm_history PRIMARY KEY CLUSTERED (event_id ASC)
    );

    PRINT N'已创建表 dbo.scada_alarm_history';
END
ELSE
BEGIN
    PRINT N'表 dbo.scada_alarm_history 已存在，跳过创建（保留原有数据）';
END
GO

/* level 取值约定（与报警引擎枚举对应）：0=提示 1=一般 2=重要 3=紧急。
 * 用 TINYINT 而不是字符串，是为了让"按级别筛选/统计"能走索引比较。 */

/* 报警查询的两种典型场景，各配一个索引：
 * 1) "某个位号在某段时间报过什么警"          -> (tag_key, raised_at)
 * 2) "全厂最近一小时的报警列表"（按时间倒序翻页）-> (raised_at) */
IF NOT EXISTS (SELECT 1 FROM sys.indexes
               WHERE name = N'IX_scada_alarm_tag_raised'
                 AND object_id = OBJECT_ID(N'dbo.scada_alarm_history'))
BEGIN
    CREATE NONCLUSTERED INDEX IX_scada_alarm_tag_raised
        ON dbo.scada_alarm_history (tag_key ASC, raised_at ASC);

    PRINT N'已创建索引 IX_scada_alarm_tag_raised (tag_key, raised_at)';
END
GO

IF NOT EXISTS (SELECT 1 FROM sys.indexes
               WHERE name = N'IX_scada_alarm_raised'
                 AND object_id = OBJECT_ID(N'dbo.scada_alarm_history'))
BEGIN
    CREATE NONCLUSTERED INDEX IX_scada_alarm_raised
        ON dbo.scada_alarm_history (raised_at ASC);

    PRINT N'已创建索引 IX_scada_alarm_raised (raised_at)';
END
GO

/* =============================================================================
 * 4. 过期数据清理存储过程
 * -----------------------------------------------------------------------------
 * 为什么必须能自动清理：单点 1 秒 1 条，一年 3150 万条；1000 点系统一年
 *   300 亿条、几百 GB 到 TB 级。工控机的数据盘写满会连带影响整个 SQL Server
 *   实例（连 tempdb 都写不进去），导致上位机所有功能一起失效——所以保留期
 *   是安全阀，不是可选功能。
 *
 * 为什么在存储过程里分批删，而不是一句 DELETE FROM ... WHERE ts < @before：
 *   一次删几千万行会产生同量级的事务日志（日志文件可能瞬间涨几十 GB），
 *   并长时间持有锁，把实时写入完全堵死（现场表现是"凌晨清理时历史曲线出现空洞"）。
 *   分批删把大事务切成很多小事务，写入线程能在批次之间插进去。
 *
 * 调用方式：
 *     EXEC dbo.sp_scada_purge_history @before = DATEADD(DAY, -90, GETDATE());
 *   程序侧对应 QScadaHistoryStorage::purge() / purgeExpired()，逻辑一致
 *   （程序内嵌的分批 DELETE 不依赖本过程，缺它也能正常工作）。
 *
 * 运维建议：用 SQL Server 代理作业每天凌晨执行一次，例如
 *     EXEC dbo.sp_scada_purge_history
 *          @before = DATEADD(DAY, -90, GETDATE()), @batch_rows = 20000;
 *   @before 是保留边界：早于它的数据会被删除。
 * ========================================================================== */
IF OBJECT_ID(N'dbo.sp_scada_purge_history', N'P') IS NULL
BEGIN
    /* 用 EXEC(N'...') 包一层：CREATE PROCEDURE 必须是批中的第一条语句，
     * 而它前面已经有 IF 判断，直接写 CREATE PROCEDURE 会报语法错误。
     * 这是"幂等建存储过程"的标准写法（内层字符串里的单引号写成两个转义）。 */
    EXEC(N'
CREATE PROCEDURE dbo.sp_scada_purge_history
    @before     DATETIME2(3),
    @batch_rows INT = 5000
AS
BEGIN
    SET NOCOUNT ON;

    DECLARE @total   INT = 0;
    DECLARE @deleted INT = 1;

    WHILE @deleted > 0
    BEGIN
        DELETE TOP (@batch_rows) FROM dbo.scada_history WHERE ts < @before;
        SET @deleted = @@ROWCOUNT;
        SET @total   = @total + @deleted;
    END

    SELECT @total AS deleted_rows;
END
');
    PRINT N'已创建存储过程 dbo.sp_scada_purge_history';
END
GO

/* =============================================================================
 * 5. 可选：按时间分区（数据量上亿之后再考虑）
 * -----------------------------------------------------------------------------
 * 什么情况下才需要分区：单表超过 1 亿行，或"删除一年前数据"即使分批删除
 *   仍然明显影响实时写入。分区本身会带来不小的运维复杂度（分区函数、
 *   分区方案、文件组规划、分区切换流程），**不要一上来就分区**。
 *
 * 分区的关键收益是"分区切换（SWITCH）"而不是查询变快：
 *   把最旧的分区直接切出去（元数据操作，秒级、几乎不写日志），再 DROP 掉，
 *   比 DELETE 几千万行快几个数量级——这是大型历史库做数据保留的标准做法。
 *
 * 分区的前提条件：
 *   1) 分区列必须是聚集索引键的一部分。当前聚集索引是 (id)，
 *      所以要分区就得改成 (id, ts) 或直接把聚集索引建在 ts 上。
 *      这会改变插入模式（不再总是"追加到末尾"），必须重新评估写入性能，
 *      属于架构级改动，需要压测后再动手。
 *   2) 每个分区通常对应一个文件组，一般按"月"或"年"做 RANGE RIGHT 分区。
 *
 * 参考步骤（仅供评估，未启用）：

   步骤 1：建分区函数，按 ts 的月份切分。
           CREATE PARTITION FUNCTION PF_scada_history_month (DATETIME2(3))
               AS RANGE RIGHT FOR VALUES ('2024-01-01', '2024-02-01');
           边界需要定期补充（一般由代理作业每月补下个月的边界）。

   步骤 2：建分区方案，初期可以都放在 PRIMARY 文件组。
           CREATE PARTITION SCHEME PS_scada_history_month
               AS PARTITION PF_scada_history_month ALL TO ([PRIMARY]);

   步骤 3：把聚集索引重建到分区方案上（这一步会重建整张表，需要停机窗口）。
           ALTER TABLE dbo.scada_history DROP CONSTRAINT PK_scada_history;
           CREATE CLUSTERED INDEX CIX_scada_history ON dbo.scada_history (ts ASC, id ASC)
               WITH (DROP_EXISTING = ON) ON PS_scada_history_month(ts);

   步骤 4：删除整月数据改为 SWITCH + DROP（秒级完成，几乎不产生日志）。
           ALTER TABLE dbo.scada_history SWITCH PARTITION 3 TO dbo.scada_history_202403;
           DROP TABLE dbo.scada_history_202403;
 * ========================================================================== */

PRINT N'scada_history 建表脚本执行完毕。';
GO
