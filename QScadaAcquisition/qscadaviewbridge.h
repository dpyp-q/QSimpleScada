#ifndef QSCADAVIEWBRIDGE_H
#define QSCADAVIEWBRIDGE_H

#include <QChar>
#include <QHash>
#include <QList>
#include <QObject>
#include <QString>
#include <QVariant>

#include "qscadatagdefinition.h"
#include "qscadatagvalue.h"

class QScadaBoardController;
class QScadaDataHub;
class QTimer;

/*!
 * \brief 视图桥：把数据中枢的位号值翻译成原框架的视图调用
 *        `QScadaBoardController::updateValue(deviceIp, boardId, objectId, value)`。
 *
 * ## 它的定位：新旧架构之间唯一的一座桥
 *
 * 新的采集架构是**位号寻址**的（谁的值就是谁的值，与界面在不在显示无关），
 * 而原框架的视图层是**视图寻址**的（值必须落到"当前这块仪表盘上的某个图元"）。
 * 两者之间需要一次翻译：位号 -> (设备, 画面, 图元)。这一层翻译就放在本类里，
 * 于是：
 *
 *  · 视图层（QScadaBoard / QScadaObject / QML 图元）**一行都不用改**，
 *    原框架已有的组态工具、工程文件、图元库全部继续可用；
 *  · 采集层完全不知道界面的存在，历史库、报警、MES 上报也都不经过这里。
 *
 * ## 刷新节流：本类最有价值的设计
 *
 * 高频采集下（100 ms 周期、几百个位号）如果每来一个值就立刻刷界面，
 * GUI 线程会被彻底淹没：一次 updateValue 最终会走到 QWidget/QML 的属性设置与
 * 重绘，几百个图元 × 每秒十几次，界面立刻卡顿甚至假死——而操作员接收到的
 * 信息量一点没变（人眼分辨不出 10 Hz 以上的数字刷新，更追不上 100 ms 的跳字）。
 *
 * 所以这里做"合并 + 限频"两级处理：
 *
 *  1. **合并（coalescing）**：待刷新的值放进一张按"图元"索引的表，同一个图元
 *     只保留最新值，后来的直接覆盖。**为什么覆盖写是正确的**：界面只需要最新值，
 *     被覆盖掉的中间值对显示毫无意义，丢弃它们不改变任何业务语义——曲线上的
 *     中间点、报警的瞬时抖动都不靠界面记录。
 *  2. **限频**：由一个定时器按 `maxUpdateRate`（默认 10 Hz）统一 flush，
 *     把原本分散的 N 次属性写合并成每 100 ms 一批。
 *
 * 关键前提：**这个节流只对界面成立，对历史库和报警绝不成立**。它们订阅的是
 * `QScadaDataHub`，拿到的是每一个值（含时间戳与质量码）——趋势的拐点、报警的
 * 瞬时抖动必须完整记录，丢一个就是数据事故。这正是"界面订阅桥、其它订阅中枢"
 * 这个分工的原因。节流效果可以用 `coalescedCount()` 量化
 * （"100 ms 周期下界面刷新次数减少 9x%"这种数字比形容词有说服力）。
 *
 * ## 坏质量怎么显示
 *
 * 质量不是 QualityGood 时默认**仍然刷新界面**：操作员必须看得见异常，
 * 一个冻在旧值上的数字比一个明显不对的数字更危险。
 * 如果客户明确要求"坏质量就别动图元"，可以用 `setShowBadQuality(false)` 切成
 * 只在值有效时刷新。
 * 工业现场更常见的做法是两件事一起做：值照刷，同时让图元变色或闪烁——
 * 那属于图元自己的属性（经 `QScadaBoardController::setPropertyWithId()` 设置），
 * 本类只负责把值送到 `updateValue()`。
 *
 * ## 已知边界
 *
 *  · 一个位号当前只绑定一个图元（绑定表是 key -> 单个寻址）。同一个位号要同时显示
 *    在多个画面上时，可以为第二块画面再建一个桥实例；若以后真有需要，把绑定表
 *    改成 key -> 寻址列表即可，接口不用动。
 *  · 绑定表是按位号 key 存的哈希表，值到达时只做一次哈希查找，不做任何线性扫描
 *    ——这条路径在 100 ms 周期、几百个位号下每秒会被调用上千次。
 *  · 控制器允许后设（`setController()`）：真实工程里往往是先建桥接好数据，
 *    等操作员打开画面、控制器创建出来之后再挂上去。
 */
