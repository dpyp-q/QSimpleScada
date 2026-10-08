#include "qscadatagdefinition.h"

#include <QJsonValue>
#include <QDataStream>
#include <QtEndian>
#include <QDebug>

#include <algorithm>
#include <cstring>

QScadaTagDefinition::QScadaTagDefinition()
    : mAddress(0)
    , mDataType(DataTypeFloat32)
    , mWordOrder(BigEndian)
    , mScale(1.0)
    , mOffset(0.0)
    , mBoardId(0)
    , mObjectId(0)
    , mEnabled(true)
{
}

QScadaTagDefinition::QScadaTagDefinition(const QString &key, quint16 address)
    : mKey(key)
    , mDisplayName(key)
    , mAddress(address)
    , mDataType(DataTypeFloat32)
    , mWordOrder(BigEndian)
    , mScale(1.0)
    , mOffset(0.0)
    , mBoardId(0)
    , mObjectId(0)
    , mEnabled(true)
{
}

int QScadaTagDefinition::registerCount() const
{
    switch (mDataType) {
    case DataTypeBool:
    case DataTypeInt16:
    case DataTypeUInt16:
        return 1;
    case DataTypeInt32:
    case DataTypeUInt32:
    case DataTypeFloat32:
        return 2;
    case DataTypeFloat64:
        return 4;
    }
    return 1;
}

/*!
 * 把原始字节按本 Tag 的字节序重排成主机序，再做类型转换与工程量换算。
 *
 * 注意：这里刻意不直接用 QDataStream，因为 QDataStream 会带上自己的
 * 版本头语义，而且无法表达"字交换"这种 PLC 特有的排布方式。
 */
QVariant QScadaTagDefinition::decode(const QByteArray &raw) const
{
    if (raw.isEmpty())
        return QVariant();

    const int need = registerCount() * 2;
    if (raw.size() < need) {
        qWarning() << "[QScadaTag] 原始数据长度不足:" << mKey
                   << "需要" << need << "字节, 实际" << raw.size();
        return QVariant();
    }

    // 先按"字"重排：把 PLC 的寄存器顺序还原成连续的字节流。
    QByteArray ordered;
    ordered.reserve(need);
    const int wordCount = registerCount();
    switch (mWordOrder) {
    case BigEndian:
    case BigEndianByteSwap: {
        // 高字在前
        for (int i = 0; i < wordCount; ++i)
            ordered.append(raw.mid(i * 2, 2));
        break;
    }
    case LittleEndian:
    case LittleEndianByteSwap: {
        // 低字在前，整体倒序
        for (int i = wordCount - 1; i >= 0; --i)
            ordered.append(raw.mid(i * 2, 2));
        break;
    }
    }

    // 再做字内字节交换（如果需要）。
    const bool swapBytes = (mWordOrder == BigEndianByteSwap || mWordOrder == LittleEndianByteSwap);
    if (swapBytes) {
        for (int i = 0; i + 1 < ordered.size(); i += 2)
            std::swap(ordered[i], ordered[i + 1]);
    }

    const uchar *p = reinterpret_cast<const uchar *>(ordered.constData());
    double engineering = 0.0;

    switch (mDataType) {
    case DataTypeBool:
        // 以整个 16 位寄存器为判断单元，与 encode 的 qToBigEndian<quint16> 对称：
        // 现场有 0x0001（低位为 1）和 0xFF00（高位为 1）两种写法，只查高字节
        // 会把 0x0001 判成 false，只查低字节又会漏掉 0xFF00。
        engineering = (qFromBigEndian<quint16>(p) != 0) ? 1.0 : 0.0;
        break;
    case DataTypeInt16:
        engineering = static_cast<double>(qFromBigEndian<qint16>(p));
        break;
    case DataTypeUInt16:
        engineering = static_cast<double>(qFromBigEndian<quint16>(p));
        break;
    case DataTypeInt32:
        engineering = static_cast<double>(qFromBigEndian<qint32>(p));
        break;
    case DataTypeUInt32:
        engineering = static_cast<double>(qFromBigEndian<quint32>(p));
        break;
    case DataTypeFloat32: {
        // 用位拷贝而不是联合体，避免严格别名问题。
        quint32 bits = qFromBigEndian<quint32>(p);
        float f;
        memcpy(&f, &bits, sizeof(float));
        engineering = static_cast<double>(f);
        break;
    }
    case DataTypeFloat64: {
        quint64 bits = qFromBigEndian<quint64>(p);
        double d;
        memcpy(&d, &bits, sizeof(double));
        engineering = d;
        break;
    }
    }

    // 线性换算到工程量。Bool 不做换算。
    if (mDataType != DataTypeBool)
        engineering = engineering * mScale + mOffset;

    if (mDataType == DataTypeBool)
        return QVariant(engineering != 0.0);

    return QVariant(engineering);
}

