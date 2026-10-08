#ifndef QSCADAALARMLEVEL_H
#define QSCADAALARMLEVEL_H

#include <QString>
#include <QColor>
#include <QList>
#include <QMetaType>

/*!
 * \brief 报警等级枚举与显示辅助（纯函数，无状态、不依赖 QObject）。
 *
 * 设计取舍：
 *  1. 为什么用普通枚举而不是 enum class：等级需要频繁写进 JSON、写进历史库、
 *     还要和界面上的筛选下拉框、统计数组打交道，普通枚举可以隐式转成 int，
 *     少写很多 static_cast；枚举量定义在类作用域内，不会和工程里其它
 *     Info / Warning 之类的名字冲突。
 *  2. 为什么把"存储标识"和"界面文案"分开：配置文件与数据库里存的是
 *     "critical" 这种稳定英文标识，界面上显示"严重"。这样以后改中文措辞
 *     或者做多语言时，不会导致老配置文件读不出来、老记录查不出来。
 *  3. 颜色写死在代码里：ISA-18.2 要求同一等级在整机上颜色一致，
 *     集中在一个函数里可以避免每个界面各自调一种红，
 *     现场"图例对不上"大多就是这么来的。
 */
class QScadaAlarmLevels
{
public:
    //! 数值即优先级，越大越严重，可以直接相互比较大小。
    enum Level {
        Info = 0,      //!< 提示：只记录，不要求操作员处理（例如配方切换、模式变更）
        Warning = 1,   //!< 警告：需要关注，一般不要求立即动作
        High = 2,      //!< 高限：需要尽快处理，否则可能影响产品质量
        Critical = 3   //!< 严重：要求立即处理，通常同时触发声光提示
    };

    //! 稳定标识（小写英文），用于 JSON / 数据库存储，不随界面语言变化。
    static QString levelName(Level level)
    {
        switch (level) {
        case Info:     return QStringLiteral("info");
        case Warning:  return QStringLiteral("warning");
        case High:     return QStringLiteral("high");
        case Critical: return QStringLiteral("critical");
        }
        return QStringLiteral("info");
    }

    //! 界面显示文案（中文）。只用于显示，不用于存储。
    static QString levelDisplayName(Level level)
    {
        switch (level) {
        case Info:     return QStringLiteral("信息");
        case Warning:  return QStringLiteral("警告");
        case High:     return QStringLiteral("高限");
        case Critical: return QStringLiteral("严重");
        }
        return QStringLiteral("信息");
    }

    /*!
     * 解析等级名。兼容英文标识与中文显示名，且忽略大小写与首尾空格——
     * 点表文件经常是人手工编辑的，写 "High"、"high"、"高限" 都应该能读出来，
     * 而不是悄悄退回默认值导致报警等级配错。
     * \param ok 可选，返回是否识别成功；未识别时返回 Info。
     */
    static Level levelFromName(const QString &name, bool *ok = nullptr)
    {
        const QString n = name.trimmed().toLower();
        if (ok != nullptr)
            *ok = true;

        if (n == QLatin1String("critical") || n == QStringLiteral("严重"))
            return Critical;
        if (n == QLatin1String("high") || n == QStringLiteral("高限"))
            return High;
        if (n == QLatin1String("warning") || n == QLatin1String("warn") || n == QStringLiteral("警告"))
            return Warning;
        if (n == QLatin1String("info") || n == QLatin1String("information") || n == QStringLiteral("信息"))
            return Info;

        if (ok != nullptr)
            *ok = false;
        return Info;
    }

    //! 等级颜色。深色底、白色字也能看清，避免现场大屏在强光下分辨不出黄色。
    static QColor levelColor(Level level)
    {
        switch (level) {
        case Info:     return QColor(0x60, 0x7D, 0x8B); // 蓝灰：存在感最低
        case Warning:  return QColor(0xF9, 0xA8, 0x25); // 琥珀：可用黄
        case High:     return QColor(0xEF, 0x6C, 0x00); // 橙：与警告明显区分
        case Critical: return QColor(0xC6, 0x28, 0x28); // 红：最高优先级
        }
        return QColor(0x60, 0x7D, 0x8B);
    }

    //! 颜色字符串形式（#RRGGBB），给 QML / 样式表 / Web 端报表用。
    static QString levelColorName(Level level)
    {
        return levelColor(level).name(QColor::HexRgb);
    }

    //! 反序列化回来的整数是否落在有效范围内。
    static bool isValidLevel(int value)
    {
        return value >= static_cast<int>(Info) && value <= static_cast<int>(Critical);
    }

    /*!
     * 全部等级，按"严重 → 轻微"排列。
     * 报警摘要、报警表排序、图元取最严重报警都按这个顺序遍历，
     * 集中一处可以保证各处显示顺序一致。
     */
    static QList<Level> orderedLevels()
    {
        QList<Level> levels;
        levels.append(Critical);
        levels.append(High);
        levels.append(Warning);
        levels.append(Info);
        return levels;
    }
};

//! 便捷别名：让各处的函数签名短一些，同时不牺牲类作用域带来的命名安全。
typedef QScadaAlarmLevels::Level QScadaAlarmLevel;

Q_DECLARE_METATYPE(QScadaAlarmLevel)

#endif // QSCADAALARMLEVEL_H
