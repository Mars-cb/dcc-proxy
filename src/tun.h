#pragma once
#include "config.h"
#include <QObject>
#include <functional>
namespace Party {
YAML::Node defaultTunConfig();
YAML::Node defaultTunDns();
QJsonObject validateTunOptions(const QJsonObject &options);
bool hasTunPermission(const QString &core);
bool processRunsAsRoot(qint64 pid);
bool processHasTunPermission(qint64 pid);
void requestTunPermission(const QString &core, QObject *context, std::function<void(QString)> done);
}
