#include "config.h"
#include <QJsonDocument>
#include <QUrl>
#include <QUrlQuery>
#include <QVariant>
#include <stdexcept>
namespace Party {
static void invalidUri(const QString &message) { throw std::runtime_error(message.toStdString()); }
static QByteArray decode64(QByteArray bytes)
{
    bytes.replace('-', '+'); bytes.replace('_', '/');
    return QByteArray::fromBase64(bytes);
}
void applyUriTransport(YAML::Node &node, QString type, const QString &host, const QString &path, const QString &service)
{
    if (type.isEmpty() || type == "tcp") return;
    if (type == "httpupgrade") invalidUri("此内核不支持 httpupgrade 分享参数，请提供兼容的 Mihomo YAML");
    if (type != "ws" && type != "grpc" && type != "h2" && type != "http") invalidUri("不支持的传输方式：" + type);
    node["network"] = type.toStdString();
    if (type == "ws") {
        node["ws-opts"]["path"] = (path.isEmpty() ? QString("/") : path).toStdString();
        if (!host.isEmpty()) node["ws-opts"]["headers"]["Host"] = host.toStdString();
    } else if (type == "grpc") node["grpc-opts"]["grpc-service-name"] = service.toStdString();
    else if (type == "h2") {
        node["h2-opts"]["path"] = (path.isEmpty() ? QString("/") : path).toStdString();
        for (const auto &entry : host.split(',', QString::SkipEmptyParts)) node["h2-opts"]["host"].push_back(entry.toStdString());
    } else {
        node["http-opts"]["method"] = "GET";
        for (const auto &entry : path.split(',', QString::SkipEmptyParts)) node["http-opts"]["path"].push_back(entry.toStdString());
        for (const auto &entry : host.split(',', QString::SkipEmptyParts)) node["http-opts"]["headers"]["Host"].push_back(entry.toStdString());
    }
}
YAML::Node extendedSubscriptionNode(const QString &line)
{
    YAML::Node node;
    if (line.startsWith("vmess://", Qt::CaseInsensitive)) {
        QJsonParseError error;
        const auto json = QJsonDocument::fromJson(decode64(line.mid(8).toLatin1()), &error);
        if (error.error != QJsonParseError::NoError || !json.isObject()) invalidUri("VMess 分享内容不是有效的 Base64 JSON");
        const auto item = json.object();
        auto str = [&](const char *key) { const auto v = item[key]; return v.isString() ? v.toString() : v.toVariant().toString(); };
        bool ok = false; const int port = str("port").toInt(&ok);
        if (!ok || port < 1 || port > 65535 || str("add").isEmpty() || str("id").isEmpty()) invalidUri("VMess 分享缺少有效的服务器、端口或 UUID");
        node["type"] = "vmess"; node["server"] = str("add").toStdString(); node["port"] = port;
        node["name"] = (str("ps").isEmpty() ? str("add") + ':' + QString::number(port) : str("ps")).toStdString();
        node["uuid"] = str("id").toStdString();
        const int aid = str("aid").isEmpty() ? 0 : str("aid").toInt(&ok);
        if ((!str("aid").isEmpty() && !ok) || aid < 0) invalidUri("VMess alterId 无效");
        node["alterId"] = aid; node["cipher"] = (str("scy").isEmpty() ? QString("auto") : str("scy")).toStdString();
        node["tls"] = str("tls") == "tls"; node["udp"] = true;
        if (!str("sni").isEmpty()) node["servername"] = str("sni").toStdString();
        if (!str("fp").isEmpty()) node["client-fingerprint"] = str("fp").toStdString();
        if (str("allowInsecure") == "1" || str("allowInsecure") == "true") node["skip-cert-verify"] = true;
        for (const auto &entry : str("alpn").split(',', QString::SkipEmptyParts)) node["alpn"].push_back(entry.toStdString());
        auto transport = str("net");
        if (transport == "tcp" && str("type") == "http") transport = "http";
        applyUriTransport(node, transport, str("host"), str("path"), str("path"));
    } else if (line.startsWith("ssr://", Qt::CaseInsensitive)) {
        const auto decoded = QString::fromUtf8(decode64(line.mid(6).toLatin1()));
        const int split = decoded.indexOf("/?");
        const auto authority = split < 0 ? decoded : decoded.left(split);
        auto fields = authority.split(':');
        if (fields.size() < 6) invalidUri("SSR 分享字段不完整");
        const auto password = fields.takeLast(); const auto obfs = fields.takeLast(); const auto cipher = fields.takeLast();
        const auto protocol = fields.takeLast(); const auto portText = fields.takeLast(); const auto server = fields.join(':');
        bool ok = false; const int port = portText.toInt(&ok);
        if (!ok || port < 1 || port > 65535 || server.isEmpty()) invalidUri("SSR 服务器或端口无效");
        const QUrlQuery query(split < 0 ? QString() : decoded.mid(split + 2));
        auto param = [&](const QString &key) { return QString::fromUtf8(decode64(query.queryItemValue(key, QUrl::FullyDecoded).toLatin1())); };
        node["type"] = "ssr"; node["server"] = server.toStdString(); node["port"] = port;
        node["name"] = (param("remarks").isEmpty() ? server + ':' + portText : param("remarks")).toStdString();
        node["password"] = QString::fromUtf8(decode64(password.toLatin1())).toStdString();
        node["cipher"] = cipher.toStdString(); node["protocol"] = protocol.toStdString(); node["obfs"] = obfs.toStdString(); node["udp"] = true;
        if (!param("obfsparam").isEmpty()) node["obfs-param"] = param("obfsparam").toStdString();
        if (!param("protoparam").isEmpty()) node["protocol-param"] = param("protoparam").toStdString();
    } else if (line.startsWith("ss://", Qt::CaseInsensitive) && !line.section('#', 0, 0).contains('@')) {
        const auto payload = line.mid(5).section('#', 0, 0);
        const auto decoded = QString::fromUtf8(decode64(payload.toLatin1()));
        const int at = decoded.lastIndexOf('@'), colon = decoded.indexOf(':');
        if (at < 0 || colon <= 0 || colon >= at) invalidUri("旧版 Shadowsocks 分享格式无效");
        const QUrl address("ss://" + decoded.mid(at + 1));
        if (address.host().isEmpty() || address.port() < 1 || address.port() > 65535) invalidUri("Shadowsocks 服务器或端口无效");
        node["type"] = "ss"; node["server"] = address.host().toStdString(); node["port"] = address.port();
        node["cipher"] = decoded.left(colon).toStdString(); node["password"] = decoded.mid(colon + 1, at - colon - 1).toStdString(); node["udp"] = true;
        const auto name = QUrl::fromPercentEncoding(line.section('#', 1).toUtf8());
        node["name"] = (name.isEmpty() ? address.host() + ':' + QString::number(address.port()) : name).toStdString();
    }
    return node;
}
}
