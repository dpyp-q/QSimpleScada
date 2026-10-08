# QScadaStorage —— 历史数据持久化模块

QSimpleScada 原版只有 `updateValue()` 被动刷值的实时显示能力，**进程一退出数据就没了**，
既画不出历史趋势，也无法在事故后复盘"当时到底是多少"。本模块补上这块能力：
把采集层的高频数据缓冲、批量、带事务地写进关系库，并提供按位号 + 时间范围的
查询、降采样与过期清理。

支持 **SQLite**（单机、零部署）和 **SQL Server**（现场多客户端、招聘要求的那一种，
走 Qt 的 `QODBC` 驱动），同一套业务代码，靠驱动名切换方言。

---

## 1. 文件清单

| 文件 | 职责 |
| --- | --- |
| `qscadadataquality.h` | 数据质量码（Good / Bad / Uncertain / NotConnected）与互转函数。纯头文件，无编译单元。 |
| `qscadahistoryrecord.h` | 一条历史记录（值语义对象），字段：位号、值、质量、时间戳、可选原始文本。纯头文件。 |
| `qscadastorageconfig.h` | 连接参数 + 写入策略（批量大小、刷新间隔、保留天数），含 SQLite / SQL Server 两个便利构造。纯头文件。 |
| `qscadahistorystorage.h` / `.cpp` | 存储实现：打开、建表、缓冲、批量事务写入、查询、降采样、清理、统计。 |
| `sql/mssql_schema.sql` | SQL Server 建表脚本：两张表 + 4 个索引 + 清理存储过程 + 分区说明，幂等。 |
| `sql/sqlite_schema.sql` | SQLite 等价脚本，含时间列存储格式的取舍说明。 |

> 本模块**不修改** `QSimpleScada.pro` 与 `com_indeema_QSimpleScada.pri`，构建集成由使用方负责
> （因为 `.cpp` 里用到了 `QtSql`，`.pro` 侧需要加 `QT += sql`）。

---

## 2. 接入方式（构建侧需要做的两件事）

```qmake
QT += sql                      # 新增：历史库依赖 QtSql

HEADERS += \
    $$PWD/QScadaStorage/qscadahistorystorage.h \
    $$PWD/QScadaStorage/qscadahistoryrecord.h \
    $$PWD/QScadaStorage/qscadastorageconfig.h \
    $$PWD/QScadaStorage/qscadadataquality.h

SOURCES += \
    $$PWD/QScadaStorage/qscadahistorystorage.cpp
```

运行时还需要 `QtSql` 的驱动插件（Qt 官方安装包自带）：

- SQLite：`plugins/sqldrivers/qsqlite.dll`
- SQL Server：`plugins/sqldrivers/qsqlodbc.dll` **并且**系统已安装
  `Microsoft ODBC Driver for SQL Server`（或 `SQL Server Native Client`）。
  这是现场"连不上数据库"最常见的原因，程序在驱动缺失时会把这句话直接写进错误信息。

---

## 3. 最小可用示例

```cpp
#include "QScadaStorage/qscadahistorystorage.h"
#include "QScadaStorage/qscadastorageconfig.h"

// 1) 建对象：建议放到独立线程，避免数据库卡顿影响界面刷新
QScadaHistoryStorage *storage = new QScadaHistoryStorage(this);

// 2) 接错信号：采集层完全不知道历史库的存在
connect(source, &QScadaDataSource::valueChanged,
        storage, &QScadaHistoryStorage::onValueChanged);

// 3) 打开（会自动建表，幂等）
QString error;
QScadaStorageConfig cfg = QScadaStorageConfig::sqliteDefault("D:/data/scada_history.db");
// SQL Server 换这一行即可，业务代码不动：
// QScadaStorageConfig cfg = QScadaStorageConfig::sqlServerDefault(
//         "192.168.1.10", "ScadaHistory", "sa", "P@ssw0rd");
if (!storage->open(cfg, &error))
    qWarning() << "历史库打开失败：" << error;

// 4) 画历史曲线：一次查询拿到包络 + 均值，maxPoints 就是屏幕能画下的点数
const QDateTime to = QDateTime::currentDateTime();
const QDateTime from = to.addDays(-1);
const QList<QScadaHistoryStorage::Row> rows =
        storage->query("1#炉.给水泵.电流", from, to, 1200, &error);
// rows[i].minValue / maxValue / avgValue -> 三条曲线；sampleCount 是桶内原始点数

// 5) 关程序前落库（close() 内部也会 flush，这里显式调用只是为了让错误可见）
storage->flush();
storage->close();
```

