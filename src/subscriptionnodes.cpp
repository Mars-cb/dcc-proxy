#include "subscriptionnodes.h"
#include "config.h"
#include <QMap>
#include <QRegularExpression>
namespace Party {
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
        result.proxies[name] = QJsonObject{{"name", name}, {"type", groupTypes.value(type, type)}, {"all", QJsonArray::fromStringList(members)}};
    }
    QString all = "__dcc_subscription_nodes__"; while (result.proxies.contains(all)) all += '_';
    result.proxies[all] = QJsonObject{{"name", all}, {"displayName", QString::fromUtf8("全部节点")}, {"type", "Subscription"}, {"all", QJsonArray::fromStringList(nodes)}};
    groups.prepend(all);
    result.proxies["GLOBAL"] = QJsonObject{{"type", "Selector"}, {"all", QJsonArray::fromStringList(groups)}, {"hidden", true}};
    return result;
}
}
