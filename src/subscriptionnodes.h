#pragma once
#include <QJsonObject>
#include <QByteArray>
namespace Party {
struct SubscriptionNodes { QJsonObject proxies; int remoteProviders = 0; };
SubscriptionNodes subscriptionNodes(const QByteArray &yaml);
}
