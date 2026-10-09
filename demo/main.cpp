// QSimpleScada 二次开发演示宿主：
//  原框架（QScadaBoardController + QScadaObjectQML 图元）+
//  二次开发采集层（点表 -> 数据中枢 -> 管理器 -> 仿真驱动）的真实数据流。
//
// 数据链路（可完整向面试官讲述）：
//  QScadaRegisterMap(点表) -> QScadaDataHub(中枢，盖时间戳/质量码)
//    -> QScadaDataSourceManager(按设备建驱动) -> QScadaSimulationSource(仿真采集)
//    -> hub::tagValueChanged -> controller::updateValue -> QML 图元 update()
// 运行后窗口里 3 个指示灯随仿真波形运动，超过阈值变红。

#include <QApplication>
#include <QGridLayout>
#include <QCoreApplication>
#include <QHostAddress>
#include <QTimer>
#include <QDebug>
#include <QDir>
#include <cstdio>

// 无控制台模式（Windows subsystem）下 qDebug 无处输出：用自定义消息处理器
// 把日志写到 exe 同目录的 demo_log.txt，界面不留黑窗、日志照样可查。
static void fileMessageHandler(QtMsgType type, const QMessageLogContext &ctx, const QString &msg)
{
    Q_UNUSED(type)
    Q_UNUSED(ctx)
    QDir::setCurrent(QCoreApplication::applicationDirPath());
    FILE *f = fopen("demo_log.txt", "a");
    if (f) {
        fprintf(f, "%s\n", msg.toUtf8().constData());
        fclose(f);
    }
}

#include "qscadaboardcontroller.h"
#include "qscadaboard.h"
#include "qscadadeviceinfo.h"
#include "qscadaobjectinfo.h"

#include "qscadaregistermap.h"
#include "qscadadatahub.h"
#include "qscadadatasourcemanager.h"
#include "qscadadatasource.h"
#include "qscadasimulationsource.h"
#include "qscadaviewbridge.h"
#include "qscadatagvalue.h"

