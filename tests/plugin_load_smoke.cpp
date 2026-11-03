#include "interface/moduleinterface.h"
#include <QApplication>
#include <QPluginLoader>
#include <QDebug>
#include <QFile>
#include <QDir>
#include <QTemporaryDir>
#include <QHBoxLayout>
#include <QPointer>
#include <QAbstractItemView>
#include <QAbstractItemModel>
#include <QFileInfo>
#include <QEventLoop>

class SmokeFrame : public DCC_NAMESPACE::FrameProxyInterface {
public:
    QWidget host;
    QHBoxLayout layout{&host};
    QList<QPointer<QWidget>> pages;
    void pushWidget(DCC_NAMESPACE::ModuleInterface *const, QWidget *const widget, PushType) override {
        pages.append(widget); layout.addWidget(widget); widget->show();
    }
    void popWidget(DCC_NAMESPACE::ModuleInterface *const) override {
        if (pages.isEmpty()) return;
        if (const auto widget = pages.takeLast()) { layout.removeWidget(widget); delete widget; }
    }
    void setModuleVisible(DCC_NAMESPACE::ModuleInterface *const, const bool) override {}
    void showModulePage(const QString &, const QString &, bool) override {}
    void setModuleSubscriptVisible(const QString &, bool) override {}
    void setRemoveableDeviceStatus(QString, bool) override {}
    bool getRemoveableDeviceStatus(QString) const override { return false; }
    void setSearchPath(DCC_NAMESPACE::ModuleInterface *const) const override {}
    void addChildPageTrans(const QString &, const QString &) override {}
    void setModuleVisible(const QString &, bool) override {}
    void setWidgetVisible(const QString &, const QString &, bool) override {}
    void setDetailVisible(const QString &, const QString &, const QString &, bool) override {}
    void updateSearchData(const QString &) override {}
    QString moduleDisplayName(const QString &name) const override { return name; }
};

int main(int argc, char **argv)
{
    QApplication application(argc, argv);
    if (argc != 2) return 2;
    QTemporaryDir temporary;
    if (!temporary.isValid()) return 2;
    qputenv("DCC_PROXY_DATA_DIR", temporary.path().toUtf8());
    qputenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=/nonexistent");
    qputenv("DBUS_SYSTEM_BUS_ADDRESS", "unix:path=/nonexistent");
    QFile settings(temporary.path() + "/settings.json");
    if (!settings.open(QIODevice::WriteOnly)) return 2;
    settings.write("{\"endpoint\":\"http://127.0.0.1:1\",\"secret\":\"isolated-smoke\",\"core\":\"\",\"autoStart\":false,\"autoSystemProxy\":false}");
    settings.close();
    QPluginLoader loader(QFileInfo(QString::fromLocal8Bit(argv[1])).absoluteFilePath());
    auto *plugin = qobject_cast<DCC_NAMESPACE::ModuleInterface *>(loader.instance());
    if (!plugin || plugin->name() != "dcc-proxy") {
        qCritical() << "Control-center plugin failed to load:" << loader.errorString();
        return 1;
    }
    SmokeFrame frame;
    frame.host.resize(1000, 700); frame.host.show();
    plugin->setFrameProxy(&frame); plugin->preInitialize(); plugin->active();
    auto *navigation = frame.host.findChild<QAbstractItemView *>("proxy-secondary-pages");
    if (!navigation || navigation->model()->rowCount() != 11 || !frame.host.findChild<QWidget *>("dcc-proxy-panel")) {
        qCritical() << "Plugin did not create its navigation and content pages";
        return 1;
    }
    for (const auto &page : plugin->availPage()) {
        if (plugin->load(page) != 0) { qCritical() << "Cannot open page:" << page; return 1; }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 30);
    }
    plugin->setFrameProxy(nullptr);
    if (QFile::exists(temporary.path() + "/service.sock")) {
        qCritical() << "Plugin smoke test unexpectedly started a daemon";
        return 1;
    }
    qInfo() << "Control-center plugin loaded and all eleven pages opened with the target interface";
    return 0;
}
