#include "qscadacnctagbinding.h"

#include <QChar>
#include <QStringList>

#include "qscadacncenums.h"

QScadaCncTagBinding::QScadaCncTagBinding(QObject *parent)
    : QObject(parent)
{
}

QScadaCncTagBinding::~QScadaCncTagBinding()
{
}

void QScadaCncTagBinding::bindTag(const QString &field, const QString &tagKey)
{
    const QString full = qualify(field);
    if (full.isEmpty()) {
        return;
    }

    const QString trimmed = tagKey.trimmed();
    if (trimmed.isEmpty()) {
        unbindTag(full);
        return;
    }

    // 重复绑定同一对关系时直接返回：点表批量刷新会反复调用 bindTag，
    // 每次都重建反查表会让界面在加载点表时出现可感知的卡顿。
    if (mFieldToTag.value(full) == trimmed) {
        return;
    }

    mFieldToTag.insert(full, trimmed);
    rebuildReverseIndex();
    emit tagBindingChanged();
}

void QScadaCncTagBinding::unbindTag(const QString &field)
{
    const QString full = qualify(field);
    if (full.isEmpty() || !mFieldToTag.contains(full)) {
        return;
    }

    mFieldToTag.remove(full);
    rebuildReverseIndex();
    emit tagBindingChanged();
}

void QScadaCncTagBinding::clearTagBindings()
{
    if (mFieldToTag.isEmpty()) {
        return;
    }

    mFieldToTag.clear();
    mTagToFields.clear();
    emit tagBindingChanged();
}

QString QScadaCncTagBinding::tagFor(const QString &field) const
{
    return mFieldToTag.value(qualify(field));
}

bool QScadaCncTagBinding::isBound(const QString &field) const
{
    return mFieldToTag.contains(qualify(field));
}

QStringList QScadaCncTagBinding::boundFields() const
{
    QStringList fields = mFieldToTag.keys();
    fields.sort();
    return fields;
}

QStringList QScadaCncTagBinding::fieldsForTag(const QString &tagKey) const
{
    QStringList fields = mTagToFields.value(tagKey.trimmed());
    fields.sort();
    return fields;
}

void QScadaCncTagBinding::setFieldPrefix(const QString &prefix)
{
    if (mFieldPrefix == prefix) {
        return;
    }

    // 前缀一变，原来的全名就全部失效了。与其留下"半截生效"的绑定表，
    // 不如显式清空，让调用方按新前缀重新绑一遍——这类问题在联调时
    // 表现为"轴不动"，静默残留比清空更难查。
    mFieldPrefix = prefix;
    mFieldToTag.clear();
    mTagToFields.clear();
    emit tagBindingChanged();
}

bool QScadaCncTagBinding::applyTagValue(const QString &tagKey, const QVariant &value)
{
    const QString key = tagKey.trimmed();
    if (key.isEmpty()) {
        return false;
    }

    const QStringList fields = mTagToFields.value(key);
    if (fields.isEmpty()) {
        return false; // 这个位号没绑到本对象的任何字段，交给上层继续分发
    }

    // 一个位号可能同时驱动多个字段（例如"当前行号"既用于显示也用于算进度），
    // 因此要遍历而不是取第一个就返回。
    bool handled = false;
    for (int i = 0; i < fields.size(); ++i) {
        const QString &full = fields.at(i);
        if (updateFromTag(full, value)) {
            handled = true;
            emit tagValueApplied(full, key);
        }
    }
    return handled;
}

QString QScadaCncTagBinding::qualify(const QString &field) const
{
    const QString name = field.trimmed();
    if (name.isEmpty()) {
        return QString();
    }
    if (mFieldPrefix.isEmpty()) {
        return name;
    }

    // 轴名可能被写成 "x"（点表是人工填的，大小写不统一是常态），
    // 但轴对象的字段前缀用的是规范化后的大写轴名。这里必须对前缀部分
    // 做同样的规范化，否则 bindTag("x.machinePosition", ...) 会绑定成功，
    // 采集回来的值却谁都不认——现场表现是"这根轴永远是 0"，极难排查。
    const int dot = name.indexOf(QLatin1Char('.'));
    if (dot <= 0) {
        return mFieldPrefix + name;
    }

    const QString rawPrefix = name.left(dot);
    const QString normalized = QScadaCncEnums::normalizeAxisName(rawPrefix);
    if (normalized.isEmpty()) {
        return name;
    }

    const QString full = normalized + name.mid(dot);
    if (full.startsWith(mFieldPrefix)) {
        return full;
    }
    return mFieldPrefix + name;
}

void QScadaCncTagBinding::rebuildReverseIndex()
{
    mTagToFields.clear();

    QHash<QString, QString>::const_iterator it = mFieldToTag.constBegin();
    for (; it != mFieldToTag.constEnd(); ++it) {
        const QString tagKey = it.value();
        if (tagKey.isEmpty()) {
            continue;
        }
        QStringList &fields = mTagToFields[tagKey];
        if (!fields.contains(it.key())) {
            fields.append(it.key());
        }
    }
}
