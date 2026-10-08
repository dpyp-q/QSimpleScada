#ifndef QSCADACNCENUMS_H
#define QSCADACNCENUMS_H

#include <QString>
#include <QStringList>
#include <QVariant>

/*!
 * \brief 数控/机床语义相关的枚举集合。
 *
 * 为什么单独放一个头文件：
 *  1. 轴、主轴、程序、机床四个类都要用到这些枚举，若各自定义一份就会
 *     出现"轴说自己 Ready、机床以为轴 Homed"这种语义漂移，HMI 上的颜色
 *     和文字会对不上；
 *  2. 枚举到中文名的映射（xxxName）集中在一处，界面层直接调用即可显示，
 *     不需要在 QML 或图元里再写一遍 if-else 翻译；
 *  3. 现场值班人员看的是中文状态，而日志、数据库、报警记录要存稳定的
 *     英文键（xxxKey），同一份枚举同时提供两套字符串，避免中文入库后
 *     再去改历史数据。
 *
 * 注意这里用的是传统 enum 而不是 enum class：本项目其余部分（QScadaStatus、
 * QScadaDataSource::ConnectionState）都是传统 enum，保持风格一致；
 * Q_ENUM 也需要枚举定义在 QObject 派生类里才能用，因此这里不依赖元对象系统，
 * 而是自己提供 name/key/fromName 三件套，保证在没有 QObject 的纯数据场景
 * （例如配置文件解析、单元测试）也能直接使用。
 */
