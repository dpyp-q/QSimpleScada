#include "qscadaboard.h"
#include "qscadaboardinfo.h"
#include "qscadaboardcontroller.h"
#include "../QScadaObject/qscadaobjectinfo.h"
#include "../QScadaObject/qscadaobjectqml.h"

#include <QApplication>
#include <QPainter>
#include <QPen>
#include <QDebug>
#include <QMouseEvent>

QScadaBoard::QScadaBoard(int id, QWidget *parent) :
    QWidget(parent),
    mId{id},
    mObjects{new QList<QScadaObject*>()},
    mEditable{false},
    mShowGrid{true},
    mGrid{10},
    mGridPixmap{nullptr},
    mUpdateGridPixmap{true} // 原代码未初始化该成员，paintEvent 读取它属未定义行为
{
    setPalette(QPalette(Qt::transparent));
    setAutoFillBackground(true);

    setMouseTracking(true);//this not mouseMoveEven is called everytime mouse is moved

    resetGridPixmap();
}

QScadaBoard::QScadaBoard(QScadaBoardInfo *boardInfo, QWidget *parent):
    QWidget(parent),
    mId{boardInfo->id()},
    mObjects{new QList<QScadaObject*>()},
    mEditable{false},
    mShowGrid{true},
    mGrid{10},
    mGridPixmap{nullptr},
    mUpdateGridPixmap{true}
{
    this->initBoard(boardInfo);
}

QScadaBoard::~QScadaBoard()
{
    qDeleteAll(*mObjects);
    delete mObjects;
    // 网格位图原本在析构时被漏掉：每销毁一块板就泄漏一张与窗口等大的位图，
    // 1920x1080 下约 8 MB，反复打开工程会很快把内存吃光。
    delete mGridPixmap;
    mGridPixmap = nullptr;
}

void QScadaBoard::initBoard(QScadaBoardInfo *boardInfo)
{
    if (boardInfo != nullptr) {
        this->setEditable(false);

        for (int i=boardInfo->objectList().count()-1; i>=0; i--) {
            for (QScadaObjectInfo *info : boardInfo->objectList()) {
                if (info->orderLevel() == i) {
                   this->initNewObject(info);
                }
            }
        }
    }
}

QScadaObject *QScadaBoard::initNewObject(QScadaObjectInfo *info)
{
    QScadaObject *rObject = new QScadaObjectQML(info, this);

    rObject->setIsEditable(mEditable);
    connect(rObject, SIGNAL(objectDoubleClicked(QScadaObject*)), this , SIGNAL(objectDoubleClicked(QScadaObject*)));
    connect(rObject, SIGNAL(objectSelected(int)), this , SLOT(newObjectSelected(int)));
    connect(rObject, SIGNAL(objectMove(int,int)), this , SLOT(objectMove(int,int)));
    connect(rObject, SIGNAL(objectResize(int,int)), this , SLOT(objectResize(int,int)));
    rObject->setSelected(true);//make object selected only after signals are connected to handle highlight of new objects
    rObject->show();
    rObject->update();
    mObjects->append(rObject);

    emit newObjectCreated(rObject);

    return rObject;
}

void QScadaBoard::createNewObject(QScadaObjectInfo *info)
{
    QScadaObject *lObject = this->initNewObject(info);

    bringToFront(lObject);
}

void QScadaBoard::createQMLObject(int id, QString path)
{
    QScadaObjectInfo *lInfo = new QScadaObjectInfo();
    lInfo->setId(id);
    lInfo->setShowBackground(true);
    lInfo->setType(QScadaObjectTypeQML);
    lInfo->setUIResourcePath(path);

    createNewObject(lInfo);
}

void QScadaBoard::createQMLObject(QString path)
{
    this->createQMLObject(mObjects->count(), path);
}

void QScadaBoard::mouseMoveEvent(QMouseEvent *event)
{
    (void)event;
    QApplication::setOverrideCursor(Qt::ArrowCursor);
}

void QScadaBoard::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        newObjectSelected(-1);
        emit objectSelected(nullptr);
    }
}

void QScadaBoard::paintEvent(QPaintEvent *e)
{
    if (mShowGrid) {
        if (mGridPixmap == nullptr) {
            resetGridPixmap();
        }

        if ((mGridPixmap->width() != this->width())
                || (mGridPixmap->height() != this->height())) {
            delete mGridPixmap;
            resetGridPixmap();
        }

        if (mUpdateGridPixmap) {
            QPainter lPainter(mGridPixmap);
            mGridPixmap->fill(Qt::white);
            QPen lLinepen(Qt::darkGray);
            lLinepen.setCapStyle(Qt::RoundCap);
            lLinepen.setWidth(1);
            lPainter.setRenderHint(QPainter::Antialiasing,true);
            lPainter.setPen(lLinepen);

            int lX = this->width();
            int lY = this->height();

            // 原实现把"像素宽高"当成了循环次数：
            //     for (i=0..lX) for (j=1..lY) drawPoint(mGrid*i, mGrid*j)
            // 于是 1920x1080 的板要执行约 207 万次 drawPoint，其中绝大多数
            // 落在位图之外被裁剪掉，但函数调用开销照付——拖动窗口会明显卡顿。
            // 网格点的数量本来就是 (宽/格距) x (高/格距)，直接按格距步进即可，
            // 点数下降两个数量级。
            for (int y = 0; y <= lY; y += mGrid) {
                for (int x = 0; x <= lX; x += mGrid) {
                    lPainter.drawPoint(QPoint(x, y));
                }
            }

            mUpdateGridPixmap = false;
        }

        QPainter lPainter(this);
        lPainter.drawPixmap(0, 0, mGridPixmap->width(), mGridPixmap->height(), *mGridPixmap);
    }

    QWidget::paintEvent(e);
}

