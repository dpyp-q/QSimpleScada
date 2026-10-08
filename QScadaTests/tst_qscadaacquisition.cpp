/*!
 * \file tst_qscadaacquisition.cpp
 * \brief 采集层单元测试。
 *
 * 选择先测这三块，理由是它们**最容易出错、又最适合用测试锁住**：
 *
 *  1. **Modbus 报文编解码**——协议是逐字节约定的，一个字节位置写错就整条链路不通。
 *     现场排查这类问题时，如果没有单元测试，只能靠抓包逐字节比对，代价极高。
 *     CRC 用的是从规范算出来的**已知答案向量**，不是"自己算完自己验证"。
 *  2. **位号的字节序与工程量换算**——不同厂家 PLC 的 32 位量排布不同，
 *     这是工控现场最经典的坑（"读回来的浮点数是个天文数字"）。
 *  3. **数据中枢的质量码**——断线时把陈旧值当成正常值，会让历史库和报警全部失真。
 *
 * 测试全部基于 QtTest，不依赖任何硬件、GUI 或数据库，可在 CI 上跑。
 */

#include <QtTest>
#include <QSignalSpy>

#include "../QScadaAcquisition/qscadamodbuscodec.h"
#include "../QScadaAcquisition/qscadatagdefinition.h"
#include "../QScadaAcquisition/qscadaregistermap.h"
#include "../QScadaAcquisition/qscadadatahub.h"

class TestQScadaAcquisition : public QObject
{
    Q_OBJECT

private slots:
    // ---------------- Modbus 编解码 ----------------
    void modbusBuildsReadRequest();
    void modbusFramingDetectsPartialFrame();
    void modbusParsesReadResponse();
    void modbusParsesExceptionResponse();
    void modbusRejectsBadProtocolId();
    void modbusCrcMatchesKnownVectors();
    void modbusCrcDetectsCorruption();
    void modbusCoilPackUnpackRoundTrip();
    void modbusRegistersRoundTrip();

    // ---------------- 位号解码 ----------------
    void tagDecodesFloat32ForAllWordOrders();
    void tagDecodesSignedInt16();
    void tagAppliesScaleAndOffset();
    void tagEncodeDecodeRoundTrip();
    void tagRejectsShortPayload();

    // ---------------- 点表 ----------------
    void registerMapJsonRoundTrip();
    void registerMapFiltersByDevice();
    void registerMapRejectsMalformedJson();

    // ---------------- 数据中枢 ----------------
    void hubPublishesAndCachesValue();
    void hubDowngradesQualityWhenDeviceOffline();
    void hubNotifiesOnDeviceQualityChange();
};

// =====================================================================
// Modbus
// =====================================================================

void TestQScadaAcquisition::modbusBuildsReadRequest()
{
    // 读从站 1 的保持寄存器，起始地址 0，读 10 个。
    const QByteArray req = QScadaModbusCodec::buildReadRequest(
        0x0001, 0x01, QScadaModbusCodec::ReadHoldingRegisters, 0x0000, 0x000A);

    // Modbus TCP 请求帧 = MBAP(7) + PDU(5) = 12 字节；13 是 Modbus RTU 的
    // 计数方式（地址/CRC 各 1 字节），TCP 帧没有这两项。
    QCOMPARE(req.size(), 12);

    // MBAP: 事务号(2) 协议号(2) 长度(2) 单元号(1)
    QCOMPARE(static_cast<quint8>(req.at(0)), quint8(0x00)); // 事务号高字节
    QCOMPARE(static_cast<quint8>(req.at(1)), quint8(0x01)); // 事务号低字节
    QCOMPARE(static_cast<quint8>(req.at(2)), quint8(0x00)); // 协议号必须为 0
    QCOMPARE(static_cast<quint8>(req.at(3)), quint8(0x00));
    QCOMPARE(static_cast<quint8>(req.at(4)), quint8(0x00)); // 长度 = 1(单元号) + 5(PDU)
    QCOMPARE(static_cast<quint8>(req.at(5)), quint8(0x06));
    QCOMPARE(static_cast<quint8>(req.at(6)), quint8(0x01)); // 单元号
    QCOMPARE(static_cast<quint8>(req.at(7)), quint8(0x03)); // 功能码
    QCOMPARE(static_cast<quint8>(req.at(8)), quint8(0x00)); // 起始地址
    QCOMPARE(static_cast<quint8>(req.at(9)), quint8(0x00));
    QCOMPARE(static_cast<quint8>(req.at(10)), quint8(0x00)); // 数量
    QCOMPARE(static_cast<quint8>(req.at(11)), quint8(0x0A));
}

