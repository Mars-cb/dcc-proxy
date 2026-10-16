// SPDX-FileCopyrightText: 2019 - 2023 UnionTech Software Technology Co., Ltd.
//
// SPDX-License-Identifier: LGPL-3.0-or-later

#pragma once

#include "frameproxyinterface.h"
#include "namespace.h"

#include <QtCore>
#include <QIcon>
#include <QWidget>

/**
 * 框架（dde-control-center 主程序）与插件之间的二进制接口（ABI）版本。
 *
 * 只要 ModuleInterface 或 FrameProxyInterface 发生破坏性变更，就必须把这里 +1：
 *   - 新增 / 删除虚函数；
 *   - 调整虚函数的声明顺序（虚表槽位随之改变）；
 *   - 修改虚函数的参数或返回值类型。
 *
 * 为什么必须有一个显式版本号：插件的虚表在编译期按接口头的声明顺序固化在 .so 里。插件与主程序若依据不同版本的接口头编译，同一个虚表槽位就会指向不同的函数，
 * 插件的一次普通调用会直接跳到主程序的另一个函数上，而且参数被原样传入 —— 现象往往表现为「插件调 A，主程序跑了 B」，例如插件调用 initSearchData()
 * 却执行到 MainWindow::showModulePage()。 C++ 没有接口反射，这类错位无法在运行时探测，只能靠版本号提前拦截。
 *
 * 这里只需要维护这一个数字：下面的 DCC_PLUGIN_MODULE_IID 由它派生，而 IID 会被 moc 编译进每个插件。因此把它 +1，等价于同时做了三件事 ——
 * 声明了新 ABI、改掉了框架期望的 IID、让所有旧插件在加载阶段就被拒绝。
 *
 * 注意改动 FrameProxyInterface 同样要 +1：插件虽然不实现它，却会调用它。
 *
 * 版本历史：
 *   - 1：早期接口头布局，showModulePage 位于虚表靠后的位置；
 *   - 2：当前布局，showModulePage 前移至虚表第 4 槽，initSearchData 移入 private。
 */
#define DCC_PLUGIN_ABI_VERSION 1.0

/**
 * 把宏的值字符串化。两级展开是必须的：只写一层，参数会被当成字面量，
 * 得到的是 DCC_PLUGIN_ABI_VERSION 这个名字而不是它的值。moc 能正确展开这套写法。
 */
#define DCC_PLUGIN_STR_(x) #x
#define DCC_PLUGIN_STR(x) DCC_PLUGIN_STR_(x)

/**
 * 插件 IID —— 由编译器固化、插件作者无需声明也漏不掉的 ABI 载体。
 *
 * 这串字符串不需要插件作者做任何额外声明：只要插件按规范写了
 *     Q_PLUGIN_METADATA(IID ModuleInterface_iid)
 *     Q_INTERFACES(DCC_NAMESPACE::ModuleInterface)
 * moc 就会把 ModuleInterface_iid 展开后的值同时写进两处：
 *     1. .so 的插件元数据段，即 QPluginLoader::metaData() 顶层的 IID 字段，加载前即可读；
 *     2. 插件类的 qt_metacast()，框架的 qobject_cast 据此比对。
 * 于是「这个插件是按哪一版接口头编译的」被编译器写进了二进制，插件改不掉、也忘不掉。
 *
 * 它由 DCC_PLUGIN_ABI_VERSION 派生，因此不存在两处版本号互相漂移的问题。
 * 需要留意的是：+1 那个数字会让所有旧插件被拒，强制它们重新编译 —— 而重新编译后，
 * ModuleInterface 与 FrameProxyInterface 两个方向的虚表是同时对齐的。
 */
#define DCC_PLUGIN_MODULE_IID "com.deepin.dde.ControlCenter.module/" DCC_PLUGIN_STR(DCC_PLUGIN_ABI_VERSION)

/**
 * QPluginLoader::metaData() 顶层对象中存放 IID 的键名。
 * 注意 IID 位于顶层，与 MetaData 子对象（Q_PLUGIN_METADATA 的 FILE 所指向的内容）同级。
 */
#define DCC_PLUGIN_IID_KEY "IID"

//struct ModuleMetadata {
//    QString icon;
//    QString title;
//};

#define MAINWINDOW "mainwindow"
#define DISPLAY "display"
#define DEFAPP "defapp"
#define PERSONALIZATION "personalization"
#define COMMONINFO "commoninfo"
#define SOUND "sound"
#define DATETIME "datetime"
#define POWER "power"
#define KEYBOARD "keyboard"
#define PRIVACY "privacy"
#define SYSTEMINFO "systeminfo"
#define MOUSE "mouse"