void QScadaBoard::newObjectSelected(int id)
{
    for(QScadaObject *object : *mObjects) {
        if (id != object->info()->id()) {
            object->setSelected(false);
        } else {
            this->bringToFront(object);
            emit objectSelected(object);
        }
    }
}

void QScadaBoard::objectMove(int, int)
{
    update();
}

void QScadaBoard::objectResize(int, int)
{
    update();
}

void QScadaBoard::setId(int id)
{
    mId = id;
}

int QScadaBoard::getId() const
{
    return mId;
}

QList<QScadaObject *> *QScadaBoard::objects() const
{
    return mObjects;
}

QList<QScadaObject*> QScadaBoard::getSeletedObjects()
{
    QList<QScadaObject*> rList;

    for(QScadaObject *object : *mObjects) {
        if (object->selected()) {
            rList.append(object);
        }
    }

    return rList;
}

void QScadaBoard::resetGridPixmap()
{
    mGridPixmap = new QPixmap(this->width(), this->height());
    mUpdateGridPixmap = true;
}

void QScadaBoard::orderObject(QScadaObject *o)
{
    bool lIsNew = false;
    for (int i=0;i<mObjects->count();i++) {
        if (mObjects->at(i)->info()->orderLevel() == o->info()->orderLevel()
                && mObjects->at(i)->info()->id() != o->info()->id()) {
            lIsNew = true;
        }
    }

    if (lIsNew){
        for (int i=0;i<mObjects->count();i++) {
            mObjects->at(i)->info()->orderDown();

            if (mObjects->at(i)->info()->orderLevel() >= mObjects->count()) {
                mObjects->at(i)->info()->setOrderLevel(mObjects->count()-1);
            }
        }
    } else {
        for (int i=0;i<mObjects->count();i++) {
            if (mObjects->at(i)->info()->orderLevel() < o->info()->orderLevel()) {
                mObjects->at(i)->info()->orderDown();
            }
        }
    }

    o->info()->setOrderLevel(0);
}

void QScadaBoard::bringToFront(QScadaObject *o)
{
    orderObject(o);
    o->raise();
}

void QScadaBoard::sendToBack(QScadaObject *o)
{
    o->lower();
}

int QScadaBoard::grid() const
{
    return mGrid;
}

void QScadaBoard::setGrid(int grid)
{
    // 格距必须为正：为 0 会让上面的绘制循环步进为 0 而陷入死循环。
    if (grid <= 0)
        return;

    mGrid = grid;

    // 改了格距必须让缓存的网格位图失效并重绘，
    // 否则新格距要等到窗口尺寸变化才会生效（原实现即有此问题）。
    mUpdateGridPixmap = true;
    update();
}

void QScadaBoard::deleteObjectWithId(int id)
{
    // 原实现在 range-for 循环体内 removeOne + delete：
    // 容器被修改后 range-for 缓存的迭代器立即失效，且后续还会解引用已释放的对象，
    // 属未定义行为。现场表现为"删除图元时偶发崩溃"，且很难复现。
    // 改为两阶段：先收集待删对象，再统一下树、析构，最后才重绘。
    QList<QScadaObject *> doomed;
    for (int i = 0; i < mObjects->size(); ++i) {
        QScadaObject *object = mObjects->at(i);
        if (object != nullptr && id == object->info()->id())
            doomed.append(object);
    }

    for (int i = 0; i < doomed.size(); ++i) {
        mObjects->removeOne(doomed.at(i));
        delete doomed.at(i);
    }

    if (!doomed.isEmpty())
        repaint();
}

void QScadaBoard::deleteObject(QScadaObject *object)
{
    if (object == nullptr)
        return;

    // 必须按指针删除而不是按 id：编辑器允许两个图元拿到同一个 id
    // （新 id 取的是图元个数，删掉一个再新建就会撞车）。
    // 若按 id 删，会连带删掉同 id 的另一个图元，调用方手里的指针随即悬垂，
    // 后续再删一次就是二次释放。
    if (mObjects->removeOne(object)) {
        delete object;
        repaint();
    }
}

void QScadaBoard::updateObjectWithId(int id)
{
    for (QScadaObject *object : *mObjects) {
        if (id == object->info()->id()) {
            object->update();
            object->updateUIProperties();
        }
    }
}

void QScadaBoard::updateValue(int id, QVariant value)
{
    for (QScadaObject *object : *mObjects) {
        if (id == object->info()->id()) {
            object->updateValue(value);
        }
    }
}

void QScadaBoard::setPropertyWithId(int id, QString property, QVariant value)
{
    for (QScadaObject *object : *mObjects) {
        if (id == object->info()->id()) {
            object->setProperty(property.toLocal8Bit().data(), value);
        }
    }
}

bool QScadaBoard::showGrid() const
{
    return mShowGrid;
}

void QScadaBoard::setShowGrid(bool showGrid)
{
    mShowGrid = showGrid;

    repaint();
}

bool QScadaBoard::editable() const
{
    return mEditable;
}

void QScadaBoard::setEditable(bool editable)
{
    mEditable = editable;

    for (QScadaObject *object : *mObjects) {
        object->setIsEditable(editable);
    }

    update();
}