void TestQScadaAcquisition::modbusFramingDetectsPartialFrame()
{
    // 一条完整的 2 寄存器响应：13 字节
    const QByteArray frame = QByteArray::fromHex("000100000007010304000a0102");
    QCOMPARE(frame.size(), 13);

    // 头部（前 7 字节）没收全时必须判为"数据不够"（-1），
    // 绝不能当成坏报文丢弃，否则 TCP 分片到达时每一条都会失败。
    for (int i = 0; i < QScadaModbusCodec::MbapHeaderSize; ++i)
        QCOMPARE(QScadaModbusCodec::expectedResponseLength(frame.left(i)), -1);

    // 头部收全后长度就已确定：调用方据此判断"响应体还差几个字节"。
    for (int i = QScadaModbusCodec::MbapHeaderSize; i < frame.size(); ++i)
        QCOMPARE(QScadaModbusCodec::expectedResponseLength(frame.left(i)), 13);

    // 收全后正好等于自身长度
    QCOMPARE(QScadaModbusCodec::expectedResponseLength(frame), 13);

    // 粘包场景：两条响应挤在一次 readyRead 里，第一个长度仍然是 13，
    // 由调用方循环拆包，不会把第二条误当成第一条的一部分。
    const QByteArray doubled = frame + frame;
    QCOMPARE(QScadaModbusCodec::expectedResponseLength(doubled), 13);
}

void TestQScadaAcquisition::modbusParsesReadResponse()
{
    const QByteArray frame = QByteArray::fromHex("000100000007010304000a0102");

    const QScadaModbusCodec::Response r = QScadaModbusCodec::parseResponse(frame);
    QVERIFY(r.valid);
    QVERIFY(!r.isException);
    QCOMPARE(r.transactionId, quint16(0x0001));
    QCOMPARE(r.unitId, quint8(0x01));
    QCOMPARE(r.functionCode, quint8(0x03));
    QCOMPARE(r.data.size(), 4);

    // 两个寄存器 0x000A 与 0x0102
    const QList<quint16> regs = QScadaModbusCodec::toRegisters(r.data);
    QCOMPARE(regs.size(), 2);
    QCOMPARE(regs.at(0), quint16(0x000A));
    QCOMPARE(regs.at(1), quint16(0x0102));
}

void TestQScadaAcquisition::modbusParsesExceptionResponse()
{
    // 功能码 | 0x80 = 0x83，异常码 0x02（非法数据地址）
    const QByteArray frame = QByteArray::fromHex("000100000003018302");

    const QScadaModbusCodec::Response r = QScadaModbusCodec::parseResponse(frame);
    QVERIFY(r.valid);          // 结构合法
    QVERIFY(r.isException);    // 但设备报了错
    QCOMPARE(r.functionCode, quint8(0x03)); // 去掉异常位后是原功能码
    QCOMPARE(r.exceptionCode, quint8(0x02));
    QVERIFY(!r.errorString.isEmpty());
}

void TestQScadaAcquisition::modbusRejectsBadProtocolId()
{
    // 协议号不是 0 说明字节流已经失步，必须判为非法（调用方据此断开重连），
    // 否则会把后续的垃圾字节当成合法报文解析出乱七八糟的值。
    QByteArray frame = QByteArray::fromHex("000100000007010304000a0102");
    frame[3] = 0x01;

    QCOMPARE(QScadaModbusCodec::expectedResponseLength(frame), 0);
}