namespace {

// 把一个位号登记进点表，并把它的对象 id 与图元 id 对齐（都是 objectId）。
void addTag(QScadaRegisterMap &map,
            const QString &key,
            const QString &displayName,
            const QString &unit,
            quint16 address,
            int objectId,
            QScadaTagDefinition::DataType type,
            double scale,
            double offset)
{
    QScadaTagDefinition tag(key, address);
    tag.setDisplayName(displayName);
    tag.setUnit(unit);
    tag.setDataType(type);
    tag.setScale(scale);
    tag.setOffset(offset);
    tag.setDeviceIp(QStringLiteral("127.0.0.1"));
    tag.setBoardId(0);
    tag.setObjectId(objectId);
    tag.setEnabled(true);
    map.addTag(tag);
}

} // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    qInstallMessageHandler(fileMessageHandler);

    // ---------- 1. 原框架：设备 + 仪表盘控制器 ----------
    QScadaBoardController *controller = new QScadaBoardController();
    QScadaDeviceInfo *devInfo = new QScadaDeviceInfo();
    devInfo->setName(QStringLiteral("Simulated CNC"));
    // 必须是合法 IP：QScadaBoardManager::deviceForIp 用 ip().toString() 精确匹配，
    // "SIM-01" 这类字符串会被 QHostAddress 解析成无效地址，导致 board 创建失败。
    devInfo->setIp(QHostAddress(QStringLiteral("127.0.0.1")));
    controller->appendDevice(devInfo);
    controller->initBoardForDeviceIp(QStringLiteral("127.0.0.1"));
    controller->setEditingMode(false); // 演示态：图元不可拖动

    QList<QScadaBoard*> boards = controller->getBoardListForDeviceIp(QStringLiteral("127.0.0.1"));
    QScadaBoard *board = boards.isEmpty() ? nullptr : boards.first();

    // ---------- 2. 点表：3 个位号，objectId 与图元 id 一一对应 ----------
    QScadaRegisterMap map;
    addTag(map, QStringLiteral("TEMP_OVEN"),    QStringLiteral("炉温 Oven Temp"),
           QStringLiteral("°C"),    0x0000, 1, QScadaTagDefinition::DataTypeFloat32, 1.0, 0.0);
    addTag(map, QStringLiteral("SPINDLE_RPM"),  QStringLiteral("主轴 Spindle"),
           QStringLiteral("rpm"),  0x0002, 2, QScadaTagDefinition::DataTypeFloat32, 1.0, 0.0);
    addTag(map, QStringLiteral("PROD_COUNT"),   QStringLiteral("产量 Count"),
           QStringLiteral("件"),   0x0004, 3, QScadaTagDefinition::DataTypeUInt32, 1.0, 0.0);

    // ---------- 3. 图元：3 个 QML 指示灯 ----------
    // 用 exe 所在目录定位 QML 图元（构建目录可能与源码目录分离，__FILE__ 不可靠）
    const QString qmlPath = QCoreApplication::applicationDirPath()
                            + QStringLiteral("/indicator.qml");
    qDebug() << "[demo] QML widget:" << qmlPath;

    struct WidgetSpec {
        int id;
        QRect geometry;
        QString caption;
        QString unit;
        double threshold;
    };
    const QList<WidgetSpec> widgets = {
        { 1, QRect(20,  20, 300, 110), QStringLiteral("炉温 Oven Temp"), QStringLiteral("°C"),  60.0 },
        { 2, QRect(20, 150, 300, 110), QStringLiteral("主轴 Spindle"),   QStringLiteral("rpm"), 4200.0 },
        { 3, QRect(20, 280, 300, 110), QStringLiteral("产量 Count"),     QStringLiteral("件"),  100000.0 },
    };

    for (const WidgetSpec &w : widgets) {
        QScadaObjectInfo *info = new QScadaObjectInfo();
        info->setId(w.id);
        info->setType(QScadaObjectTypeQML);
        info->setUIResourcePath(qmlPath);
        info->setGeometry(w.geometry);
        QMultiMap<QString, QVariant> props;
        props.insert(QStringLiteral("caption"), w.caption);
        props.insert(QStringLiteral("unit"), w.unit);
        props.insert(QStringLiteral("threshold"), w.threshold);
        info->setUIProperties(props);
        if (board)
            board->initNewObject(info);
        else
            qWarning() << "[demo] 未取到 board，图元未创建";
    }

    // ---------- 4. 二次开发采集层：中枢 + 管理器 + 仿真驱动 ----------
    QScadaDataHub *hub = new QScadaDataHub();
    hub->registerTags(map.tags());

    QScadaDataSourceManager *mgr = new QScadaDataSourceManager(hub);
    QStringList startErrors;
    const int started = mgr->startFromRegisterMap(map, QStringLiteral("simulator"), &startErrors);
    qDebug() << "[demo] sources started:" << started;
    for (const QString &e : startErrors)
        qWarning() << "[demo] start error:" << e;

    // 显式配置波形，让演示效果直观（正弦温度 / 锯齿转速 / 计数产量）
    if (QScadaSimulationSource *sim = qobject_cast<QScadaSimulationSource*>(mgr->source(QStringLiteral("127.0.0.1")))) {
        sim->setWaveform(QStringLiteral("TEMP_OVEN"), QScadaSimulationSource::Sine);
        sim->setOffset(QStringLiteral("TEMP_OVEN"), 50.0);
        sim->setAmplitude(QStringLiteral("TEMP_OVEN"), 30.0);
        sim->setPeriod(QStringLiteral("TEMP_OVEN"), 12.0);
        sim->setNoise(QStringLiteral("TEMP_OVEN"), 0.01);

        sim->setWaveform(QStringLiteral("SPINDLE_RPM"), QScadaSimulationSource::Sawtooth);
        sim->setOffset(QStringLiteral("SPINDLE_RPM"), 3500.0);
        sim->setAmplitude(QStringLiteral("SPINDLE_RPM"), 1500.0);
        sim->setPeriod(QStringLiteral("SPINDLE_RPM"), 8.0);
        sim->setNoise(QStringLiteral("SPINDLE_RPM"), 0.005);

        sim->setWaveform(QStringLiteral("PROD_COUNT"), QScadaSimulationSource::Counter);
    }

    // ---------- 5. 桥：中枢 -> 视图（用自研 QScadaViewBridge，含节流与质量策略） ----------
    QScadaViewBridge *bridge = new QScadaViewBridge(hub, controller);
    bridge->bindTags(map.tags());
    qDebug() << "[demo] view bridge bindings:" << bridge->bindingCount();

    // 每秒打印一次中枢与桥的统计，用于确认数据流是否真的在跑
    QTimer *statsTimer = new QTimer(hub);
    QObject::connect(statsTimer, &QTimer::timeout, [hub, bridge]() {
        qDebug() << "[demo] hub: published=" << hub->publishedCount()
                 << " cached=" << hub->cachedValueCount()
                 << " pts/s=" << hub->pointsPerSecond()
                 << " | bridge: applied=" << bridge->appliedUpdateCount()
                 << " coalesced=" << bridge->coalescedCount();
    });
    statsTimer->start(1000);

    // ---------- 6. 主窗口 ----------
    QWidget window;
    window.setWindowTitle(QStringLiteral("QSimpleScada Demo — 二次开发：仿真采集数据流"));
    window.resize(1080, 640);
    QGridLayout *layout = new QGridLayout(&window);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(controller);
    window.show();

    qDebug() << "[demo] 数据链路已启动：点表 -> 中枢 -> 仿真驱动 -> updateValue -> QML 图元";
    return app.exec();
}