namespace DCC_NAMESPACE {

// ModuleInterface作为每个规范每个Module的接口，每个Module实现必须实现其所有虚函数。
class ModuleInterface
{
public:
    ModuleInterface() = default;
    ModuleInterface(FrameProxyInterface *frameProxy) : m_frameProxy(frameProxy) {}
    virtual ~ModuleInterface() {}

    void setFrameProxy(FrameProxyInterface *frameProxy) { m_frameProxy = frameProxy; }

    // preInitialize会在模块初始化时被调用，用于模块在准备阶段进行资源的初始化；
    // preInitialize不允许进行高资源的操作；
    virtual void preInitialize(bool sync = false,FrameProxyInterface::PushType = FrameProxyInterface::PushType::Normal) {Q_UNUSED(sync)}

    // initialize初始化相应的模块，参数proxy用于Moudle向Frame信息查询和主动调用；
    // 返回Module的id；
    // initialize的时候不能做资源占用较高的操作；
    virtual void initialize() = 0;

    // reset module settings
    virtual void reset() {}

    ///
    /// \brief name
    /// your module name
    /// \return
    ///
    virtual const QString name() const = 0;

    ///
    /// \brief name
    /// 模块名，用于显示
    /// \return
    ///
    virtual const QString displayName() const = 0;

    ///
    /// \brief icon
    /// get module icon path
    /// \return
    ///
    virtual QIcon icon() const {
        return QIcon::fromTheme(QString("dcc_nav_%1").arg(name()));
    }

    ///
    /// \brief translationPath
    /// 获取多语言文件的路径，用以搜索
    /// \return QString
    ///
    virtual QString translationPath() const {
        return QStringLiteral(":/translations/dde-control-center_%1.ts");
    }

    ///
    /// \brief showPage
    /// show specified module page
    /// \param pageName
    /// the page name
    ///
    virtual void showPage(const QString &pageName) { Q_UNUSED(pageName) }

    // 返回模块主Widget；
    virtual QWidget *moduleWidget() { return nullptr;}

    ///
    /// \brief contentPopped
    /// call when specific widget popped
    /// \param w
    ///
    virtual void contentPopped(QWidget *const w) { Q_UNUSED(w)}

    ///
    /// \brief active
    /// 当模块第一次被点击进入时，active会被调用,如果是插件,重载的时候必须声明为slots,否则加载不了
    virtual void active() {}

    ///
    /// \brief active
    /// 当模块被销毁时，deactive会被调用
    virtual void deactive() {}

    ///
    /// \brief load
    /// 当搜索到相关字段后，lead会被调用
    /// 如果可以正常显示则返回 0, 否则返回非0
    virtual int load(const QString &path) {
        Q_UNUSED(path)
        return 0;
    }

    virtual QStringList availPage() const { return QStringList(); }

    /**
     * @brief path
     * @return 插件级别及二级菜单插件所属模块
     */
    virtual QString path() const {
        return QString();
    }

    /**
     * @brief follow
     * @return 插件插入位置，可以字符串或者数字
     */
    virtual QString follow() const {
        return QString();
    }

    /**
     * @brief enabled
     * @return 插件是否处于可用状态
     */
    virtual bool enabled() const {
        return true;
    }

    /**
     * @brief addChildPageTrans
     * @return 添加插件二级菜单翻译内容
     */
    virtual void addChildPageTrans() const {};

public:
    inline void setAvailable(bool isAvailable) { m_available = isAvailable; }
    inline bool isAvailable() const { return m_available; }
    inline void setEnabled(bool value) {
        QWidget *mainwindow = dynamic_cast<QWidget *>(m_frameProxy);
        if (mainwindow) {
            mainwindow->setEnabled(value);
        }
    }

    inline bool deviceUnavailabel() const
    {
        return m_deviceUnavailabel;
    }

    inline void setDeviceUnavailabel(bool deviceUnavailabel)
    {
        m_deviceUnavailabel = deviceUnavailabel;
    }

protected:
    FrameProxyInterface *m_frameProxy{nullptr};
    bool m_available{true};
    bool m_deviceUnavailabel{false};

private:
    virtual void initSearchData() {};
};

}

// 本宏同时是接口标识与 ABI 版本载体（DCC_PLUGIN_ABI_VERSION 见本文件顶部）：
// 插件在 Q_PLUGIN_METADATA(IID ...) 与 Q_INTERFACES(...) 中引用它，moc 展开后
// 把 IID 固化进 .so；框架加载时逐个比对，对不上的插件不会被加载、更不会被调用。
#define ModuleInterface_iid DCC_PLUGIN_MODULE_IID
Q_DECLARE_INTERFACE(DCC_NAMESPACE::ModuleInterface, ModuleInterface_iid)