void TestQScadaAcquisition::modbusCrcMatchesKnownVectors()
{
    // 已知答案向量（按 Modbus RTU 规范独立算得，非"自己算自己验"）：
    //   01 03 00 00 00 0A            -> 0xCDC5，线上低字节在前 = C5 CD
    //   01 06 00 01 00 64            -> 0xE1D9，线上 = D9 E1
    //   01 10 00 00 00 02 04 00 0A 01 02 -> 0xFC53，线上 = 53 FC
    struct Vector { const char *hex; quint16 crc; const char *wire; };
    const Vector vectors[] = {
        { "01030000000a",            0xCDC5, "c5cd" },
        { "010600010064",            0xE1D9, "d9e1" },
        { "01100000000204000a0102",  0xFC53, "53fc" },
    };

    for (int i = 0; i < 3; ++i) {
        const QByteArray pdu = QByteArray::fromHex(vectors[i].hex);

        QCOMPARE(QScadaModbusCodec::crc16(pdu), vectors[i].crc);

        const QByteArray withCrc = QScadaModbusCodec::appendCrc(pdu);
        QCOMPARE(withCrc.right(2).toHex(), QByteArray(vectors[i].wire));
        QVERIFY(QScadaModbusCodec::checkCrc(withCrc));
    }
}

void TestQScadaAcquisition::modbusCrcDetectsCorruption()
{
    QByteArray frame = QScadaModbusCodec::appendCrc(QByteArray::fromHex("01030000000a"));
    QVERIFY(QScadaModbusCodec::checkCrc(frame));

    // 翻转任意一个数据位都必须被检出——这是现场判断"是干扰还是设备问题"的依据。
    frame[2] = static_cast<char>(frame.at(2) ^ 0x01);
    QVERIFY(!QScadaModbusCodec::checkCrc(frame));
}

void TestQScadaAcquisition::modbusCoilPackUnpackRoundTrip()
{
    // Modbus 把 8 个线圈压成一个字节，且**第一个线圈占最低位（LSB first）**。
    // 这个位序约定极易写反，写反后表现为"开关量全体错位"。
    QList<bool> coils;
    coils << true << false << true << true << false << false << false << true << true;

    const QByteArray packed = QScadaModbusCodec::packCoils(coils);
    QCOMPARE(packed.size(), 2);
    QCOMPARE(static_cast<quint8>(packed.at(0)), quint8(0b10001101));
    QCOMPARE(static_cast<quint8>(packed.at(1)), quint8(0b00000001));

    const QList<bool> unpacked = QScadaModbusCodec::unpackCoils(packed, coils.size());
    QCOMPARE(unpacked, coils);
}

void TestQScadaAcquisition::modbusRegistersRoundTrip()
{
    QList<quint16> regs;
    regs << 0x0000 << 0x000A << 0xFFFF << 0x1234;

    const QByteArray bytes = QScadaModbusCodec::fromRegisters(regs);
    QCOMPARE(bytes.size(), 8);
    QCOMPARE(QScadaModbusCodec::toRegisters(bytes), regs);
}

// =====================================================================
// 位号解码
// =====================================================================

void TestQScadaAcquisition::tagDecodesFloat32ForAllWordOrders()
{
    // 25.5 的 IEEE754 大端字节为 41 CC 00 00（由独立计算得到）。
    // 四种排布描述的是**同一台设备上同一种数据的四种线上表示**，
    // 因此它们都必须解出 25.5 —— 这正是现场换一个厂家就要改配置的原因。
    struct Case { QScadaTagDefinition::WordOrder order; const char *wire; };
    const Case cases[] = {
        { QScadaTagDefinition::BigEndian,           "41cc0000" },
        { QScadaTagDefinition::LittleEndian,        "000041cc" },
        { QScadaTagDefinition::BigEndianByteSwap,   "cc410000" },
        { QScadaTagDefinition::LittleEndianByteSwap,"0000cc41" },
    };

    for (int i = 0; i < 4; ++i) {
        QScadaTagDefinition t("T", 0);
        t.setDataType(QScadaTagDefinition::DataTypeFloat32);
        t.setWordOrder(cases[i].order);

        const QVariant v = t.decode(QByteArray::fromHex(cases[i].wire));
        QVERIFY2(v.isValid(), cases[i].wire);
        QVERIFY2(qAbs(v.toDouble() - 25.5) < 1e-9, cases[i].wire);
    }
}

