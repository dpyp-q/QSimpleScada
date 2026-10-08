# QScadaMes —— MES 对接模块

> QSimpleScada 的工业上位机扩展模块：把现场采集到的心跳、产量、设备状态、
> 报警、OEE 与关键位号，通过 REST/JSON 稳定地报送到 MES（制造执行系统）。
>
> 目标环境：Qt 5.12+ 与 Qt 6 双兼容，Windows / Linux 工控机。

---

## 1. 为什么需要这个模块

上位机的第一职责是"让产线看得见、操作得了"。MES 上报再重要，也只是附属功能——
它**绝不能**因为网络断了、服务端卡了，就让界面转圈或者把数据悄悄丢掉。
所以本模块的全部设计都围绕三条硬约束：

| 约束 | 落地手段 |
| --- | --- |
| 绝不阻塞界面线程 | 全部走 `QNetworkAccessManager` 异步接口，发送节奏由 `QTimer` 驱动；代码中没有任何 `waitFor*()` 同步调用 |
| 绝不静默丢数据 | 所有报文先入有界离线队列，发送成功才出队；失败按策略重试，重试耗尽仍失败则重新入队等下一轮 |
| 失败必须分类 | 区分"网络失败"（挂起等待恢复，不消耗重试额度）与"业务失败"（立刻出队，重试无意义），见第 6 节 |

现场最容易踩的三个坑，本模块都做了针对性处理：

1. **重传导致产量重复统计** —— 网络抖动让"请求已到达但响应没回来"，客户端按超时重传。
   若 MES 侧不能去重，产量就会虚高。因此每条报文都带 `messageId`（幂等去重主键）
   与 `sequenceNo`（顺序与丢包检测）。
2. **一个坏报文拖垮整条队列** —— 若把"MES 业务拒绝"也当网络错误无限重试，
   队首会被永久占住，后面本来能成功的报文一条都发不出去。业务失败必须立刻出队。
3. **重启后未上报数据全丢** —— 断电、软件升级是常态。离线队列支持落盘，
   重启后接着发，且序列号继续递增，MES 侧不会看到序号回退。

---

## 2. 文件说明

| 文件 | 职责 | 关键设计取舍 |
| --- | --- | --- |
| `qscadamesconfig.h` | 连接参数与上报策略配置，`toJson()` / `fromJson()` | 纯数据、无行为、无状态，可按值拷贝、可跨线程只读共享；**头文件内联实现**（配置是纯数据，无需隐藏实现细节，避免引入额外链接依赖） |
| `qscadamespayload.h/.cpp` | 报文组装，全部静态方法 | 只回答"一条报文长什么样"，不管"什么时候发、发失败怎么办"。纯函数便于脱离网络做单元测试与接口联调 |
| `qscadamesofflinequeue.h/.cpp` | 有界环形缓冲离线队列 + 可选持久化 | 满了**丢最旧**（最新数据对生产决策更有价值），且丢弃必须计数留痕 |
| `qscadamesclient.h/.cpp` | REST 上报客户端、发送状态机、重试与退避 | 单条串行发送，保证 MES 侧收到的顺序与产生顺序一致 |
| `README.md` | 本文件，含与 MES 的接口约定 | —— |

依赖关系是单向的：`client → queue → payload`，`config` 被 `client` 与 `payload` 共用。
`payload`、`queue`、`config` 都不认识网络，因此可以独立测试。

---

## 3. 快速上手

