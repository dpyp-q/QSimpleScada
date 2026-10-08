#ifndef QSCADAMODBUSCODEC_H
#define QSCADAMODBUSCODEC_H

#include <QByteArray>
#include <QList>
#include <QString>

/*!
 * \brief Modbus 协议编解码（纯函数，无状态）。
 *
 * 单独抽出来的原因有两个：
 *  1. 协议解析是本项目最容易出错、也最值得被测试覆盖的部分，
 *     做成无状态静态函数后可以用 Qt Test 直接构造字节流做单元测试，
 *     不需要真实 PLC 参与；
 *  2. 现场调试时可以把收发的原始报文打印出来逐字节比对，
 *     出问题能立刻定位是"没连上"还是"连上了但解析错"。
 *
 * 同时支持 Modbus TCP（MBAP 头）与 Modbus RTU（CRC16 校验）两种承载。
 */
class QScadaModbusCodec
{
public:
    //! 功能码。只实现工程上最常用的 8 个。
    enum FunctionCode {
        ReadCoils              = 0x01, //!< 读线圈（可读写开关量）
        ReadDiscreteInputs     = 0x02, //!< 读离散输入（只读开关量）
        ReadHoldingRegisters   = 0x03, //!< 读保持寄存器（最常用）
        ReadInputRegisters     = 0x04, //!< 读输入寄存器
        WriteSingleCoil        = 0x05,
        WriteSingleRegister    = 0x06,
        WriteMultipleCoils     = 0x0F,
        WriteMultipleRegisters = 0x10
    };

    //! MBAP 头长度（TCP 承载）：事务号2 + 协议号2 + 长度2 + 单元号1。
    static const int MbapHeaderSize = 7;
    //! Modbus 规定单次最多读写的位/寄存器数量。
    static const int MaxReadBits = 2000;
    static const int MaxReadRegisters = 125;
    static const int MaxWriteRegisters = 123;

    //! 一次请求的描述，便于日志与重试。
    struct Request {
        quint16 transactionId;
        quint8 unitId;
        quint8 functionCode;
        quint16 startAddress;
        quint16 quantity;
        Request() : transactionId(0), unitId(1), functionCode(ReadHoldingRegisters)
                  , startAddress(0), quantity(0) {}
    };

    //! 一次响应的解析结果。
    struct Response {
        bool valid;            //!< 报文结构是否合法
        bool isException;      //!< 设备是否返回了异常码
        quint16 transactionId;
        quint8 unitId;
        quint8 functionCode;   //!< 已去掉异常位
        quint8 exceptionCode;  //!< 异常码，见 exceptionText()
        QByteArray data;       //!< 数据区（寄存器字节 / 线圈字节）
        QString errorString;

        Response()
            : valid(false), isException(false), transactionId(0)
            , unitId(0), functionCode(0), exceptionCode(0) {}
    };

    // ---- 请求构造（TCP 承载）----
    static QByteArray buildReadRequest(quint16 transactionId, quint8 unitId,
                                       quint8 functionCode,
                                       quint16 startAddress, quint16 quantity);
    static QByteArray buildWriteSingleRegister(quint16 transactionId, quint8 unitId,
                                               quint16 address, quint16 value);
    static QByteArray buildWriteSingleCoil(quint16 transactionId, quint8 unitId,
                                           quint16 address, bool on);
    static QByteArray buildWriteMultipleRegisters(quint16 transactionId, quint8 unitId,
                                                  quint16 startAddress,
                                                  const QList<quint16> &values);
    static QByteArray buildWriteMultipleCoils(quint16 transactionId, quint8 unitId,
                                              quint16 startAddress,
                                              const QList<bool> &coils);

    /*!
     * 判断缓冲区里是否已收到一条完整响应。
     * \return 完整帧的字节数；-1 表示数据还不够，需要继续接收；0 表示头部非法。
     * 之所以要这个函数：TCP 是字节流，一次 readyRead 可能只收到半个报文，
     * 也可能一次收到两条，必须自己按 MBAP 里的长度字段做拆包。
     */
    static int expectedResponseLength(const QByteArray &buffer);

    //! 解析一条完整的 Modbus TCP 响应帧。
    static Response parseResponse(const QByteArray &frame);

    // ---- 位与寄存器的打包 / 解包 ----
    static QByteArray packCoils(const QList<bool> &coils);
    static QList<bool> unpackCoils(const QByteArray &data, int count);
    static QList<quint16> toRegisters(const QByteArray &bytes);
    static QByteArray fromRegisters(const QList<quint16> &registers);

    // ---- Modbus RTU 承载 ----
    static quint16 crc16(const QByteArray &data);
    static QByteArray appendCrc(const QByteArray &pdu);
    static bool checkCrc(const QByteArray &frameWithCrc);

    // ---- 辅助 ----
    static bool isExceptionFunction(quint8 functionCode);
    static QString functionName(quint8 functionCode);
    static QString exceptionText(quint8 exceptionCode);

private:
    //! 组装 MBAP 头 + PDU。
    static QByteArray frame(quint16 transactionId, quint8 unitId, const QByteArray &pdu);
};

#endif // QSCADAMODBUSCODEC_H