/*!
 * decode 的逆运算，用于写值 / 反向控制（例如下发目标位置、启停命令）。
 */
QByteArray QScadaTagDefinition::encode(const QVariant &value) const
{
    QByteArray plain;
    plain.resize(registerCount() * 2);
    plain.fill('\0');
    uchar *p = reinterpret_cast<uchar *>(plain.data());

    if (mDataType == DataTypeBool) {
        const quint16 v = value.toBool() ? 1 : 0;
        qToBigEndian<quint16>(v, p);
    } else {
        // 先反算回原始值
        const double raw = (value.toDouble() - mOffset) / (mScale == 0.0 ? 1.0 : mScale);
        switch (mDataType) {
        case DataTypeInt16:
            qToBigEndian<qint16>(static_cast<qint16>(qRound(raw)), p);
            break;
        case DataTypeUInt16:
            qToBigEndian<quint16>(static_cast<quint16>(qRound(raw)), p);
            break;
        case DataTypeInt32:
            qToBigEndian<qint32>(static_cast<qint32>(qRound64(raw)), p);
            break;
        case DataTypeUInt32:
            qToBigEndian<quint32>(static_cast<quint32>(qRound64(raw)), p);
            break;
        case DataTypeFloat32: {
            const float f = static_cast<float>(raw);
            quint32 bits;
            memcpy(&bits, &f, sizeof(float));
            qToBigEndian<quint32>(bits, p);
            break;
        }
        case DataTypeFloat64: {
            quint64 bits;
            memcpy(&bits, &raw, sizeof(double));
            qToBigEndian<quint64>(bits, p);
            break;
        }
        default:
            break;
        }
    }

    // 按本 Tag 的字节序反向重排回寄存器顺序
    QByteArray result;
    result.resize(plain.size());
    const int wordCount = registerCount();
    const bool swapBytes = (mWordOrder == BigEndianByteSwap || mWordOrder == LittleEndianByteSwap);

    QByteArray tmp = plain;
    if (swapBytes) {
        for (int i = 0; i + 1 < tmp.size(); i += 2)
            std::swap(tmp[i], tmp[i + 1]);
    }

    for (int i = 0; i < wordCount; ++i) {
        int srcWord = i;
        if (mWordOrder == LittleEndian || mWordOrder == LittleEndianByteSwap)
            srcWord = wordCount - 1 - i;
        result.replace(i * 2, 2, tmp.mid(srcWord * 2, 2));
    }
    return result;
}

