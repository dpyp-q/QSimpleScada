# QScadaCnc —— 数控/机床语义数据模型

本模块在 QSimpleScada 的**通用位号（Tag）采集层**之上，加了一层**机床语义模型**：
把"寄存器 40001 的值是 1234"翻译成"X 轴机械坐标 -12.340 mm、整机正在自动运行"。

```
        设备/数控系统               采集层（已有）              本模块（新增）           界面
 ┌──────────────────────┐   ┌────────────────────────┐  ┌───────────────────┐  ┌──────────┐
 │ 西门子 828D / 三菱 M80│──▶│ QScadaDataSource       │─▶│ QScadaCncMachine  │─▶│ 看板/图元 │
 │ FANUC 0i / Modbus 网关│   │ valueChanged(tagKey,v) │  │  ├ QScadaCncAxis  │  │ 状态色/曲线│
 └──────────────────────┘   │ QScadaTagDefinition    │  │  ├ ...Spindle      │  └──────────┘
   地址随厂家而变 ↑          │  (key/地址/类型/换算)   │  │  └ ...Program      │
                            └────────────────────────┘  └───────────────────┘
                                    点表配置文件决定 ──────────┘
```

## 1. 目录内容

| 文件 | 职责 |
|------|------|
| `qscadacncenums.h` / `.cpp` | 数控相关枚举集合（轴状态、整机状态、主轴状态、插补方式、刀补方式）与中文名/英文键/反查函数 |
| `qscadacnctagbinding.h` / `.cpp` | **位号绑定基类**：语义字段 ⇄ 采集位号的正反向映射与分发 |
| `qscadacncaxis.h` / `.cpp` | 单个进给轴：坐标、进给、倍率、负载、跟随误差、回零、报警 |
| `qscadacncspindle.h` / `.cpp` | 主轴：转速、给定转速、倍率、负载、当前刀具、定向、报警 |
| `qscadacncprogram.h` / `.cpp` | 加工程序上下文：程序号、行号进度、当前 G 代码块、插补方式、节拍、产量 |
| `qscadacncmachine.h` / `.cpp` | 整台机床：聚合轴/主轴/程序/刀具/整机状态，对外只暴露一个采集入口 |

> 说明：任务清单里 `qscadacncenums.h` 是枚举声明头，但 `xxxName()` 这类函数必须有实现，
> 因此额外提供了 `qscadacncenums.cpp`。集成时请把 **6 个 .cpp** 都加进 `.pro`。

本模块**不修改任何已有文件**，也**不依赖**除了以下三个已冻结接口以外的任何东西：

- `QScadaAcquisition/qscadatagdefinition.h`（位号定义：`key()` / `address()` / `dataType()` / `unit()`）
- `QScadaAcquisition/qscadadatasource.h`（`valueChanged(QString tagKey, QVariant value)`）
- `qscadaconfig.h`（已有的 `QScadaStatus` 枚举，用于 `toScadaStatus()` 给图元上色）

## 2. 位号绑定机制（本模块的核心设计）

### 2.1 为什么要这层间接

同一个"X 轴机械坐标"，在不同数控系统上的地址完全不同，而且没有任何标准：

| 系统 | 典型取法 | 地址/变量形态 |
|------|----------|----------------|
| 西门子 828D / 840D sl | OPC UA 或 HMI 变量服务 | `/Channel/MachineAxis/actToolBasePos[u1,1]` |
| 三菱 M80 / M70 | 以太网 MELSEC（QnA 兼容帧）读 R/D 区 | `R2000` 起，具体位置随梯图改 |
| FANUC 0i / 31i | FOCAS2 | `cnc_rdposition` 的绝对/相对坐标 |
| 老机床 + Modbus 网关 | Modbus TCP 读保持寄存器 | `40001`，还得配字序与字节序 |

如果把地址写进 `QScadaCncAxis`，那么"换一台机床"就等于"改模型代码 + 重新编译 +
重新验证界面"，而模型与界面恰恰是项目里最不该反复改动、也最难重新验证的部分。

所以模型里**一个地址都不出现**，只保存一个不透明的字符串——位号 Key：

```cpp
axis->bindTag("machinePosition", "X.MACH_POS");   // 语义字段 -> 位号
```

地址、数据类型、字序、工程量换算（`scale`/`offset`）、单位，全部落在点表
（`QScadaTagDefinition`）里。适配新机床时的动作变成：

