#ifndef QSCADAMESPAYLOAD_H
#define QSCADAMESPAYLOAD_H

#include <QString>
#include <QDateTime>
#include <QJsonObject>
#include <QJsonValue>
#include <QMap>
#include <QVariant>

/*!
 * \brief MES 上报报文的组装器（无状态，全部为静态方法）。
 *
 * 职责边界：本类只回答"一条报文长什么样"，不关心"什么时候发、发失败怎么办"。
 * 拆开的理由是 MES 报文格式是最容易变、也最需要被验证的部分——甲方换个
 * 字段名就要改一次。把它做成纯函数后可以脱离网络直接断言 JSON 结构，
 * 也能用同一套报文做接口联调文档的样例，改动不会波及重试与队列逻辑。
 *
 * ============================ 报文三段式约定 ============================
 * 所有报文都由"信封字段 + 业务字段"组成，其中信封字段有三个是强制的：
 *
 *  1. messageId —— 全局唯一 ID（QUuid::createUuid().toString(QUuid::WithoutBraces)）
 *  2. sequenceNo —— 单调递增序列号（quint64）
 *  3. timestamp —— ISO8601 带时区与毫秒（Qt::ISODateWithMs）
 *
 * 为什么必须有 messageId + sequenceNo：
 * 现场网络抖动、交换机重启、WiFi 漫游都会造成"请求已经到达 MES，但响应
 * 没回到上位机"，客户端按超时判定失败并重传。如果 MES 侧不能去重，同一次
 * 生产计数就会被统计两遍甚至三遍——产量虚高、OEE 算错、工单数量对不上，
 * 这是 MES 对接最经典也最容易被忽略的坑。因此：
 *
 *  - messageId 用于**幂等去重**：MES 侧以它为主键建唯一索引，重复到达时
 *    直接返回成功（而不是报错），客户端重传才能安全收敛；
 *  - sequenceNo 用于**顺序与丢包检测**：同设备内单调递增，MES 侧发现跳号
 *    就知道中间丢过报文，可以告警或触发补抄；时间戳因客户端改系统时间、
 *    NTP 校时而可能回退，单靠 timestamp 无法判断先后。
 *
 * timestamp 之所以要带时区（Qt::ISODateWithMs 会输出 +08:00 这样的偏移），
 * 是因为现场上位机、MES 服务器、报表系统常常不在同一时区，用本地时间字符串
 * 会直接导致"夜班产量算到白班"。
 */
class QScadaMesPayload
{
public:
    /*!
     * 心跳。MES 侧靠它维护"设备在线"状态，并识别软件版本以便远程推升级。
     * uptimeMs 为客户端本次运行时长，用于区分"设备掉线重启"和"网络闪断"。
     */
    static QJsonObject heartbeat(const QString &deviceCode, const QDateTime &timestamp,
                                 const QString &softwareVersion = QString(),
                                 qint64 uptimeMs = -1);

    /*!
     * 产量上报。goodCount / scrapCount 是**增量**而不是累计值：
     * 增量报文天然可以重放与去重，而累计值一旦乱序到达就会把计数改小，
     * MES 侧还得自己做最大值保护，得不偿失。
     */
    static QJsonObject productionCount(const QString &deviceCode,
                                       const QString &workOrderNo,
                                       const QString &partNo,
                                       int goodCount,
                                       int scrapCount,
                                       const QDateTime &timestamp);

    /*!
     * 设备状态。statusCode 用固定枚举字符串（running/idle/fault/stop），
     * statusText 放人可读的中文描述——MES 与看板按 code 判断逻辑，
     * 现场人员看 text，避免界面直接显示英文枚举值。
     */
    static QJsonObject equipmentStatus(const QString &deviceCode,
                                       const QString &statusCode,
                                       const QString &statusText,
                                       const QDateTime &timestamp);

