#include "qscadamodbuscodec.h"

/*!
 * \file qscadamodbuscodec.cpp
 * \brief Modbus TCP/RTU 报文编解码实现。
 *
 * 所有多字节字段一律使用"大端"（网络字节序），这是 Modbus 规范的规定；
 * 工程上所谓"字节序问题"指的是 32 位数据的寄存器内排布，那属于
 * QScadaTagDefinition 的职责，不在这里处理。
 */

namespace {

inline void appendUInt16(QByteArray &out, quint16 v)
{
    out.append(static_cast<char>((v >> 8) & 0xFF));
    out.append(static_cast<char>(v & 0xFF));
}

inline quint16 readUInt16(const char *p)
{
    return static_cast<quint16>((static_cast<quint8>(p[0]) << 8) | static_cast<quint8>(p[1]));
}

} // namespace

QByteArray QScadaModbusCodec::frame(quint16 transactionId, quint8 unitId, const QByteArray &pdu)
{
    QByteArray out;
    out.reserve(MbapHeaderSize + pdu.size());

    appendUInt16(out, transactionId);   // 事务标识符：用于把响应和请求配对
    appendUInt16(out, 0);               // 协议标识符：Modbus 固定为 0
    appendUInt16(out, static_cast<quint16>(pdu.size() + 1)); // 后续字节数（含单元标识符）
    out.append(static_cast<char>(unitId));
    out.append(pdu);
    return out;
}

QByteArray QScadaModbusCodec::buildReadRequest(quint16 transactionId, quint8 unitId,
                                               quint8 functionCode,
                                               quint16 startAddress, quint16 quantity)
{
    QByteArray pdu;
    pdu.append(static_cast<char>(functionCode));
    appendUInt16(pdu, startAddress);
    appendUInt16(pdu, quantity);
    return frame(transactionId, unitId, pdu);
}

QByteArray QScadaModbusCodec::buildWriteSingleRegister(quint16 transactionId, quint8 unitId,
                                                       quint16 address, quint16 value)
{
    QByteArray pdu;
    pdu.append(static_cast<char>(WriteSingleRegister));
    appendUInt16(pdu, address);
    appendUInt16(pdu, value);
    return frame(transactionId, unitId, pdu);
}

QByteArray QScadaModbusCodec::buildWriteSingleCoil(quint16 transactionId, quint8 unitId,
                                                   quint16 address, bool on)
{
    QByteArray pdu;
    pdu.append(static_cast<char>(WriteSingleCoil));
    appendUInt16(pdu, address);
    // Modbus 规范：写单个线圈时 0xFF00 表示 ON，0x0000 表示 OFF，其它值非法。
    appendUInt16(pdu, on ? 0xFF00 : 0x0000);
    return frame(transactionId, unitId, pdu);
}

QByteArray QScadaModbusCodec::buildWriteMultipleRegisters(quint16 transactionId, quint8 unitId,
                                                          quint16 startAddress,
                                                          const QList<quint16> &values)
{
    QByteArray pdu;
    pdu.append(static_cast<char>(WriteMultipleRegisters));
    appendUInt16(pdu, startAddress);
    appendUInt16(pdu, static_cast<quint16>(values.size()));
    pdu.append(static_cast<char>(values.size() * 2)); // 字节数
    for (int i = 0; i < values.size(); ++i)
        appendUInt16(pdu, values.at(i));
    return frame(transactionId, unitId, pdu);
}

QByteArray QScadaModbusCodec::buildWriteMultipleCoils(quint16 transactionId, quint8 unitId,
                                                      quint16 startAddress,
                                                      const QList<bool> &coils)
{
    const QByteArray packed = packCoils(coils);

    QByteArray pdu;
    pdu.append(static_cast<char>(WriteMultipleCoils));
    appendUInt16(pdu, startAddress);
    appendUInt16(pdu, static_cast<quint16>(coils.size()));
    pdu.append(static_cast<char>(packed.size()));
    pdu.append(packed);
    return frame(transactionId, unitId, pdu);
}

int QScadaModbusCodec::expectedResponseLength(const QByteArray &buffer)
{
    if (buffer.size() < MbapHeaderSize)
        return -1; // 头还没收全

    const quint16 protocolId = readUInt16(buffer.constData() + 2);
    if (protocolId != 0)
        return 0; // 协议号不是 0，说明字节流失步了，需要上层断开重连

    const quint16 length = readUInt16(buffer.constData() + 4);
    // 长度字段 = 单元标识符(1) + PDU，PDU 最短是异常响应 2 字节。
    if (length < 2 || length > 260)
        return 0;

    return 6 + static_cast<int>(length);
}

QScadaModbusCodec::Response QScadaModbusCodec::parseResponse(const QByteArray &frame)
{
    Response r;

    if (frame.size() < MbapHeaderSize + 2) {
        r.errorString = QStringLiteral("报文长度不足，无法解析");
        return r;
    }

    const quint16 protocolId = readUInt16(frame.constData() + 2);
    if (protocolId != 0) {
        r.errorString = QStringLiteral("协议标识符非 0（%1）").arg(protocolId);
        return r;
    }

    const quint16 length = readUInt16(frame.constData() + 4);
    if (6 + static_cast<int>(length) != frame.size()) {
        r.errorString = QStringLiteral("长度字段与实际字节数不一致");
        return r;
    }

    r.transactionId = readUInt16(frame.constData());
    r.unitId = static_cast<quint8>(frame.at(6));

    const quint8 rawFunction = static_cast<quint8>(frame.at(7));
    r.functionCode = rawFunction & 0x7F;

    if (isExceptionFunction(rawFunction)) {
        r.isException = true;
        r.exceptionCode = static_cast<quint8>(frame.at(8));
        r.errorString = exceptionText(r.exceptionCode);
        r.valid = true; // 结构合法，只是设备报错
        return r;
    }

    // 正常响应：第 8 字节是字节数，其后是数据区
    const quint8 byteCount = static_cast<quint8>(frame.at(8));
    const int dataStart = 9;
    if (dataStart + byteCount > frame.size()) {
        r.errorString = QStringLiteral("数据区长度不足");
        return r;
    }

    r.data = frame.mid(dataStart, byteCount);
    r.valid = true;
    return r;
}

