#ifndef QSCADAMESCONFIG_H
#define QSCADAMESCONFIG_H

#include <QString>
#include <QJsonObject>
#include <QJsonValue>

/*!
 * \brief MES 对接模块的连接参数与上报策略配置。
 *
 * 为什么把"连接参数"和"上报策略"放在同一个结构里：MES 对接在现场出问题时，
 * 九成不是代码写错了，而是"地址填错 / 密钥过期 / 上报频率把服务端打挂"。
 * 把这两类参数集中成一个可序列化的值对象，就能做到：
 *  1. 整份配置存进项目文件或独立 json，改参数不用重新编译；
 *  2. 界面上的"MES 设置"对话框只需读写这一个对象，不必了解客户端内部状态机；
 *  3. 现场排障时把 toJson() 打印出来（apiKey 需自行脱敏），一眼能看出改的是哪一项。
 *
 * 设计取舍：
 *  - 本类只描述"应该怎么连、多久报一次"，不含任何状态与网络行为，
 *    因此可以安全地按值传递、拷贝、跨线程只读共享，单元测试时也不依赖网络；
 *  - 全部实现内联在头文件里，使它可以被界面层、配置层、测试工程分别包含，
 *    不会引入额外的链接依赖（配置是纯数据，没有需要隐藏的实现细节）。
 */
class QScadaMesConfig
{
public:
    QScadaMesConfig()
        : enabled(false)
        , timeoutMs(5000)
        , retryCount(3)
        , retryBackoffMs(1000)
        , heartbeatIntervalMs(30000)
        , productionReportIntervalMs(0)
        , maxQueueSize(5000)
        , verifySsl(true)
    {
    }

    /*!
     * 是否启用 MES 对接。默认关闭：现场部署时 MES 地址、密钥往往要等甲方提供，
     * 默认关闭可以保证"没配置也能正常开机"，不会因为连不上 MES 让上位机不可用。
     */
    bool enabled;

    //! MES REST 根地址，末尾不带斜杠。例如 http://mes.example.com/api/v1
    QString baseUrl;

    /*!
     * 接口密钥。这里存明文是为了能随配置文件一起下发；实际交付时该字段
     * 应来自受 ACL 保护的文件或环境变量，日志与界面中必须先脱敏再输出。
     */
    QString apiKey;

    //! 设备/工位编号，MES 侧用它定位"哪台设备"报上来的数据，启用时必填。
    QString deviceCode;

    //! 车间/产线编号，供 MES 侧按车间维度做看板与统计。
    QString workshopCode;

    //! 单次 HTTP 请求超时（毫秒）。默认 5000。
    int timeoutMs;

    /*!
     * 单条报文失败后的最大重试次数（不含首次发送）。默认 3。
     * 只对"可重试"的失败消耗额度：网络错误、超时、HTTP 5xx/429；
     * 业务失败（MES 返回 code != 0）与 4xx 重试没有意义，不消耗这个额度。
     */
    int retryCount;

    /*!
     * 重试退避基数（毫秒），默认 1000，按指数增长：1s、2s、4s……
     * 用指数退避而不是固定间隔，是为了避免 MES 侧短暂过载时被我们的
     * 重试流量持续打爆，同时给服务端留出恢复时间。
     */
    int retryBackoffMs;

    //! 心跳周期（毫秒），默认 30000。MES 侧通常靠它判断设备在线状态。
    int heartbeatIntervalMs;

    /*!
     * 产量自动上报周期（毫秒）。<= 0 表示关闭自动上报，改由上层业务
     * （工单完成、班次结束等）主动调用 reportProduction()。
     * 默认 0：产量是有业务含义的数据，什么时候该报应由业务决定，
     * 库不替用户猜——报早了 MES 认为是中间态，报晚了影响工单闭环。
     */
    int productionReportIntervalMs;

    /*!
     * 离线队列容量上限，默认 5000 条。超出后丢最旧的并计数。
     * 设这个上限是为长时间断网兜底：一台设备按秒级上报，断网一天就是几万条，
     * 不设上限会吃光内存并拖垮上位机主界面。
     */
    int maxQueueSize;

    /*!
     * 是否校验 HTTPS 证书。默认 true。
     * 工业现场大量使用自签证书，此时才允许关掉；由厂级 CA 签发证书的环境
     * 必须保持开启，否则等于放弃了中间人攻击防护。
     */
    bool verifySsl;

    /*!
     * 离线队列的持久化文件路径。为空表示不持久化（默认）。
     * 做成配置项而不是写死在代码里：上位机可能装在受保护的 Program Files
     * 目录，需要由部署方指定一个有写权限的数据目录。
     */
    QString queueFilePath;

    //! 合法性的最低前提：启用时必须同时给出根地址与设备编号。
    bool isValid() const
    {
        return !baseUrl.trimmed().isEmpty() && !deviceCode.trimmed().isEmpty();
    }

