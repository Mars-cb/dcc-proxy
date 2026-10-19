#include "plugin.h"
#include "panel.h"
#include <QApplication>
#include <QPalette>
#include <QMessageBox>
#include <DListView>
#include <DStandardItem>
#include <QVBoxLayout>
#include <QStandardItemModel>
#include <QSignalBlocker>
#include <QItemSelectionModel>
Q_DECLARE_METATYPE(QMargins)
void ProxyPlugin::preInitialize(bool sync, DCC_NAMESPACE::FrameProxyInterface::PushType type)
{
    Q_UNUSED(sync) Q_UNUSED(type)
    addChildPageTrans(); initSearchData();
}
QIcon ProxyPlugin::icon() const
{
    return QIcon(qApp->palette().color(QPalette::Window).lightness() < 128 ? ":/dcc-proxy/proxy-dark.svg" : ":/dcc-proxy/proxy.svg");
}
QStringList ProxyPlugin::availPage() const
{
    return {"Overview", "Proxies", "Profiles", "Connections", "Rules", "Providers", "Logs", "Overrides", "Tools", "Settings", "Tun"};
}
QWidget *ProxyPlugin::moduleWidget() { return m_navigation; }
void ProxyPlugin::active()
{
    if (!m_frameProxy) return;
    // The host removes pages with deleteLater(); check attachment as well as QPointer.
    if (!m_navigation || !m_navigation->parentWidget()) {
        auto *navigation = new QWidget;
        navigation->setObjectName("proxy-secondary-navigation");
        auto *layout = new QVBoxLayout(navigation); layout->setContentsMargins(0, 0, 0, 0);
        auto *list = new Dtk::Widget::DListView;
        list->setObjectName("proxy-secondary-pages"); list->setAccessibleName(tr("私人代理功能"));
        list->setFrameShape(QFrame::NoFrame); list->setEditTriggers(QAbstractItemView::NoEditTriggers);
        list->setSelectionMode(QAbstractItemView::SingleSelection); list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        list->setResizeMode(QListView::Adjust); list->setMovement(QListView::Static);
        list->setViewportMargins(QMargins(10, 10, 10, 10));
        list->setItemSize(QSize(168, 58)); list->setIconSize(QSize(32, 32)); list->setSpacing(0);
        auto *model = new QStandardItemModel(list);
        const QStringList titles{tr("概览"), tr("代理组"), tr("订阅"), tr("连接"), tr("规则"), tr("资源"), tr("日志"), tr("覆写"), tr("工具"), tr("设置"), tr("虚拟网卡")};
        const QStringList icons{"dcc_common", "network-wired", ":/dcc-proxy/menu-subscription.svg", "network-workgroup", "view-filter", "folder-download", "text-x-log", "document-edit", "applications-utilities", ":/dcc-proxy/menu-settings.svg", ":/dcc-proxy/menu-tun.svg"};
        for (int i = 0; i < titles.size(); ++i) {
            QIcon pageIcon;
            if (icons[i].startsWith(":/")) {
                pageIcon = QIcon(icons[i]);
                const auto selected = QString(icons[i]).replace(".svg", "-selected.svg");
                pageIcon.addFile(selected, QSize(), QIcon::Selected, QIcon::Off);
                pageIcon.addFile(selected, QSize(), QIcon::Selected, QIcon::On);
            } else pageIcon = QIcon::fromTheme(icons[i], icon());
            auto *item = new Dtk::Widget::DStandardItem(pageIcon, titles[i]);
            item->setAccessibleText(titles[i]); item->setData(availPage()[i], Qt::UserRole);
            item->setData(QVariant::fromValue(QMargins(10, 8, 10, 8)), Dtk::MarginsRole);
            model->appendRow(item);
        }
        list->setModel(model); layout->addWidget(list);
        m_navigation = navigation; m_pageList = list;
        auto choose = [this, list](const QModelIndex &current) {
            if (m_pageList != list || !current.isValid()) return;
            m_selectedPage = current.data(Qt::UserRole).toString();
            if (!m_panel || !m_panel->parentWidget()) active();
            else selectPage(m_selectedPage);
        };
        connect(list->selectionModel(), &QItemSelectionModel::currentChanged, navigation, choose);
        connect(list, &QAbstractItemView::clicked, navigation, choose);
        m_frameProxy->pushWidget(this, navigation, DCC_NAMESPACE::FrameProxyInterface::Normal);
    }
    if (!m_panel || !m_panel->parentWidget()) {
        try { m_panel = new Party::Panel; m_panel->setExternalNavigation(true); }
        catch (const std::exception &e) { QMessageBox::warning(nullptr, tr("私人代理"), QString::fromUtf8(e.what())); return; }
        const QPointer<Party::Panel> panel = m_panel;
        connect(panel, &Party::Panel::pageChanged, this, [this, panel](int index) {
            if (panel != m_panel || index < 0 || index >= availPage().size()) return;
            m_selectedPage = availPage()[index];
            if (m_pageList) {
                const QSignalBlocker block(m_pageList->selectionModel());
                const auto item = m_pageList->model()->index(index, 0);
                m_pageList->setCurrentIndex(item); m_pageList->scrollTo(item, QAbstractItemView::EnsureVisible);
            }
        });
        m_frameProxy->pushWidget(this, m_panel, DCC_NAMESPACE::FrameProxyInterface::Normal);
    }
    selectPage(m_selectedPage);
}
int ProxyPlugin::selectPage(const QString &page)
{
    const auto name = page == "Groups" ? QString("Proxies") : page;
    const auto index = availPage().indexOf(name); if (index < 0 || !m_panel) return -1;
    m_selectedPage = name; m_panel->openPage(name);
    if (m_pageList) {
        const QSignalBlocker block(m_pageList->selectionModel());
        const auto item = m_pageList->model()->index(index, 0);
        m_pageList->setCurrentIndex(item); m_pageList->scrollTo(item, QAbstractItemView::PositionAtCenter);
    }
    return 0;
}
int ProxyPlugin::load(const QString &path)
{
    active(); return selectPage(path.section('/', -1));
}
void ProxyPlugin::addChildPageTrans() const
{
    if (!m_frameProxy) return;
    m_frameProxy->addChildPageTrans("Overview", tr("概览"));
    m_frameProxy->addChildPageTrans("Proxies", tr("代理组"));
    m_frameProxy->addChildPageTrans("Profiles", tr("订阅"));
    m_frameProxy->addChildPageTrans("Connections", tr("连接"));
    m_frameProxy->addChildPageTrans("Rules", tr("规则"));
    m_frameProxy->addChildPageTrans("Providers", tr("资源"));
    m_frameProxy->addChildPageTrans("Logs", tr("日志"));
    m_frameProxy->addChildPageTrans("Overrides", tr("覆写"));
    m_frameProxy->addChildPageTrans("Tools", tr("工具"));
    m_frameProxy->addChildPageTrans("Settings", tr("设置"));
    m_frameProxy->addChildPageTrans("Tun", tr("虚拟网卡"));
}
void ProxyPlugin::initSearchData()
{
    if (!m_frameProxy) return;
    for (const auto &page : {tr("概览"), tr("代理组"), tr("订阅"), tr("连接"), tr("规则"), tr("资源"), tr("日志"), tr("覆写"), tr("工具"), tr("设置"), tr("虚拟网卡")})
        m_frameProxy->setWidgetVisible(displayName(), page, true);
    m_frameProxy->updateSearchData(displayName());
}
