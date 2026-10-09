# QSimpleScada Demo（二次开发验证宿主）

本目录是**在电脑上真实运行**整个二次开发链路的演示宿主：GUI 窗口 + 仿真采集数据流。
不含第三方图元库（官方 EEIoT/Sample 是独立仓库），图元为自写 `indicator.qml`。

## 数据链路（五段）

```
QScadaSimulationSource（正弦/锯齿/计数，1s 周期）
  → QScadaDataHub（publish 盖时间戳 + 质量码 + 设备归属；published/s 统计）
  → QScadaViewBridge（bindTags 点表绑定，10Hz 节流 + 合并）
  → QScadaBoardController::updateValue(deviceIp, boardId, objectId, value)
  → QML 指示灯图元 update(value)
```

- 3 个位号：`TEMP_OVEN`（正弦 20~80 ℃）、`SPINDLE_RPM`（锯齿 2000~5000 rpm）、
  `PROD_COUNT`（计数 每秒 +1 件）。
- 视图桥节流只作用于界面；历史库/报警订阅中枢拿**每一个**值（含质量码），不受节流影响。
- 设备 IP 必须是合法地址（`127.0.0.1`）：`QScadaBoardManager::deviceForIp`
  用 `ip().toString()` 精确匹配，`"SIM-01"` 会解析成无效地址导致 board 创建失败。

## 构建（Windows / Qt 6.12 MinGW）

```powershell
$env:Path = "D:\Qt\Tools\mingw1310_64\bin;D:\Qt\6.12.0\mingw_64\bin;" + $env:Path
Set-Location build-mingw\demo
qmake demo.pro
mingw32-make -j8
Copy-Item ..\..\demo\indicator.qml release\   # QML 图元随构建产物复制
```

链接要点：导入库 `libQSimpleScada0.a` 需在 `QSimpleScadaLib\`（与 DLL 同目录）；
`demo.pro` 的 `LIBS += -L../../QSimpleScadaLib -lQSimpleScada0`。

## 运行与验证

```powershell
python build-mingw\demo\check_run.py        # 前置 Qt/MinGW PATH，抓日志，8s 后报存活
```

预期日志（`build-mingw\demo\release\run_log2.txt`）：

```
[demo] view bridge bindings: 3
qml: [indicator] update received: 主轴 Spindle -> 2016.44      # 每秒一个值，节流=0
[demo] hub: published= 3  cached= 3  pts/s= 3  | bridge: applied= 3  coalesced= 0
```

`applied` 与 `published` 同步增长 = 数据链路畅通；`coalesced=0` = 低频采集无合并（高频点表下该值会增长）。

## 调试踩坑记录（可复现）

1. **QML 路径**：`__FILE__` 在 qmake 构建时被展开为构建目录，须改用
   `QCoreApplication::applicationDirPath() + "/indicator.qml"`。
2. **connect 重载差异**：`QObject::connect(hub, &QScadaDataHub::tagValueChanged, lambda)`
   在本工具链 release 下返回失败并告警 `signal not found`（PMF+functor 重载），
   换成自研 `QScadaViewBridge`（4 参成员函数槽 connect）后正常。
3. **PrintWindow 抓 QQuickWidget 有伪影**（OpenGL 内容镜像/部分文本丢失），
   验证 GUI 以 QML console.log 日志为准，截图仅作参考。