1. 改点表：`X.MACH_POS -> address=40001, type=Float32, wordOrder=..., scale=0.001`；
2. 必要时改一次绑定表（哪个字段对哪个位号）；
3. 模型、界面、报警、历史库**一行代码不动**。

### 2.2 机制实现

`QScadaCncTagBinding` 是所有模型类的基类，内部维护两张表：

```cpp
QHash<QString, QString>     mFieldToTag;   // 正向：字段全名 -> 位号
QHash<QString, QStringList> mTagToFields;  // 反向：位号 -> 字段列表（每次绑定后重建）
```

反向索引是必需的：采集驱动只给出 `tagKey`，模型必须立刻知道"这个值该写进哪个字段"。
一个位号允许绑到多个字段（例如"当前行号"既用于显示也用于算进度），所以值是列表。

采集值的两条通路都收敛到同一个虚函数 `updateFromTag(field, value)`：

```cpp
// 通路一：直接连采集驱动的信号（参数顺序刻意与 valueChanged 保持一致）
connect(dataSource, &QScadaDataSource::valueChanged,
        machine,    &QScadaCncMachine::onTagValue);

// 通路二：手工喂一个位号（单元测试、报文回放、离线演示）
machine->onTagValue("X.MACH_POS", QVariant(-12.34));
```

基类还提供 `applyTagValue(tagKey, value)`，让机床模型能把值转发给某根具体的轴：

```cpp
// QScadaCncMachine::onTagValue 内部（按字段前缀分发）
QScadaCncAxis *a = axis("X");
a->applyTagValue(a->tagFor("X.machinePosition"), value);
```

### 2.3 字段命名规范（全局唯一键）

| 对象 | 前缀 | 示例字段全名 |
|------|------|--------------|
| 进给轴 | 轴名 + `.` | `X.machinePosition`、`Z.followingError`、`Y.homed` |
| 主轴 | 主轴名 + `.`（默认 `S`） | `S.speed`、`S.targetSpeed`、`S.toolNumber` |
| 加工程序 | `O.`（O 号是数控惯例） | `O.currentLine`、`O.partsCompleted` |
| 机床本体 | `M.` | `M.machineState`、`M.currentTool`、`M.workOffset` |

前缀的唯一作用是让"一张表装下所有对象"而不撞名（`X.machinePosition` 与
`Y.machinePosition` 必须能区分）。调用 `bindTag()` 时短名会被自动补全，
上层不必记这些前缀。

每个模型类都提供 `static QStringList bindableFields()`，列出全部可绑定字段，
可直接用于生成点表模板、校验配置文件里有没有写错的字段名。

### 2.4 一个完整的配置示例

```cpp
QScadaCncMachine *machine = new QScadaCncMachine(this);
machine->setMachineName("VMC850-01");
machine->setMachineModel("VMC850 / SIEMENS 828D");

machine->addAxis("X");
machine->addAxis("Y");
machine->addAxis("Z");

// 字段 -> 位号（位号本身在点表里定义地址与换算）
machine->bindAxisTag("X", "machinePosition",  "X.MACH_POS");
machine->bindAxisTag("X", "workPosition",     "X.WORK_POS");
machine->bindAxisTag("X", "loadPercent",      "X.LOAD");
machine->bindAxisTag("X", "followingError",   "X.FERR");
machine->bindAxisTag("X", "state",            "X.STATE");
machine->bindAxisTag("Y", "machinePosition",  "Y.MACH_POS");
machine->bindAxisTag("Z", "machinePosition",  "Z.MACH_POS");

machine->bindSpindleTag("speed",       "S.RPM_ACT");
machine->bindSpindleTag("targetSpeed", "S.RPM_CMD");
machine->bindSpindleTag("toolNumber",  "S.TOOL");

machine->bindProgramTag("programNumber",  "NC.PROG_NO");
machine->bindProgramTag("currentLine",    "NC.LINE");
machine->bindProgramTag("totalLines",     "NC.LINE_TOTAL");
machine->bindProgramTag("currentBlock",   "NC.BLOCK");
machine->bindProgramTag("partsCompleted", "NC.PARTS");

machine->bindTag("machineState", "M.STATE");
machine->bindTag("currentTool",  "M.TOOL");
machine->bindTag("workOffset",   "M.G54");

// 接上采集层
connect(dataSource, &QScadaDataSource::valueChanged,
        machine,    &QScadaCncMachine::onTagValue);

// 界面上色
widget->setStatus(machine->toScadaStatus());     // QScadaStatusGreen / Red / ...
label->setText(machine->statusSummary());
// "运行中 | X:-12.340 Y:8.900 Z:-45.000 | S:8000rpm | O1234 L128/540 | 已加工 37 件"
```

