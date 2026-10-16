#include "subscriptionnodes.h"
#include "config.h"
#include <QMap>
#include <QSet>
#include <QUrl>
#include <QHostAddress>
#include <QRegularExpression>
namespace Party {
QString websiteRule(const QString &input, const QString &policy)
{
    auto value = input.trimmed();
    if (value.startsWith("*.")) value.remove(0, 2);
    if (value.isEmpty() || value.contains(QRegularExpression("[\\s,\\r\\n]")) || policy.isEmpty() || policy.contains(QRegularExpression("[,\\r\\n]")))
        throw std::runtime_error("请输入有效网址，并选择可用的策略组");
    if (!value.contains("://")) value.prepend("https://");
    const QUrl url(value, QUrl::StrictMode);
    if (!url.isValid() || (url.scheme() != "http" && url.scheme() != "https") || !url.userInfo().isEmpty() || url.host().isEmpty())
        throw std::runtime_error("仅支持域名或 HTTP / HTTPS 网址");
    QString host = url.host().toLower(); while (host.endsWith('.')) host.chop(1);
    QHostAddress address;
    if (address.setAddress(host))
        return QString(address.protocol() == QAbstractSocket::IPv6Protocol ? "IP-CIDR6,%1/128,%2,no-resolve" : "IP-CIDR,%1/32,%2,no-resolve").arg(address.toString(), policy);
    host = QString::fromLatin1(QUrl::toAce(host));
    const QRegularExpression domain("^(?=.{1,253}$)[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?(?:\\.[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?)*$");
    if (!domain.match(host).hasMatch()) throw std::runtime_error("网址中的域名无效");
    return "DOMAIN-SUFFIX," + host + ',' + policy;
}
QByteArray prependWebsiteRule(const QByteArray &config, const QString &rule)
{
    auto node = parseConfig(config); YAML::Node rules(YAML::NodeType::Sequence); rules.push_back(rule.toStdString());
    const auto fields = rule.split(','); const auto prefix = fields.value(0) + ',' + fields.value(1) + ',';
    if (node["rules"]) for (const auto &entry : node["rules"])
        if (!QString::fromStdString(entry.as<std::string>()).startsWith(prefix)) rules.push_back(entry);
    node["rules"] = rules; validateConfig(node); return emitYaml(node);
}
QStringList proxyGroupNames(const QJsonObject &proxies)
{
    QStringList names, result;
    for (const auto &value : proxies["GLOBAL"].toObject()["all"].toArray()) names << value.toString();
    for (auto it = proxies.begin(); it != proxies.end(); ++it) if (!names.contains(it.key())) names << it.key();
    for (const auto &name : names) {
        const auto group = proxies[name].toObject();
        if (group["all"].isArray() && !group["hidden"].toBool()) result << name;
    }
    return result;
}
QString proxyGroupRoute(const QJsonObject &proxies, const QString &group)
{
    QStringList route; QSet<QString> visited; QString current = group;
    while (!current.isEmpty()) {
        route << current;
        if (visited.contains(current)) { route << QObject::tr("循环引用"); break; }
        visited.insert(current);
        const auto info = proxies[current].toObject();
        if (!info["all"].isArray()) break;
        if (info["type"].toString() == "LoadBalance") { route << QObject::tr("按连接负载均衡"); break; }
        if (info["type"].toString() == "Relay") { route << QObject::tr("链式转发"); break; }
        current = info["now"].toString();
        if (current.isEmpty()) route << QObject::tr("启动后显示出口");
    }
    return route.join(QString::fromUtf8(" → "));
}
SubscriptionNodes subscriptionNodes(const QByteArray &yaml)
{
    const auto config = parseConfig(yaml);
    SubscriptionNodes result;
    QStringList nodes, groups;
    QMap<QString, QStringList> providers;
    const QMap<QString, QString> types{{"ss", "Shadowsocks"}, {"ssr", "ShadowsocksR"}, {"vmess", "Vmess"}, {"vless", "VLESS"}, {"trojan", "Trojan"}, {"hysteria2", "Hysteria2"}, {"hysteria", "Hysteria"}, {"tuic", "Tuic"}, {"http", "Http"}, {"socks5", "Socks5"}, {"wireguard", "WireGuard"}};
    auto text = [](const YAML::Node &node) { return node && node.IsScalar() ? QString::fromStdString(node.as<std::string>()) : QString(); };
    auto add = [&](const YAML::Node &proxy) {
        const auto name = text(proxy["name"]); if (name.isEmpty()) return QString();
        const auto type = text(proxy["type"]);
        result.proxies[name] = QJsonObject{{"name", name}, {"type", types.value(type, type)}};
        if (!nodes.contains(name)) nodes.append(name);
        return name;
    };
    if (config["proxies"] && config["proxies"].IsSequence()) for (const auto &proxy : config["proxies"]) add(proxy);
    if (config["proxy-providers"] && config["proxy-providers"].IsMap()) for (const auto &provider : config["proxy-providers"]) {
        const auto name = text(provider.first); const auto data = provider.second;
        if (data["payload"] && data["payload"].IsSequence()) {
            for (const auto &proxy : data["payload"]) { const auto node = add(proxy); if (!node.isEmpty()) providers[name].append(node); }
        } else ++result.remoteProviders;
    }
    result.proxies["DIRECT"] = QJsonObject{{"type", "Direct"}};
    result.proxies["REJECT"] = QJsonObject{{"type", "Reject"}};
    if (config["proxy-groups"] && config["proxy-groups"].IsSequence()) for (const auto &group : config["proxy-groups"]) {
        const auto name = text(group["name"]); if (!name.isEmpty()) groups.append(name);
    }
    const QMap<QString, QString> groupTypes{{"select", "Selector"}, {"url-test", "URLTest"}, {"fallback", "Fallback"}, {"load-balance", "LoadBalance"}, {"relay", "Relay"}};
    if (config["proxy-groups"] && config["proxy-groups"].IsSequence()) for (const auto &group : config["proxy-groups"]) {
        const auto name = text(group["name"]); if (name.isEmpty()) continue;
        QStringList members;
        if (group["proxies"] && group["proxies"].IsSequence()) for (const auto &node : group["proxies"]) members.append(text(node));
        const auto flag = [&](const char *key) { return group[key] && group[key].as<bool>(); };
        if (flag("include-all") || flag("include-all-proxies")) members.append(nodes);
        if (group["use"] && group["use"].IsSequence()) for (const auto &provider : group["use"]) members.append(providers.value(text(provider)));
        if (flag("include-all-providers")) for (const auto &provider : providers) members.append(provider);
        members.removeAll(QString()); members.removeDuplicates();
        if (group["filter"]) {
            const QRegularExpression filter(text(group["filter"]));
            if (filter.isValid()) for (int i = members.size() - 1; i >= 0; --i) if (!filter.match(members[i]).hasMatch()) members.removeAt(i);
        }
        const auto type = text(group["type"]);
        result.proxies[name] = QJsonObject{{"name", name}, {"type", groupTypes.value(type, type)}, {"all", QJsonArray::fromStringList(members)}, {"hidden", flag("hidden")}};
    }
    QString all = "__dcc_subscription_nodes__"; while (result.proxies.contains(all)) all += '_';
    result.proxies[all] = QJsonObject{{"name", all}, {"displayName", QString::fromUtf8("全部节点")}, {"type", "Subscription"}, {"all", QJsonArray::fromStringList(nodes)}};
    groups.prepend(all);
    result.proxies["GLOBAL"] = QJsonObject{{"type", "Selector"}, {"all", QJsonArray::fromStringList(groups)}, {"hidden", true}};
    return result;
}
}