    /*!
     * 报警上报。level 约定为 info / warning / fault / critical，
     * triggerValue 记录触发时的实际值（例如温度 87.3），便于质量追溯时
     * 判断"是报警阈值设错了还是工艺真的跑偏了"。
     */
    static QJsonObject alarmReport(const QString &deviceCode,
                                   const QString &alarmCode,
                                   const QString &level,
                                   const QString &message,
                                   const QVariant &triggerValue,
                                   const QDateTime &timestamp);

    /*!
     * OEE 指标。四个比值统一用 0.0 ~ 1.0 的小数而不是百分数，
     * 避免"到底传 85 还是 0.85"这种跨系统约定不一致的低级错误。
     */
    static QJsonObject oeeReport(const QString &deviceCode,
                                 double availability,
                                 double performance,
                                 double quality,
                                 double oee,
                                 const QDateTime &timestamp);

    /*!
     * 关键位号快照。只报关键位号，不做全量点表上报：
     * 全量点表数据量大、变化快，适合走采集层的历史库，MES 关心的是
     * 与工单/质量相关的那几个值（节拍、温度、压力……）。
     */
    static QJsonObject tagSnapshot(const QString &deviceCode,
                                   const QMap<QString, QVariant> &values,
                                   const QDateTime &timestamp);

    /*!
     * 报文类型名（heartbeat / production / ...）。
     * 客户端用它拼 endpoint 路径，并作为 reportSent / reportFailed 信号的
     * 第一个参数，日志与界面据此区分是哪类报文出了问题。
     */
    static QString messageTypeOf(const QJsonObject &payload);

    //! 软件版本号，心跳里带上，便于现场确认"这台机器跑的是哪个版本"。
    static QString softwareVersion();

    /*!
     * 设置报文里自动携带的车间编号。
     * 放在这里而不是让每个静态方法多一个参数：车间编号属于"部署环境常量"，
     * 与设备编号同源（都来自配置），逐个方法传递只会让调用点更容易漏填。
     * QScadaMesClient::start() 会按配置自动调用，一般无需手工设置。
     */
    static void setWorkshopCode(const QString &workshopCode);
    static QString workshopCode();

    //! 生成一个新的全局唯一 messageId（去掉花括号，便于直接当数据库主键）。
    static QString newMessageId();

    /*!
     * 取下一个单调递增序列号。
     * 用原子变量而不是普通计数器：报文既可能由界面线程的业务事件触发，
     * 也可能由客户端自己的定时器触发，两者并发时会取到相同值，
     * 从而让 MES 侧的顺序检测失效。
     */
    static quint64 nextSequenceNo();

    /*!
     * 复位序列号并设定起始值。
     * 用于离线队列从文件恢复之后：队列里可能存着重启前 sequenceNo = 120 的
     * 报文，若本次从 1 重新计数，MES 侧会看到序号回退，误判为乱序或重放。
     */
    static void resetSequenceNo(quint64 startValue = 0);

    /*!
     * 把 QVariant 转成可放入 JSON 的值。
     * 之所以不用 QVariant::type() 判断类型：该接口在 Qt 6 中已弃用，
     * 会破坏 Qt 5.12 / Qt 6 双兼容；这里改用"能否转换"的探测方式。
     */
    static QJsonValue jsonValue(const QVariant &value);

private:
    /*!
     * 信封字段的公共组装：messageId / sequenceNo / timestamp / messageType
     * 加设备与车间信息。统一走这里是为了保证"不可能漏字段"——
     * 少一个 messageId，MES 侧就没法去重，而漏字段是联调阶段最常见的返工原因。
     */
    static QJsonObject envelope(const QString &messageType,
                                const QString &deviceCode,
                                const QDateTime &timestamp);

    //! 时间戳格式统一收口在这里，全局只此一处调用 toString。
    static QString formatTimestamp(const QDateTime &timestamp);
};

#endif // QSCADAMESPAYLOAD_H
