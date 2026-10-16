#include "plugin.h"
#include "panel.h"
#include <QApplication>
#include <QPalette>
#include <QMessageBox>
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
    return {"Overview", "Proxies", "Profiles", "Connections", "Rules", "Providers", "Logs", "Overrides", "Tools", "Settings"};
}
QWidget *ProxyPlugin::moduleWidget() { return m_panel; }
void ProxyPlugin::active()
{
    if (m_panel && m_panel->parentWidget()) return;
    // The host removes pages with deleteLater() before calling load()/active().
    // A non-null QPointer can still refer to that retired page in this event loop.
    try { m_panel = new Party::Panel; }
    catch (const std::exception &e) { QMessageBox::warning(nullptr, tr("私人代理"), QString::fromUtf8(e.what())); return; }
    if (m_frameProxy) m_frameProxy->pushWidget(this, m_panel, DCC_NAMESPACE::FrameProxyInterface::Normal);
}
int ProxyPlugin::load(const QString &path)
{
    active(); if (m_panel) m_panel->openPage(path.section('/', -1)); return 0;
}
void ProxyPlugin::addChildPageTrans() const
{
    if (!m_frameProxy) return;
    m_frameProxy->addChildPageTrans("Overview", tr("概览"));
    m_frameProxy->addChildPageTrans("Proxies", tr("节点"));
    m_frameProxy->addChildPageTrans("Profiles", tr("订阅"));
    m_frameProxy->addChildPageTrans("Connections", tr("连接"));
    m_frameProxy->addChildPageTrans("Rules", tr("规则"));
    m_frameProxy->addChildPageTrans("Providers", tr("资源"));
    m_frameProxy->addChildPageTrans("Logs", tr("日志"));
    m_frameProxy->addChildPageTrans("Overrides", tr("覆写"));
    m_frameProxy->addChildPageTrans("Tools", tr("工具"));
    m_frameProxy->addChildPageTrans("Settings", tr("设置"));
}
void ProxyPlugin::initSearchData()
{
    if (!m_frameProxy) return;
    for (const auto &page : {tr("概览"), tr("节点"), tr("订阅"), tr("连接"), tr("规则"), tr("资源"), tr("日志"), tr("覆写"), tr("工具"), tr("设置")})
        m_frameProxy->setWidgetVisible(displayName(), page, true);
    m_frameProxy->updateSearchData(displayName());
}
