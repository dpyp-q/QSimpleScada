#include "qscadamespayload.h"
#include "qscadamesconfig.h"

#include <QJsonValue>
#include <QJsonArray>
#include <QUuid>
#include <QAtomicInteger>
#include <QDateTime>
#include <QTimeZone>

namespace {

/*!
 * 报文类型标识。固定成常量而不是各方法里手写字符串：
 * 客户端要用它拼 endpoint 路径，还要用它作为信号的参数，
 * 一旦两处写法不一致（"equipment_status" 与 "equipmentStatus"），
 * 就会出现"日志里明明发出去了、MES 侧却查不到这个接口"的排查噩梦。
 */
const char *const MessageTypeHeartbeat = "heartbeat";
const char *const MessageTypeProduction = "production";
const char *const MessageTypeEquipmentStatus = "equipment_status";
const char *const MessageTypeAlarm = "alarm";
const char *const MessageTypeOee = "oee";
const char *const MessageTypeTagSnapshot = "tag_snapshot";

/*!
 * 序列号。用原子整数而不是普通 quint64：
 * 报文可能同时由界面线程（人工确认工单后补报产量）和客户端定时器线程/主线程
 * 的事件循环触发，普通自增会产生相同序号，让 MES 侧的顺序检测失效。
 * 静态存储期保证"进程内单调"，这正是 MES 侧需要的语义。
 */
QAtomicInteger<quint64> gSequenceNo(0);

/*!
 * 车间编号由全局配置提供，报文里自动带上，避免每个调用点都要多传一个参数。
 * 只在客户端线程读写，因此不加固。
 */
QString gWorkshopCode;

} // namespace

void QScadaMesPayload::setWorkshopCode(const QString &workshopCode)
{
    gWorkshopCode = workshopCode;
}

QString QScadaMesPayload::workshopCode()
{
    return gWorkshopCode;
}

QString QScadaMesPayload::softwareVersion()
{
    // 与 QSimpleScada.pro 的 VERSION 保持一致。写死在这里而不是引入 QSimpleScada
    // 的版本头，是为了让 MES 模块保持零依赖——它要能被单独编译进测试工程。
    return QStringLiteral("0.9.0");
}

QString QScadaMesPayload::newMessageId()
{
    // 去掉花括号：MES 侧一般直接把它当数据库主键或唯一索引，带 {} 会让
    // "ID 在日志里看起来一样、在数据库里匹配不上"。
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

quint64 QScadaMesPayload::nextSequenceNo()
{
    return gSequenceNo.fetchAndAddRelaxed(1) + 1;
}

void QScadaMesPayload::resetSequenceNo(quint64 startValue)
{
    gSequenceNo.storeRelaxed(startValue);
}

QJsonValue QScadaMesPayload::jsonValue(const QVariant &value)
{
    if (!value.isValid())
        return QJsonValue();
    // 日期时间必须在 fromVariant 之前拦下来：否则会退化成 Qt 默认格式的字符串，
    // MES 侧解析不出时区。统一按与报文 timestamp 相同的 ISO8601 规则输出。
    if (value.canConvert<QDateTime>())
        return QJsonValue(formatTimestamp(value.toDateTime()));
    // 注意这里没有用 QVariant::type()——它在 Qt 6 已弃用，会破坏双版本兼容。
    // 走到这里的 QVariant 由调用方保证是可 JSON 化的基本类型。
    return QJsonValue::fromVariant(value);
}

QString QScadaMesPayload::formatTimestamp(const QDateTime &timestamp)
{
    // 全局唯一的时间戳出口。三个细节都直接对应现场问题：
    //  1. 无效时间一律回退到当前时间，宁可时间略有偏差，也不要让 MES 侧
    //     收到空字符串导致整条报文被拒；
    //  2. 未指定时区时补上本机与 UTC 的偏移，Qt::ISODateWithMs 会把 +08:00
    //     带上，避免跨时区部署时"夜班产量算到白班"；
    //  3. 只有 Qt::LocalTime 才补偏移：上层若显式给了 UTC 或带偏移的时间，
    //     说明它自己清楚时区语义，这里不要擅自改写（否则国内会整体差 8 小时）。
    QDateTime ts = timestamp;
    if (!ts.isValid())
        ts = QDateTime::currentDateTime();
    if (ts.timeSpec() == Qt::LocalTime) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        // Qt 6 弃用了 Qt::OffsetFromUtc / setOffsetFromUtc，统一走 QTimeZone：
        // 取出该时刻的实际 UTC 偏移（含夏令时规则），再固化成固定偏移时区，
        // 这样 toString(Qt::ISODateWithMs) 仍会带上 +08:00 之类的后缀。
        const int offsetSeconds = ts.timeZone().offsetFromUtc(ts);
        ts.setTimeZone(QTimeZone(offsetSeconds));
#else
        // Qt 5 沿用原有写法，行为一致。
        const int offsetFromUtc = static_cast<int>(ts.offsetFromUtc());
        ts.setTimeSpec(Qt::OffsetFromUtc);
        ts.setOffsetFromUtc(offsetFromUtc);
#endif
    }
    return ts.toString(Qt::ISODateWithMs);
}

