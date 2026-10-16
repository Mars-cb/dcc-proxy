#include "panel.h"
#include <QApplication>
#include <QTimer>
int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    app.setApplicationName("dcc-proxy-preview");
    Party::Panel panel;
    panel.setWindowTitle("私人代理 · dcc-proxy");
    panel.resize(920, 680);
    panel.show();
    if (app.arguments().size() > 1) panel.openPage(app.arguments().at(1));
    const int shot = app.arguments().indexOf("--screenshot");
    if (shot >= 0 && shot + 1 < app.arguments().size()) {
        const QString path = app.arguments().at(shot + 1);
        QTimer::singleShot(1500, &panel, [&panel, path, &app] { panel.grab().save(path); app.quit(); });
    }
    return app.exec();
}
