# QScadaAlarm —— 报警判定引擎

> QSimpleScada 的工业上位机扩展模块：把采集层的值变化翻译成报警的**产生 / 恢复 / 确认**，
> 并维护一份"活动报警表 + 有界报警历史"，供报警条、报警表格、图元变色、MES 上报共用。
>
> 目标环境：Qt 5.12+ 与 Qt 6 双兼容，Windows / Linux 工控机。纯 QtCore 逻辑层，不依赖 GUI。

---

## 1. 为什么需要这个模块

上位机报警系统上线后最常见的三个"被操作员关掉"的原因，本模块全部做了针对性设计：

| 现场问题 | 根因 | 本模块的应对 |
| --- | --- | --- |
| **"闪一下就恢复"的报警没人看到** | 值短暂越限又恢复，报警从列表里消失，操作员交接班时根本不知道发生过 | 恢复 ≠ 关闭：已恢复但**没人确认过**的报警继续留在活动报警表里（状态 `Cleared` / RTN unacknowledged），直到有人确认才移除（ISA-18.2 要求） |
| **报警表疯狂刷屏** | 信号在阈值附近抖动，反复产生 / 恢复 / 再产生，几分钟刷出几百条记录 | 两个滤抖手段：**死区（deadband）**——恢复判定用"阈值 ∓ 死区"而不是同一根阈值线；**延时确认（delayMs）**——越限必须持续满延时才真正报警，滤掉电机启动电流、阀门换向压力波动这类瞬时尖峰 |
| **确认与故障消失混为一谈** | 操作员按下"确认"后报警被当成"已处理"，但设备可能还在超温；或恢复后报警直接消失，没人留下处理痕迹 | "是否仍在越限"与"是否已被确认"是两个**独立维度**，组合出 Active / Acknowledged / Cleared / Closed 四种状态；报警表、历史、MES 上报都基于同一份状态机（见第 4 节） |

除此之外还有两条硬约束：

1. **一条规则同时最多只有一条未关闭的报警。** 值继续恶化不会重复产生新事件，否则一个持续超温的点会在一分钟内刷出几百条记录，报警系统就废了。
2. **报警必须留痕。** 每次产生 / 恢复 / 确认都会写 `qWarning()` 日志（现场争议"当时到底报没报"时，日志比界面截图可靠），同时进内存历史；需要长期留档由上层把事件写进数据库（MES / SQL Server）。

---

## 2. 文件说明

| 文件 | 职责 | 关键设计取舍 |
| --- | --- | --- |
| `qscadaalarmlevel.h` | 等级枚举（Info / Warning / High / Critical）与显示辅助 | 纯静态函数、无状态。**存储标识与界面文案分离**：配置文件与数据库存 `"critical"` 这种稳定英文标识，界面显示"严重"，以后改中文措辞或做多语言不会让老配置 / 老记录失效；等级颜色集中在 `levelColor()` 一处，保证 ISA-18.2 要求的"同一等级整机颜色一致" |
| `qscadaalarmrule.h/.cpp` | 一条报警规则：怎么判 | 值语义、可自由拷贝（配置对话框、JSON 读写、列表显示都用同一份数据，不会"两处各存一份状态对不上"）。支持 6 种比较类型（见第 5 节）、死区、延时、消息模板占位符、`isValid()` 配置校验 |
| `qscadaalarmevent.h/.cpp` | 一条报警事件：从产生到关闭的完整生命周期 | 值语义、可序列化。状态判定唯一依据是两个布尔量 `active` / `acknowledged`，**不靠时间戳推断**（时间戳经时区转换 / 数据库存取可能失真，只负责记录不负责表达状态）；状态迁移幂等，重复确认不会覆盖第一个确认人 |
| `qscadaalarmengine.h/.cpp` | 判定引擎：规则管理、判定调度、活动报警表、历史、确认、系统关闭 | 单线程无锁（见第 6 节）；每条规则一个运行时槽（RuleSlot），延迟计时用单次 `QTimer`；历史有界（默认 1000 条）超限裁剪 |