QJsonObject QScadaMesPayload::envelope(const QString &messageType,
                                       const QString &deviceCode,
                                       const QDateTime &timestamp)
{
    QJsonObject obj;
    obj.insert(QStringLiteral("messageId"), newMessageId());
    obj.insert(QStringLiteral("sequenceNo"), static_cast<double>(nextSequenceNo()));
    obj.insert(QStringLiteral("timestamp"), formatTimestamp(timestamp));
    obj.insert(QStringLiteral("messageType"), messageType);
    obj.insert(QStringLiteral("deviceCode"), deviceCode);
    if (!gWorkshopCode.isEmpty())
        obj.insert(QStringLiteral("workshopCode"), gWorkshopCode);
    return obj;
}

QString QScadaMesPayload::messageTypeOf(const QJsonObject &payload)
{
    return payload.value(QStringLiteral("messageType")).toString();
}

QJsonObject QScadaMesPayload::heartbeat(const QString &deviceCode,
                                        const QDateTime &timestamp,
                                        const QString &softwareVersion,
                                        qint64 uptimeMs)
{
    QJsonObject obj = envelope(QLatin1String(MessageTypeHeartbeat), deviceCode, timestamp);
    obj.insert(QStringLiteral("softwareVersion"),
               softwareVersion.isEmpty() ? QScadaMesPayload::softwareVersion() : softwareVersion);
    // 运行态字段固定写 running：能发出心跳就说明进程活着、网络通着，
    // 真正的"设备停机/故障"由 equipmentStatus 报文单独表达，两者语义不要混。
    obj.insert(QStringLiteral("runState"), QStringLiteral("running"));
    obj.insert(QStringLiteral("uptimeMs"), static_cast<double>(uptimeMs));
    return obj;
}

QJsonObject QScadaMesPayload::productionCount(const QString &deviceCode,
                                             const QString &workOrderNo,
                                             const QString &partNo,
                                             int goodCount,
                                             int scrapCount,
                                             const QDateTime &timestamp)
{
    QJsonObject obj = envelope(QLatin1String(MessageTypeProduction), deviceCode, timestamp);
    obj.insert(QStringLiteral("workOrderNo"), workOrderNo);
    obj.insert(QStringLiteral("partNo"), partNo);
    // 负数产量一定是上层传错了（例如从累计值做差时顺序颠倒），
    // 在这里夹到 0 并保留字段存在，比让 MES 侧收到 -3 更难排查要好。
    obj.insert(QStringLiteral("goodCount"), goodCount < 0 ? 0 : goodCount);
    obj.insert(QStringLiteral("scrapCount"), scrapCount < 0 ? 0 : scrapCount);
    obj.insert(QStringLiteral("totalCount"),
               static_cast<double>((goodCount < 0 ? 0 : goodCount) + (scrapCount < 0 ? 0 : scrapCount)));
    return obj;
}

QJsonObject QScadaMesPayload::equipmentStatus(const QString &deviceCode,
                                              const QString &statusCode,
                                              const QString &statusText,
                                              const QDateTime &timestamp)
{
    QJsonObject obj = envelope(QLatin1String(MessageTypeEquipmentStatus), deviceCode, timestamp);
    obj.insert(QStringLiteral("statusCode"), statusCode);
    obj.insert(QStringLiteral("statusText"), statusText);
    return obj;
}

QJsonObject QScadaMesPayload::alarmReport(const QString &deviceCode,
                                          const QString &alarmCode,
                                          const QString &level,
                                          const QString &message,
                                          const QVariant &triggerValue,
                                          const QDateTime &timestamp)
{
    QJsonObject obj = envelope(QLatin1String(MessageTypeAlarm), deviceCode, timestamp);
    obj.insert(QStringLiteral("alarmCode"), alarmCode);
    obj.insert(QStringLiteral("level"), level);
    obj.insert(QStringLiteral("message"), message);
    // triggerValue 允许为空：有些报警是逻辑报警（例如"首件未检"），
    // 没有对应的数值，此时不要塞一个空字符串进去污染 MES 侧的数值列。
    if (triggerValue.isValid())
        obj.insert(QStringLiteral("triggerValue"), jsonValue(triggerValue));
    return obj;
}

QJsonObject QScadaMesPayload::oeeReport(const QString &deviceCode,
                                        double availability,
                                        double performance,
                                        double quality,
                                        double oee,
                                        const QDateTime &timestamp)
{
    QJsonObject obj = envelope(QLatin1String(MessageTypeOee), deviceCode, timestamp);
    // 统一 0.0~1.0 的小数，不做 0~100 与 % 的自动兼容：
    // 自动"猜"单位会让 1.0 到底是 1% 还是 100% 变得不可判定，
    // 约定死一个量纲、在 README 里写清楚，比事后兼容可靠得多。
    obj.insert(QStringLiteral("availability"), availability);
    obj.insert(QStringLiteral("performance"), performance);
    obj.insert(QStringLiteral("quality"), quality);
    obj.insert(QStringLiteral("oee"), oee);
    return obj;
}

QJsonObject QScadaMesPayload::tagSnapshot(const QString &deviceCode,
                                          const QMap<QString, QVariant> &values,
                                          const QDateTime &timestamp)
{
    QJsonObject obj = envelope(QLatin1String(MessageTypeTagSnapshot), deviceCode, timestamp);
    QJsonObject tags;
    // QMap 已按 key 排序遍历，报文内容稳定，方便联调时两份 JSON 直接做 diff。
    QMap<QString, QVariant>::const_iterator it = values.constBegin();
    for (; it != values.constEnd(); ++it) {
        if (it.key().isEmpty())
            continue;
        tags.insert(it.key(), jsonValue(it.value()));
    }
    obj.insert(QStringLiteral("tags"), tags);
    return obj;
}