QByteArray QScadaModbusCodec::packCoils(const QList<bool> &coils)
{
    // Modbus 把 8 个线圈压成一个字节，且第一个线圈占最低位（LSB first）。
    QByteArray out;
    const int byteCount = (coils.size() + 7) / 8;
    out.resize(byteCount);
    out.fill('\0');

    for (int i = 0; i < coils.size(); ++i) {
        if (coils.at(i)) {
            const int byteIndex = i / 8;
            const int bitIndex = i % 8;
            out[byteIndex] = static_cast<char>(static_cast<quint8>(out.at(byteIndex)) | (1 << bitIndex));
        }
    }
    return out;
}

QList<bool> QScadaModbusCodec::unpackCoils(const QByteArray &data, int count)
{
    QList<bool> coils;
    coils.reserve(count);

    for (int i = 0; i < count; ++i) {
        const int byteIndex = i / 8;
        const int bitIndex = i % 8;
        if (byteIndex >= data.size()) {
            coils.append(false);
            continue;
        }
        const quint8 byte = static_cast<quint8>(data.at(byteIndex));
        coils.append((byte & (1 << bitIndex)) != 0);
    }
    return coils;
}

QList<quint16> QScadaModbusCodec::toRegisters(const QByteArray &bytes)
{
    QList<quint16> regs;
    for (int i = 0; i + 1 < bytes.size(); i += 2)
        regs.append(readUInt16(bytes.constData() + i));
    return regs;
}

QByteArray QScadaModbusCodec::fromRegisters(const QList<quint16> &registers)
{
    QByteArray out;
    out.reserve(registers.size() * 2);
    for (int i = 0; i < registers.size(); ++i)
        appendUInt16(out, registers.at(i));
    return out;
}

quint16 QScadaModbusCodec::crc16(const QByteArray &data)
{
    // Modbus RTU 使用的 CRC-16/ARC：多项式 0xA001（反射形式），初值 0xFFFF。
    quint16 crc = 0xFFFF;
    for (int i = 0; i < data.size(); ++i) {
        crc ^= static_cast<quint8>(data.at(i));
        for (int bit = 0; bit < 8; ++bit) {
            if (crc & 0x0001)
                crc = static_cast<quint16>((crc >> 1) ^ 0xA001);
            else
                crc = static_cast<quint16>(crc >> 1);
        }
    }
    return crc;
}

QByteArray QScadaModbusCodec::appendCrc(const QByteArray &pdu)
{
    QByteArray out = pdu;
    const quint16 crc = crc16(pdu);
    // RTU 的 CRC 是低字节在前。
    out.append(static_cast<char>(crc & 0xFF));
    out.append(static_cast<char>((crc >> 8) & 0xFF));
    return out;
}

bool QScadaModbusCodec::checkCrc(const QByteArray &frameWithCrc)
{
    if (frameWithCrc.size() < 3)
        return false;
    const QByteArray payload = frameWithCrc.left(frameWithCrc.size() - 2);
    return appendCrc(payload) == frameWithCrc;
}

bool QScadaModbusCodec::isExceptionFunction(quint8 functionCode)
{
    return (functionCode & 0x80) != 0;
}

QString QScadaModbusCodec::functionName(quint8 functionCode)
{
    switch (functionCode & 0x7F) {
    case ReadCoils:              return QStringLiteral("读线圈(01)");
    case ReadDiscreteInputs:     return QStringLiteral("读离散输入(02)");
    case ReadHoldingRegisters:   return QStringLiteral("读保持寄存器(03)");
    case ReadInputRegisters:     return QStringLiteral("读输入寄存器(04)");
    case WriteSingleCoil:        return QStringLiteral("写单个线圈(05)");
    case WriteSingleRegister:    return QStringLiteral("写单个寄存器(06)");
    case WriteMultipleCoils:     return QStringLiteral("写多个线圈(0F)");
    case WriteMultipleRegisters: return QStringLiteral("写多个寄存器(10)");
    default:                     return QStringLiteral("未知功能码(%1)").arg(functionCode);
    }
}

QString QScadaModbusCodec::exceptionText(quint8 exceptionCode)
{
    switch (exceptionCode) {
    case 0x01: return QStringLiteral("非法功能码：设备不支持该功能");
    case 0x02: return QStringLiteral("非法数据地址：寄存器地址超出设备范围");
    case 0x03: return QStringLiteral("非法数据值：写入的数值超出允许范围");
    case 0x04: return QStringLiteral("从站设备故障：设备内部错误");
    case 0x05: return QStringLiteral("确认：设备已接受命令，正在长时间处理");
    case 0x06: return QStringLiteral("从站设备忙：设备正在处理长任务，稍后重试");
    case 0x08: return QStringLiteral("存储奇偶校验错");
    case 0x0A: return QStringLiteral("网关路径不可用");
    case 0x0B: return QStringLiteral("网关目标设备无响应");
    default:   return QStringLiteral("未知异常码(%1)").arg(exceptionCode);
    }
}
