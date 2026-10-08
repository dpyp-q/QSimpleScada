#ifndef QSCADATAGDEFINITION_H
#define QSCADATAGDEFINITION_H

#include <QString>
#include <QVariant>
#include <QByteArray>
#include <QJsonObject>

/*!
 * \brief 数据点位（位号 / Tag）定义。
 *
 * 一个 Tag 描述"从设备哪个地址读、按什么类型解释、如何线性换算成工程量、
 * 最终映射到界面哪个图元"。采集层只认识 Tag，不关心底层是 Modbus 还是
 * OPC UA，因此新增一种协议只需新增一个 QScadaDataSource 实现，
 * 界面层、报警层、历史库都不需要改动。
 */
class QScadaTagDefinition
{
public:
    //! 原始数据类型，决定占用几个 16 位寄存器以及解码方式。
    enum DataType {
        DataTypeBool = 0,
        DataTypeInt16,
        DataTypeUInt16,
        DataTypeInt32,
        DataTypeUInt32,
        DataTypeFloat32,
        DataTypeFloat64
    };

    /*!
     * 32/64 位量在多个 16 位寄存器之间的字节序。
     * 不同厂家 PLC 约定不同（例如三菱与西门子相反），必须做成可配置项，
     * 否则会出现"读回来的浮点数是个天文数字"这类经典现场问题。
     */
    enum WordOrder {
        BigEndian = 0,        //!< 高字在前、字内高字节在前（Modbus 标准）
        LittleEndian,         //!< 低字在前、字内低字节在前（x86 习惯）
        BigEndianByteSwap,    //!< 高字在前、字内字节交换
        LittleEndianByteSwap  //!< 低字在前、字内字节交换
    };

    QScadaTagDefinition();
    QScadaTagDefinition(const QString &key, quint16 address);

    QString key() const { return mKey; }
    void setKey(const QString &k) { mKey = k; }

    QString displayName() const { return mDisplayName; }
    void setDisplayName(const QString &n) { mDisplayName = n; }

    quint16 address() const { return mAddress; }
    void setAddress(quint16 a) { mAddress = a; }

    DataType dataType() const { return mDataType; }
    void setDataType(DataType t) { mDataType = t; }

    WordOrder wordOrder() const { return mWordOrder; }
    void setWordOrder(WordOrder o) { mWordOrder = o; }

    //! 工程量换算：engineering = raw * scale + offset
    double scale() const { return mScale; }
    void setScale(double s) { mScale = s; }
    double offset() const { return mOffset; }
    void setOffset(double o) { mOffset = o; }

    QString unit() const { return mUnit; }
    void setUnit(const QString &u) { mUnit = u; }

    //! 界面映射：采集值最终经 updateValue(deviceIp, boardId, objectId, v) 落到图元。
    QString deviceIp() const { return mDeviceIp; }
    void setDeviceIp(const QString &ip) { mDeviceIp = ip; }
    int boardId() const { return mBoardId; }
    void setBoardId(int id) { mBoardId = id; }
    int objectId() const { return mObjectId; }
    void setObjectId(int id) { mObjectId = id; }

    //! 是否启用（点表里可以临时停用某些点而不删除）。
    bool enabled() const { return mEnabled; }
    void setEnabled(bool e) { mEnabled = e; }

    //! 占用的寄存器个数（16 位为一单位），批量读取时用于合并连续地址。
    int registerCount() const;

    //! 把原始字节解码成本 Tag 对应的工程量（已应用 scale/offset）。
    QVariant decode(const QByteArray &raw) const;

    //! 把工程量反向编码成寄存器字节（用于写值 / 反向控制）。
    QByteArray encode(const QVariant &value) const;

    bool isNull() const { return mKey.isEmpty(); }

    QJsonObject toJson() const;
    static QScadaTagDefinition fromJson(const QJsonObject &obj);

    //! 数据类型 / 字节序与字符串的互转，便于点表文件可读。
    static QString dataTypeName(DataType t);
    static DataType dataTypeFromName(const QString &name);
    static QString wordOrderName(WordOrder o);
    static WordOrder wordOrderFromName(const QString &name);

private:
    QString mKey;
    QString mDisplayName;
    quint16 mAddress;
    DataType mDataType;
    WordOrder mWordOrder;
    double mScale;
    double mOffset;
    QString mUnit;
    QString mDeviceIp;
    int mBoardId;
    int mObjectId;
    bool mEnabled;
};

#endif // QSCADATAGDEFINITION_H