```cpp
#include "qscadamesclient.h"
#include "qscadamespayload.h"

// 1) 配置：通常来自项目文件 / 设置对话框，而不是硬编码
QScadaMesConfig mesConfig;
mesConfig.enabled                    = true;
mesConfig.baseUrl                    = QStringLiteral("http://mes.example.com/api/v1");
mesConfig.apiKey                     = QStringLiteral("填写甲方下发的密钥");
mesConfig.deviceCode                 = QStringLiteral("CNC-01");     // 工位/设备编号
mesConfig.workshopCode               = QStringLiteral("WS-A");       // 车间编号
mesConfig.timeoutMs                  = 5000;
mesConfig.retryCount                 = 3;
mesConfig.retryBackoffMs             = 1000;                          // 指数退避基数
mesConfig.heartbeatIntervalMs        = 30000;
mesConfig.productionReportIntervalMs = 60000;                         // 0 = 关闭自动上报
mesConfig.maxQueueSize               = 5000;
mesConfig.verifySsl                  = true;                          // 现场自签证书时才关
mesConfig.queueFilePath              = QStringLiteral("./data/mes_queue.json");

// 2) 创建并启动
QScadaMesClient *mes = new QScadaMesClient(this);
mes->setConfig(mesConfig);
mes->start();

// 3) 接信号：界面只用这些信号就能做出完整的 MES 状态面板
connect(mes, &QScadaMesClient::connectionStateChanged, this, [](bool online){
    // 点亮/熄灭"通讯正常"指示灯，掉线时触发报警
});
connect(mes, &QScadaMesClient::reportSent, this,
        [](const QString &type, const QString &id){
    // 追加日志：QScadaMesClient::messageTypeName(type) 得到中文名
});
connect(mes, &QScadaMesClient::reportFailed, this,
        [](const QString &type, const QString &reason){
    // 失败必须上日志窗口，不能只写 qDebug
});
connect(mes, &QScadaMesClient::productionReportDue, this, [this](){
    // 库不知道当前工单和已生产数量，请查业务数据后主动上报
    // reportProduction(workOrderNo, partNo, goodCount, scrapCount);
});

// 4) 业务侧主动上报（全部走队列，非阻塞）
mes->reportProduction(QStringLiteral("WO-20240517-001"),
                      QStringLiteral("P-8842"), 120, 3);   // 增量：良品 120，废品 3
mes->reportEquipmentStatus(QStringLiteral("fault"), QStringLiteral("主轴过载"));
mes->reportAlarm(QStringLiteral("ALM-1031"), QStringLiteral("fault"),
                 QStringLiteral("主轴温度过高"), 87.3);
mes->reportOee(0.92, 0.88, 0.995, 0.806);                 // 均为 0.0 ~ 1.0
// 关键位号快照：值来自采集层
QMap<QString, QVariant> snapshot;
snapshot.insert(QStringLiteral("spindle_speed"), 8200);
snapshot.insert(QStringLiteral("coolant_temp"), 31.5);
mes->reportTagSnapshot(snapshot);

// 5) 退出前落盘（stop() 内部也会自动落盘一次）
mes->stop();
```

### 若只想组装报文、自己发送

`QScadaMesPayload` 全部是静态方法，可以脱离客户端单独使用：

```cpp
const QJsonObject body = QScadaMesPayload::productionCount(
    QStringLiteral("CNC-01"), QStringLiteral("WO-001"),
    QStringLiteral("P-8842"), 120, 3, QDateTime::currentDateTime());
```

调试期建议打印 `QJsonDocument(body).toJson(QJsonDocument::Indented)`，
联调时可直接把这段 JSON 贴给 MES 开发，比口述字段快得多。

---

## 4. 与 MES 的接口约定

### 4.1 请求

| 项目 | 约定 |
| --- | --- |
| 方法 | `POST` |
| 路径 | `{baseUrl}/{messageType}`，例如 `http://mes.example.com/api/v1/production` |
| `Content-Type` | `application/json`（UTF-8） |
| 鉴权 | 请求头 `X-Api-Key: <apiKey>` |
| 超时 | 客户端侧 `timeoutMs`（默认 5000ms），超时按网络失败重试 |

`messageType` 取值与含义：