### 把质量码也写进去（推荐）

`onValueChanged` 只拿得到位号和值，质量码需要上层按驱动状态判断：

```cpp
const QScadaDataQuality quality = source->isConnected()
        ? QScadaDataQuality::Good
        : QScadaDataQuality::NotConnected;
storage->append(QScadaHistoryRecord(source->tag(key).key(), value, quality,
                                    QDateTime::currentDateTime()));
```

---

## 4. 公开接口要点

### QScadaDataQuality

`Good(0)` / `Bad(1)` / `Uncertain(2)` / `NotConnected(3)`，配套
`qscadaQualityName()`、`qscadaQualityFromName()`、`qscadaQualityFromInt()`、
`qscadaQualityIsTrusted()`。

**为什么必须存质量码**：通讯断线时驱动读不到数据，若往库里补一个默认的 0，
工艺人员事后看曲线会以为"温度真的掉到 0 又涨回来"，可能据此写进事故报告。
有了质量码，这段可以画成灰色虚线；统计日均值和 OEE 时也能把 Bad 区间剔除，
而不是把停机 8 小时算成"产量 0"。

### QScadaHistoryRecord

`tagKey` / `value(double)` / `quality` / `timestamp` / `rawText`（可选），
提供 `isValid()`、`timestampMs()`、`toString()`，以及 2 参数和 4 参数两个构造函数。

值统一用 `double` 的原因：布尔量存 0/1、整数量也在 double 的精度范围内（53 位尾数），
这样一条聚合 SQL 就能覆盖所有位号；需要保留原始语义（`RUN`/`STOP`、原始寄存器值）时
再填 `rawText`。

### QScadaStorageConfig

| 字段 | 说明 |
| --- | --- |
| `driver` | `"QSQLITE"` / `"QODBC"` / `"QPSQL"` … |
| `databaseName` | SQLite 是文件路径；QODBC 可以是 DSN 名，也可以是含 `DRIVER=` 的完整连接串 |
| `hostName` / `port` | 支持 `SERVER\SQLEXPRESS` 命名实例写法（此时不要填 port） |
| `userName` / `password` | 留空则对 SQL Server 使用 Windows 集成认证（`Trusted_Connection=yes`） |
| `connectionName` | 留空时按"驱动 + 主机 + 库名"生成稳定连接名，重复 `open()` 不会泄漏连接 |
| `maxBatchSize` | 单次批量写入的最大条数，默认 500（SQLite 200） |
| `flushIntervalMs` | 定时落库间隔，默认 1000ms（SQLite 500） |
| `retentionDays` | 保留天数，默认 90（SQLite 30）；`<= 0` 表示不自动清理 |
| `odbcOptions` | 追加到 ODBC 连接串尾部的选项，默认 `TrustServerCertificate=yes` |

便利构造：`sqliteDefault(filePath)`、`sqlServerDefault(host, db, user, pwd, port = 1433)`；
辅助方法：`isValid()`、`effectiveConnectionName()`、`connectionString()`、
`description()`（**不含密码**，可安全进日志）。

### QScadaHistoryStorage

| 方法 | 说明 |
| --- | --- |
| `open(config, errorString)` / `close()` / `isOpen()` | 打开即自动建表（幂等）；`close()` 会先 `flush()` |
| `createSchema(errorString)` / `applySchemaFile(path, ...)` / `executeSql(sql, ...)` | 按驱动执行内嵌 DDL / 外部 `.sql` 脚本 / 任意 SQL |
| `append(record)` / `appendBatch(list)` | 只入内存缓冲；缓冲达到 `maxBatchSize` 时立即触发一次 flush |
| `flush()` / `pendingCount()` / `clearPending()` | 批量 + 事务落库；返回本批成功条数 |
| `query(tagKey, from, to, maxPoints, errorString)` | 返回包络行（含降采样） |
| `queryRaw(tagKey, from, to, errorString)` | 原始点，用于导出 CSV / 自行计算 |
| `countRecords(...)` / `distinctTagKeys(...)` | 数据量预估 / 位号列表 |
| `purge(before)` / `purgeExpired()` / `retentionDays()` | 分批删除过期数据，返回删除行数 |
| `setEnabled(bool)` / `isEnabled()` / `statistics()` / `resetStatistics()` | 开关与运行统计 |
| `onValueChanged(tagKey, value)`（槽） | 直接接 `QScadaDataSource::valueChanged` |