class QScadaViewBridge : public QObject
{
    Q_OBJECT
public:
    explicit QScadaViewBridge(QScadaDataHub *hub, QScadaBoardController *controller,
                              QObject *parent = nullptr);
    ~QScadaViewBridge() override;

    QScadaDataHub *hub() const { return mHub; }
    QScadaBoardController *controller() const { return mController; }
    //! 允许后设控制器（画面还没创建时可以先建桥）。
    void setController(QScadaBoardController *controller);

    /*!
     * 绑定一个位号到某个图元。重复绑定同一个位号会覆盖旧的寻址。
     * \param deviceIp 原框架里的设备 IP（与点表里的 deviceIp 一致）。
     * \param boardId  画面 ID；\param objectId 图元 ID。
     */
    void bindTag(const QString &tagKey, const QString &deviceIp, int boardId, int objectId);
    /*!
     * 按点表批量绑定：直接用位号自带的 deviceIp()/boardId()/objectId()。
     * 未启用、或没配设备 IP 的位号会被跳过（前者是点表里临时停用的点，
     * 后者根本没法视图寻址，绑了也刷不动）。
     */
    void bindTags(const QList<QScadaTagDefinition> &definitions);
    void unbindTag(const QString &key);
    void clearBindings();
    int bindingCount() const { return mBindings.size(); }

    /*!
     * 界面刷新频率上限（Hz），默认 10。
     * 实际节拍是 1000/hz 毫秒（例如 3 Hz -> 333 ms，比 3 Hz 略慢，这是可以接受的
     * 近似：界面刷新不需要精确的频率，而 QTimer 只接受整数毫秒）。
     */
    void setMaxUpdateRate(int hz);
    int maxUpdateRate() const { return mMaxUpdateRate; }

    //! 真正落到 updateValue() 的次数。
    quint64 appliedUpdateCount() const { return mAppliedCount; }
    //! 被合并掉（同一个图元在两次刷新之间来了多个值）、没有真正刷新的次数。
    quint64 coalescedCount() const { return mCoalescedCount; }
    //! 被质量策略挡掉的次数（见 setShowBadQuality）。
    quint64 filteredCount() const { return mFilteredCount; }

    /*!
     * true（默认）：质量不好也照常刷新界面，让操作员看见异常；
     * false：只在质量 Good 且值有效时刷新，坏质量时图元保持上一次的好值。
     */
    void setShowBadQuality(bool on);
    bool showBadQuality() const { return mShowBadQuality; }

public slots:
    /*!
     * 中枢的值变化槽。参数用 `QScadaTagValue` 而不是裸值：桥虽然只把值送进
     * updateValue()，但质量码决定了这次值要不要刷、要不要另做处理。
     */
    void onTagValueChanged(const QScadaTagValue &tagValue);
    //! 立刻把待刷新的值推给界面（例如画面刚打开，不想等最多一个节拍）。
    void flushPending();

signals:
    //! 一次界面刷新真正发生（供自检、单元测试与"节流效果"统计使用）。
    void viewUpdated(QString deviceIp, int boardId, int objectId, QVariant value);

private:
    //! 位号 -> 视图寻址。
    struct Binding {
        QString deviceIp;
        int boardId;
        int objectId;
        //! 预先算好的图元寻址键，见 makeViewKey()：放在绑定里，数据通路上就不用再拼字符串。
        QString viewKey;
    };

    //! 一次等待刷新的界面更新（按图元聚合，只保留最新值）。
    struct PendingUpdate {
        QString deviceIp;
        int boardId;
        int objectId;
        QVariant value;
        QScadaQuality quality;
    };

    static int refreshIntervalMs(int hz);
    static QString makeViewKey(const QString &deviceIp, int boardId, int objectId);

    QScadaDataHub *mHub;
    QScadaBoardController *mController;
    QHash<QString, Binding> mBindings;
    QHash<QString, PendingUpdate> mPending;
    QTimer *mFlushTimer;
    int mMaxUpdateRate;
    quint64 mAppliedCount;
    quint64 mCoalescedCount;
    quint64 mFilteredCount;
    bool mShowBadQuality;
};

#endif // QSCADAVIEWBRIDGE_H
