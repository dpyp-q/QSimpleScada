#ifndef QSCADACNCTAGBINDING_H
#define QSCADACNCTAGBINDING_H

#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariant>

/*!
 * \brief 数控语义字段 <-> 采集位号(Tag) 的绑定基类。
 *
 * ## 为什么一定要有这层间接
 *
 * 同一个语义量在不同厂家、不同数控系统上的寄存器地址完全不同：
 *   - 西门子 828D 的 X 轴机械坐标要从 `/Channel/MachineAxis/actToolBasePos` 这类
 *     系统变量里读（走 828D 的 OPC UA / 840D 变量服务）；
 *   - 三菱 M80 则在 R 寄存器区（例如 R2000 起的自定义映射区），地址随梯图改；
 *   - 发那科 0i-MF 又要读诊断号 DGN 300 附近的区域，还得先按位拆符号。
 * 如果把这些地址直接写进 `QScadaCncAxis`，那么"换一台机床"就等于"改一次
 * 模型代码 + 重新编译 + 重新验证"，而项目里最不该动的就是已经验证过的模型
 * 和界面逻辑。
 *
 * 因此模型只认一个不透明的字符串——位号(Tag) Key，例如 `"X.MACH_POS"`；
 * 真正的地址、数据类型、字节序、量程换算全部落在 `QScadaTagDefinition` 里，
 * 由点表配置文件描述。适配新机床时：
 *   1. 只改点表（`QScadaTagDefinition` 的 key/address/scale/unit）；
 *   2. 必要时改一次绑定表（哪个语义字段对哪个位号）；
 *   3. 模型、界面、报警、历史库一行代码都不用动。
 *
 * ## 反向查找
 *
 * 采集驱动 `QScadaDataSource::valueChanged(tagKey, value)` 只给出 tagKey，
 * 模型必须立刻知道"这个值该写到哪个字段"，所以在正向表之外额外维护一张
 * 反向索引 `tagKey -> fields`。一张位号被绑到多个字段是允许的（例如
 * "当前块号"同时用于显示和进度计算），因此值是字段列表而不是单个字段。
 *
 * ## 线程约定
 *
 * 采集驱动在自己的线程里工作，但它通过 `valueChanged` 信号（自动队列连接）
 * 把值投递回界面线程，所有模型对象都归属界面线程，因此本类不加锁：
 * 少一把锁就少一类在现场极难复现的死锁。
 */
class QScadaCncTagBinding : public QObject
{
    Q_OBJECT
public:
    explicit QScadaCncTagBinding(QObject *parent = nullptr);
    ~QScadaCncTagBinding();

    /*!
     * 把语义字段绑到一个位号上。field 可以用不带前缀的短名（如
     * "machinePosition"），基类会自动补上本对象的字段前缀（如 "X."），
     * 因此调用者不必关心前缀命名规则。
     * 传空 tagKey 等价于解绑该字段。
     */
    void bindTag(const QString &field, const QString &tagKey);
    void unbindTag(const QString &field);
    //! 一次性清空（例如切换机床/重新加载点表时）。
    void clearTagBindings();

    //! 该字段当前绑定的位号；未绑定返回空串。
    QString tagFor(const QString &field) const;
    bool isBound(const QString &field) const;
    //! 已绑定字段的全名列表（含前缀），按字典序，便于界面稳定显示。
    QStringList boundFields() const;
    //! 某个位号影响的所有字段（用于排查"一个点表点写错了导致哪个字段不动"）。
    QStringList fieldsForTag(const QString &tagKey) const;

    //! 字段前缀，用于把多个同类对象的字段放进同一张全局映射表而不撞名。
    QString fieldPrefix() const { return mFieldPrefix; }
    void setFieldPrefix(const QString &prefix);

    /*!
     * 采集值统一入口。内部先取反查表得到字段，再调用子类的 updateFromTag()。
     * 与 `QScadaDataSource::valueChanged(QString, QVariant)` 参数顺序一致，
     * 可以直接用 QObject::connect 连到采集驱动的信号上，省掉一层转发槽。
     * \return true 表示本对象至少识别并处理了一个字段；false 表示位号未绑定
     *         到本对象的任何字段，调用方（如机床模型的转发逻辑）可据此继续
     *         把值分发到其他对象。
     */
    bool applyTagValue(const QString &tagKey, const QVariant &value);

signals:
    /*!
     * 一次采集值被成功应用到某个字段（field 为全名，已含前缀）。
     * 注意与采集驱动的 `valueChanged` 区分：那个是"原始位号值到了"，
     * 这个是"已落到语义字段上"。机床模型靠它把子模型的事件汇总转发出去。
     */
    void tagValueApplied(const QString &field, const QString &tagKey);
    //! 绑定关系发生变化，界面上的映射表可以据此刷新。
    void tagBindingChanged();

protected:
    /*!
     * 子类在这里把 field 对应的值写入成员、必要时发变更信号。
     * \return true 表示字段被本对象识别并处理（值可能因为"没变化"而未发信号）；
     *         false 表示字段不认识，调用方可继续往上抛。
     */
    virtual bool updateFromTag(const QString &field, const QVariant &value) = 0;

    //! 把短名补成带前缀的全名；已带前缀或本身是"别名"时原样返回。
    QString qualify(const QString &field) const;

    //! 重建 tagKey -> fields 反向索引（绑定关系变动后调用）。
    void rebuildReverseIndex();

    QString mFieldPrefix;
    QHash<QString, QString> mFieldToTag; //!< field(全名) -> tagKey
    QHash<QString, QStringList> mTagToFields; //!< tagKey -> field(全名) 列表
};

#endif // QSCADACNCTAGBINDING_H
