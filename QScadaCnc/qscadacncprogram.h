#ifndef QSCADACNCPROGRAM_H
#define QSCADACNCPROGRAM_H

#include <QString>
#include <QStringList>
#include <QVariant>

#include "qscadacncenums.h"
#include "qscadacnctagbinding.h"

/*!
 * \brief 加工程序上下文：当前在跑哪个程序、跑到哪一行、什么插补方式、做了几件。
 *
 * 职责：为界面提供"程序名 + 当前 G 代码块 + 进度 + 节拍 + 产量"这一组信息，
 * 也就是操作工站在机床前最常看的那一屏。
 *
 * 设计取舍：
 *  1. **进度按"行号"估算，不按"字节/时间"**。数控系统普遍能提供当前行号
 *     与总行数位号，而"已切削长度"多数系统不给；行号进度虽然对长程序段
 *     不均（一行 G01 可能走十几秒，一行 G00 只走 0.1 秒），但胜在永远可得，
 *     且对本项目要解决的"看板显示"完全够用。真实节拍分析应交给后端。
 *  2. **剩余时间只在进度 > 0 时估算**，且明确标注是估算：进度为 0 时
 *     除法没有意义，硬算出来会得到几个小时这种荒唐值，操作工一旦发现
 *     "这软件的数不准"就再也不会信任整个看板。
 *  3. **currentBlock 保留原始 G 代码文本**，不做解析成结构体。各家宏程序、
 *     自定义 G 代码语法差异极大，上位机去解析等于重写一个 NC 解析器；
 *     原样显示 + 高亮关键字才是投入产出比最高的做法。
 *  4. **产量（partsCompleted / partsTarget）放在程序上下文里**：计数是
 *     按"程序循环结束"递增的，与程序绑定；放到机床模型会与程序状态不一致。
 */
class QScadaCncProgram : public QScadaCncTagBinding
{
    Q_OBJECT
public:
    static QStringList bindableFields();

    explicit QScadaCncProgram(QObject *parent = nullptr);
    ~QScadaCncProgram();

    QString programName() const { return mProgramName; }
    int programNumber() const { return mProgramNumber; }
    //! "O1234" 形式的程序号文本，供界面直接显示（O 号是数控惯例）。
    QString programNumberText() const;

    int currentLine() const { return mCurrentLine; }
    int totalLines() const { return mTotalLines; }
    //! 当前程序段原文（可能含多个 G 代码，如 "G01 X12.34 F200"）。
    QString currentBlock() const { return mCurrentBlock; }

    QScadaCncEnums::InterpolationMode mode() const { return mMode; }
    QString modeText() const;
    QString modeGCodeText() const;

    double feedOverride() const { return mFeedOverride; }
    double rapidOverride() const { return mRapidOverride; }

    qint64 cycleTimeMs() const { return mCycleTimeMs; }
    //! "00:12:34" 形式的循环时间，直接显示。
    QString cycleTimeText() const;

    int partsCompleted() const { return mPartsCompleted; }
    int partsTarget() const { return mPartsTarget; }
    //! "37 / 100" 形式的产量文本；未设目标时只显示已完成数。
    QString partsText() const;

    //! 程序进度（0~100）。totalLines <= 0 时返回 0，绝不除零。
    double progressPercent() const;
    //! 是否正在加工（有程序号且行号有效）。
    bool hasProgram() const;

    /*!
     * 剩余时间估算文本。
     * 估算公式：剩余 = 已用时间 / 进度百分比 - 已用时间。
     * 进度 <= 0 或总行数未知、或已用时间为 0 时返回 "--:--:--"，
     * 宁可不显示也不显示一个会被现场证伪的数字。
     */
    QString remainingTimeText() const;

    //! 把毫秒格式化成 HH:MM:SS（超过 24 小时不进位，直接累加小时数）。
    static QString formatDuration(qint64 ms);

    // ---- 手工赋值接口（仿真 / 单元测试 / 离线演示）----
    void setProgramName(const QString &name);
    void setProgramNumber(int number);
    void setCurrentLine(int line);
    void setTotalLines(int lines);
    void setCurrentBlock(const QString &block);
    void setMode(QScadaCncEnums::InterpolationMode mode);
    void setFeedOverride(double percent);
    void setRapidOverride(double percent);
    void setCycleTimeMs(qint64 ms);
    //! 累加循环时间。采集周期固定时用它比每个周期都对时更稳（不受系统时间跳变影响）。
    void addCycleTimeMs(qint64 deltaMs);
    void setPartsCompleted(int parts);
    /*!
     * 完成一件：件数 +1 并把循环时间清零。
     * 之所以把这两件事绑在一起：计数脉冲和循环时间复位在数控系统里
     * 来自同一个"循环结束"信号，分开设容易出现"件数涨了但计时没归零"。
     */
    void markPartCompleted();
    void setPartsTarget(int target);

signals:
    void programChanged();
    void blockChanged(const QString &block);

protected:
    bool updateFromTag(const QString &field, const QVariant &value);

private:
    QString mProgramName;
    int mProgramNumber;
    int mCurrentLine;
    int mTotalLines;
    QString mCurrentBlock;
    QScadaCncEnums::InterpolationMode mMode;
    double mFeedOverride;
    double mRapidOverride;
    qint64 mCycleTimeMs;
    int mPartsCompleted;
    int mPartsTarget;
};

#endif // QSCADACNCPROGRAM_H
