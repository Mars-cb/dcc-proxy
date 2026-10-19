#pragma once
#include "interface/moduleinterface.h"
#include <QPointer>
namespace Party { class Panel; }
class QAbstractItemView;
class ProxyPlugin : public QObject, public DCC_NAMESPACE::ModuleInterface {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID ModuleInterface_iid FILE "api.json")
    Q_INTERFACES(DCC_NAMESPACE::ModuleInterface)
    Q_PROPERTY(QString description READ description)
public:
    void initialize() override {}
    void preInitialize(bool sync = false, DCC_NAMESPACE::FrameProxyInterface::PushType type = DCC_NAMESPACE::FrameProxyInterface::Normal) override;
    const QString name() const override { return "dcc-proxy"; }
    const QString displayName() const override { return tr("私人代理"); }
    QIcon icon() const override;
    QString path() const override { return MAINWINDOW; }
    QString follow() const override { return "network"; }
    QString translationPath() const override { return ":/dcc-proxy/translations/dcc-proxy_%1.ts"; }
    QStringList availPage() const override;
    int load(const QString &path) override;
    QWidget *moduleWidget() override;
    QString description() const { return tr("节点选择、订阅与代理设置"); }
public slots:
    void active() override;
    void showPage(const QString &page) override { load(page); }
    void addChildPageTrans() const override;
private:
    void initSearchData() override;
    int selectPage(const QString &page);
    QPointer<QWidget> m_navigation;
    QPointer<QAbstractItemView> m_pageList;
    QPointer<Party::Panel> m_panel;
    QString m_selectedPage = "Overview";
};