依赖关系：`engine → rule/event → level`，全部是纯 QtCore 逻辑，不认识 Modbus、不认识界面、不依赖数据库。

---

## 3. 快速上手

```cpp
#include "qscadaalarmengine.h"
#include "qscadaalarmrule.h"

// 1) 创建引擎（与采集驱动同处一个 QObject 树即可）
QScadaAlarmEngine *alarm = new QScadaAlarmEngine(this);

// 2) 配规则：高位号 "CNC01.SpindleTemp" 超过 85 ℃ 报警，死区 2 ℃、延时 3 秒
QScadaAlarmRule rule("CNC01.SpindleTemp",
                     QScadaAlarmRule::GreaterThan,
                     QScadaAlarmLevels::High,
                     85.0);
rule.setDeadband(2.0);
rule.setDelayMs(3000);
rule.setUnit(QStringLiteral("℃"));
rule.setDescription(QStringLiteral("主轴温度过高"));
alarm->addRule(rule);

// 3) 接采集层：签名与 QScadaDataSource::valueChanged 完全一致，直接 connect
//    （跨线程时 Qt 自动用队列连接，判定全部串行执行在引擎线程，见第 6 节）
connect(source, &QScadaDataSource::valueChanged,
        alarm,  &QScadaAlarmEngine::onValueChanged);

// 4) 接信号：界面用这三条就能做出完整的报警面板
connect(alarm, &QScadaAlarmEngine::alarmRaised, this, [](const QScadaAlarmEvent &e) {
    // 弹新报警条 / 图元变色 / 声光提示（按 e.level() 区分优先级）
});
connect(alarm, &QScadaAlarmEngine::alarmCleared, this, [](const QScadaAlarmEvent &e) {
    // 恢复提示（e.state() 可能是 Cleared，也可能是系统关闭后的 Closed）
});
connect(alarm, &QScadaAlarmEngine::alarmAcknowledged, this, [](const QScadaAlarmEvent &e) {
    // 记录确认人与时间
});
// 活动报警表变化（产生/恢复/确认/复位）→ 刷新表格
connect(alarm, &QScadaAlarmEngine::activeAlarmsChanged, this, &AlarmTableWidget::refresh);

// 5) 操作员确认（界面按钮）
alarm->acknowledge(eventId, QStringLiteral("张三"));

// 6) 界面刷新：活动报警表直接拿
const QList<QScadaAlarmEvent> open = alarm->openAlarms();      // 未关闭（含已恢复未确认）
const QList<QScadaAlarmEvent> active = alarm->activeAlarms();  // 仅仍在越限
```

---

## 4. 核心概念一：报警状态机（ISA-18.2）

"是否仍在越限"与"是否已被确认"是**两个独立维度**，状态枚举只是它们的组合投影：

| active（仍越限） | acknowledged（已被确认） | state() | 含义 | 是否还在活动报警表 |
|:---:|:---:|:---:| --- |:---:|
| true | false | `Active` | 越限中、未确认，必须处理 | 是 |
| true | true | `Acknowledged` | 越限中、已确认，仍在处理 | 是 |
| false | false | `Cleared` | 已恢复、未确认（RTN unacknowledged） | 是 |
| false | true | `Closed` | 生命周期结束 | 否 |

关键推论（都是现场事故的直接来源，面试常问）：

- **确认 ≠ 故障消失**：设备还在超温时按下确认，报警只是从"未确认"变"已确认"，仍在报警表里，图元仍变色——界面与报警表不会互相矛盾。
- **恢复 ≠ 已处理**：值回到正常范围但没人确认过的报警，必须继续留在表里，否则一次短暂的超压可能整个班都没人知道。
- 活动报警表的保留条件是 `!isClosed()`：只有"既恢复又确认过"的才离开。

