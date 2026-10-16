#pragma once
#include <QJsonObject>
#include <QByteArray>
#include <QStringList>
namespace Party {
struct SubscriptionNodes { QJsonObject proxies; int remoteProviders = 0; };
QString websiteRule(const QString &input, const QString &policy);
QByteArray prependWebsiteRule(const QByteArray &config, const QString &rule);
QStringList proxyGroupNames(const QJsonObject &proxies);
QString proxyGroupRoute(const QJsonObject &proxies, const QString &group);
SubscriptionNodes subscriptionNodes(const QByteArray &yaml);
}
