#ifndef QSCADATAGVALUE_H
#define QSCADATAGVALUE_H

#include <QString>
#include <QVariant>
#include <QDateTime>
#include <QMetaType>

// 质量码是全项目共享的"词汇类型"（值、时间戳、质量三者构成一次采集的完整语义），
// 采集层、报警层、历史库都要用它。因此**只允许存在一个定义**，
// 权威定义放在 QScadaStorage/qscadadataquality.h，这里只做别名。
//
// 早先采集层自己定义了一份 QScadaQuality、存储层定义了 QScadaDataQuality，
// 结果是同一个概念出现两种类型：每个模块边界都要写一遍转换函数，
// 而且 switch 分支漏掉新枚举值时编译器不会报错。合并成一个类型后，
// 新增质量等级会在所有 switch 处触发编译告警，从机制上杜绝遗漏。
#include "../QScadaStorage/qscadadataquality.h"

//! 采集层沿用的类型名；与 QScadaDataQuality 是同一个类型，不需要任何转换。
typedef QScadaDataQuality QScadaQuality;

// 存储层用 Good/Bad/Uncertain/NotConnected 这种不带前缀的写法，
// 采集层习惯带 Quality 前缀（避免与业务枚举里的 Good/Bad 撞名）。
// 这里给出同义常量，两种写法都可用，且指向同一组值。
constexpr QScadaQuality QualityGood         = Good;
constexpr QScadaQuality QualityUncertain    = Uncertain;
constexpr QScadaQuality QualityBad          = Bad;
constexpr QScadaQuality QualityNotConnected = NotConnected;

/*!
 * \brief 一次采集结果的完整载体：值 + 时间戳 + 质量码 + 来源。
 *
 * 为什么不能只用 QVariant：裸值没有时间戳就无法做趋势与追溯，
 * 没有质量码就无法区分"真值"与"通讯故障"，没有来源就无法回溯到设备。
 * 报警、历史库、MES 上报全部以这个结构为输入。
 */
struct QScadaTagValue
{
    QString      key;        //!< 位号唯一键
    QString      deviceIp;   //!< 来源设备
    QVariant     value;      //!< 已换算为工程量的值
    QDateTime    timestamp;  //!< 采集时刻（本地时间）
    QScadaQuality quality;   //!< 质量码

    QScadaTagValue()
        : quality(QualityGood)
    {
    }

    bool isValid() const
    {
        return !key.isEmpty() && value.isValid() && quality == QualityGood;
    }

    //! 便利：按数值取用，质量不佳时返回 fallback。
    double toDouble(double fallback = 0.0) const
    {
        if (quality != QualityGood || !value.isValid())
            return fallback;
        return value.toDouble();
    }

    bool toBool(bool fallback = false) const
    {
        if (quality != QualityGood || !value.isValid())
            return fallback;
        return value.toBool();
    }
};

// Q_DECLARE_METATYPE(QScadaQuality) 不需要：QScadaQuality 只是别名，
// 存储层已为 QScadaDataQuality 声明过元类型，重复声明会构成重定义。
Q_DECLARE_METATYPE(QScadaTagValue)

#endif // QSCADATAGVALUE_H