引擎提供两组查询：`openAlarms()`（报警表用）与 `activeAlarms()`（"当前工况"统计用），以及 `unacknowledgedAlarms()` / `countByLevel(level, onlyActive)` 等。

---

## 5. 核心概念二：两条判定路径

每条规则独立维护一个运行时槽，值到达时先统一算变化率（见第 7 节），再走产生或恢复路径：

```
值到达 → 该位号的所有启用规则各自 evaluate()
        ├── 未在报警中：
        │     ├── 未越限 → 若有延时计时，整段作废（cancelPending）
        │     ├── 越限且 delayMs==0 → 立即产生报警
        │     └── 越限且 delayMs>0 → 启动单次 QTimer，计时期间：
        │           · 再次越限只刷新候选值，绝不重启计时器
        │           · 回到正常范围 → 取消计时（整段作废，不累计）
        │           · 到点后用候选值复核一次，仍越限才真正产生报警
        └── 已在报警中：
              └── 值回到"阈值 ∓ 死区"的安全区 → 标记恢复（isRecovered）
```

**为什么延时计时不重启、到点要复核**（两处都是滤抖的关键）：

- 如果每次越限都重启计时器，信号在阈值附近来回跳时就永远等不到到点——报警要么永远不来、要么被迫改成"立即报"，两种都失去了延时确认的意义。
- 定时器到点与最后一次采样之间值可能已回落，不复核就会出现"最后一拍已经正常却仍然报警"。

**为什么死区只作用于恢复判定、不作用于产生判定**：产生侧用配置的原阈值（`evaluate()` 刻意不含死区），点位验收时报警触发点必须与配置一致；恢复侧才让出死区宽度，防止抖动时反复产生 / 恢复。OutOfRange 规则的可恢复区是 `[low+db, high-db]`——若死区配得大于半个区间宽度，可恢复区为空，报警会一直保持到操作员确认（某些高噪声测点确实故意这么配，只想要一个"确认即可"的提示）。

支持的比较类型：

| comparison | 判定 | 配置字段 |
| --- | --- | --- |
| `GreaterThan` / `GreaterThanOrEqual` | value > / >= threshold | threshold |
| `LessThan` / `LessThanOrEqual` | value < / <= threshold | threshold |
| `OutOfRange` | value < lowLimit 或 value > highLimit | lowLimit / highLimit |
| `RateOfChange` | \|dv/dt\| > threshold（单位/秒） | threshold |

`addRule()` 会拒绝无效配置（`isValid()`）：区间外规则要求上限严格大于下限，变化率阈值必须为正——现场改错一个阈值不该让报警规则悄悄失效。

---

## 6. 核心概念三：变化率报警的基线管理

变化率 = (本次值 - 上次值) / 间隔，两个参数把最容易误报的两个场景掐掉：

- **最小时间窗 `minRateIntervalMs`（默认 50 ms）**：两次采样间隔小于它时不更新估计，沿用上一次。否则 dt→0 会把毫伏级噪声放大成"每秒几千"的假变化率。
- **最大间隔 `rateMaxGapMs`（默认 30000 ms）**：间隔超过它说明中间断过（通讯中断、软件重启、点表重载），首尾两点之差反映的是"断线期间累计的变化"而不是变化率——此时只重建基线、不产生变化率报警。否则"通讯恢复的第一次刷新"会稳定地误报一次。

变化率对**所有**规则都算（不只变化率规则）：一条规则随时可能被改成变化率类型，基线必须一直在积累，否则改完类型的头几拍会用 0 或旧值去判定。

变化率规则恢复用死区（`|dv/dt| <= threshold - db`），但要压住真正的持续异常应配 delayMs 而不是靠死区——变化率是瞬时量，正常情况下下一拍就回落。

---

## 7. 线程模型与集成方式