信号：`errorOccurred(QString)`、`flushCompleted(int writtenCount)`、`openedChanged(bool)`。
**库代码不弹 `QMessageBox`**：上位机是无人值守运行，弹窗没人点会卡住界面；
所有错误统一走 `errorString` 出参与 `errorOccurred` 信号。

---

## 5. 两个核心实现思路

### 5.1 批量写入 + 事务（`flush()`）

现场 3000 个位号按 1 秒采集就是 3000 条/秒。逐条 `INSERT` 意味着每秒 3000 次独立事务：

- **日志暴涨**：每条记录都留下 `begin / insert / commit` 三段事务日志，
  1000 点的历史库一天能写出几十 GB 日志，日志盘先满；
- **磁盘 IO 打满**：每次提交都要 `fsync`，机械盘每秒几百次就是上限，SSD 也扛不住；
  数据库一慢，写入线程排队，内存缓冲涨到 OOM；
- **锁竞争**：高频小事务不断申请行锁/页锁，和趋势查询互相阻塞，
  典型现象是"界面一开历史曲线，采集数据就开始丢"。

实现上分三步：

1. `append()` 只把记录放进内存 `QList`，达到 `maxBatchSize` 或定时器到期才写库；
2. `flush()` 把整个缓冲 `swap` 出来（写库期间新数据进新缓冲，采集侧不阻塞），
   按时间戳 `std::sort` 排序（同一批时间接近 → 索引页插入位置连续 → 页分裂与随机 IO 更少）；
3. 一个事务内执行多行 `VALUES` 的 `INSERT`，每行参数用位置绑定 `?`
   （QODBC 对命名占位符的翻译各版本不一，位置绑定跨驱动一致）。
   单条语句按 400 行兜底切分，避开 SQL Server 单语句 2100 个参数的硬上限。

事务数与 `fsync` 次数按批大小成比例下降（500 条/批时是原来的 1/500）。
**失败时整批回滚并且不自动重试**：一批就是一条语句，"要么全进、要么全不进"，
不会出现需要事后修补的半批数据；不重试是因为连接断/磁盘满的场景下重试只会反复失败，
还会让缓冲持续膨胀（丢弃的条数由 `statistics()` 记录，错误经 `errorOccurred` 上报，
上层可以决定转存本地文件兜底）。

### 5.2 包络降采样（`query(..., maxPoints)`）

看一个月的曲线，1 秒 1 条就是 259 万个点。直接把这些点喂给绘图控件：
查询要传几十万行、占几十 MB 内存，每个点都要走一次坐标变换和 painter 路径，
鼠标缩放、窗口拉伸会彻底失去响应 —— 而屏幕横向只有一两千像素，多出来的点没有信息量。

降采样采用**等时间间隔分桶 + 每桶取 MIN/MAX/AVG 组成包络**：

- 桶宽 = 区间长度 / `maxPoints`，桶数不超过 `maxPoints`，总点数有硬上界 `3 × maxPoints`；
- 每个桶输出 `minValue` / `maxValue` / `avgValue` 三条线，时间戳取桶起点，三条线严格对齐；
- 空桶不产生点（造一个 0 值点会画出一条假的"掉到零"）；
- 桶内质量码取**最差**的一条（保守原则），只要桶里有一个 Bad 点，这个桶就不能当成完全可信。

**为什么不是纯平均、也不是抽样**：一个持续 5 秒的超压尖峰，在 1 小时一桶的均值曲线里
几乎看不见，而工艺上恰恰是这种尖峰导致安全阀起跳 —— 用平均值做监控曲线是典型的
"看起来平滑、出事时查不出原因"；抽样更糟，可能刚好跳过尖峰。
MIN/MAX 包络保证尖峰一定被画出来，AVG 给出趋势中线，既不失真也不卡。

---

## 6. SQL Server 与 SQLite 的差异是怎么被屏蔽掉的

代码里只有一个分支点：`dialectForDriver()`（`QODBC → DialectMssql`、`QSQLITE → DialectSqlite`），
其余全部共用。差异集中在四处，每处都有注释说明：

| 差异点 | SQL Server | SQLite |
| --- | --- | --- |
| 建表 | `IF OBJECT_ID(...) IS NULL` + `GO` 分批（`CREATE PROCEDURE` 必须是批中第一条语句） | `CREATE TABLE / INDEX IF NOT EXISTS`，按 `;` 切分 |
| 自增主键 | `BIGINT IDENTITY(1,1)` | `INTEGER PRIMARY KEY AUTOINCREMENT` |
| 分批删除 | `DELETE TOP (n) FROM ...` | `DELETE FROM ... WHERE id IN (SELECT id ... LIMIT n)` |
| 时间列类型 | `DATETIME2(3)`（毫秒） | `TEXT` 存 `yyyy-MM-dd HH:mm:ss.zzz` |