| messageType | 中文 | 触发时机 |
| --- | --- | --- |
| `heartbeat` | 心跳 | 每 `heartbeatIntervalMs` 一次，MES 据此判断设备在线 |
| `production` | 产量 | 业务侧主动调用，或 `productionReportIntervalMs` 到点后由上层补报 |
| `equipment_status` | 设备状态 | 状态变化时由业务侧调用 |
| `alarm` | 报警 | 报警产生/确认时由业务侧调用 |
| `oee` | OEE | 班次结束或周期统计后调用 |
| `tag_snapshot` | 位号快照 | 业务侧按需调用（只报关键位号，不做全量点表上报） |

### 4.2 响应（**必须遵守**）

```json
{ "code": 0, "message": "ok", "data": { "serverTime": "2024-05-17T10:30:00+08:00" } }
```

| 情形 | 客户端判定 | 处置 |
| --- | --- | --- |
| HTTP 200 且 `code == 0` | 成功 | 出队，`sentCount++`，发 `reportSent` |
| HTTP 200 但 `code != 0` | **业务失败** | 立刻出队，`failedCount++`，发 `reportFailed`，**不重试** |
| HTTP 200 但响应不是合法 JSON | 协议失败 | 视为业务失败（多半是路径配错、被反向代理接管） |
| HTTP 5xx / 429 / 408 | 可重试 | 指数退避后重发，最多 `retryCount` 次 |
| HTTP 4xx（其余） | 业务失败 | 立刻出队（路径写错、密钥无效、报文格式不符，重试无意义） |
| 连接失败 / DNS 失败 / 超时 | **网络失败** | 报文插回队首，**不消耗重试额度**，整队挂起等待恢复 |

> `code != 0` 时请把可读原因放在 `message` 里。客户端会把它拼进 `reportFailed`
> 的原因字符串，直接显示在界面日志上——这是现场排查最快的一条线索。

### 4.3 请求体：信封字段（每条报文都带）

所有 `messageType` 共用同一套信封字段，业务字段随后追加。

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `messageId` | string | 全局唯一 ID（UUID，无花括号）。**幂等去重主键** |
| `sequenceNo` | number | 同设备内单调递增序号（从 1 开始）。用于顺序判定与丢包检测 |
| `timestamp` | string | ISO8601 带时区与毫秒，例如 `2024-05-17T10:30:00.123+08:00` |
| `messageType` | string | 见上表 |
| `deviceCode` | string | 工位/设备编号 |
| `workshopCode` | string | 车间编号（`workshopCode` 非空时才出现） |

**为什么 `messageId` 与 `sequenceNo` 都是必需的：**

- `messageId` 用于**幂等去重**。网络抖动会导致"请求已到达、响应没回来"，
  客户端按超时重传。MES 侧请以 `messageId` 建唯一索引，重复到达时**返回成功**
  （而不是报错），客户端重传才能安全收敛。否则同一次生产计数会被统计两遍——
  产量虚高、OEE 算错、工单数量对不上，这是 MES 对接最经典的坑。
- `sequenceNo` 用于**顺序与丢包检测**。同设备内单调递增，MES 侧发现跳号即知
  中间丢过报文，可告警或触发补抄。不能只靠 `timestamp`：上位机改系统时间或
  NTP 校时都会让它回退，无法据此判断先后。
- `timestamp` 必须带时区偏移。现场上位机、MES 服务器、报表系统常不在同一时区，
  用裸本地时间会直接导致"夜班产量算到白班"。

### 4.4 各报文业务字段示例

**心跳 `heartbeat`**

```json
{
  "messageId": "5f2b9c1e8a7d4f0b9c3e6a1d2b4f8e70",
  "sequenceNo": 1,
  "timestamp": "2024-05-17T10:30:00.123+08:00",
  "messageType": "heartbeat",
  "deviceCode": "CNC-01",
  "workshopCode": "WS-A",
  "softwareVersion": "0.9.0",
  "runState": "running",
  "uptimeMs": 3600000
}
```