**引擎是单线程对象**：规则表、活动报警表、历史都在引擎所属线程里读写，不加锁。采集驱动在自己的线程里发信号，Qt 跨线程时自动改用队列连接，于是所有判定都排在引擎线程的事件循环里串行执行——不存在竞态，也不需要 `QMutex`。

由此推出的两条纪律：

1. 界面线程不能直接调 `rules()` / `acknowledge()`（跨线程读写无锁容器是未定义行为）；需要跨线程投递时用 `QMetaObject::invokeMethod(engine, "onValueChanged", Qt::QueuedConnection, ...)`。
2. 信号参数 `QScadaAlarmEvent` 按值传递，构造时已 `Q_DECLARE_METATYPE` 注册元类型，跨线程队列连接可以安全投递（否则运行时会报 "Cannot queue arguments of type"）。

**与采集层的唯一耦合点是 `onValueChanged(QString, QVariant)`**，签名与 `QScadaDataSource::valueChanged` 完全一致，直接 connect 即可；换协议、换驱动都不需要改报警引擎一行代码。

**无效值不参与判定**：采集失败时驱动会发无效 `QVariant`，引擎直接跳过——既不当成 0、也不当成正常值。通讯中断期间若把所有报警判定为恢复，操作员会看到满屏绿色以为工况已正常，这是报警系统最危险的一类误判。正确做法是让报警保持在中断前的状态，并在界面上单独提示"通讯中断"。

**通知合并**：一次采集可能同时产生 / 恢复多条报警，引擎把 `activeAlarmsChanged()` 合并到本次处理的末尾发一次——界面不会因为一条报文里的 5 条报警而重排 5 次表格。

---

## 8. 系统关闭与审计

以下场景中，规则名下的未关闭报警会由**系统代操作员关闭**（补上恢复 + 确认，`ackedBy = "system"`，记入历史），而不是留在表里变成"永远无法关闭的僵尸报警"：

- 规则被**删除**（`removeRule` / `clearRules`）
- 规则被**停用**（`setRuleEnabled(false)`）
- 规则**更换目标位号**（`updateRule` 改 tagKey）
- 引擎**复位**（`reset()`：点表/规则整体重载、通讯长时间中断后重建基线、调试清表）

复位只清运行时状态（延时计时、候选值、变化率基线），**规则与历史都保留**——报警记录是要留档的，不能因为一次复位就抹掉。

另外两条审计纪律：

- **报警已产生后不会被配置变更改写**：`updateRule` 只影响后续判定，已产生的报警记录保持原样（报警记录是审计依据，事后改配置不能篡改历史）。
- **确认幂等**：重复确认同一事件返回 false，不覆盖第一个确认人与确认时间。

历史上限默认 1000 条（`setHistoryLimit` 可调），超限从最旧的开始裁剪——上位机常年在现场无人值守运行，内存必须有界；需要长期留档的场景由上层把事件写进数据库。

---

## 9. 接口约定

### 信号

| 信号 | 触发时机 |
| --- | --- |
| `alarmRaised(QScadaAlarmEvent)` | 报警产生（延时到点复核通过或即时报警） |
| `alarmCleared(QScadaAlarmEvent)` | 报警恢复（含系统关闭；此时事件为 Closed 状态） |
| `alarmAcknowledged(QScadaAlarmEvent)` | 操作员确认 |
| `activeAlarmsChanged()` | 活动报警表变化（产生/恢复/确认/复位），一次处理末尾合并发送 |
| `historyChanged()` | 历史新增或更新 |

### 事件 JSON（MES 上报 / 历史库 / 记录导出的统一格式）