    //! 把超出范围的字段夹到安全区间，避免一个手误的配置让客户端行为异常。
    void normalize()
    {
        baseUrl = baseUrl.trimmed();
        // 去掉末尾斜杠：拼接 endpoint 时不会出现 "//"，部分反向代理会把
        // 双斜杠当非法路径直接返回 404，这种问题从日志里很难看出来。
        while (baseUrl.endsWith(QLatin1Char('/')))
            baseUrl.chop(1);
        apiKey = apiKey.trimmed();
        deviceCode = deviceCode.trimmed();
        workshopCode = workshopCode.trimmed();
        queueFilePath = queueFilePath.trimmed();

        // 超时留一个下限：小于 200ms 的阈值在工业网络里几乎必然误判超时，
        // 会造成"请求其实成功了但我们当成失败又重发一遍"。
        if (timeoutMs < 200)
            timeoutMs = 200;
        if (retryCount < 0)
            retryCount = 0;
        if (retryBackoffMs < 0)
            retryBackoffMs = 0;
        if (heartbeatIntervalMs < 1000)
            heartbeatIntervalMs = 1000;
        if (productionReportIntervalMs < 0)
            productionReportIntervalMs = 0;
        // 队列至少要能装下一条数据，否则客户端永远发不出东西。
        if (maxQueueSize < 1)
            maxQueueSize = 1;
    }

    QJsonObject toJson() const
    {
        QJsonObject obj;
        obj.insert(QStringLiteral("enabled"), enabled);
        obj.insert(QStringLiteral("baseUrl"), baseUrl);
        obj.insert(QStringLiteral("apiKey"), apiKey);
        obj.insert(QStringLiteral("deviceCode"), deviceCode);
        obj.insert(QStringLiteral("workshopCode"), workshopCode);
        obj.insert(QStringLiteral("timeoutMs"), timeoutMs);
        obj.insert(QStringLiteral("retryCount"), retryCount);
        obj.insert(QStringLiteral("retryBackoffMs"), retryBackoffMs);
        obj.insert(QStringLiteral("heartbeatIntervalMs"), heartbeatIntervalMs);
        obj.insert(QStringLiteral("productionReportIntervalMs"), productionReportIntervalMs);
        obj.insert(QStringLiteral("maxQueueSize"), maxQueueSize);
        obj.insert(QStringLiteral("verifySsl"), verifySsl);
        obj.insert(QStringLiteral("queueFilePath"), queueFilePath);
        return obj;
    }

    static QScadaMesConfig fromJson(const QJsonObject &obj)
    {
        // 以默认构造的值为基准：配置项缺失时保持默认，而不是变成 0 或空串，
        // 否则漏写 timeoutMs 会导致所有请求立刻超时。
        QScadaMesConfig cfg;
        cfg.enabled = readBool(obj, QStringLiteral("enabled"), cfg.enabled);
        cfg.baseUrl = readString(obj, QStringLiteral("baseUrl"), cfg.baseUrl);
        cfg.apiKey = readString(obj, QStringLiteral("apiKey"), cfg.apiKey);
        cfg.deviceCode = readString(obj, QStringLiteral("deviceCode"), cfg.deviceCode);
        cfg.workshopCode = readString(obj, QStringLiteral("workshopCode"), cfg.workshopCode);
        cfg.timeoutMs = readInt(obj, QStringLiteral("timeoutMs"), cfg.timeoutMs);
        cfg.retryCount = readInt(obj, QStringLiteral("retryCount"), cfg.retryCount);
        cfg.retryBackoffMs = readInt(obj, QStringLiteral("retryBackoffMs"), cfg.retryBackoffMs);
        cfg.heartbeatIntervalMs = readInt(obj, QStringLiteral("heartbeatIntervalMs"),
                                          cfg.heartbeatIntervalMs);
        cfg.productionReportIntervalMs = readInt(obj, QStringLiteral("productionReportIntervalMs"),
                                                 cfg.productionReportIntervalMs);
        cfg.maxQueueSize = readInt(obj, QStringLiteral("maxQueueSize"), cfg.maxQueueSize);
        cfg.verifySsl = readBool(obj, QStringLiteral("verifySsl"), cfg.verifySsl);
        cfg.queueFilePath = readString(obj, QStringLiteral("queueFilePath"), cfg.queueFilePath);
        cfg.normalize();
        return cfg;
    }

private:
    /*!
     * 按字段声明的类型取值，而不是"JSON 里是什么类型就直接用"。
     * 手写或脚本生成的配置很容易把 5000 写成 "5000"，直接取用会得到 0，
     * 进而让 timeoutMs 变成 0，这类问题在现场极难排查，所以在入口统一转换。
     */
    static QString readString(const QJsonObject &obj, const QString &key,
                              const QString &fallback)
    {
        const QJsonValue v = obj.value(key);
        return v.isString() ? v.toString() : fallback;
    }

    static int readInt(const QJsonObject &obj, const QString &key, int fallback)
    {
        const QJsonValue v = obj.value(key);
        if (v.isDouble())
            return v.toInt(fallback);
        if (v.isString())
            return v.toString().toInt();
        return fallback;
    }

    static bool readBool(const QJsonObject &obj, const QString &key, bool fallback)
    {
        const QJsonValue v = obj.value(key);
        if (v.isBool())
            return v.toBool(fallback);
        if (v.isDouble())
            return v.toInt(0) != 0;
        if (v.isString()) {
            // 兼容 ini 风格配置里常见的 "true"/"1"/"yes" 写法。
            const QString s = v.toString().trimmed().toLower();
            if (s == QLatin1String("true") || s == QLatin1String("1") || s == QLatin1String("yes"))
                return true;
            if (s == QLatin1String("false") || s == QLatin1String("0") || s == QLatin1String("no"))
                return false;
        }
        return fallback;
    }
};

#endif // QSCADAMESCONFIG_H
