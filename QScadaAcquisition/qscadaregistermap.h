#ifndef QSCADAREGISTERMAP_H
#define QSCADAREGISTERMAP_H

#include <QString>
#include <QStringList>
#include <QList>

#include "qscadatagdefinition.h"

/*!
 * \brief 采集点表（Tag 表）。
 *
 * 点表是"设备寄存器地址"与"界面图元"之间唯一的对应关系表，以 JSON 落盘。
 *
 * 为什么要把点表做成外部配置文件而不是写死在代码里：
 * 同一个型号的机床，不同客户现场接线、PLC 程序版本、扩展模块数量都不同，
 * 寄存器地址会变。如果地址硬编码在代码里，每换一个现场就要改代码、重新编译、
 * 重新验证——这在交付阶段是灾难。做成配置后，现场工程师用记事本改一个
 * JSON 文件、重启软件即可完成适配。
 */
class QScadaRegisterMap
{
public:
    QScadaRegisterMap();

    //! 从 JSON 文件加载。文件不存在或格式错误时返回 false 并填 error。
    bool loadFromFile(const QString &filePath, QString *error = nullptr);
    bool saveToFile(const QString &filePath, QString *error = nullptr) const;

    //! 从/到 JSON 字符串，便于单元测试与网络下发点表。
    bool loadFromJson(const QByteArray &json, QString *error = nullptr);
    QByteArray toJson() const;

    void addTag(const QScadaTagDefinition &tag);
    //! 按 key 覆盖或追加（key 是唯一标识）。
    void upsertTag(const QScadaTagDefinition &tag);
    bool removeTag(const QString &key);
    void clear();

    QList<QScadaTagDefinition> tags() const { return mTags; }
    //! 只返回启用的位号，采集层用这个。
    QList<QScadaTagDefinition> enabledTags() const;
    QList<QScadaTagDefinition> tagsForDevice(const QString &deviceIp) const;

    QScadaTagDefinition tag(const QString &key) const;
    bool contains(const QString &key) const;

    int count() const { return mTags.size(); }
    bool isEmpty() const { return mTags.isEmpty(); }

    //! 出现过的设备 IP 列表，采集管理器据此为每台设备建一个驱动实例。
    QStringList deviceIps() const;

    QString lastError() const { return mLastError; }

private:
    QList<QScadaTagDefinition> mTags;
    //! 最后一次错误描述。saveToFile() 为 const 成员但需记录错误，故为 mutable。
    mutable QString mLastError;
};

#endif // QSCADAREGISTERMAP_H