`uptimeMs` 让 MES 能区分"设备重启"和"网络闪断"——两者在上位机侧看起来都是
"一段时间没数据"，但运维处置完全不同。

**产量 `production`**（`goodCount` / `scrapCount` 是**增量**，不是累计值）

```json
{
  "messageId": "…", "sequenceNo": 2, "timestamp": "…",
  "messageType": "production", "deviceCode": "CNC-01",
  "workOrderNo": "WO-20240517-001",
  "partNo": "P-8842",
  "goodCount": 120,
  "scrapCount": 3,
  "totalCount": 123
}
```

> 传增量而非累计值：增量报文天然可重放、可去重；累计值一旦乱序到达就会把
> 计数改小，MES 侧还得自己做最大值保护，得不偿失。

**设备状态 `equipment_status`**（`statusCode` ∈ `running` / `idle` / `fault` / `stop`）

```json
{
  "messageId": "…", "sequenceNo": 3, "timestamp": "…",
  "messageType": "equipment_status", "deviceCode": "CNC-01",
  "statusCode": "fault",
  "statusText": "主轴过载"
}
```

按 `statusCode` 判断逻辑、按 `statusText` 给人看，避免界面直接显示英文枚举。

**报警 `alarm`**（`level` ∈ `info` / `warning` / `fault` / `critical`）

```json
{
  "messageId": "…", "sequenceNo": 4, "timestamp": "…",
  "messageType": "alarm", "deviceCode": "CNC-01",
  "alarmCode": "ALM-1031",
  "level": "fault",
  "message": "主轴温度过高",
  "triggerValue": 87.3
}
```

`triggerValue` 记录触发时的实际值，质量追溯时才能分清"是阈值设错了，
还是工艺真的跑偏了"。逻辑类报警没有数值时该字段不出现。

**OEE `oee`**（四个比值均为 `0.0 ~ 1.0` 的小数）

```json
{
  "messageId": "…", "sequenceNo": 5, "timestamp": "…",
  "messageType": "oee", "deviceCode": "CNC-01",
  "availability": 0.92, "performance": 0.88, "quality": 0.995, "oee": 0.806
}
```

约定死量纲（小数而非百分数），不做 0~100 与 0~1 的自动兼容——自动"猜"单位会让
`1.0` 到底是 1% 还是 100% 变得不可判定。

**位号快照 `tag_snapshot`**（只报关键位号）

```json
{
  "messageId": "…", "sequenceNo": 6, "timestamp": "…",
  "messageType": "tag_snapshot", "deviceCode": "CNC-01",
  "tags": { "coolant_temp": 31.5, "spindle_speed": 8200 }
}
```

---

## 5. 离线队列与重试机制

### 5.1 队列为什么是"有界 + 丢最旧"

- **有界**（`maxQueueSize`，默认 5000）：一台设备按秒级上报，断网一天就是几万条。
  无界队列会吃光内存并把上位机主界面拖到卡死——那比丢数据更严重：数据丢了 MES
  侧还能看出缺口，上位机卡死则是全线停产。
- **满了丢最旧，不丢最新**：最新数据对生产决策更有价值，MES 关心的是"这条线现在
  在做什么、良率如何"；几分钟前的历史产量可以由历史库补齐。反过来若丢最新，
  现场看到的会是"MES 上一直停在十分钟前"，运维会误判成采集故障。
- **丢弃必须留痕**：`droppedCount()` 可查，界面应把它显示出来并在丢弃时告警，
  否则数据缺了却没人知道，追溯时才发现对不上账。

实现是环形缓冲（`QVector` 一次性预分配），稳态运行期零内存分配——上报循环跑在
界面线程上，任何一次 `realloc` 都可能表现为界面掉帧。

### 5.2 发送状态机

队列里始终只有**一个在途报文**，四个状态流转：

