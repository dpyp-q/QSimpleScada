#ifndef QSCADAHISTORYRECORD_H
#define QSCADAHISTORYRECORD_H

#include <QString>
#include <QDateTime>
#include <QMetaType>

#include "qscadadataquality.h"

/*!
 * \brief 一条历史记录（值语义对象）。
 *
 * 设计取舍：
 *
 * 1. 值统一用 double 存。现场点表里既有 4-20mA 换算出来的浮点温度，也有
 *    布尔量的启停状态、整数累计量。如果为每种类型各建一张表或各用一个字段，
 *    查询、降采样、画曲线都要写多套分支；统一成 double 以后，
 *    一个聚合函数（MIN/MAX/AVG）就能覆盖所有位号。布尔量约定存 0/1，
 *    int16/uint16/int32 也都能无损放进 double（double 有 53 位尾数，
 *    表达 32 位整数绰绰有余），所以这个统一不会丢精度。
 *
 * 2. 但"统一成 double"会丢掉原始语义（例如枚举型状态字 3 到底是"手动"还是"故障"，
 *    或者一个 64 位累计量），所以额外保留一个可选的 rawText 字段：
 *    默认空串，需要时把驱动侧的原始文本（"RUN"/"STOP"、未换算的原始值）
 *    一起落库，代价只是多一列，排查问题时不用再去翻当时的日志文件。
 *
 * 3. 纯值语义、无虚函数、无 QObject。历史记录会以几十万条的量级在内存缓冲、
 *    QList、查询结果之间搬运，做成重对象会让写入路径的拷贝开销变成瓶颈。
 *    这个类只有几个字段，QList 里按值存即可（QList 的元素本身也是隐式共享的）。
 *
 * 4. 对象自校验：isValid() 要求 tagKey 非空、时间戳有效、质量码在枚举范围内。
 *    写入前统一校验，避免脏数据进库以后才发现（历史库一旦写脏，清理成本极高）。
 */
class QScadaHistoryRecord
{
public:
    QScadaHistoryRecord()
        : mValue(0.0)
        , mQuality(Good)
    {
        // 默认时间戳取当前时间：绝大多数调用点就是"刚采集到"，让调用方少写一个参数。
        mTimestamp = QDateTime::currentDateTime();
    }

    /*!
     * \brief 最常用的四个参数构造。
     * \param tagKey    位号唯一键，对应 QScadaTagDefinition::key()
     * \param value     工程量数值（已应用 scale/offset）；布尔量传 0.0 / 1.0
     * \param quality   数据质量码
     * \param timestamp 采集时间戳。必须全局统一口径（统一用上位机本地时间，
     *                  或统一用设备时间），否则跨时区/夏令时会让曲线错位。
     */
    QScadaHistoryRecord(const QString &tagKey, double value,
                        QScadaDataQuality quality, const QDateTime &timestamp)
        : mTagKey(tagKey)
        , mValue(value)
        , mQuality(quality)
        , mTimestamp(timestamp)
    {
    }

    //! 便利构造：默认 Good 质量，时间戳取当前时间。用于采集回调里直接落一条记录。
    QScadaHistoryRecord(const QString &tagKey, double value)
        : mTagKey(tagKey)
        , mValue(value)
        , mQuality(Good)
        , mTimestamp(QDateTime::currentDateTime())
    {
    }

    QString tagKey() const { return mTagKey; }
    void setTagKey(const QString &key) { mTagKey = key; }

    double value() const { return mValue; }
    void setValue(double v) { mValue = v; }

    QScadaDataQuality quality() const { return mQuality; }
    void setQuality(QScadaDataQuality q) { mQuality = q; }

    QDateTime timestamp() const { return mTimestamp; }
    void setTimestamp(const QDateTime &ts) { mTimestamp = ts; }

    /*!
     * 可选的原始文本。为什么用空串表示"没有"而不是 std::optional：
     * 项目要求兼容 Qt 5.12 且不使用 C++17 以上语法，std::optional 不可用；
     * 而空串语义在这里足够（"原始文本为空"和"字段为 NULL"没有实际区别）。
     */
    QString rawText() const { return mRawText; }
    void setRawText(const QString &text) { mRawText = text; }
    bool hasRawText() const { return !mRawText.isEmpty(); }

    /*!
     * 是否为一条可落库的记录。
     * 注意：Bad / Uncertain / NotConnected 都是**合法**记录——现场必须把"通讯中断"
     * 这件事本身记下来，否则曲线会缺一段，事后分不清是设备没数据还是上位机没存。
     * 这里只拦真正无意义的数据（无位号、无效时间戳、越界质量码）。
     */
    bool isValid() const
    {
        return !mTagKey.isEmpty()
                && mTimestamp.isValid()
                && qscadaQualityIsValid(static_cast<int>(mQuality));
    }

    /*!
     * 时间戳的毫秒表示（Unix epoch，UTC 基准）。
     * 降采样分桶、批量写入前的排序、跨驱动比较都靠它，避免各处反复做时区换算
     * （toMSecsSinceEpoch() 内部按 UTC 计算，不受本地时区影响，结果稳定可比）。
     * 注意：写库时不用这个整数，而是统一格式化成固定字符串
     * （见 qscadahistorystorage.cpp 里的 formatTimestamp），
     * 目的是让 SQLite 的 TEXT 时间列保持"字符串序 == 时间序"。
     */
    qint64 timestampMs() const { return mTimestamp.toMSecsSinceEpoch(); }

    //! 便于日志、断言、单测失败信息里直接打印。
    QString toString() const
    {
        return QStringLiteral("[%1] %2 = %3 (%4)")
                .arg(mTimestamp.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz")))
                .arg(mTagKey)
                .arg(mValue, 0, 'g', 12)
                .arg(qscadaQualityName(mQuality));
    }

private:
    QString mTagKey;
    double mValue;
    QScadaDataQuality mQuality;
    QDateTime mTimestamp;
    QString mRawText;
};

Q_DECLARE_METATYPE(QScadaHistoryRecord)

#endif // QSCADAHISTORYRECORD_H
