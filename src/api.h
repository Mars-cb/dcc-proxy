#pragma once
#include <QObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QJsonObject>
#include <QUrl>
#include <QSet>
#include <QPointer>
#include <functional>

namespace Party {
class Api : public QObject {
    Q_OBJECT
public:
    using Done = std::function<void(const QJsonValue &, const QString &)>;
    explicit Api(QObject *parent = nullptr);
    ~Api() override;
    bool configure(const QString &endpoint, const QString &secret, QString *error = nullptr);
    void request(const QByteArray &method, const QString &path, const QJsonObject &body, Done done, int timeoutMs = 12000);
    void get(const QString &path, Done done, int timeoutMs = 12000) { request("GET", path, {}, std::move(done), timeoutMs); }
    void stream(const QString &path, bool enable);
    void stopStreams();
    static QString segment(const QString &name);
    static bool validEndpoint(const QUrl &url);
signals:
    void configurationChanged();
    void streamData(const QString &path, const QJsonObject &data);
    void streamError(const QString &path, const QString &error);
private:
    QNetworkRequest makeRequest(const QString &path) const;
    QNetworkAccessManager m_network;
    QUrl m_endpoint;
    QByteArray m_secret;
    quint64 m_generation = 0;
    QSet<QNetworkReply *> m_pending;
    QMap<QString, QPointer<QNetworkReply>> m_streams;
};
void serviceRequest(const QJsonObject &command, QObject *context, Api::Done done);
using SubscriptionDone = std::function<void(const QByteArray &, const QJsonObject &, const QString &)>;
void fetchSubscription(const QUrl &url, const QJsonObject &options, int proxyPort, QObject *context, SubscriptionDone done);
}
