#include "api.h"
#include "config.h"
#include <QNetworkProxy>
#include <QJsonDocument>
#include <QJsonArray>
#include <QTimer>
#include <QLocalSocket>
#include <QSharedPointer>
#include <memory>
#include <QUrlQuery>
#include <QRegularExpression>

namespace Party {
Api::Api(QObject *parent) : QObject(parent)
{
    m_network.setProxy(QNetworkProxy::NoProxy);
    m_endpoint = QUrl("http://127.0.0.1:19090");
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
    timer->start(action == "start" || action == "restart" || action == "tun" ? 140000 : 20000);
    socket->connectToServer(socketName());
}
void fetchSubscription(const QUrl &url, const QJsonObject &options, int proxyPort, QObject *context, SubscriptionDone done)
{
    if (!url.isValid() || !url.userInfo().isEmpty() || (url.scheme() != "http" && url.scheme() != "https")) { done({}, {}, QObject::tr("订阅 URL 无效")); return; }
    QString userAgent = options["userAgent"].toString();
    if (userAgent.isEmpty()) userAgent = "mihomo.party/v2.0.3 (clash.meta)";
    const QString authorization = options["authToken"].toString();
    if (userAgent.contains('\n') || userAgent.contains('\r') || authorization.contains('\n') || authorization.contains('\r')) { done({}, {}, QObject::tr("订阅请求头无效")); return; }
    using Attempt = std::function<void(QUrl, bool, int)>;
    const auto attempt = std::make_shared<Attempt>();
    const std::weak_ptr<Attempt> weak(attempt);
    *attempt = [weak, context, done, options, url, userAgent, authorization, proxyPort](QUrl current, bool proxy, int redirects) {
        auto retained = weak.lock();
        if (!retained) return;
        auto *manager = new QNetworkAccessManager(context);
        manager->setProxy(proxy ? QNetworkProxy(QNetworkProxy::HttpProxy, "127.0.0.1", quint16(proxyPort)) : QNetworkProxy(QNetworkProxy::NoProxy));
        QNetworkRequest request(current);
        request.setRawHeader("User-Agent", userAgent.toUtf8());
        request.setRawHeader("Accept-Encoding", "identity");
        if (!authorization.isEmpty() && current.host() == url.host()) request.setRawHeader("Authorization", authorization.toUtf8());
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
        auto *reply = manager->get(request);
        QTimer::singleShot(qBound(5, options["timeoutSeconds"].toInt(30), 180) * 1000, reply, &QNetworkReply::abort);
        QObject::connect(reply, &QNetworkReply::readyRead, context, [reply] { if (reply->bytesAvailable() > 8 * 1024 * 1024) reply->abort(); });
        QObject::connect(reply, &QNetworkReply::finished, context, [manager, reply, retained, done, options, url, current, proxy, redirects, proxyPort] {
            manager->deleteLater();
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const auto redirected = reply->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl();
            if (status >= 300 && status < 400 && !redirected.isEmpty()) {
                const auto next = current.resolved(redirected);
                if (redirects >= 5 || (next.scheme() != "http" && next.scheme() != "https") || (current.scheme() == "https" && next.scheme() != "https") || !next.userInfo().isEmpty()) { done({}, {}, QObject::tr("订阅重定向无效或次数过多")); return; }
                (*retained)(next, proxy, redirects + 1); return;
            }
            if (reply->error() != QNetworkReply::NoError || status < 200 || status >= 300) {
                if (!proxy && proxyPort > 0 && options["proxyFallback"].toBool(true)) { (*retained)(url, true, 0); return; }
                done({}, {}, QObject::tr("订阅下载失败（HTTP %1）：%2").arg(status).arg(reply->errorString())); return;
            }
            QJsonObject metadata;
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
    (*attempt)(url, options["useProxy"].toBool(false) && proxyPort > 0, 0);
}
}