```json
{
  "eventId": "…UUID…",
  "ruleId": "…",
  "tagKey": "CNC01.SpindleTemp",
  "level": "high",
  "message": "位号 CNC01.SpindleTemp 当前值 88.5℃ 超过高限 85.0℃",
  "triggerValue": 88.5,
  "limitValue": 85.0,
  "raisedAt": "2026-10-08T09:41:23.125+08:00",
  "clearedAt": "2026-10-08T09:42:10.000+08:00",
  "ackedAt": "2026-10-08T09:42:15.000+08:00",
  "ackedBy": "张三",
  "active": false,
  "acknowledged": true,
  "state": "closed"
}
```

时间戳统一 `Qt::ISODateWithMs`（带毫秒与时区偏移）：报警排查经常要看到秒以下的先后关系，字符串形式跨时区、跨数据库都安全。`state` 是冗余字段，给 MES / 报表这类只读消费方直接用，不用自己按两个布尔量推导。`fromJson` 兼容只存了 `state` 字段的外部记录（按状态表反推出两个维度）。

### 规则 JSON

```json
{
  "ruleId": "…", "tagKey": "CNC01.SpindleTemp", "description": "主轴温度过高",
  "level": "high", "enabled": true, "comparison": "greaterThan",
  "threshold": 85.0, "lowLimit": 0.0, "highLimit": 0.0,
  "deadband": 2.0, "delayMs": 3000,
  "messageTemplate": "", "unit": "℃"
}
```

等级与比较类型存稳定英文标识（`levelName` / `comparisonName`），界面文案可改不影响配置与历史兼容。

---

## 10. 与其它模块的协作

| 消费方 | 拿什么 | 用途 |
| --- | --- | --- |
| 界面报警条 / 表格 | `alarmRaised` / `alarmCleared` 信号 + `openAlarms()` | 新报警提示、表格刷新、图元按最严重等级变色（按 `orderedLevels()` 严重→轻微遍历，各处显示顺序一致） |
| 图元状态 | `alarmsOfTag(tagKey)` / `countByLevel(level, onlyActive)` | 每块图元只关心自己绑定位号的报警，不必看全表 |
| 历史库（QScadaStorage） | `history()` / `recentHistory(n)` | 按时间正序全量导出 / 最新 N 条倒序 |
| MES 上报（QScadaMes） | `alarmRaised` 等信号的事件对象 → `event.toJson()` | 报警即时报 MES，配合 MES 客户端离线队列保证不丢 |
| 报表 / 审计 | `historyForTag` / `durationMs()` / `durationText()` | 报警持续时长统计（已恢复的按 raisedAt→clearedAt，仍在越限的算到当前） |

---

## 11. 构建与边界

- 已包含在 `com_indeema_QSimpleScada.pri` 的 `SOURCES` / `HEADERS` 中，随主库一起编译；业务逻辑只依赖 `QtCore`，唯一的 GUI 模块引用是 `qscadaalarmlevel.h` 里的 `QColor`（等级颜色辅助，纯数据、无显示环境可用）。
- Qt 5.12 与 Qt 6 双兼容，无 `#if QT_VERSION` 分支（本模块未用到版本差异 API）。
- 引擎**不是数据库、不是界面**：历史默认只留内存 1000 条；报警条样式、声光联动由界面层基于信号实现；长期留档走 QScadaStorage / QScadaMes。
- 规则 id 与事件 id 都是去花括号的 UUID（`newRuleId()` / `newEventId()`），保证跨重启、跨机器不重复——MES 侧按 `eventId` 幂等去重。
- 单元测试：`QScadaTests/alarm/tst_qscadaalarm.cpp`（46 个用例：规则判定与死区 16、事件状态机 10、引擎生命周期 20），只依赖 QtCore + QtTest + QtGui（QColor），可在无显示环境直接跑。测试还抓出并修复了一个真实缺陷：变化率规则 `evaluate()` 原先把 `Evaluation.value` 填成**测点值**而非速率，导致报警文本出现"变化率 200/s"而 200 实为当时的测点值；现已修正为记录 dv/dt（带方向），与默认模板、`QScadaAlarmEvent::triggerValue` 的既有约定一致。