```
                 取队首                      HTTP 请求
   Idle ──────────────────▶ PendingSending ──────────────▶ Sending
    ▲                            ▲                            │
    │ 队列空                      │ 退避到点                    ├─ 成功 ─▶ 出队 ─┐
    └────────────────────────────┴─────────── PendingRetry ◀──┤                │
                                                              └─ 失败 ─────────┤
                                                                               │
   Idle ◀────────────────── 队列空 / 继续取下一条 ◀───────────────────────────┘
```

取单条串行而不是并发多请求：产量报文之间有时序关系，并发发送会让 MES 侧收到乱序；
而且弱网下并发只会让所有请求一起超时。

### 5.3 重试策略

| 失败类型 | 是否消耗 `retryCount` | 退避 | 报文去向 |
| --- | --- | --- | --- |
| 网络失败（连不上 / DNS / 超时） | **否** | 心跳周期（下限为退避基数，上限 5 分钟） | 插回**队首**，等恢复后按原顺序继续发 |
| 5xx / 429 / 408 | 是（最多 `retryCount` 次） | `retryBackoffMs × 2^(n-1)`，上限 5 分钟 | 耗尽后出队并计 `failedCount` |
| 业务失败（`code != 0`、4xx、非 JSON 响应） | 否 | —— | 立刻出队并计 `failedCount` |

网络失败不消耗重试额度的原因：若在这里扣额度，一次半小时的断网就会把队列里的
报文全部判死刑，等网络恢复时反而一条都发不出去。

退避采用指数增长，是为了避免 MES 侧短暂过载时被重试流量持续打爆；同时做溢出
保护——`2^n` 若溢出成负数，间隔会退化成"立刻重发"，把退避机制彻底废掉。

### 5.4 持久化

`saveToFile()` / `loadFromFile()` 把队列连**序列号**一起写成 JSON：

```json
{ "version": 1, "sequenceNo": 137, "items": [ { "messageId": "…" }, … ] }
```

- 写盘用 `QSaveFile`（同目录临时文件 + 原子替换）。直接覆盖写时若掉电，
  会留下半截 JSON，下次启动解析失败，整个队列就永久废了。
- 序列号必须一起存：重启后接着递增，MES 侧才不会把新报文误判成"历史重放"。
- 客户端对写盘做了 2 秒**去抖**（`mQueueSaveTimer`），高频上报时不会每条都写磁盘；
  但在"网络失败挂起"和"停止运行"时都会**立即**落盘——"断网 + 断电 + 重启"
  是最坏组合，落盘能把损失压到最小。
- 恢复失败不会阻断 `start()`：宁可丢历史队列，也不能让上位机起不来；
  失败原因记录在 `lastError()` 里。

---

## 6. 网络失败 vs 业务失败：为什么必须区分

这是本模块最核心的一处判断，也是面试与实际现场都最容易出错的地方。

| | 网络失败 | 业务失败 |
| --- | --- | --- |
| 典型现象 | 连接被拒、DNS 解析失败、请求超时、HTTP 5xx / 429 | HTTP 200 但 `code != 0`："工位未绑定工单"、"密钥无此接口权限"、"数量为负" |
| 报文本身 | **没问题**，只是时机不对 | **已被服务端读懂并拒绝** |
| 重传是否有意义 | 有 | **没有**，再发一百次结果完全一样 |
| 正确处置 | 挂起队列，等网络恢复，不消耗重试额度 | 立刻出队、计数、通知上层人工处理 |

**如果不区分会怎样**：把业务失败也当网络失败无限重试，那条"格式错误"的报文会
永远占着队首，后面所有本来能成功的报文一起发不出去——表现为"MES 上一条数据都没有"，
而日志里刷满了同一个错误。这就是"一个坏报文拖垮整条队列"。
反之，把网络失败当业务失败立刻丢弃，则一次网络割接就会丢掉几小时的产量数据。