void TestQScadaAcquisition::tagDecodesSignedInt16()
{
    QScadaTagDefinition t("T", 0);
    t.setDataType(QScadaTagDefinition::DataTypeInt16);

    // -100 的补码是 0xFF9C：必须带符号解释，否则会解成 65436
    QCOMPARE(t.decode(QByteArray::fromHex("ff9c")).toInt(), -100);
    QCOMPARE(t.decode(QByteArray::fromHex("0064")).toInt(), 100);

    t.setDataType(QScadaTagDefinition::DataTypeUInt16);
    QCOMPARE(t.decode(QByteArray::fromHex("ff9c")).toInt(), 65436);
}

void TestQScadaAcquisition::tagAppliesScaleAndOffset()
{
    // 现场常见：PLC 里存的是 4~20mA 对应的 0~27648 原始值，
    // 需要换算成工程量（例如 0~100 摄氏度）。
    QScadaTagDefinition t("Temp", 0);
    t.setDataType(QScadaTagDefinition::DataTypeUInt16);
    t.setScale(100.0 / 27648.0);
    t.setOffset(0.0);

    const QVariant v = t.decode(QByteArray::fromHex("6c00")); // 27648
    QVERIFY(qAbs(v.toDouble() - 100.0) < 1e-6);

    // 带偏置：raw * 2 + 10
    t.setScale(2.0);
    t.setOffset(10.0);
    const QVariant v2 = t.decode(QByteArray::fromHex("000a")); // 10 -> 30
    QVERIFY(qAbs(v2.toDouble() - 30.0) < 1e-6);
}

void TestQScadaAcquisition::tagEncodeDecodeRoundTrip()
{
    // 写入路径与读取路径必须互为逆运算，否则"下发设定值"会与"回读值"不一致。
    const QScadaTagDefinition::DataType types[] = {
        QScadaTagDefinition::DataTypeBool,
        QScadaTagDefinition::DataTypeInt16,
        QScadaTagDefinition::DataTypeUInt16,
        QScadaTagDefinition::DataTypeInt32,
        QScadaTagDefinition::DataTypeUInt32,
        QScadaTagDefinition::DataTypeFloat32,
        QScadaTagDefinition::DataTypeFloat64
    };
    const QScadaTagDefinition::WordOrder orders[] = {
        QScadaTagDefinition::BigEndian,
        QScadaTagDefinition::LittleEndian,
        QScadaTagDefinition::BigEndianByteSwap,
        QScadaTagDefinition::LittleEndianByteSwap
    };

    for (int ti = 0; ti < 7; ++ti) {
        for (int oi = 0; oi < 4; ++oi) {
            QScadaTagDefinition t("T", 0);
            t.setDataType(types[ti]);
            t.setWordOrder(orders[oi]);

            // 整型无法表示小数，encode 会按 qRound 舍入；只有浮点类型才能
            // 做严格的 1234.5 往返。对整型用整数，避免测试自己制造舍入误差。
            const bool isFloat = (types[ti] == QScadaTagDefinition::DataTypeFloat32 ||
                                  types[ti] == QScadaTagDefinition::DataTypeFloat64);
            const double value = (types[ti] == QScadaTagDefinition::DataTypeBool) ? 1.0
                               : (isFloat ? 1234.5 : 1234.0);
            const QByteArray wire = t.encode(QVariant(value));
            const QVariant back = t.decode(wire);

            QVERIFY2(back.isValid(), "roundtrip decode failed");
            QVERIFY2(qAbs(back.toDouble() - value) < 1e-3,
                     qPrintable(QString("type=%1 order=%2 got=%3")
                                .arg(QScadaTagDefinition::dataTypeName(types[ti]))
                                .arg(QScadaTagDefinition::wordOrderName(orders[oi]))
                                .arg(back.toDouble())));
        }
    }
}