统一的手段：

1. **时间统一由 C++ 侧格式化成固定字符串**再绑定，不把 `QDateTime` 交给驱动序列化。
   该格式对 `SET LANGUAGE / SET DATEFORMAT` 免疫，SQL Server 能直接解析成 `DATETIME2`，
   且在 SQLite 里满足"字符串序 == 时间序"，范围查询能走索引。
   读回来时 `parseTimestamp()` 还兼容 ISO 8601 和整型 Unix 时间戳。
2. **占位符统一用 `?` 位置绑定**，不用命名占位符。
3. **不依赖任何驱动特有 API**：从不调用 `lastInsertId()`（Qt 6 各驱动行为不一致），
   主键完全交给数据库；不依赖 `numRowsAffected()` 返回精确行数（QODBC 可能返回 -1，
   清理循环因此按"是否还有可删的行"收敛）。
4. **同一套表名与列名**（`scada_history(tag_key, value, quality, ts, raw_text)`），
   索引名也一致，所以运维在两套环境里的排障命令是一样的。

SQLite 侧还有两个针对性的处理：连接选项 `QSQLITE_BUSY_TIMEOUT=5000`
（默认 journal 模式下"边写边查"会直接报 `database is locked`），
以及文档里建议开启 `PRAGMA journal_mode=WAL`。

---

## 7. SQL 脚本说明

`sql/mssql_schema.sql`：

- `scada_history`：`id BIGINT IDENTITY` 主键、`tag_key NVARCHAR(128)`、`value FLOAT`、
  `quality TINYINT`、`ts DATETIME2(3)`、`raw_text NVARCHAR(256)`；
- 复合索引 `IX_scada_history_tag_ts (tag_key, ts)`：**列顺序必须是 tag_key 在前** ——
  查询永远是"先按位号等值过滤、再按时间范围扫"，反过来会先扫完整个时间窗内
  所有位号的数据再过滤，读放大可能差三个数量级；`ts` 在第二位还能直接支撑 `ORDER BY ts`；
- 单列索引 `IX_scada_history_ts (ts)`：给不带位号的清理语句和跨位号时段统计用；
- `scada_alarm_history`：`event_id / tag_key / level / message / trigger_value / limit_value /
  raised_at / cleared_at / acked_at / acked_by`，`cleared_at IS NULL` 表示至今未恢复，
  `limit_value` 存触发时的限值快照（限值会被人改，事后只看当前限值会得出错误结论）；
- 清理存储过程 `sp_scada_purge_history @before DATETIME2(3), @batch_rows INT = 5000`，
  循环 `DELETE TOP (n)` 分批删，避免一次删几千万行撑爆日志并长时间锁表；
- 附按时间分区的评估说明（分区真正的收益是把最旧分区 `SWITCH` 出去再 `DROP`，
  秒级完成且几乎不写日志，代价是需要停机窗口重建聚集索引）。

两个脚本都是**幂等**的，可以重复执行；全文用块注释书写，且注释里不出现行尾分号 ——
因为 `applySchemaFile()` 会先剥离块注释、再按 `;` 与 `GO` 切分后逐条执行，
注释里的分号如果被当成语句结束符，会把一条完整 DDL 拦腰切断。

---

## 8. 已知边界与后续可做

- **线程模型**：本对象与它的 `QSqlDatabase` 只能在创建它的线程使用，
  跨线程调用会被明确拒绝并上报错误（Qt 的硬约束）。推荐做成独立的历史库线程，
  采集侧通过信号投递。
- **掉电窗口**：最多丢失 `flushIntervalMs` 时间内的数据（默认 1 秒）。
  这是有意接受的取舍 —— 换取采集实时性不受数据库抖动影响。
  需要更小的窗口就调小间隔、调大批量，但数据库压力会上升。
- **不自动建库**：只建表和索引。建库需要实例级权限，而现场给上位机的账号
  通常只有某个库的读写权限，脚本里给出了手工建库语句。
- 可扩展方向：报警历史表的写入接口、`scada_history` 的按月分区切换、
  写入失败落本地文件并在恢复后补传、CSV 导出。
