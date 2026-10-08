#ifndef QSCADADATAQUALITY_H
#define QSCADADATAQUALITY_H

#include <QString>
#include <QMetaType>
#include <QObject>

/*!
 * \brief 历史数据质量码，思路参考 OPC DA / OPC UA 的 Quality 概念。
 *
 * 为什么工业历史库必须存质量码（面试常问，也是本项目与"学生作业式存库"的分水岭）：
 *
 * 1. 不能把"通讯断线时的 0"和"真实测量值 0"混为一谈。现场最容易出的事故就是：
 *    网线松动、PLC 掉电或网关重启，采集驱动读不到数据，于是往历史库里补了一个
 *    默认的 0；工艺人员事后翻曲线，看到"温度在凌晨 2 点掉到 0 又涨回来"，
 *    会误判成设备真的降温了，甚至据此写进事故报告。存了质量码，曲线才能把这一段
 *    画成虚线/灰色，而不是画成一条真实的 0 曲线。
 *
 * 2. 报表统计需要"可信数据"的口径。日均值、能耗累计、合格率这类 KPI，必须能
 *    把 Bad 区间的点剔除或按 Uncertain 打折，否则停机 8 小时会被算成"产量 0"，
 *    从而拉低整月 OEE。质量码是统计口径的开关。
 *
 * 3. 排查责任边界。质量码能区分"设备侧有问题"（Bad / NotConnected）还是
 *    "上位机侧有问题"（Uncertain：采集周期内没等到新值、时间戳过期），
 *    现场扯皮时这是唯一客观证据。
 *
 * 4. 报警联动。基于历史值做报警复算（例如事后补算超限）时，只有 Good 的数据才
 *    允许触发报警，否则通讯中断会刷出成百上千条假报警。
 *
 * 存储上质量码只占 1 字节（SQL Server TINYINT / SQLite INTEGER），代价极低，
 * 但换回来的是整条历史数据的可信度标注，这笔账非常划算。
 *
 * 实现上刻意做成"纯枚举 + 一组自由函数"，而不是带 Q_OBJECT 的类：
 * 质量码既不属于某个业务对象，又必须在数据库层、界面层、报警层之间自由传递，
 * 独立成一个只含头文件的轻量类型最省事（不引入新的编译单元）。
 */
enum QScadaDataQuality {
    //! 正常：本次采集成功，数据可信。
    Good = 0,
    //! 采集失败 / 通讯中断 / 从站返回异常码，值不可信（通常是上次值或占位 0）。
    Bad = 1,
    //! 存疑：采集周期内没有等到新值（超时未更新）、时间戳过期、值长时间未变化，
    //! 或来自未经校验的计算量。不像 Bad 那样完全不可用，但不能作为统计依据。
    Uncertain = 2,
    //! 尚未建立连接（驱动处于 Disconnected / Reconnecting），此时根本谈不上采集。
    NotConnected = 3
};

/*!
 * 质量码与字符串的互转。
 *
 * 为什么需要字符串形式：数据库里存 TINYINT 最省空间、比较最快，但运维人员
 * 直接查库（SELECT * FROM scada_history）时看到 0/1/2/3 无法判断含义，
 * 导出报表给工艺人员看也一样。所以提供一个可读名称，用于界面显示、
 * 日志输出、CSV 导出；需要按字符串存库时也可以用它。
 */
inline QString qscadaQualityName(QScadaDataQuality quality)
{
    switch (quality) {
    case Good:         return QStringLiteral("Good");
    case Bad:          return QStringLiteral("Bad");
    case Uncertain:    return QStringLiteral("Uncertain");
    case NotConnected: return QStringLiteral("NotConnected");
    }
    return QStringLiteral("Unknown");
}

//! 解析质量码名称（大小写不敏感）；无法识别时返回 fallback。
inline QScadaDataQuality qscadaQualityFromName(const QString &name,
                                               QScadaDataQuality fallback = Good)
{
    const QString key = name.trimmed().toLower();
    if (key == QLatin1String("good"))
        return Good;
    if (key == QLatin1String("bad"))
        return Bad;
    if (key == QLatin1String("uncertain"))
        return Uncertain;
    if (key == QLatin1String("notconnected") || key == QLatin1String("not_connected"))
        return NotConnected;
    return fallback;
}

/*!
 * 是否为"可用于统计与报警"的可信质量。
 * 只有 Good 返回 true；Uncertain 必须由调用方显式处理（例如画成虚线但保留数值），
 * 不能默认当成好数据——这是历史曲线可信与否的最后一道闸门。
 */
inline bool qscadaQualityIsTrusted(QScadaDataQuality quality)
{
    return quality == Good;
}

//! 质量码是否在合法取值范围内（写库前校验用，防止脏数据进历史库）。
inline bool qscadaQualityIsValid(int rawQuality)
{
    return rawQuality >= static_cast<int>(Good)
            && rawQuality <= static_cast<int>(NotConnected);
}

//! 名称转质量码的重载，供只拿到数值的场景使用（例如直接读库里的 TINYINT）。
inline QScadaDataQuality qscadaQualityFromInt(int rawQuality)
{
    return qscadaQualityIsValid(rawQuality)
            ? static_cast<QScadaDataQuality>(rawQuality)
            : Bad;   // 库里出现越界值说明数据已损坏，按最保守的 Bad 处理
}

Q_DECLARE_METATYPE(QScadaDataQuality)

#endif // QSCADADATAQUALITY_H