void TestQScadaAcquisition::tagRejectsShortPayload()
{
    // 设备或网关可能返回比请求更短的数据。越界读会崩，必须返回无效值。
    QScadaTagDefinition t("T", 0);
    t.setDataType(QScadaTagDefinition::DataTypeFloat64); // 需要 8 字节

    QVERIFY(!t.decode(QByteArray::fromHex("41cc")).isValid());
    QVERIFY(!t.decode(QByteArray()).isValid());
}

// =====================================================================
// 点表
// =====================================================================

void TestQScadaAcquisition::registerMapJsonRoundTrip()
{
    const QByteArray json = R"({
        "version": 1,
        "tags": [
            { "key": "AxisX.Pos", "address": 100, "dataType": "float32",
              "wordOrder": "bigEndian", "scale": 0.001, "offset": 0,
              "unit": "mm", "deviceIp": "192.168.1.10", "boardId": 0,
              "objectId": 3, "enabled": true },
            { "key": "Spindle.Speed", "address": 200, "dataType": "uint16",
              "unit": "rpm", "deviceIp": "192.168.1.10", "boardId": 0,
              "objectId": 4, "enabled": true },
            { "key": "Disabled.Tag", "address": 300, "dataType": "uint16",
              "deviceIp": "192.168.1.11", "enabled": false }
        ]
    })";

    QScadaRegisterMap map;
    QString error;
    QVERIFY2(map.loadFromJson(json, &error), qPrintable(error));
    QCOMPARE(map.count(), 3);
    QCOMPARE(map.enabledTags().size(), 2);

    const QScadaTagDefinition t = map.tag("AxisX.Pos");
    QVERIFY(!t.isNull());
    QCOMPARE(t.address(), quint16(100));
    QCOMPARE(t.dataType(), QScadaTagDefinition::DataTypeFloat32);
    QVERIFY(qAbs(t.scale() - 0.001) < 1e-12);

    // 落盘再读回，字段不能丢——点表是现场工程师唯一要改的文件，
    // 读写不对称会让改过的配置在下一次保存时被悄悄冲掉。
    const QByteArray again = map.toJson();
    QScadaRegisterMap map2;
    QVERIFY(map2.loadFromJson(again, &error));
    QCOMPARE(map2.count(), 3);
    // 浮点用容差比较，不要用 QCOMPARE 做精确相等
    QVERIFY(qAbs(map2.tag("AxisX.Pos").scale() - map.tag("AxisX.Pos").scale()) < 1e-15);
    QCOMPARE(map2.tag("AxisX.Pos").objectId(), 3);
    QCOMPARE(map2.tag("AxisX.Pos").wordOrder(), QScadaTagDefinition::BigEndian);
    QCOMPARE(map2.tag("AxisX.Pos").deviceIp(), QString("192.168.1.10"));
}

void TestQScadaAcquisition::registerMapFiltersByDevice()
{
    const QByteArray json = R"({"tags":[
        {"key":"A","address":1,"deviceIp":"10.0.0.1","enabled":true},
        {"key":"B","address":2,"deviceIp":"10.0.0.1","enabled":true},
        {"key":"C","address":3,"deviceIp":"10.0.0.2","enabled":true},
        {"key":"D","address":4,"deviceIp":"10.0.0.2","enabled":false}
    ]})";

    QScadaRegisterMap map;
    QString error;
    QVERIFY(map.loadFromJson(json, &error));

    QCOMPARE(map.deviceIps().size(), 2);
    QCOMPARE(map.tagsForDevice("10.0.0.1").size(), 2);
    QCOMPARE(map.tagsForDevice("10.0.0.2").size(), 1); // D 被停用
}

