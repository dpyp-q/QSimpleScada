#ifndef QSCADADATAHUB_H
#define QSCADADATAHUB_H

#include <QObject>
#include <QHash>
#include <QList>
#include <QElapsedTimer>

#include "qscadatagdefinition.h"
#include "qscadatagvalue.h"

class QTimer;

/*!
 * \brief 数据中枢：采集层与上层应用之间唯一的汇合点。
 *
 * ## 为什么必须有这一层
 *
 * 原框架唯一的数据入口是 `QScadaBoardController::updateValue(deviceIp, boardId, id, value)`，
 * 它是**视图寻址**的——只能把值送到"当前显示的那块仪表盘"上的某个图元。
 * 如果把历史库、报警、MES 上报直接挂在它上面，会同时踩两个坑：
 *
 *  1. 没有被打开过的画面，其位号根本不会产生数据，历史库会变成
 *     "操作员看过什么才记录什么"，这在追溯事故时是致命的；
 *  2. `QVariant` 里没有时间戳和质量码，无法表达"这个 0 是真的 0 还是断线"。
 *
 * 所以这里做一个与界面无关的中枢：驱动把值交给它，它盖上时间戳与质量码，
 * 再广播给所有订阅者——视图、报警引擎、历史库、MES 客户端彼此不知道对方存在。
 * 新增一个订阅者（例如以后接 OPC UA 服务端）不需要改动任何已有代码。
 *
 * ## 线程模型
 *
 * 采集驱动可以在各自的工作线程里运行。`publish()` 被声明为槽，
 * 驱动应当用 `Qt::QueuedConnection` 连接它，于是跨线程的数据会经由事件队列
 * 汇入中枢所属线程，之后所有订阅者都在同一个线程里被通知，
 * 不会出现多线程同时读写界面对象的问题。
 */
class QScadaDataHub : public QObject
{
    Q_OBJECT
public:
    explicit QScadaDataHub(QObject *parent = nullptr);

    //! 注册点表元数据（设备归属、工程量纲等），供快照与界面展示使用。
    void registerTags(const QList<QScadaTagDefinition> &definitions);
    void registerTag(const QScadaTagDefinition &definition);
    void clearDefinitions();

    QScadaTagDefinition definition(const QString &key) const;

    //! 查询缓存中的最新值。
    QScadaTagValue tagValue(const QString &key) const;
    bool hasValue(const QString &key) const;
    QList<QScadaTagValue> snapshot() const;
    QList<QScadaTagValue> snapshotForDevice(const QString &deviceIp) const;
    int cachedValueCount() const { return mValues.size(); }

    //! 设备级质量：断线时该设备所有位号都应被标记为 NotConnected。
    QScadaQuality deviceQuality(const QString &deviceIp) const;

    //! 采集速率统计（点/秒），用于界面显示与性能验证。
    double pointsPerSecond() const { return mPointsPerSecond; }
    quint64 publishedCount() const { return mPublishedCount; }

public slots:
    /*!
     * 驱动提交一个新值。质量默认 Good；
     * 若该位号所属设备当前是断线状态，会被自动降级为 NotConnected，
     * 避免驱动在断开时仍上报上一次读到的陈旧值。
     */
    void publish(const QString &tagKey, const QVariant &value,
                 QScadaQuality quality = QualityGood);

    //! 设备连接状态变化时调用，会同时影响该设备下所有位号的质量。
    void setDeviceQuality(const QString &deviceIp, QScadaQuality quality);

    void reset();

signals:
    void tagValueChanged(const QScadaTagValue &tagValue);
    void deviceQualityChanged(const QString &deviceIp, QScadaQuality quality);
    void statisticsChanged();

private slots:
    void onRateTick();

private:
    QHash<QString, QScadaTagDefinition> mDefinitions;
    QHash<QString, QScadaTagValue> mValues;
    QHash<QString, QScadaQuality> mDeviceQuality;

    QTimer *mRateTimer;
    QElapsedTimer mRateClock;
    quint64 mPublishedCount;
    quint64 mRateWindowCount;
    double mPointsPerSecond;
};

#endif // QSCADADATAHUB_H
