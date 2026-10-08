#include "qscadaobjectqml.h"
#include "qscadaobjectinfo.h"

#include <QQuickWidget>
#include <QVBoxLayout>
#include <QQuickItem>
#include <QJsonObject>
#include <QDebug>

const char *QScadaObjectQML::funcUpdate = "update";
const char *QScadaObjectQML::tagMetaData = "metaData";

QScadaObjectQML::QScadaObjectQML(QScadaObjectInfo *info, QWidget *parent) :
    QScadaObject(info, parent),
    mQMLObject{nullptr}
{
    initFromQML(info);
}

void QScadaObjectQML::setProperty(QString key, QVariant value)
{
    setProperty(key.toLocal8Bit().data(), value);
}

void QScadaObjectQML::setProperty(char *key, QVariant value)
{
    // QML 根对象为空 = 资源没加载成功（路径写错、资源没打包、EEIoT 包缺失）。
    // 原实现直接解引用，一个 .irp 里的路径笔误就等于启动即崩。
    // 这里降级为"忽略这次赋值"，让界面其余部分仍然可用。
    if (mQMLObject == nullptr)
        return;

    mQMLObject->setProperty(key, value);

    update();
}

void QScadaObjectQML::updateValue(QVariant value)
{
    // 同上：没有 QML 根对象时静默跳过；原实现会把 nullptr 交给 invokeMethod。
    if (mQMLObject != nullptr) {
        QVariant rReturn;
        QMetaObject::invokeMethod(mQMLObject, QScadaObjectQML::funcUpdate,
            Q_RETURN_ARG(QVariant, rReturn),
            Q_ARG(QVariant, value));
    }

    update();
}

void QScadaObjectQML::update()
{
    QScadaObject::update();

    this->updateQMLGeometry();
}

QQuickItem *QScadaObjectQML::QMLObject() const
{
    return mQMLObject;
}

void QScadaObjectQML::setQMLObject(QQuickItem *QMLObject)
{
    mQMLObject = QMLObject;
}

void QScadaObjectQML::updateQMLGeometry()
{
    if (mQMLObject == nullptr)
        return;

    mQMLObject->setX(0);
    // 原实现这里是 setX(0) 重复了两次，Y 坐标从未被归零，
    // 导致图元内容相对外框有一个随机的纵向偏移。
    mQMLObject->setY(0);
    mQMLObject->setWidth(this->width());
    mQMLObject->setHeight(this->height());
}

void QScadaObjectQML::updateUIProperties()
{
    // 原实现循环体里两次调用 info()->UIProperties()，每次都按值拷回整张属性表，
    // 属性多时是 O(N) 次整表拷贝。取一次副本即可，语义完全不变。
    const QMultiMap<QString, QVariant> properties = info()->UIProperties();
    const QStringList keys = properties.keys();
    for (int i = 0; i < keys.size(); ++i) {
        const QString &key = keys.at(i);
        this->setProperty(key, properties.value(key));
    }
}

//private methods
void QScadaObjectQML::initFromQML(QScadaObjectInfo *info)
{
    QQuickWidget *lQmlWidget = new QQuickWidget();
    lQmlWidget->setClearColor(Qt::transparent);
    lQmlWidget->setSource(QUrl::fromLocalFile(info->uiResourcePath()));
    lQmlWidget->show();

    mQMLObject = static_cast<QQuickItem*>(lQmlWidget->rootObject());
    if (mQMLObject == nullptr) {
        // 明确指出是哪个资源没加载成功。否则运行时只能看到一个空白图元，
        // 排查时只能靠猜路径（这是原实现最耗时的一类问题）。
        qWarning() << "[QScadaObjectQML] 未取到 QML 根对象，该图元将显示为空白。资源路径:"
                   << info->uiResourcePath();
    }

    QVBoxLayout *lLayout = new QVBoxLayout(this);
    lLayout->setContentsMargins(0, 0, 0, 0);
    lLayout->addWidget(lQmlWidget);

    this->updateQMLGeometry();
    //properties should be always read only after geometry was set
    if (info->UIProperties().isEmpty()) {
        info->setUIProperties(this->QMLProperties());
        this->setInfo(info);
    } else {
        updateUIProperties();
    }
}

void QScadaObjectQML::resize(int x, int y)
{
    QScadaObject::resize(x, y);

    update();
}

QMultiMap<QString, QVariant> QScadaObjectQML::QMLProperties() const
{
    QMultiMap<QString, QVariant> rProp;

    if (mQMLObject == nullptr)
        return rProp;

    for (QVariant key : mQMLObject->property(QScadaObjectQML::tagMetaData).toList()) {
        rProp.insert(key.toString(), mQMLObject->property(key.toByteArray().data()));
    }

    return rProp;
}

