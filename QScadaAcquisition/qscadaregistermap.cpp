#include "qscadaregistermap.h"

#include <QFile>
#include <QDir>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSet>

QScadaRegisterMap::QScadaRegisterMap()
{
}

bool QScadaRegisterMap::loadFromFile(const QString &filePath, QString *error)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        mLastError = QStringLiteral("无法打开点表文件: %1 (%2)").arg(filePath, file.errorString());
        if (error) *error = mLastError;
        return false;
    }

    const QByteArray data = file.readAll();
    file.close();
    return loadFromJson(data, error);
}

bool QScadaRegisterMap::saveToFile(const QString &filePath, QString *error) const
{
    // 目标目录可能不存在（例如首次运行时配置目录还没建），主动创建。
    const QFileInfo info(filePath);
    QDir dir = info.absoluteDir();
    if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) {
        mLastError = QStringLiteral("无法创建点表目录: %1").arg(dir.absolutePath());
        if (error) *error = mLastError;
        return false;
    }

    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        mLastError = QStringLiteral("无法写入点表文件: %1 (%2)").arg(filePath, file.errorString());
        if (error) *error = mLastError;
        return false;
    }

    file.write(toJson());
    file.close();
    return true;
}

bool QScadaRegisterMap::loadFromJson(const QByteArray &json, QString *error)
{
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        mLastError = QStringLiteral("点表 JSON 解析失败(偏移 %1): %2")
                         .arg(parseError.offset)
                         .arg(parseError.errorString());
        if (error) *error = mLastError;
        return false;
    }

    // 兼容两种写法：顶层是数组，或者顶层是 {"tags": [...]}。
    QJsonArray array;
    if (doc.isArray()) {
        array = doc.array();
    } else if (doc.isObject()) {
        array = doc.object().value(QStringLiteral("tags")).toArray();
    } else {
        mLastError = QStringLiteral("点表根节点既不是数组也不是对象");
        if (error) *error = mLastError;
        return false;
    }

    QList<QScadaTagDefinition> loaded;
    for (int i = 0; i < array.size(); ++i) {
        const QScadaTagDefinition t = QScadaTagDefinition::fromJson(array.at(i).toObject());
        if (t.isNull()) {
            // 单条坏数据不应该让整张点表加载失败，跳过并继续。
            continue;
        }
        loaded.append(t);
    }

    if (loaded.isEmpty()) {
        mLastError = QStringLiteral("点表里没有任何有效位号");
        if (error) *error = mLastError;
        return false;
    }

    mTags = loaded;
    mLastError.clear();
    return true;
}

QByteArray QScadaRegisterMap::toJson() const
{
    QJsonArray array;
    for (int i = 0; i < mTags.size(); ++i)
        array.append(mTags.at(i).toJson());

    QJsonObject root;
    root.insert(QStringLiteral("version"), 1);
    root.insert(QStringLiteral("tags"), array);

    return QJsonDocument(root).toJson(QJsonDocument::Indented);
}

void QScadaRegisterMap::addTag(const QScadaTagDefinition &tag)
{
    if (tag.isNull())
        return;
    upsertTag(tag);
}

void QScadaRegisterMap::upsertTag(const QScadaTagDefinition &tag)
{
    if (tag.isNull())
        return;

    for (int i = 0; i < mTags.size(); ++i) {
        if (mTags.at(i).key() == tag.key()) {
            mTags[i] = tag;
            return;
        }
    }
    mTags.append(tag);
}

bool QScadaRegisterMap::removeTag(const QString &key)
{
    for (int i = 0; i < mTags.size(); ++i) {
        if (mTags.at(i).key() == key) {
            mTags.removeAt(i);
            return true;
        }
    }
    return false;
}

void QScadaRegisterMap::clear()
{
    mTags.clear();
    mLastError.clear();
}

QList<QScadaTagDefinition> QScadaRegisterMap::enabledTags() const
{
    QList<QScadaTagDefinition> out;
    for (int i = 0; i < mTags.size(); ++i) {
        if (mTags.at(i).enabled())
            out.append(mTags.at(i));
    }
    return out;
}

QList<QScadaTagDefinition> QScadaRegisterMap::tagsForDevice(const QString &deviceIp) const
{
    QList<QScadaTagDefinition> out;
    for (int i = 0; i < mTags.size(); ++i) {
        const QScadaTagDefinition &t = mTags.at(i);
        if (t.enabled() && t.deviceIp() == deviceIp)
            out.append(t);
    }
    return out;
}

QScadaTagDefinition QScadaRegisterMap::tag(const QString &key) const
{
    for (int i = 0; i < mTags.size(); ++i) {
        if (mTags.at(i).key() == key)
            return mTags.at(i);
    }
    return QScadaTagDefinition();
}

bool QScadaRegisterMap::contains(const QString &key) const
{
    return !tag(key).isNull();
}

QStringList QScadaRegisterMap::deviceIps() const
{
    QStringList ips;
    for (int i = 0; i < mTags.size(); ++i) {
        const QString ip = mTags.at(i).deviceIp();
        if (!ip.isEmpty() && !ips.contains(ip))
            ips.append(ip);
    }
    return ips;
}