`machine->fieldTagMap()` 返回"字段全名 -> 位号"的全量快照，启动时与点表里的
位号集合对一下，就能立刻发现"点表加了点但忘了绑定"这类联调期最常见的错误
（运行时这类错误会走 `qWarning()` 写日志，库代码不弹窗）。

## 3. 状态映射与报警汇总

`QScadaCncMachine::toScadaStatus()` 把机床状态压成已有的 `QScadaStatus`，
映射原则按车间习惯：

| 机床状态 | QScadaStatus | 车间的理解 |
|----------|--------------|------------|
| 报警 / 任一轴故障 / 任一轴报警号非 0 | `QScadaStatusRed` | 要立刻过去看 |
| 自动运行 | `QScadaStatusGreen` | 正常出活 |
| 待机 / 暂停 / 保持 / 停止 | `QScadaStatusYellow` | 有情况但不致命（待机久了要催料） |
| 离线 / 维护 | `QScadaStatusGray` | 不用管 / 数据不可信 |

报警汇总走三个信号：

- `anyAlarmRaised(source, alarmCode, message)`：**只在报警从无到有时发一次**，
  避免每个采集周期重复弹同一条；
- `alarmCleared(source, alarmCode)`；
- 报警号本身留在各轴/主轴对象上（`axis->alarmCode()`），上层要完整报警列表时去读，
  模型不重复存储。

## 4. Qt 兼容性

- 目标：**Qt 5.12 LTS 与 Qt 6 双兼容**（都能直接编译，无 `#if QT_VERSION` 分支）；
- 显式 include 所有用到的 Qt 头文件，不依赖间接包含；
- 不使用：`QRegExp`、`qSort`、`qrand`/`qsrand`、裸 `endl`、`QVariant::type()`、
  `QString::SkipEmptyParts`；如需正则/排序/随机/日志一律用 `QRegularExpression`、
  `std::sort`、`QRandomGenerator`、`Qt::endl`（本模块当前未用到这四类功能）；
- 语法限制在 C++11/14：不用 `std::optional`、结构化绑定、`if constexpr`；
- `QStringList::join(QChar)` 是 Qt 5.14 才有的重载，本模块统一用
  `join(QLatin1String("..."))`；
- 浮点比较不用 `qFuzzyCompare` 直接比 0，而是自定义"基本没变"判断，
  否则已回零（坐标恒为 0）的轴会每个采集周期都发一次变更信号。

## 5. 集成步骤

1. 把 `QScadaCnc/*.cpp`（6 个）与 `QScadaCnc/*.h`（6 个）分别加入 `.pro` 的
   `SOURCES` / `HEADERS`；
2. 本模块默认从 `../qscadaconfig.h` 取 `QScadaStatus`。若构建系统的
   `INCLUDEPATH` 已包含工程根目录（现有 `com_indeema_QSimpleScada.pri` 里有
   `INCLUDEPATH += $$PWD`），这一行也可改成 `#include "qscadaconfig.h"`；
3. 若使用 qmake，确保 `CONFIG += c++11`（Qt 5.12 默认已满足）。

## 6. 有意**没有**做的事（设计边界）

- **不存历史数据**：模型只保留最新值。历史/趋势属于独立的历史库模块，
  放在模型里会让每个采集周期都产生内存分配（现场采集周期常见 50~200ms）。
- **不做单位换算**：单位统一交给点表的 `scale`/`offset`，模型只存工程量数值，
  避免两处换算互相打架。
- **不解析 G 代码**：`currentBlock` 保留原文。各家宏程序、自定义 G 代码语法
  差异极大，上位机去解析等于重写一个 NC 解析器；原样显示 + 关键字高亮才是
  投入产出比最高的做法。
- **不提供在线圆度/振动等分析量**：多数数控系统不给这些位号，需要外部仪器
  （球杆仪）或后端对轨迹做拟合，硬塞进采集模型会造成"数据来源说不清"。
- **模型里不弹任何对话框**：诊断信息一律走 `qWarning()`，弹窗会阻塞采集线程。