**判别顺序**（`QScadaMesClient::classifyReply()`）：先看 `QNetworkReply::error()`，
连接类错误（`ConnectionRefusedError`、`HostNotFoundError`、`TimeoutError` 等）直接归为
网络失败；否则再看 HTTP 状态码，5xx/429/408 可重试、其余 4xx 归业务失败；
状态码为 0 且非连接类错误（例如 SSL 握手失败）也归业务失败——重发通常无效，堆积无益。

---

## 7. 构建集成

模块尚未加入构建脚本（按约定由项目负责人统一处理）。需要时在
`com_indeema_QSimpleScada.pri` 中补充：

```qmake
HEADERS += \
    $$PWD/QScadaMes/qscadamesconfig.h \
    $$PWD/QScadaMes/qscadamespayload.h \
    $$PWD/QScadaMes/qscadamesofflinequeue.h \
    $$PWD/QScadaMes/qscadamesclient.h

SOURCES += \
    $$PWD/QScadaMes/qscadamespayload.cpp \
    $$PWD/QScadaMes/qscadamesofflinequeue.cpp \
    $$PWD/QScadaMes/qscadamesclient.cpp
```

`QSimpleScada.pro` 已经包含 `QT += network`，无需再加。
`QScadaMesConfig` 是头文件内联实现，没有 `.cpp`，不要往 `SOURCES` 里写。

---

## 8. Qt 版本兼容性

代码同时面向 **Qt 5.12+ 与 Qt 6**，为此遵守以下约束：

| 约束 | 原因 |
| --- | --- |
| 只连接 `QNetworkReply::finished`，用 `reply->error()` 判断结果 | `QNetworkReply::errorOccurred` 是 **Qt 5.15** 才引入的信号，Qt 5.12 上不存在，用了直接编译失败 |
| 不用 `QRegExp` / `qSort` / `qrand` / `qsrand` / 裸 `endl` / `QVariant::type()` / `QString::SkipEmptyParts` | 均已在 Qt 5.15/6 中弃用或移除；改用 `QRegularExpression`、`std::sort`、`QRandomGenerator`、`Qt::endl`、`Qt::SkipEmptyParts` |
| 不用 `QJsonObject{{"k", v}}` 之类的初始化列表构造 | Qt 5.12 上的重载解析与 Qt 6 不同，容易产生歧义；统一用 `insert()` |
| 显式包含所有用到的 Qt 头文件 | 不依赖 `QtWidgets` / 其它模块的间接包含，单独编译本模块也能通过 |
| 不使用 C++17 及以上语法（`std::optional`、结构化绑定、`if constexpr`） | 现场仍有工程使用 C++11 标准构建 |
| `QDateTime::toString(Qt::ISODateWithMs)` | Qt 5.8+ 才有；本模块要求 5.12+，安全 |

另外注意：Qt 6 的 `QNetworkRequest` 默认重定向策略由"手动"变为
`NoLessSafeRedirectPolicy`，且默认启用 HTTP/2。本模块依赖 Qt 的默认行为
（跟随安全重定向、协议自动协商），不额外设置，两端表现一致。

---

## 9. 已知限制与后续可做

- **未实现断点续传式批量补报**：积压较多时是一条条发。可增加"批量接口"，
  把多条报文打包成数组一次提交，显著缩短恢复时间。
- **`messageId` 未做持久化去重表**：客户端只保证生成唯一 ID，不记录"已发过哪些"。
  真正的幂等由 MES 侧唯一索引保证（这也是行业常规做法）。
- **未做上报数据压缩**：`tag_snapshot` 位号很多时可考虑 gzip。
- **未内置 TLS 客户端证书**：目前只支持 `X-Api-Key` 鉴权与可关闭的证书校验。
- **`QScadaDataSource` 未直接接线**：本模块刻意不依赖采集层（只依赖 `QVariant`），
  由上层把 `valueChanged` 信号转成 `reportTagSnapshot()` 调用，保持模块可独立测试。
  若需要"值变化即自动上报"，建议在上层加一个薄薄的适配器，而不是让 MES 层认识采集驱动。
