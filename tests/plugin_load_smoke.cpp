#include "interface/moduleinterface.h"
#include <QApplication>
#include <QPluginLoader>
#include <QDebug>

int main(int argc, char **argv)
{
    QApplication application(argc, argv);
    if (argc != 2) return 2;
    QPluginLoader loader(QString::fromLocal8Bit(argv[1]));
    auto *plugin = qobject_cast<DCC_NAMESPACE::ModuleInterface *>(loader.instance());
    if (!plugin || plugin->name() != "dcc-proxy") {
        qCritical() << "Control-center plugin failed to load:" << loader.errorString();
        return 1;
    }
    qInfo() << "Control-center plugin loaded with the target interface";
    return 0;
}