namespace QScadaCncEnums {

/*!
 * 进给轴状态。
 * 之所以把"未使能"和"故障"分开：这两者在现场处理方式完全不同——
 * Disabled 通常是操作工没按使能或门未关，Fault 必须叫维修，HMI 上要区别
 * 显示（灰 vs 红），报警等级也不一样。
 */
enum AxisState {
    AxisDisabled = 0,   //!< 未使能：伺服没有上电/使能，任何运动指令都会被拒绝
    AxisReady,          //!< 就绪：已使能但没动，可以接受运动指令
    AxisHoming,         //!< 回零中：正在找参考点，此时禁止手动干预
    AxisHomed,          //!< 已回零：参考点已建立，机床坐标可信
    AxisMoving,         //!< 运动中：进给中（含 G00 快速定位）
    AxisHolding,        //!< 保持：进给保持按下或进给暂停，位置锁死
    AxisFault           //!< 故障：伺服报警、超程、跟随误差超限等
};

/*!
 * 整机状态。
 * Paused 与 Stopped 在国标/欧系数控面板上是两个不同按钮（进给保持 / 循环停止），
 * 前者可以继续（断点续切），后者本次循环作废，因此不能在模型里合并成一个。
 */
enum MachineState {
    MachineOffline = 0, //!< 离线：采集不上数据，界面应显示为"无通讯"，而不是"停机"
    MachineIdle,        //!< 待机：上电但没跑程序，随时可启动
    MachineRunning,     //!< 自动运行：正在执行加工程序
    MachinePaused,      //!< 暂停（进给保持）：程序停在断点，可继续
    MachineHold,        //!< 暂停：部分系统把"保持"与"暂停"分开表达，保留独立取值
    MachineStopped,     //!< 停止：循环被中止，程序指针未复位
    MachineAlarm,       //!< 报警：存在未清除的报警
    MachineMaintenance  //!< 维护：人为置的保养/调试状态，不应该报成故障
};

/*!
 * 主轴状态。
 * Orienting（定向）必须单独存在：换刀、精镗退刀时主轴要停到固定角度，
 * 这期间转速为 0 但并不是"停止"，误判成 Stopped 会导致换刀时序判断出错。
 */
enum SpindleState {
    SpindleStopped = 0, //!< 停止
    SpindleRunning,     //!< 旋转中
    SpindleOrienting,   //!< 定向中（换刀 / 精镗定位）
    SpindleFault        //!< 故障
};

/*!
 * 插补方式，直接对应 G 代码组 01。
 * 保留 Rapid：快移与切削进给在进给倍率上的表现不同（多数系统快移不受
 * 切削倍率影响），界面若把 G00 显示成"直线插补"，操作工会误判节拍。
 */
enum InterpolationMode {
    InterpNone = 0,     //!< 无（未运行 / 未识别）
    InterpRapid,        //!< G00 快速定位
    InterpLinear,       //!< G01 直线插补
    InterpCircularCW,   //!< G02 顺时针圆弧插补
    InterpCircularCCW,  //!< G03 逆时针圆弧插补
    InterpThreading     //!< G33 螺纹切削
};

/*!
 * 刀具补偿方式。
 * 左刀补/右刀补不是"半径补偿的正负号"：它们决定刀具在编程轨迹的哪一侧，
 * 直接关系到零件尺寸是大了还是小了，必须能分别显示。
 */
enum ToolOffsetMode {
    ToolOffsetNone = 0, //!< 未启用补偿
    ToolRadiusComp,     //!< 刀具半径补偿（G41/G42 的统称）
    ToolLengthComp,     //!< 刀具长度补偿（G43/G44）
    CutterCompLeft,     //!< G41 左刀补
    CutterCompRight     //!< G42 右刀补
};

// ---- AxisState ----
QString axisStateName(AxisState s);                 //!< 中文名，直接显示在界面
QString axisStateKey(AxisState s);                  //!< 英文键，用于日志/数据库/配置
AxisState axisStateFromName(const QString &name);   //!< 反查，兼容中文名与英文键
bool axisStateFromValue(const QVariant &value, AxisState *out); //!< 从采集值解析状态

// ---- MachineState ----
QString machineStateName(MachineState s);
QString machineStateKey(MachineState s);
MachineState machineStateFromName(const QString &name);
bool machineStateFromValue(const QVariant &value, MachineState *out);

// ---- SpindleState ----
QString spindleStateName(SpindleState s);
QString spindleStateKey(SpindleState s);
SpindleState spindleStateFromName(const QString &name);
bool spindleStateFromValue(const QVariant &value, SpindleState *out);

// ---- InterpolationMode ----
QString interpolationModeName(InterpolationMode m);
QString interpolationModeKey(InterpolationMode m);
InterpolationMode interpolationModeFromName(const QString &name);
bool interpolationModeFromValue(const QVariant &value, InterpolationMode *out);
//! 该插补方式对应的 G 代码号，例如 G01 返回 1；无对应时返回 -1。
int interpolationModeGCode(InterpolationMode m);

// ---- ToolOffsetMode ----
QString toolOffsetModeName(ToolOffsetMode m);
QString toolOffsetModeKey(ToolOffsetMode m);
ToolOffsetMode toolOffsetModeFromName(const QString &name);
bool toolOffsetModeFromValue(const QVariant &value, ToolOffsetMode *out);

// ---- 供配置界面 / 自动生成点表模板用的全集 ----
QStringList axisStateKeys();
QStringList machineStateKeys();
QStringList spindleStateKeys();
QStringList interpolationModeKeys();
QStringList toolOffsetModeKeys();

/*!
 * 现场设备的报警号是纯数字，操作工看不懂。
 * 这里只覆盖数控系统里出现频率最高的通用报警号（各家编号有差异，
 * 最终应以后处理里的报警文本位号为准），作为"位号里没有报警文本"时的兜底。
 */
QString defaultAlarmText(int alarmCode);

/*!
 * 轴名规范化：现场点表里经常写成 "x" / " X " / "X轴"，而机械坐标必须是
 * 单一规范的键才能跟界面上的轴表对应起来，否则会出现"多出来一根不存在的轴"。
 */
QString normalizeAxisName(const QString &name);

} // namespace QScadaCncEnums

#endif // QSCADACNCENUMS_H