QJsonObject QScadaTagDefinition::toJson() const
{
    QJsonObject o;
    o.insert(QStringLiteral("key"), mKey);
    o.insert(QStringLiteral("displayName"), mDisplayName);
    o.insert(QStringLiteral("address"), static_cast<int>(mAddress));
    o.insert(QStringLiteral("dataType"), dataTypeName(mDataType));
    o.insert(QStringLiteral("wordOrder"), wordOrderName(mWordOrder));
    o.insert(QStringLiteral("scale"), mScale);
    o.insert(QStringLiteral("offset"), mOffset);
    o.insert(QStringLiteral("unit"), mUnit);
    o.insert(QStringLiteral("deviceIp"), mDeviceIp);
    o.insert(QStringLiteral("boardId"), mBoardId);
    o.insert(QStringLiteral("objectId"), mObjectId);
    o.insert(QStringLiteral("enabled"), mEnabled);
    return o;
}

QScadaTagDefinition QScadaTagDefinition::fromJson(const QJsonObject &obj)
{
    QScadaTagDefinition t;
    t.mKey = obj.value(QStringLiteral("key")).toString();
    t.mDisplayName = obj.value(QStringLiteral("displayName")).toString(t.mKey);
    t.mAddress = static_cast<quint16>(obj.value(QStringLiteral("address")).toInt());
    t.mDataType = dataTypeFromName(obj.value(QStringLiteral("dataType")).toString());
    t.mWordOrder = wordOrderFromName(obj.value(QStringLiteral("wordOrder")).toString());
    t.mScale = obj.value(QStringLiteral("scale")).toDouble(1.0);
    t.mOffset = obj.value(QStringLiteral("offset")).toDouble(0.0);
    t.mUnit = obj.value(QStringLiteral("unit")).toString();
    t.mDeviceIp = obj.value(QStringLiteral("deviceIp")).toString();
    t.mBoardId = obj.value(QStringLiteral("boardId")).toInt(0);
    t.mObjectId = obj.value(QStringLiteral("objectId")).toInt(0);
    t.mEnabled = obj.value(QStringLiteral("enabled")).toBool(true);
    return t;
}

QString QScadaTagDefinition::dataTypeName(DataType t)
{
    switch (t) {
    case DataTypeBool:    return QStringLiteral("bool");
    case DataTypeInt16:   return QStringLiteral("int16");
    case DataTypeUInt16:  return QStringLiteral("uint16");
    case DataTypeInt32:   return QStringLiteral("int32");
    case DataTypeUInt32:  return QStringLiteral("uint32");
    case DataTypeFloat32: return QStringLiteral("float32");
    case DataTypeFloat64: return QStringLiteral("float64");
    }
    return QStringLiteral("float32");
}

QScadaTagDefinition::DataType QScadaTagDefinition::dataTypeFromName(const QString &name)
{
    const QString n = name.trimmed().toLower();
    if (n == QLatin1String("bool"))    return DataTypeBool;
    if (n == QLatin1String("int16"))   return DataTypeInt16;
    if (n == QLatin1String("uint16"))  return DataTypeUInt16;
    if (n == QLatin1String("int32"))   return DataTypeInt32;
    if (n == QLatin1String("uint32"))  return DataTypeUInt32;
    if (n == QLatin1String("float64")) return DataTypeFloat64;
    if (n == QLatin1String("double"))  return DataTypeFloat64;
    return DataTypeFloat32;
}

QString QScadaTagDefinition::wordOrderName(WordOrder o)
{
    switch (o) {
    case BigEndian:           return QStringLiteral("bigEndian");
    case LittleEndian:        return QStringLiteral("littleEndian");
    case BigEndianByteSwap:   return QStringLiteral("bigEndianByteSwap");
    case LittleEndianByteSwap:return QStringLiteral("littleEndianByteSwap");
    }
    return QStringLiteral("bigEndian");
}

QScadaTagDefinition::WordOrder QScadaTagDefinition::wordOrderFromName(const QString &name)
{
    const QString n = name.trimmed().toLower();
    if (n == QLatin1String("littleendian"))         return LittleEndian;
    if (n == QLatin1String("bigendianbyteswap"))    return BigEndianByteSwap;
    if (n == QLatin1String("littleendianbyteswap")) return LittleEndianByteSwap;
    return BigEndian;
}
