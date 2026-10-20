#include "api.h"
#include "config.h"
#include <QNetworkProxy>
#include <QNetworkProxyFactory>
#include <QNetworkProxyQuery>
#include <QHostAddress>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QJsonDocument>
#include <QJsonArray>
#include <QTimer>
#include <QLocalSocket>
#include <QSharedPointer>
#include <memory>
#include <QUrlQuery>
#include <QRegularExpression>
#include <QProcess>
#include <stdexcept>

namespace Party {
void validateAndCommit(const QString &root, const QJsonObject &change, QObject *context, Api::Done done, bool commitChanges)
{
    auto *process = new QProcess(context);
    auto completed = QSharedPointer<bool>::create(false);
    const auto initial = configFingerprint(root);
    auto finish = [root, change, process, completed, initial, done, commitChanges](bool success) mutable {
        if (*completed) return;
        *completed = true;
        const auto result = QJsonDocument::fromJson(process->readAllStandardOutput()).object();
        const auto detail = QString::fromUtf8(process->readAllStandardError()).trimmed();
        process->deleteLater();
        try {
            if (!success || !result["valid"].toBool()) throw std::runtime_error((detail.isEmpty() ? QString("配置校验失败，原配置已保留") : detail).toStdString());
            if (result["fingerprint"].toString().toLatin1() != initial) throw std::runtime_error("校验期间配置发生变化，请重新保存");
            auto commit = change; commit["fingerprint"] = result["fingerprint"];
            if (commitChanges) commitStoreChange(root, commit);
            done(result, {});
        } catch (const std::exception &e) { done({}, QString::fromUtf8(e.what())); }
    };
    QObject::connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), context,
                     [finish](int code, QProcess::ExitStatus status) mutable { finish(code == 0 && status == QProcess::NormalExit); });
    QObject::connect(process, &QProcess::errorOccurred, context, [finish](QProcess::ProcessError error) mutable { if (error == QProcess::FailedToStart) finish(false); });
    QTimer::singleShot(125000, process, [process] { if (process->state() != QProcess::NotRunning) process->kill(); });
    process->start(serviceExecutable(), {"--validate-store", root});
    process->write(QJsonDocument(change).toJson(QJsonDocument::Compact)); process->closeWriteChannel();
}
Api::Api(QObject *parent) : QObject(parent)
{
    m_network.setProxy(QNetworkProxy::NoProxy);
    m_endpoint = QUrl("http://127.0.0.1:19999");
}
Api::~Api()
{
    ++m_generation;
    const auto pending = m_pending;
    for (auto *reply : pending) { reply->disconnect(this); reply->abort(); }
}
bool Api::validEndpoint(const QUrl &url)
{
    if (!url.isValid() || url.host().isEmpty() || !url.userInfo().isEmpty() || url.hasQuery() || url.hasFragment() ||
        (!url.path().isEmpty() && url.path() != "/") || url.port(80) < 1) return false;
    return url.scheme() == "https" || (url.scheme() == "http" &&
        (url.host() == "127.0.0.1" || url.host() == "localhost" || url.host() == "::1"));
}
bool Api::configure(const QString &endpoint, const QString &secret, QString *error)
{
    const QUrl url(endpoint);
    if (!validEndpoint(url) || secret.contains('\r') || secret.contains('\n')) {
        if (error) *error = tr("Use HTTP on localhost or HTTPS for a remote controller, without a path or embedded credentials.");
        return false;
    }
    if (url == m_endpoint && secret.toUtf8() == m_secret) return true;
    ++m_generation;
    stopStreams();
    const auto pending = m_pending;
    for (auto *reply : pending) reply->abort();
    m_endpoint = url;
    m_secret = secret.toUtf8();
    emit configurationChanged();
    return true;
}
QString Api::segment(const QString &name) { return QString::fromLatin1(QUrl::toPercentEncoding(name)); }
QNetworkRequest Api::makeRequest(const QString &path) const
{
    auto base = m_endpoint.toString(QUrl::RemovePath | QUrl::RemoveQuery | QUrl::RemoveFragment);
    QNetworkRequest req(QUrl::fromEncoded(base.toUtf8() + path.toUtf8(), QUrl::StrictMode));
    if (!m_secret.isEmpty()) req.setRawHeader("Authorization", "Bearer " + m_secret);
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    return req;
}
void Api::request(const QByteArray &method, const QString &path, const QJsonObject &body, Done done, int timeoutMs)
{
    if (m_callbackWrapper) done = m_callbackWrapper(std::move(done));
    auto *reply = m_network.sendCustomRequest(makeRequest(path), method,
        body.isEmpty() ? QByteArray() : QJsonDocument(body).toJson(QJsonDocument::Compact));
    m_pending.insert(reply);
    const auto generation = m_generation;
    auto *timer = new QTimer(reply);
    timer->setSingleShot(true);
    QObject::connect(timer, &QTimer::timeout, reply, &QNetworkReply::abort);
    timer->start(qBound(1000, timeoutMs, 65000));
    QObject::connect(reply, &QNetworkReply::readyRead, this, [reply] {
        if (reply->bytesAvailable() > 16 * 1024 * 1024) reply->abort();
    });
    QObject::connect(reply, &QNetworkReply::finished, this, [this, reply, timer, generation, done] {
        timer->stop();
        m_pending.remove(reply);
        reply->deleteLater();
        if (generation != m_generation) return;
        const auto bytes = reply->readAll();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        auto doc = QJsonDocument::fromJson(bytes);
        QString error;
        if (reply->error() != QNetworkReply::NoError || status < 200 || status >= 300) {
            error = doc.object().value("message").toString();
            if (error.isEmpty()) error = status ? tr("HTTP %1: %2").arg(status).arg(reply->errorString()) : reply->errorString();
        } else if (!bytes.isEmpty() && doc.isNull()) error = tr("Controller returned invalid JSON");
        done(doc.isArray() ? QJsonValue(doc.array()) : QJsonValue(doc.object()), error);
    });
}
void Api::stream(const QString &path, bool enable)
{
    if (m_streams.value(path)) {
        if (enable) return;
        auto *reply = m_streams.take(path).data();
        reply->abort();
        return;
    }
    if (!enable) return;
    auto *reply = m_network.get(makeRequest(path));
    m_streams[path] = reply;
    m_pending.insert(reply);
    auto buffer = QSharedPointer<QByteArray>::create();
    QObject::connect(reply, &QNetworkReply::readyRead, this, [this, path, reply, buffer] {
        buffer->append(reply->readAll());
        if (buffer->size() > 1024 * 1024) { reply->abort(); return; }
        int pos;
        while ((pos = buffer->indexOf('\n')) >= 0) {
            const auto line = buffer->left(pos);
            buffer->remove(0, pos + 1);
            const auto doc = QJsonDocument::fromJson(line);
            if (doc.isObject()) emit streamData(path, doc.object());
        }
    });
    QObject::connect(reply, &QNetworkReply::finished, this, [this, path, reply] {
        m_pending.remove(reply);
        if (m_streams.value(path) == reply) {
            m_streams.remove(path);
            emit streamError(path, reply->errorString());
        }
        reply->deleteLater();
    });
}
void Api::stopStreams()
{
    const auto streams = m_streams;
    m_streams.clear();
    for (auto reply : streams) if (reply) reply->abort();
}
void serviceRequest(const QJsonObject &command, QObject *context, Api::Done done)
{
    auto *socket = new QLocalSocket(context);
    auto *timer = new QTimer(socket);
    timer->setSingleShot(true);
    const auto finished = QSharedPointer<bool>::create(false);
    auto finish = [socket, timer, done, finished](const QJsonValue &result, const QString &error) {
        if (*finished) return;
        *finished = true;
        timer->stop();
        socket->disconnectFromServer();
        socket->deleteLater();
        done(result, error);
    };
    QObject::connect(timer, &QTimer::timeout, context, [finish] { finish({}, QObject::tr("Background service timed out")); });
    QObject::connect(socket, QOverload<QLocalSocket::LocalSocketError>::of(&QLocalSocket::error), context,
        [socket, finish](QLocalSocket::LocalSocketError) { finish({}, socket->errorString()); });
    QObject::connect(socket, &QLocalSocket::connected, context, [socket, command] {
        socket->write(QJsonDocument(command).toJson(QJsonDocument::Compact) + '\n');
    });
    auto buffer = QSharedPointer<QByteArray>::create();
    QObject::connect(socket, &QLocalSocket::readyRead, context, [socket, buffer, finish] {
        buffer->append(socket->readAll());
        if (buffer->size() > 1024 * 1024) { finish({}, QObject::tr("Invalid service response")); return; }
        if (!buffer->contains('\n')) return;
        const auto doc = QJsonDocument::fromJson(buffer->left(buffer->indexOf('\n')));
        if (!doc.isObject()) { finish({}, QObject::tr("Invalid service response")); return; }
        finish(doc.object(), doc.object()["error"].toString());
    });
    const auto action = command["action"].toString();
    timer->start(action == "start" || action == "restart" || action == "tun" || action == "ports" ||
                 action == "apply" || action == "recover-orphan" ? 140000 : 20000);
    socket->connectToServer(socketName());
}
static bool usableDownloadProxy(const QNetworkProxy &proxy)
{
    return (proxy.type() == QNetworkProxy::HttpProxy || proxy.type() == QNetworkProxy::Socks5Proxy) &&
        !proxy.hostName().isEmpty() && proxy.port() > 0;
}
static QNetworkProxy systemDownloadProxy(const QUrl &url)
{
    for (const auto &proxy : QNetworkProxyFactory::systemProxyForQuery(QNetworkProxyQuery(url)))
        if (usableDownloadProxy(proxy)) return proxy;
    // An explicit environment proxy may have been bypassed by no_proxy.
    // In that case, keep Qt's direct decision instead of substituting a desktop proxy.
    for (const auto &key : {"http_proxy", "https_proxy", "HTTP_PROXY", "HTTPS_PROXY", "all_proxy", "ALL_PROXY"})
        if (!qgetenv(key).isEmpty()) return QNetworkProxy::NoProxy;

    // Qt builds without libproxy only read environment variables on Linux.
    // The control center also needs the proxy configured by the Deepin desktop.
    for (const auto &service : {QString("org.deepin.dde.Network1"), QString("com.deepin.daemon.Network")}) {
        QDBusInterface network(service, service.startsWith("org.") ? "/org/deepin/dde/Network1" : "/com/deepin/daemon/Network",
                               service, QDBusConnection::sessionBus());
        network.setTimeout(1000);
        if (!network.isValid()) continue;
        const auto mode = network.call("GetProxyMethod");
        if (mode.type() == QDBusMessage::ErrorMessage || mode.arguments().value(0).toString() != "manual") break;
        const auto ignored = network.call("GetProxyIgnoreHosts");
        if (ignored.type() == QDBusMessage::ErrorMessage) break;
        const auto host = url.host().toLower();
        QHostAddress address(host);
        for (auto entry : ignored.arguments().value(0).toString().split(',', QString::SkipEmptyParts)) {
            entry = entry.trimmed().toLower();
            const auto subnet = QHostAddress::parseSubnet(entry);
            if (entry == "*" || host == entry || (!address.isNull() && address == QHostAddress(entry)) ||
                (!subnet.first.isNull() && address.isInSubnet(subnet))) return QNetworkProxy::NoProxy;
            if (entry.startsWith("*.")) entry.remove(0, 1);
            if (entry.startsWith('.') && host.endsWith(entry)) return QNetworkProxy::NoProxy;
        }
        QStringList kinds{url.scheme() == "https" ? "https" : "http", "http", "socks"};
        kinds.removeDuplicates();
        for (const auto &kind : kinds) {
            const auto reply = network.call("GetProxy", kind);
            if (reply.type() == QDBusMessage::ErrorMessage) continue;
            const auto args = reply.arguments();
            const int port = args.value(1).toString().toInt();
            const auto proxy = QNetworkProxy(kind == "socks" ? QNetworkProxy::Socks5Proxy : QNetworkProxy::HttpProxy,
                                             args.value(0).toString(), port > 0 && port <= 65535 ? quint16(port) : 0);
            if (usableDownloadProxy(proxy)) return proxy;
        }
        break;
    }
    return QNetworkProxy::NoProxy;
}
static QString subscriptionFailureDetail(QNetworkReply *reply, int status)
{
    const auto body = status >= 400 ? reply->read(4096) : QByteArray();
    QString detail = !status || (status >= 200 && status < 300) ? reply->errorString() : QString();
    if (status == 403 && body.contains(QStringLiteral("关闭代理").toUtf8()))
        detail = QObject::tr("订阅服务拒绝当前代理出口，要求关闭代理后访问。请尝试直连或更换网络。");
    else if (body.contains("doesn't contain any valid node info") || body.contains("No nodes were found"))
        detail = QObject::tr("订阅转换服务未能从原始订阅获取有效节点。");
    else if (status >= 300) {
        const auto type = reply->header(QNetworkRequest::ContentTypeHeader).toString().section(';', 0, 0).trimmed().toLower();
        if (type == "text/plain") detail = QString::fromUtf8(body);
        else if (type == "application/json" || type.endsWith("+json")) {
            const auto object = QJsonDocument::fromJson(body).object();
            for (const auto &key : {"message", "error", "detail"})
                if (object[key].isString()) { detail = object[key].toString(); break; }
        }
        if (detail.isEmpty() || detail.contains('<') || detail.contains('>'))
            detail = reply->attribute(QNetworkRequest::HttpReasonPhraseAttribute).toString();
    }
    if (detail.isEmpty()) detail = status ? QObject::tr("服务器拒绝请求") : reply->errorString();
    // Error bodies and Qt's errorString can echo credential-bearing subscription URLs.
    detail.replace(QRegularExpression("https?://[^\\s<>\"']+", QRegularExpression::CaseInsensitiveOption),
                   QObject::tr("[订阅地址已隐藏]"));
    return detail.simplified().left(500);
}
void fetchSubscription(const QUrl &url, const QJsonObject &options, int proxyPort, QObject *context, SubscriptionDone done)
{
    if (!url.isValid() || !url.userInfo().isEmpty() || (url.scheme() != "http" && url.scheme() != "https")) { done({}, {}, QObject::tr("订阅 URL 无效")); return; }
    QString userAgent = options["userAgent"].toString();
    if (userAgent.isEmpty()) userAgent = "Mozilla/5.0";
    const QString authorization = options["authToken"].toString();
    if (userAgent.contains('\n') || userAgent.contains('\r') || authorization.contains('\n') || authorization.contains('\r')) { done({}, {}, QObject::tr("订阅请求头无效")); return; }
    const bool automaticUserAgent = options["userAgent"].toString().isEmpty();
    enum Route { Direct, LocalProxy, SystemProxy };
    using Attempt = std::function<void(QUrl, Route, int, bool)>;
    const auto attempt = std::make_shared<Attempt>();
    const auto failures = std::make_shared<QStringList>();
    const std::weak_ptr<Attempt> weak(attempt);
    *attempt = [weak, failures, context, done, options, url, userAgent, automaticUserAgent, authorization, proxyPort](QUrl current, Route route, int redirects, bool compatibleAgent) {
        auto retained = weak.lock();
        if (!retained) return;
        auto *manager = new QNetworkAccessManager(context);
        const auto localProxy = QNetworkProxy(QNetworkProxy::HttpProxy, "127.0.0.1", quint16(proxyPort));
        manager->setProxy(route == SystemProxy ? systemDownloadProxy(current) :
                          (route == LocalProxy ? localProxy : QNetworkProxy(QNetworkProxy::NoProxy)));
        QNetworkRequest request(current);
        // Some subscription endpoints only accept a recognized client identifier.
        const QByteArray effectiveAgent = compatibleAgent ? QByteArray("clash-verge/v2.4.2") : userAgent.toUtf8();
        request.setRawHeader("User-Agent", effectiveAgent);
        request.setRawHeader("Accept-Encoding", "identity");
        if (!authorization.isEmpty() && current.host() == url.host()) request.setRawHeader("Authorization", authorization.toUtf8());
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
        auto *reply = manager->get(request);
        QTimer::singleShot(qBound(5, options["timeoutSeconds"].toInt(30), 180) * 1000, reply, &QNetworkReply::abort);
        QObject::connect(reply, &QNetworkReply::readyRead, context, [reply] { if (reply->bytesAvailable() > 8 * 1024 * 1024) reply->abort(); });
        QObject::connect(reply, &QNetworkReply::finished, context, [manager, reply, retained, failures, done, options, url, current, route, redirects, proxyPort, localProxy, automaticUserAgent, compatibleAgent, effectiveAgent] {
            manager->deleteLater();
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const auto redirected = reply->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl();
            if (status >= 300 && status < 400 && !redirected.isEmpty()) {
                const auto next = current.resolved(redirected);
                if (redirects >= 5 || (next.scheme() != "http" && next.scheme() != "https") || (current.scheme() == "https" && next.scheme() != "https") || !next.userInfo().isEmpty()) { done({}, {}, QObject::tr("订阅重定向无效或次数过多")); return; }
                (*retained)(next, route, redirects + 1, compatibleAgent); return;
            }
            if (reply->error() != QNetworkReply::NoError || status < 200 || status >= 300) {
                const auto routeName = route == Direct ? QObject::tr("直连") :
                    (route == LocalProxy ? QObject::tr("本地代理") : QObject::tr("系统代理"));
                const auto reason = status ? QObject::tr("%1（HTTP %2）：%3").arg(routeName).arg(status).arg(subscriptionFailureDetail(reply, status)) :
                    QObject::tr("%1（未收到 HTTP 响应）：%2").arg(routeName, subscriptionFailureDetail(reply, status));
                if (!failures->contains(reason)) failures->append(reason);
                if (status == 403 && automaticUserAgent && !compatibleAgent) { (*retained)(url, route, 0, true); return; }
                if (options["proxyFallback"].toBool(true)) {
                    if (route == Direct && proxyPort > 0) { (*retained)(url, LocalProxy, 0, compatibleAgent); return; }
                    if (route != SystemProxy) {
                        const auto systemProxy = systemDownloadProxy(current);
                        if (usableDownloadProxy(systemProxy) && (route != LocalProxy || systemProxy != localProxy)) {
                            (*retained)(url, SystemProxy, 0, compatibleAgent); return;
                        }
                    }
                }
                done({}, {}, QObject::tr("订阅下载失败：\n%1").arg(failures->join('\n'))); return;
            }
            QJsonObject metadata;
            if (compatibleAgent) metadata["userAgent"] = QString::fromUtf8(effectiveAgent);
            const auto info = QString::fromLatin1(reply->rawHeader("subscription-userinfo"));
            QJsonObject usage;
            for (const auto &field : info.split(';', QString::SkipEmptyParts)) {
                const auto pair = field.trimmed().split('=');
                if (pair.size() == 2 && QStringList{"upload", "download", "total", "expire"}.contains(pair[0])) usage[pair[0]] = pair[1];
            }
            metadata["usage"] = usage;
            bool ok = false;
            const int hours = QString::fromLatin1(reply->rawHeader("profile-update-interval")).toInt(&ok);
            if (ok && hours > 0 && hours <= 168) metadata["suggestedUpdateMinutes"] = hours * 60;
            const auto disposition = QString::fromLatin1(reply->rawHeader("content-disposition"));
            const auto encoded = QRegularExpression("filename\\*=UTF-8''([^;]+)", QRegularExpression::CaseInsensitiveOption).match(disposition);
            if (encoded.hasMatch()) metadata["suggestedName"] = QUrl::fromPercentEncoding(encoded.captured(1).toLatin1());
            else {
                const auto plain = QRegularExpression("filename=\"?([^;\"]+)", QRegularExpression::CaseInsensitiveOption).match(disposition);
                if (plain.hasMatch()) metadata["suggestedName"] = plain.captured(1);
            }
            done(reply->readAll(), metadata, {});
        });
    };
    (*attempt)(url, options["useProxy"].toBool(false) ? (proxyPort > 0 ? LocalProxy : SystemProxy) : Direct, 0, false);
}
}