void TestQScadaAcquisition::registerMapRejectsMalformedJson()
{
    QScadaRegisterMap map;
    QString error;

    QVERIFY(!map.loadFromJson("{ this is not json", &error));
    QVERIFY(!error.isEmpty());

    // 空点表也应当被拒绝：静默加载出一张空表，现场会表现为
    // "软件起来了但一个数都没有"，比直接报错难查得多。
    QVERIFY(!map.loadFromJson("{\"tags\":[]}", &error));
}

// =====================================================================
// 数据中枢
// =====================================================================

void TestQScadaAcquisition::hubPublishesAndCachesValue()
{
    QScadaDataHub hub;

    QScadaTagDefinition def("Motor.Load", 10);
    def.setDeviceIp("192.168.1.10");
    hub.registerTag(def);

    QSignalSpy spy(&hub, SIGNAL(tagValueChanged(QScadaTagValue)));
    hub.publish("Motor.Load", QVariant(42.5));

    QCOMPARE(spy.count(), 1);
    QCOMPARE(hub.cachedValueCount(), 1);

    const QScadaTagValue tv = hub.tagValue("Motor.Load");
    QCOMPARE(tv.key, QString("Motor.Load"));
    QCOMPARE(tv.deviceIp, QString("192.168.1.10"));
    QCOMPARE(tv.quality, QualityGood);
    QVERIFY(qAbs(tv.toDouble() - 42.5) < 1e-9);
    QVERIFY(tv.timestamp.isValid());
}

void TestQScadaAcquisition::hubDowngradesQualityWhenDeviceOffline()
{
    // 这是整个质量码机制存在的理由：
    // 设备掉线后，驱动若仍上报上一次读到的值，那个"陈旧值"必须被标记为不可信。
    // 否则历史库里会出现一段平直的假数据，事后分析会得出完全错误的结论。
    QScadaDataHub hub;

    QScadaTagDefinition def("Temp", 10);
    def.setDeviceIp("192.168.1.20");
    hub.registerTag(def);

    hub.publish("Temp", QVariant(80.0));
    QCOMPARE(hub.tagValue("Temp").quality, QualityGood);

    hub.setDeviceQuality("192.168.1.20", QualityNotConnected);

    // 断线后即使驱动又报了一个值，也必须降级为"未连接"
    hub.publish("Temp", QVariant(80.0));
    const QScadaTagValue tv = hub.tagValue("Temp");
    QCOMPARE(tv.quality, QualityNotConnected);
    QVERIFY(!tv.isValid());          // isValid() 要求质量为 Good
    QCOMPARE(tv.toDouble(-1.0), -1.0); // 取用时回退到调用方给的默认值，而不是返回假数据

    // 恢复连接后质量随之恢复
    hub.setDeviceQuality("192.168.1.20", QualityGood);
    hub.publish("Temp", QVariant(81.0));
    QCOMPARE(hub.tagValue("Temp").quality, QualityGood);
}

void TestQScadaAcquisition::hubNotifiesOnDeviceQualityChange()
{
    QScadaDataHub hub;

    QScadaTagDefinition def("P", 10);
    def.setDeviceIp("192.168.1.30");
    hub.registerTag(def);
    hub.publish("P", QVariant(1.0));

    QSignalSpy qualitySpy(&hub, SIGNAL(deviceQualityChanged(QString,QScadaQuality)));
    QSignalSpy valueSpy(&hub, SIGNAL(tagValueChanged(QScadaTagValue)));

    hub.setDeviceQuality("192.168.1.30", QualityBad);

    QCOMPARE(qualitySpy.count(), 1);
    // 设备状态变化必须把名下已缓存的值重新广播一次，
    // 否则界面会一直显示断线前的旧值，操作员看不出异常。
    QCOMPARE(valueSpy.count(), 1);

    // 重复设置同一质量不应重复广播
    hub.setDeviceQuality("192.168.1.30", QualityBad);
    QCOMPARE(qualitySpy.count(), 1);
}

QTEST_GUILESS_MAIN(TestQScadaAcquisition)
#include "tst_qscadaacquisition.moc"
