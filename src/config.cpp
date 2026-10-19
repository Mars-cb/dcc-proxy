#include "config.h"
#include "tun.h"
#include "subscriptionnodes.h"
#include <QTemporaryDir>
#include <QCoreApplication>
#include <QDir>
#include <QSet>
#include <QUrl>
#include <QUrlQuery>
#include <QLockFile>
#include <QDBusInterface>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusError>
#include <memory>
#include <QDirIterator>
#include <QFile>
#include <QSaveFile>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QStandardPaths>
#include <QRegularExpression>
#include <QUuid>
#include <QDateTime>
#include <QProcess>
#include <QProcessEnvironment>
#include <stdexcept>
#include <utility>
#include <unistd.h>

namespace Party {
static void fail(const QString &message) { throw std::runtime_error(message.toStdString()); }
QString dataDir()
{
    const QString overridePath = qEnvironmentVariable("DCC_PROXY_DATA_DIR");
    if (!overridePath.isEmpty()) return overridePath;
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/dcc-proxy";
}
QString socketName() { return dataDir() + "/service.sock"; }
QString serviceExecutable()
{
    const QString sibling = QCoreApplication::applicationDirPath() + "/dde-mihomo-daemon";
    return QFile::exists(sibling) ? sibling : "/usr/lib/dcc-proxy/dde-mihomo-daemon";
}
QByteArray readFile(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) fail(f.errorString() + ": " + path);
    if (f.size() > 32 * 1024 * 1024) fail("File exceeds 32 MiB");
    return f.readAll();
}
void writeFile(const QString &path, const QByteArray &data)
{
    const QString parent = QFileInfo(path).absolutePath();
    const bool existed = QDir(parent).exists();
    if (!QDir().mkpath(parent)) fail("Cannot create " + parent);
    if (!existed) QFile::setPermissions(parent, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) fail(f.errorString());
    f.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
    if (f.write(data) != data.size() || !f.commit()) fail(f.errorString());
}
QJsonObject readJson(const QString &path)
{
    if (!QFile::exists(path)) return {};
    QJsonParseError error;
    const auto doc = QJsonDocument::fromJson(readFile(path), &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject()) fail("Invalid JSON: " + path);
    return doc.object();
}
QString newId() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
bool validId(const QString &id)
{
    return QRegularExpression("^[a-f0-9]{8}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{12}$").match(id).hasMatch();
}
static void inspect(const YAML::Node &node, int depth, int &budget)
{
    if (--budget < 0 || depth > 64) fail("YAML nesting or size limit exceeded");
    if (node.IsMap()) {
        QSet<QString> keys;
        for (const auto &entry : node) {
            if (!entry.first.IsScalar()) fail("Configuration keys must be strings");
            const auto key = QString::fromStdString(entry.first.Scalar());
            if (keys.contains(key)) fail("Duplicate configuration key: " + key);
            keys.insert(key);
            inspect(entry.second, depth + 1, budget);
        }
    } else if (node.IsSequence()) {
        for (const auto &entry : node) inspect(entry, depth + 1, budget);
    }
}
YAML::Node parseConfig(const QByteArray &data)
{
    if (data.size() > 8 * 1024 * 1024) fail("Configuration exceeds 8 MiB");
    auto docs = YAML::LoadAll(data.constData());
    if (docs.size() != 1 || !docs.front().IsMap()) fail("Expected a single YAML mapping");
    int budget = 100000;
    inspect(docs.front(), 0, budget);
    return docs.front();
}
static QByteArray wrapSubscriptionNodes(const YAML::Node &proxies)
{
    YAML::Node names(YAML::NodeType::Sequence);
    for (const auto &proxy : proxies) {
        if (!proxy["name"] || !proxy["name"].IsScalar()) fail("Subscription node is missing a name");
        names.push_back(proxy["name"].as<std::string>());
    }
    YAML::Node config;
    config["mixed-port"] = 16666; config["mode"] = "rule"; config["log-level"] = "info"; config["allow-lan"] = false;
    config["proxies"] = proxies;
    YAML::Node automatic;
    automatic["name"] = "自动选择"; automatic["type"] = "url-test"; automatic["url"] = "https://www.gstatic.com/generate_204"; automatic["interval"] = 300; automatic["proxies"] = names;
    YAML::Node selector;
    selector["name"] = "节点选择"; selector["type"] = "select"; selector["proxies"].push_back("自动选择");
    for (const auto &n : names) selector["proxies"].push_back(n.as<std::string>());
    selector["proxies"].push_back("DIRECT");
    config["proxy-groups"].push_back(selector); config["proxy-groups"].push_back(automatic);
    config["rules"].push_back("IP-CIDR,127.0.0.0/8,DIRECT,no-resolve");
    config["rules"].push_back("IP-CIDR,10.0.0.0/8,DIRECT,no-resolve");
    config["rules"].push_back("IP-CIDR,172.16.0.0/12,DIRECT,no-resolve");
    config["rules"].push_back("IP-CIDR,192.168.0.0/16,DIRECT,no-resolve");
    config["rules"].push_back("MATCH,节点选择");
    validateConfig(config);
    return emitYaml(config);
}
QByteArray normalizeSubscription(const QByteArray &data)
{
    if (data.size() > 8 * 1024 * 1024) fail("Subscription exceeds 8 MiB");
    try { auto node = parseConfig(data); validateConfig(node); if (node.size() == 1 && node["proxies"] && node["proxies"].IsSequence()) return wrapSubscriptionNodes(node["proxies"]); return data; }
    catch (const YAML::Exception &) { }
    catch (const std::runtime_error &) { }
    auto text = QString::fromUtf8(data).trimmed();
    auto decode64 = [](QByteArray encoded) {
        encoded.replace('-', '+'); encoded.replace('_', '/');
        return QByteArray::fromBase64(encoded);
    };
    if (!text.contains("://")) text = QString::fromUtf8(decode64(data)).trimmed();
    YAML::Node proxies(YAML::NodeType::Sequence);
    YAML::Node names(YAML::NodeType::Sequence);
    QSet<QString> usedNames;
    for (const auto &line : text.split(QRegularExpression("[\r\n]+"), QString::SkipEmptyParts)) {
        if (line.startsWith('#')) continue;
        auto extended = extendedSubscriptionNode(line.trimmed());
        if (extended.IsMap()) {
            auto name = QString::fromStdString(extended["name"].as<std::string>());
            const auto base = name; int index = 2;
            while (usedNames.contains(name)) name = base + " (" + QString::number(index++) + ')';
            extended["name"] = name.toStdString(); usedNames.insert(name);
            proxies.push_back(extended); names.push_back(name.toStdString()); continue;
        }
        QUrl url(line.trimmed());
        const QUrlQuery query(url);
        auto q = [&](const QString &key) { return query.queryItemValue(key, QUrl::FullyDecoded); };
        const auto scheme = url.scheme().toLower();
        YAML::Node p;
        QString name = url.fragment(QUrl::FullyDecoded);
        if (name.isEmpty()) name = url.host() + ':' + QString::number(url.port());
        if (usedNames.contains(name)) name += " (" + QString::number(proxies.size() + 1) + ')';
        usedNames.insert(name);
        p["name"] = name.toStdString();
        p["server"] = url.host().toStdString();
        p["port"] = url.port(443);
        if (!url.isValid() || url.host().isEmpty() || url.port(443) < 1 || url.port(443) > 65535) fail("Invalid node URI (server or port)");
        if (scheme == "hysteria2" || scheme == "hy2") {
            p["type"] = "hysteria2";
            auto password = url.userName(QUrl::FullyDecoded);
            if (!url.password().isEmpty()) password += ':' + url.password(QUrl::FullyDecoded);
            p["password"] = password.toStdString();
            if (!q("obfs").isEmpty()) p["obfs"] = q("obfs").toStdString();
            if (!q("obfs-password").isEmpty()) p["obfs-password"] = q("obfs-password").toStdString();
            if (!q("upmbps").isEmpty()) p["up"] = q("upmbps").toStdString();
            if (!q("downmbps").isEmpty()) p["down"] = q("downmbps").toStdString();
        } else if (scheme == "anytls") {
            p["type"] = "anytls";
            auto password = url.userName(QUrl::FullyDecoded);
            if (!url.password().isEmpty()) password += ':' + url.password(QUrl::FullyDecoded);
            p["password"] = password.toStdString();
            p["udp"] = true;
            if (!q("fp").isEmpty()) p["client-fingerprint"] = q("fp").toStdString();
        } else if (scheme == "trojan" || scheme == "vless") {
            p["type"] = scheme.toStdString();
            p[scheme == "trojan" ? "password" : "uuid"] = url.userName(QUrl::FullyDecoded).toStdString();
            p["udp"] = true;
            p["tls"] = scheme == "trojan" || q("security") == "tls" || q("security") == "reality";
            if (!q("flow").isEmpty()) p["flow"] = q("flow").toStdString();
            if (q("security") == "reality") { p["reality-opts"]["public-key"] = q("pbk").toStdString(); p["reality-opts"]["short-id"] = q("sid").toStdString(); }
            if (!q("fp").isEmpty()) p["client-fingerprint"] = q("fp").toStdString();
            const auto transport = q("type").isEmpty() ? q("network") : q("type");
            applyUriTransport(p, transport, q("host"), q("path"), q("serviceName"));
        } else if (scheme == "ss") {
            p["type"] = "ss";
            QString method = url.userName(QUrl::FullyDecoded), password = url.password(QUrl::FullyDecoded);
            if (password.isEmpty()) {
                const auto decoded = QString::fromUtf8(decode64(method.toUtf8()));
                const int colon = decoded.indexOf(':');
                if (colon < 1) fail("Unsupported Shadowsocks URI encoding");
                method = decoded.left(colon); password = decoded.mid(colon + 1);
            }
            p["cipher"] = method.toStdString(); p["password"] = password.toStdString(); p["udp"] = true;
            if (!q("plugin").isEmpty()) {
                const auto options = q("plugin").split(';'); const auto plugin = options.value(0);
                if (plugin != "obfs-local" && plugin != "simple-obfs" && plugin != "v2ray-plugin") fail("不支持该 Shadowsocks 插件，请提供 Mihomo YAML");
                p["plugin"] = plugin == "v2ray-plugin" ? "v2ray-plugin" : "obfs";
                for (int i = 1; i < options.size(); ++i) {
                    const int equal = options[i].indexOf('='); const auto key = equal < 0 ? options[i] : options[i].left(equal); const auto value = equal < 0 ? QString() : options[i].mid(equal + 1);
                    const auto mapped = key == "obfs" ? QString("mode") : (key == "obfs-host" ? QString("host") : key);
                    if (equal < 0) p["plugin-opts"][mapped.toStdString()] = true;
                    else p["plugin-opts"][mapped.toStdString()] = value.toStdString();
                }
            }
        } else if (scheme == "tuic") {
            p["type"] = "tuic"; p["uuid"] = url.userName(QUrl::FullyDecoded).toStdString(); p["password"] = url.password(QUrl::FullyDecoded).toStdString();
            if (!q("congestion_control").isEmpty()) p["congestion-controller"] = q("congestion_control").toStdString();
            if (!q("udp_relay_mode").isEmpty()) p["udp-relay-mode"] = q("udp_relay_mode").toStdString();
        } else fail("Unsupported subscription format or node scheme: " + scheme + "; request a Clash YAML subscription");
        const auto sni = !q("sni").isEmpty() ? q("sni") : q("peer");
        if (!sni.isEmpty()) p[scheme == "vless" ? "servername" : "sni"] = sni.toStdString();
        if (!q("alpn").isEmpty()) for (const auto &v : q("alpn").split(',', QString::SkipEmptyParts)) p["alpn"].push_back(v.toStdString());
        if (q("insecure") == "1" || q("insecure") == "true" || q("allowInsecure") == "1" || q("allowInsecure") == "true") p["skip-cert-verify"] = true;
        proxies.push_back(p); names.push_back(name.toStdString());
    }
    if (proxies.size() == 0) fail("Subscription contains no supported nodes");
    return wrapSubscriptionNodes(proxies);
}
void validateConfig(const YAML::Node &node)
{
    for (const char *section : {"rule-providers", "proxy-providers"}) {
        const auto providers = node[section];
        if (!providers) continue;
        if (!providers.IsMap()) fail(QString(section) + " must be a mapping");
        for (const auto &entry : providers) {
            const auto p = entry.second;
            if (!p.IsMap()) fail("Provider must be a mapping");
            const auto type = p["type"] ? p["type"].as<std::string>() : "";
            if (type != "http" && type != "file" && type != "inline") fail("Unsupported provider type: " + QString::fromStdString(type));
            if (p["command"] || p["exec"] || p["script"]) fail("Executable providers are not supported");
        }
    }
}
YAML::Node mergeYaml(const YAML::Node &base, const YAML::Node &overrides)
{
    if (!base.IsMap() || !overrides.IsMap()) return YAML::Clone(overrides);
    auto out = YAML::Clone(base);
    for (const auto &e : overrides) {
        const auto key = e.first.as<std::string>();
        if (e.second.IsNull()) out.remove(key);
        else out[key] = mergeYaml(out[key], e.second);
    }
    return out;
}
QByteArray emitYaml(const YAML::Node &node)
{
    YAML::Emitter emitter;
    emitter << node;
    if (!emitter.good()) fail(QString::fromStdString(emitter.GetLastError()));
    return QByteArray(emitter.c_str()) + '\n';
}
QJsonValue yamlToJson(const YAML::Node &node)
{
    if (node.IsMap()) {
        QJsonObject out;
        for (const auto &e : node) out[QString::fromStdString(e.first.as<std::string>())] = yamlToJson(e.second);
        return out;
    }
    if (node.IsSequence()) {
        QJsonArray out;
        for (const auto &e : node) out.append(yamlToJson(e));
        return out;
    }
    if (node.IsNull() || !node.IsDefined()) return QJsonValue();
    const QString s = QString::fromStdString(node.Scalar());
    if (node.Tag() != "!") {
        if (s == "true") return true;
        if (s == "false") return false;
        bool ok = false;
        const double n = s.toDouble(&ok);
        if (ok) return n;
    }
    return s;
}
Store::Store(QString root) : m_root(std::move(root))
{
    QDir().mkpath(m_root + "/profiles");
    QFile::setPermissions(m_root, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    if (!QFile::exists(m_root + "/settings.json")) {
        auto initial = settings(); initial["secret"] = newId() + newId();
        setSettings(initial);
    }
}
QJsonObject Store::settings() const
{
    auto out = readJson(m_root + "/settings.json");
    if (!out.contains("endpoint")) out["endpoint"] = "http://127.0.0.1:19999";
    if (!out.contains("core")) {
        QString core = QFile::exists("/usr/lib/dcc-proxy/mihomo") ? "/usr/lib/dcc-proxy/mihomo" : QStandardPaths::findExecutable("mihomo");
        if (core.isEmpty() && QFile::exists("/opt/clash-party/resources/sidecar/mihomo")) core = "/opt/clash-party/resources/sidecar/mihomo";
        out["core"] = core;
    }
    return out;
}
void Store::setSettings(const QJsonObject &settings) { writeFile(m_root + "/settings.json", QJsonDocument(settings).toJson()); }
QJsonArray Store::profiles() const { return readJson(m_root + "/profiles.json").value("profiles").toArray(); }
QString Store::profilePath(const QString &id) const
{
    if (!validId(id)) fail("Invalid profile identifier");
    return m_root + "/profiles/" + id + ".yaml";
}
QByteArray Store::profile(const QString &id) const { return readFile(profilePath(id)); }
QString Store::saveProfile(const QString &name, const QString &url, const QByteArray &data, const QString &existing)
{
    QLockFile lock(m_root + "/profiles.lock");
    if (!lock.tryLock(1000)) fail("Profile store is busy");
    const auto normalized = normalizeSubscription(data);
    validateConfig(parseConfig(normalized));
    const QString id = existing.isEmpty() ? newId() : existing;
    auto list = profiles();
    QJsonObject record;
    int index = -1;
    for (int i = 0; i < list.size(); ++i) if (list[i].toObject()["id"].toString() == id) { record = list[i].toObject(); index = i; break; }
    record["id"] = id;
    record["name"] = name.isEmpty() ? "Profile" : name;
    record["url"] = url;
    record["updated"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    writeFile(profilePath(id), normalized);
    if (index >= 0) list[index] = record; else list.append(record);
    writeFile(m_root + "/profiles.json", QJsonDocument(QJsonObject{{"profiles", list}}).toJson());
    return id;
}
void Store::removeProfile(const QString &id)
{
    QLockFile lock(m_root + "/profiles.lock");
    if (!lock.tryLock(1000)) fail("Profile store is busy");
    if (settings()["active"].toString() == id) fail("Switch to another profile before deleting the active profile");
    auto list = profiles();
    for (int i = list.size() - 1; i >= 0; --i) if (list[i].toObject()["id"].toString() == id) list.removeAt(i);
    writeFile(m_root + "/profiles.json", QJsonDocument(QJsonObject{{"profiles", list}}).toJson());
    QFile::remove(profilePath(id));
}
void Store::updateProfileOptions(const QString &id, const QJsonObject &options)
{
    if (!validId(id)) fail("Invalid profile identifier");
    QLockFile lock(m_root + "/profiles.lock");
    if (!lock.tryLock(1000)) fail("Profile store is busy");
    auto list = profiles();
    for (int i = 0; i < list.size(); ++i) if (list[i].toObject()["id"].toString() == id) {
        auto record = list[i].toObject();
        for (auto it = options.begin(); it != options.end(); ++it) if (it.key() != "id" && it.key() != "updated") record[it.key()] = it.value();
        list[i] = record;
        writeFile(m_root + "/profiles.json", QJsonDocument(QJsonObject{{"profiles", list}}).toJson());
        return;
    }
    fail("Profile no longer exists");
}
int localProxyPort(const Store &store)
{
    try {
        const auto config = parseConfig(readFile(store.root() + "/work/config.yaml"));
        const int port = config["mixed-port"] ? config["mixed-port"].as<int>() : 0;
        return port > 0 && port <= 65535 ? port : 0;
    } catch (...) { return 0; }
}
QStringList proxyPortKeys() { return {"mixed-port", "port", "socks-port", "redir-port", "tproxy-port"}; }
QJsonObject validateProxyPorts(const QJsonObject &ports)
{
    if (ports.isEmpty()) fail("请填写代理端口");
    const auto keys = proxyPortKeys(); QSet<int> used;
    for (auto it = ports.begin(); it != ports.end(); ++it) {
        const int port = it.value().toInt(-1);
        if (!keys.contains(it.key()) || !it.value().isDouble() || it.value().toDouble() != port ||
            port < (it.key() == "mixed-port" ? 1 : 0) || port > 65535)
            fail("无效的代理端口：" + it.key());
        if (port && used.contains(port)) fail(QString("代理端口 %1 重复，请为不同监听设置不同端口").arg(port));
        if (port) used.insert(port);
    }
    return ports;
}
static YAML::Node applyScript(YAML::Node node, const QString &scriptPath, const QString &profileName)
{
    QProcess process;
    process.start(serviceExecutable(), {"--eval", scriptPath, profileName});
    if (!process.waitForStarted(1000)) fail("Cannot start override evaluator");
    process.write(QJsonDocument(yamlToJson(node).toObject()).toJson(QJsonDocument::Compact));
    process.closeWriteChannel();
    if (!process.waitForFinished(4000)) { process.kill(); process.waitForFinished(); fail("Override script exceeded time limit"); }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) fail(QString::fromUtf8(process.readAllStandardError()));
    return parseConfig(process.readAllStandardOutput());
}
QByteArray Store::runtime(const QString &id) const
{
    auto node = parseConfig(profile(id));
    const auto s = managedSettings();
    // Full YAML subscriptions can omit application listener settings.
    if (!node["mixed-port"]) node["mixed-port"] = 16666;
    // Base geodata is supplied offline; updates remain an explicit user action.
    node["geo-auto-update"] = false;
    if (!node["global-ua"]) node["global-ua"] = "Mozilla/5.0";
    // Clash Party v2.0.3 controls unified-delay and enables it by default.
    node["unified-delay"] = true;
    if (!node["log-level"]) node["log-level"] = "info";
    if (QFile::exists(m_root + "/controlled.yaml"))
        node = mergeYaml(node, parseConfig(readFile(m_root + "/controlled.yaml")));
    if (s.value("overrideEnabled").toBool(true) && QFile::exists(m_root + "/override.yaml"))
        node = mergeYaml(node, parseConfig(readFile(m_root + "/override.yaml")));
    QString profileName;
    for (const auto &entry : profiles()) if (entry.toObject()["id"].toString() == id) profileName = entry.toObject()["name"].toString();
    if (s.value("scriptEnabled").toBool(false) && QFile::exists(m_root + "/override.js"))
        node = applyScript(node, m_root + "/override.js", profileName);
    const auto custom = customizations(); checkCustomizations(custom);
    for (const auto &entry : custom["overrides"].toArray()) {
        const auto item = entry.toObject();
        if (!item.value("enabled").toBool(true) || (!item["profile"].toString().isEmpty() && item["profile"].toString() != id)) continue;
        if (item["kind"].toString() == "yaml") node = mergeYaml(node, parseConfig(item["content"].toString().toUtf8()));
        else {
            QTemporaryDir script; if (!script.isValid()) fail("Cannot create override evaluator directory");
            const auto path = script.path() + "/override.js"; writeFile(path, item["content"].toString().toUtf8());
            node = applyScript(node, path, profileName);
        }
    }
    const auto customRules = custom["rules"].toObject()[id].toArray();
    for (int i = customRules.size() - 1; i >= 0; --i)
        node = parseConfig(prependWebsiteRule(emitYaml(node), customRules[i].toString()));
    // Clash Party applies the controlled TUN defaults after subscription overrides.
    auto controlled = QFile::exists(m_root + "/controlled.yaml") ? parseConfig(readFile(m_root + "/controlled.yaml")) : YAML::Node(YAML::NodeType::Map);
    // Explicit listener controls have the same final precedence as TUN controls.
    for (const auto &key : proxyPortKeys())
        if (controlled[key.toStdString()]) node[key.toStdString()] = controlled[key.toStdString()];
    if (controlled["log-level"]) node["log-level"] = controlled["log-level"];
    auto tun = defaultTunConfig();
    if (controlled["tun"]) tun = mergeYaml(tun, controlled["tun"]);
    node["tun"] = node["tun"] ? mergeYaml(node["tun"], tun) : tun;
    if (node["tun"]["enable"].as<bool>()) {
        node["dns"] = node["dns"] ? mergeYaml(defaultTunDns(), node["dns"]) : defaultTunDns();
        node["dns"]["enable"] = true;
    } else if (!node["dns"] || !node["dns"]["enable"] || !node["dns"]["enable"].as<bool>()) {
        node["tun"]["dns-hijack"] = YAML::Node(YAML::NodeType::Sequence);
    }
    // Bootstrap node hostnames independently from subscription DNS, which may
    // require an already working proxy or DNS servers blocked by this network.
    auto dns = node["dns"];
    if (s.value("systemNodeDns").toBool(true) && dns && dns.IsMap() && dns["enable"] &&
        dns["enable"].as<bool>() && !dns["proxy-server-nameserver"])
        dns["proxy-server-nameserver"].push_back("system");
    validateConfig(node);
    const QUrl url(s["endpoint"].toString());
    const QString host = url.host();
    if (host != "127.0.0.1" && host != "localhost" && host != "::1") fail("Managed core controller must use a loopback address");
    node["external-controller"] = (host == "::1" ? "[::1]" : host).toStdString() + ":" + std::to_string(url.port(19999));
    node["secret"] = s["secret"].toString().toStdString();
    node["external-controller-cors"].remove("allow-private-network");
    node.remove("external-controller-unix");
    node.remove("external-controller-pipe");
    node.remove("external-controller-tls");
    node["profile"]["store-selected"] = true;
    return emitYaml(node);
}
QByteArray backup(const QString &root)
{
    QJsonObject files;
    for (const auto &name : {"settings.json", "profiles.json", "override.yaml", "override.js", "controlled.yaml", "customizations.json"})
        if (QFile::exists(root + '/' + name)) files[name] = QString::fromLatin1(readFile(root + '/' + name).toBase64());
    QDir dir(root + "/profiles");
    for (const auto &name : dir.entryList({"*.yaml"}, QDir::Files))
        files["profiles/" + name] = QString::fromLatin1(readFile(dir.filePath(name)).toBase64());
    return QJsonDocument(QJsonObject{{"format", 1}, {"files", files}}).toJson();
}
void restore(const QString &root, const QByteArray &bytes)
{
    if (bytes.size() > 32 * 1024 * 1024) fail("Backup exceeds 32 MiB");
    const auto doc = QJsonDocument::fromJson(bytes);
    const auto obj = doc.object();
    if (obj["format"].toInt() != 1 || !obj["files"].isObject()) fail("Unsupported backup format");
    const auto files = obj["files"].toObject();
    QMap<QString, QByteArray> decoded;
    for (auto it = files.begin(); it != files.end(); ++it) {
        const QString name = it.key();
        if (name != "settings.json" && name != "profiles.json" && name != "override.yaml" && name != "override.js" && name != "controlled.yaml" && name != "customizations.json" &&
            !(name.startsWith("profiles/") && name.endsWith(".yaml") && validId(name.mid(9, name.size() - 14)))) fail("Invalid backup path");
        const auto data = QByteArray::fromBase64(it.value().toString().toLatin1());
        if (name.endsWith(".yaml")) validateConfig(parseConfig(data));
        if (name.endsWith(".json") && !QJsonDocument::fromJson(data).isObject()) fail("Invalid backup JSON");
        decoded[name] = data;
    }
    if (decoded.contains("customizations.json")) checkCustomizations(QJsonDocument::fromJson(decoded["customizations.json"]).object());
    // Restore the exact managed snapshot, including removal of stale overrides.
    QMap<QString, QByteArray> before;
    for (const auto &name : managedConfigFiles(root))
        if (QFile::exists(root + '/' + name)) before[name] = readFile(root + '/' + name);
    writeFile(root + "/before-restore.json", backup(root));
    try {
        for (auto it = decoded.begin(); it != decoded.end(); ++it) writeFile(root + '/' + it.key(), it.value());
        for (auto it = before.begin(); it != before.end(); ++it)
            if (!decoded.contains(it.key()) && !QFile::remove(root + '/' + it.key())) fail("Cannot remove stale configuration: " + it.key());
    } catch (...) {
        for (auto it = before.begin(); it != before.end(); ++it) writeFile(root + '/' + it.key(), it.value());
        for (auto it = decoded.begin(); it != decoded.end(); ++it) if (!before.contains(it.key())) QFile::remove(root + '/' + it.key());
        throw;
    }
}
static QByteArray gsettings(const QStringList &args)
{
    QProcess p;
    p.start("gsettings", args);
    if (!p.waitForFinished(2000) || p.exitStatus() != QProcess::NormalExit || p.exitCode() != 0)
        fail(QString::fromUtf8(p.readAllStandardError()).trimmed());
    return p.readAllStandardOutput().trimmed();
}
void restoreSystemProxy(bool force)
{
    const QString path = dataDir() + "/system-proxy-before.json";
    if (!QFile::exists(path)) return;
    const auto snapshot = readJson(path);
    if (snapshot["backend"].toString() == "dbus") {
        QDBusInterface network(snapshot["service"].toString(), snapshot["path"].toString(), snapshot["interface"].toString(), QDBusConnection::sessionBus());
        network.setTimeout(2000);
        if (!network.isValid()) fail(network.lastError().message());
        if (!force && snapshot["applied"].isObject()) {
            auto get = [&](const QString &method, const QVariantList &args = QVariantList()) {
                const auto reply = network.callWithArgumentList(QDBus::Block, method, args);
                if (reply.type() == QDBusMessage::ErrorMessage) fail(reply.errorMessage());
                return QJsonArray::fromVariantList(reply.arguments());
            };
            QJsonObject current{{"mode", get("GetProxyMethod")}, {"pac", get("GetAutoProxy")}, {"bypass", get("GetProxyIgnoreHosts")}};
            for (const auto &kind : {"http", "https", "socks", "ftp"}) current[kind] = get("GetProxy", {QString(kind)});
            if (current != snapshot["applied"].toObject()) {
                if (!QFile::remove(path)) fail("Cannot remove stale proxy snapshot");
                return; // Another application/user changed the proxy; preserve that state.
            }
        }
        for (const auto &v : snapshot["commands"].toArray()) {
            const auto c = v.toObject();
            const auto reply = network.callWithArgumentList(QDBus::Block, c["method"].toString(), c["args"].toArray().toVariantList());
            if (reply.type() == QDBusMessage::ErrorMessage) fail(reply.errorMessage());
        }
        if (!QFile::remove(path)) fail("Cannot remove system proxy snapshot");
        return;
    }
    const auto values = snapshot["values"].toArray();
    if (!force && snapshot["appliedValues"].isArray()) {
        for (const auto &entry : snapshot["appliedValues"].toArray()) {
            const auto item = entry.toObject();
            if (QString::fromUtf8(gsettings({"get", item["schema"].toString(), item["key"].toString()})) != item["value"].toString()) {
                if (!QFile::remove(path)) fail("Cannot remove stale proxy snapshot");
                return;
            }
        }
    }
    // Set protocol hosts and ports before returning to the previous mode.
    for (const auto &v : values) {
        const auto e = v.toObject();
        gsettings({"set", e["schema"].toString(), e["key"].toString(), e["value"].toString()});
    }
    if (!QFile::remove(path)) fail("Cannot remove system proxy snapshot");
}
void setSystemProxy(const QString &mode, int port, const QString &pacUrl, const QStringList &bypass)
{
    if (mode == "none") { restoreSystemProxy(); return; }
    if (mode != "manual" && mode != "auto") fail("Invalid proxy mode");
    if (port < 1 || port > 65535) fail("Enable mixed-port before configuring system proxy");
    const auto normalizedBypass = normalizeProxyBypass(bypass);
    std::unique_ptr<QDBusInterface> network;
    QString service, objectPath;
    for (const auto &candidate : {QString("org.deepin.dde.Network1"), QString("com.deepin.daemon.Network")}) {
        service = candidate;
        objectPath = candidate.startsWith("org.") ? "/org/deepin/dde/Network1" : "/com/deepin/daemon/Network";
        network.reset(new QDBusInterface(service, objectPath, service, QDBusConnection::sessionBus()));
        network->setTimeout(2000);
        if (network->isValid()) break;
    }
    if (network && network->isValid()) {
        auto call = [&](const QString &method, const QVariantList &args = QVariantList()) {
            const auto reply = network->callWithArgumentList(QDBus::Block, method, args);
            if (reply.type() == QDBusMessage::ErrorMessage) fail(reply.errorMessage());
            return reply.arguments();
        };
        const QString snapshotPath = dataDir() + "/system-proxy-before.json";
        if (!QFile::exists(snapshotPath)) {
            QJsonArray commands;
            for (const auto &kind : {"http", "https", "socks", "ftp"}) {
                const auto old = call("GetProxy", {QString(kind)});
                if (old.size() < 2) fail("Invalid GetProxy response");
                commands.append(QJsonObject{{"method", "SetProxy"}, {"args", QJsonArray{kind, old[0].toString(), old[1].toString()}}});
            }
            commands.append(QJsonObject{{"method", "SetAutoProxy"}, {"args", QJsonArray{call("GetAutoProxy").value(0).toString()}}});
            commands.append(QJsonObject{{"method", "SetProxyIgnoreHosts"}, {"args", QJsonArray{call("GetProxyIgnoreHosts").value(0).toString()}}});
            commands.append(QJsonObject{{"method", "SetProxyMethod"}, {"args", QJsonArray{call("GetProxyMethod").value(0).toString()}}});
            writeFile(snapshotPath, QJsonDocument(QJsonObject{{"backend", "dbus"}, {"service", service}, {"path", objectPath}, {"interface", service}, {"commands", commands}}).toJson());
        }
        auto snapshot = readJson(snapshotPath);
        auto commands = snapshot["commands"].toArray();
        bool capturedBypass = false;
        for (const auto &v : commands) if (v.toObject()["method"].toString() == "SetProxyIgnoreHosts") capturedBypass = true;
        if (!capturedBypass) {
            commands.insert(qMax(0, commands.size() - 1), QJsonObject{{"method", "SetProxyIgnoreHosts"}, {"args", QJsonArray{call("GetProxyIgnoreHosts").value(0).toString()}}});
            snapshot["commands"] = commands;
            writeFile(snapshotPath, QJsonDocument(snapshot).toJson());
        }
        try {
            call("SetProxyIgnoreHosts", {normalizedBypass.join(',')});
            if (mode == "manual") for (const auto &kind : {"http", "https", "socks", "ftp"}) call("SetProxy", {QString(kind), QString("127.0.0.1"), QString::number(port)});
            if (mode == "auto") call("SetAutoProxy", {pacUrl});
            call("SetProxyMethod", {mode});
            auto appliedValues = [&](const QString &method, const QVariantList &args = QVariantList()) { return QJsonArray::fromVariantList(call(method, args)); };
            QJsonObject applied{{"mode", appliedValues("GetProxyMethod")}, {"pac", appliedValues("GetAutoProxy")}, {"bypass", appliedValues("GetProxyIgnoreHosts")}};
            for (const auto &kind : {"http", "https", "socks", "ftp"}) applied[kind] = appliedValues("GetProxy", {QString(kind)});
            snapshot["applied"] = applied; writeFile(snapshotPath, QJsonDocument(snapshot).toJson());
        } catch (...) { restoreSystemProxy(true); throw; }
        return;
    }
    const auto schemas = gsettings({"list-schemas"});
    const QString schema = schemas.contains("com.deepin.wrap.gnome.system.proxy\n") ? "com.deepin.wrap.gnome.system.proxy" : "org.gnome.system.proxy";
    const QString path = dataDir() + "/system-proxy-before.json";
    if (!QFile::exists(path)) {
        QJsonArray values;
        auto capture = [&](const QString &s, const QString &key) {
            values.append(QJsonObject{{"schema", s}, {"key", key}, {"value", QString::fromUtf8(gsettings({"get", s, key}))}});
        };
        for (const auto &kind : {"http", "https", "socks", "ftp"}) { capture(schema + '.' + kind, "host"); capture(schema + '.' + kind, "port"); }
        capture(schema, "autoconfig-url");
        capture(schema, "ignore-hosts");
        capture(schema, "mode");
        writeFile(path, QJsonDocument(QJsonObject{{"values", values}}).toJson());
    }
    auto snapshot = readJson(path);
    auto values = snapshot["values"].toArray();
    bool capturedBypass = false;
    for (const auto &v : values) if (v.toObject()["key"].toString() == "ignore-hosts") capturedBypass = true;
    if (!capturedBypass) {
        values.insert(qMax(0, values.size() - 1), QJsonObject{{"schema", schema}, {"key", "ignore-hosts"}, {"value", QString::fromUtf8(gsettings({"get", schema, "ignore-hosts"}))}});
        snapshot["values"] = values;
        writeFile(path, QJsonDocument(snapshot).toJson());
    }
    try {
        const QString hosts = normalizedBypass.isEmpty() ? "@as []" : QString::fromUtf8(QJsonDocument(QJsonArray::fromStringList(normalizedBypass)).toJson(QJsonDocument::Compact));
        gsettings({"set", schema, "ignore-hosts", hosts});
        if (mode == "manual") for (const auto &kind : {"http", "https", "socks", "ftp"}) {
            gsettings({"set", schema + '.' + kind, "host", "127.0.0.1"});
            gsettings({"set", schema + '.' + kind, "port", QString::number(port)});
        }
        if (mode == "auto") gsettings({"set", schema, "autoconfig-url", pacUrl});
        gsettings({"set", schema, "mode", mode});
        QJsonArray applied;
        for (const auto &entry : values) { auto item = entry.toObject(); item["value"] = QString::fromUtf8(gsettings({"get", item["schema"].toString(), item["key"].toString()})); applied.append(item); }
        snapshot["appliedValues"] = applied; writeFile(path, QJsonDocument(snapshot).toJson());
    } catch (...) { restoreSystemProxy(true); throw; }
}
}
