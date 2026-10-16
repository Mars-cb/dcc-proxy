#include "config.h"
#include "api.h"
#include <QCoreApplication>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QProcess>
#include <QProcessEnvironment>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QNetworkProxy>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJSEngine>
#include <QTimer>
#include <QFile>
#include <QDir>
#include <QDateTime>
#include <QUrl>
#include <QSet>
#include <QPointer>
#include <QFileInfo>
#include <QSharedPointer>
#include <QTextStream>
#include <QThread>
#include <QSocketNotifier>
#include <QTcpServer>
#include <QTcpSocket>
#include <sys/socket.h>
#include <signal.h>
#include <unistd.h>
#include <functional>

using namespace Party;
static int signalFds[2];
static void stopSignal(int) { const char c = 1; const auto ignored = ::write(signalFds[0], &c, 1); Q_UNUSED(ignored) }

class Daemon : public QObject {
public:
    Daemon() : m_store(), m_core(this), m_network(this), m_lock(dataDir() + "/daemon.lock")
    {
        if (!m_lock.tryLock()) throw std::runtime_error("Background service is already running");
        m_network.setProxy(QNetworkProxy::NoProxy);
        try { restoreSystemProxy(); } catch (const std::exception &e) { log(QByteArray(e.what()) + '\n'); }
        connect(&m_pacServer, &QTcpServer::newConnection, this, [this] {
            while (auto *socket = m_pacServer.nextPendingConnection()) {
                socket->setParent(this);
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                QTimer::singleShot(2000, socket, &QTcpSocket::disconnectFromHost);
                connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
                    const auto request = socket->readAll();
                    if (!request.startsWith("GET /proxy.pac ")) { socket->disconnectFromHost(); return; }
                    const auto body = proxyPac(m_pacProxyPort, m_pacBypass);
                    socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/x-ns-proxy-autoconfig\r\nConnection: close\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\n\r\n" + body);
                    socket->disconnectFromHost();
                });
            }
        });
        m_server.setSocketOptions(QLocalServer::UserAccessOption);
        QLocalServer::removeServer(socketName());
        if (!m_server.listen(socketName())) throw std::runtime_error(m_server.errorString().toStdString());
        connect(&m_server, &QLocalServer::newConnection, this, [this] {
            while (auto *socket = m_server.nextPendingConnection()) {
                auto buffer = QSharedPointer<QByteArray>::create();
                connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
                QTimer::singleShot(145000, socket, [socket] { socket->disconnectFromServer(); });
                connect(socket, &QLocalSocket::readyRead, this, [this, socket, buffer] {
                    buffer->append(socket->readAll());
                    if (buffer->size() > 65536) { socket->disconnectFromServer(); return; }
                    if (!buffer->contains('\n')) return;
                    const auto doc = QJsonDocument::fromJson(buffer->left(buffer->indexOf('\n')));
                    socket->disconnect(this);
                    const QPointer<QLocalSocket> guarded(socket);
                    auto answer = [guarded](QJsonObject out) {
                        if (!guarded) return;
                        guarded->write(QJsonDocument(out).toJson(QJsonDocument::Compact) + '\n');
                        guarded->flush();
                        guarded->disconnectFromServer();
                    };
                    if (!doc.isObject()) { answer({{"error", "Invalid command"}}); return; }
                    try { handle(doc.object(), answer); }
                    catch (const std::exception &e) { answer({{"error", QString::fromUtf8(e.what())}}); }
                });
            }
        });
        m_core.setProcessChannelMode(QProcess::MergedChannels);
        connect(&m_core, &QProcess::readyRead, this, [this] { log(m_core.readAll()); });
        connect(&m_core, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) { m_error = m_core.errorString(); log(m_error.toUtf8()); });
        connect(&m_core, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this](int code, QProcess::ExitStatus exitStatus) {
                log(QString("Core exited: %1\n").arg(code).toUtf8());
                if (!m_stopping && m_wanted && (code != 0 || exitStatus == QProcess::CrashExit) && ++m_failures <= 3) {
                    const auto epoch = m_restartEpoch;
                    QTimer::singleShot(3000, this, [this, epoch] {
                        if (!m_wanted || epoch != m_restartEpoch) return;
                        try { startCore(m_profile, [](QJsonObject) {}, true); } catch (const std::exception &e) { m_error = QString::fromUtf8(e.what()); }
                    });
                }
                m_stopping = false;
            });
        connect(&m_updateTimer, &QTimer::timeout, this, [this] { updateProfiles(); });
        m_updateTimer.start(60000);
        const auto s = m_store.settings();
        if (s["autoStart"].toBool() && !s["active"].toString().isEmpty())
            QTimer::singleShot(0, this, [this, s] {
                try { startCore(s["active"].toString(), [](QJsonObject) {}); }
                catch (const std::exception &e) { m_error = QString::fromUtf8(e.what()); log(m_error.toUtf8()); }
            });
    }
    ~Daemon() override {
        stopCore();
        try { restoreSystemProxy(); } catch (...) { }
        m_server.close(); QLocalServer::removeServer(socketName());
    }
private:
    using Answer = std::function<void(QJsonObject)>;
    void log(const QByteArray &bytes)
    {
        const QString path = dataDir() + "/daemon.log";
        if (QFileInfo(path).size() > 2 * 1024 * 1024) { QFile::remove(path + ".1"); QFile::rename(path, path + ".1"); }
        QFile f(path);
        if (f.open(QIODevice::Append)) { f.setPermissions(QFile::ReadOwner | QFile::WriteOwner); f.write(bytes); }
    }
    QJsonObject status() const
    {
        return {{"running", m_core.state() == QProcess::Running}, {"pid", double(m_core.processId())},
                {"profile", m_profile}, {"error", m_error}, {"busy", m_busy}};
    }
    void stopCore(bool restoreProxy = true)
    {
        if (restoreProxy) {
            try { restoreSystemProxy(); } catch (const std::exception &e) { log(QByteArray(e.what()) + '\n'); }
            m_pacServer.close();
            m_systemProxyMode.clear();
        }
        if (m_core.state() == QProcess::NotRunning) return;
        m_stopping = true;
        m_core.terminate();
        if (!m_core.waitForFinished(3000)) { m_core.kill(); m_core.waitForFinished(1000); }
    }
    void completeStart(Answer answer, int attempt = 0)
    {
        const auto settings = m_store.settings();
        QUrl url(settings["endpoint"].toString()); url.setPath("/configs");
        QNetworkRequest request(url);
        request.setRawHeader("Authorization", "Bearer " + settings["secret"].toString().toUtf8());
        auto *reply = m_network.get(request);
        QTimer::singleShot(500, reply, &QNetworkReply::abort);
        connect(reply, &QNetworkReply::finished, this, [this, reply, answer, attempt] {
            reply->deleteLater();
            const auto document = QJsonDocument::fromJson(reply->readAll());
            const bool ready = m_core.state() == QProcess::Running && reply->error() == QNetworkReply::NoError && document.isObject();
            if (!ready) {
                if (m_core.state() == QProcess::Running && attempt < 15) {
                    QTimer::singleShot(100, this, [this, answer, attempt] { completeStart(answer, attempt + 1); });
                    return;
                }
                m_busy = false; m_wanted = false; ++m_restartEpoch;
                stopCore();
                m_error = "内核启动失败：控制器未就绪，请检查日志和端口占用";
                answer({{"error", m_error}});
                return;
            }
            try {
                auto current = m_store.settings();
                const bool automatic = current.value("autoSystemProxy").toBool(true);
                current["active"] = m_profile;
                m_store.setSettings(current);
                if (automatic) {
                    const int port = document.object()["mixed-port"].toInt();
                    if (port < 1 || port > 65535) throw std::runtime_error("自动启用系统代理需要配置有效的 mixed-port");
                    QTcpSocket probe;
                    probe.connectToHost(QHostAddress::LocalHost, quint16(port));
                    if (!probe.waitForConnected(100)) {
                        if (attempt < 15) {
                            QTimer::singleShot(100, this, [this, answer, attempt] { completeStart(answer, attempt + 1); });
                            return;
                        }
                        throw std::runtime_error("内核混合端口未就绪，请检查端口占用和监听地址");
                    }
                    QStringList bypass = defaultProxyBypass();
                    if (current["proxyBypass"].isArray()) {
                        bypass.clear();
                        for (const auto &v : current["proxyBypass"].toArray()) bypass.append(v.toString());
                    }
                    setSystemProxy("manual", port, {}, bypass);
                    m_systemProxyMode = "manual";
                    m_systemProxyPort = port;
                    m_systemPacUrl.clear();
                    m_pacBypass = bypass;
                    m_pacServer.close();
                }
                m_busy = false;
                auto result = status();
                result["message"] = automatic ? "内核已启动，系统代理已启用" : "内核已启动";
                answer(result);
            } catch (const std::exception &e) {
                m_busy = false;
                try { restoreSystemProxy(); } catch (...) { }
                m_systemProxyMode.clear(); m_pacServer.close();
                m_error = "内核已启动，但系统代理启用失败：" + QString::fromUtf8(e.what());
                auto result = status(); result["error"] = m_error; answer(result);
            }
        });
    }
    void startCore(const QString &id, Answer answer, bool recovery = false)
    {
        if (m_busy) { answer({{"error", "A core operation is in progress"}}); return; }
        const auto settings = m_store.settings();
        QString core = settings["core"].toString();
        if (!QFileInfo(core).isExecutable() || !QFileInfo(core).isFile()) throw std::runtime_error("Select an executable Mihomo core in Settings");
        const QString work = dataDir() + "/work";
        QDir().mkpath(work);
        const QString candidate = work + "/candidate.yaml";
        writeFile(candidate, m_store.runtime(id));
        m_busy = true;
        auto *validator = new QProcess(this);
        validator->setProcessChannelMode(QProcess::MergedChannels);
        validator->setWorkingDirectory(work);
        auto env = QProcessEnvironment::systemEnvironment();
        env.insert("SAFE_PATHS", dataDir());
        validator->setProcessEnvironment(env);
        QTimer::singleShot(120000, validator, [validator] { if (validator->state() != QProcess::NotRunning) validator->kill(); });
        auto complete = QSharedPointer<bool>::create(false);
        auto finish = [this, validator, complete, core, work, candidate, id, answer, env, recovery](bool success) {
            if (*complete) return;
            *complete = true;
            const auto output = QString::fromUtf8(validator->readAll()).right(2000);
            validator->deleteLater();
            if (!success) {
                m_busy = false;
                m_error = output.isEmpty() ? "内核配置验证失败" : output;
                if (validator->exitStatus() == QProcess::CrashExit && output.contains("download", Qt::CaseInsensitive))
                    m_error += "\n首次启动下载规则数据库超时，请检查官方数据下载地址的网络连通性";
                answer({{"error", m_error}}); return;
            }
            try {
                // Validate first, including ports, then replace the running process.
                const auto config = parseConfig(readFile(candidate));
                const int mixedPort = config["mixed-port"] ? config["mixed-port"].as<int>() : 0;
                const bool ownsPort = m_core.state() == QProcess::Running && mixedPort == localProxyPort(m_store);
                if (mixedPort > 0 && !ownsPort) {
                    QTcpServer probe;
                    if (!probe.listen(QHostAddress::LocalHost, quint16(mixedPort))) {
                        m_busy = false;
                        m_error = QString("混合端口 %1 已被其他程序占用，请更换 mixed-port 后重试").arg(mixedPort);
                        answer({{"error", m_error}});
                        return;
                    }
                }
                writeFile(work + "/config.yaml", readFile(candidate));
                stopCore(false);
                m_profile = id;
                m_error.clear();
                if (!recovery) m_failures = 0;
                m_wanted = true;
                m_core.setProcessEnvironment(env);
                m_core.setWorkingDirectory(work);
                m_core.start(core, {"-d", work, "-f", work + "/config.yaml"});
                if (!m_core.waitForStarted(2000)) throw std::runtime_error(m_core.errorString().toStdString());
                completeStart(answer);
            } catch (const std::exception &e) {
                m_busy = false; m_wanted = false; ++m_restartEpoch; stopCore();
                m_error = QString::fromUtf8(e.what()); answer({{"error", m_error}});
            }
        };
        connect(validator, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [finish](int code, QProcess::ExitStatus state) { finish(code == 0 && state == QProcess::NormalExit); });
        connect(validator, &QProcess::errorOccurred, this, [finish](QProcess::ProcessError e) { if (e == QProcess::FailedToStart) finish(false); });
        validator->start(core, {"-t", "-d", work, "-f", candidate});
    }
    void handle(const QJsonObject &command, Answer answer)
    {
        const auto action = command["action"].toString();
        if (action == "status") { answer(status()); return; }
        if (action == "stop") {
            if (m_busy) { answer({{"error", "A core operation is in progress"}}); return; }
            m_wanted = false; ++m_restartEpoch; stopCore(); answer(status()); return;
        }
        if (action == "start" || action == "restart") {
            QString id = command["profile"].toString();
            if (id.isEmpty()) id = m_store.settings()["active"].toString();
            if (id.isEmpty()) {
                const auto profiles = m_store.profiles();
                if (profiles.isEmpty()) throw std::runtime_error("请先导入订阅，再启动内核");
                id = profiles.first().toObject()["id"].toString();
            }
            startCore(id, answer); return;
        }
        if (action == "quit") { answer({{"ok", true}}); QTimer::singleShot(50, qApp, &QCoreApplication::quit); return; }
        if (action == "update") { updateProfiles(true); answer({{"ok", true}}); return; }
        if (action == "bypass") {
            if (!command["entries"].isArray()) throw std::runtime_error("Invalid bypass entries");
            QStringList entries;
            for (const auto &v : command["entries"].toArray()) {
                if (!v.isString()) throw std::runtime_error("Invalid bypass entry");
                entries.append(v.toString());
            }
            const auto bypass = normalizeProxyBypass(entries);
            if (!m_systemProxyMode.isEmpty()) {
                try { setSystemProxy(m_systemProxyMode, m_systemProxyPort, m_systemPacUrl, bypass); }
                catch (...) { m_systemProxyMode.clear(); m_pacServer.close(); throw; }
            }
            auto settings = m_store.settings();
            settings["proxyBypass"] = QJsonArray::fromStringList(bypass);
            m_store.setSettings(settings);
            m_pacBypass = bypass;
            answer({{"message", m_systemProxyMode.isEmpty() ? "代理绕过已保存，启用系统代理或 PAC 后生效" : "代理绕过已保存并应用"}});
            return;
        }
        if (action == "proxy") {
            const auto mode = command["mode"].toString();
            if (mode == "none") {
                restoreSystemProxy();
                m_systemProxyMode.clear();
                m_pacServer.close();
                answer({{"message", "已恢复原系统代理设置"}});
                return;
            }
            const int port = command["port"].toInt();
            QString pac = command["pac"].toString();
            const auto settings = m_store.settings();
            QStringList bypass = defaultProxyBypass();
            if (settings["proxyBypass"].isArray()) {
                bypass.clear();
                for (const auto &v : settings["proxyBypass"].toArray()) bypass.append(v.toString());
            }
            bypass = normalizeProxyBypass(bypass);
            if (mode == "auto" && pac.isEmpty()) {
                if (!m_pacServer.isListening() && !m_pacServer.listen(QHostAddress::LocalHost, 19091))
                    throw std::runtime_error(m_pacServer.errorString().toStdString());
                m_pacProxyPort = port;
                pac = "http://127.0.0.1:19091/proxy.pac";
            }
            try { setSystemProxy(mode, port, pac, bypass); }
            catch (...) { m_systemProxyMode.clear(); m_pacServer.close(); throw; }
            m_systemProxyMode = mode;
            m_systemProxyPort = port;
            m_systemPacUrl = pac;
            m_pacBypass = bypass;
            if (mode != "auto") m_pacServer.close();
            answer({{"message", "系统代理已启用"}});
            return;
        }
        answer({{"error", "Unknown command"}});
    }
    void updateProfiles(bool force = false)
    {
        const auto settings = m_store.settings();
        const int interval = settings["updateMinutes"].toInt(360);
        if (interval <= 0 && !force) return;
        for (const auto &value : m_store.profiles()) {
            const auto p = value.toObject();
            const auto id = p["id"].toString();
            const QUrl url(p["url"].toString());
            if (!url.isValid() || (url.scheme() != "https" && url.scheme() != "http") || m_downloads.contains(id)) continue;
            const auto updated = QDateTime::fromString(p["updated"].toString(), Qt::ISODate);
            if (!force && !p["autoUpdate"].toBool(true)) continue;
            const int profileInterval = p["updateMinutes"].toInt(p["suggestedUpdateMinutes"].toInt(interval));
            if (!force && (profileInterval <= 0 || updated.secsTo(QDateTime::currentDateTimeUtc()) < profileInterval * 60)) continue;
            m_downloads.insert(id);
            fetchSubscription(url, p, localProxyPort(m_store), this, [this, id, p](const QByteArray &bytes, const QJsonObject &metadata, const QString &error) {
                m_downloads.remove(id);
                try {
                    if (!error.isEmpty()) throw std::runtime_error(error.toStdString());
                    bool exists = false;
                    for (const auto &current : m_store.profiles()) if (current.toObject()["id"].toString() == id) exists = true;
                    if (!exists) return;
                    m_store.saveProfile(p["name"].toString(), p["url"].toString(), bytes, id);
                    m_store.updateProfileOptions(id, metadata);
                    log("Subscription updated: " + id.toUtf8() + '\n');
                    if (id == m_profile && m_core.state() == QProcess::Running) startCore(id, [](QJsonObject) {});
                } catch (const std::exception &e) { log(QByteArray("Subscription update failed: ") + e.what() + '\n'); }
            });
        }
    }
    Store m_store;
    QProcess m_core;
    QLocalServer m_server;
    QNetworkAccessManager m_network;
    QTcpServer m_pacServer;
    int m_pacProxyPort = 0;
    QStringList m_pacBypass = defaultProxyBypass();
    QString m_systemProxyMode;
    QString m_systemPacUrl;
    int m_systemProxyPort = 0;
    QLockFile m_lock;
    QTimer m_updateTimer;
    QSet<QString> m_downloads;
    QString m_profile;
    QString m_error;
    int m_failures = 0;
    bool m_stopping = false;
    bool m_busy = false;
    bool m_wanted = false;
    quint64 m_restartEpoch = 0;
};

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName("dde-mihomo-daemon");
    try {
        if (app.arguments().contains("--import")) {
            const int i = app.arguments().indexOf("--import");
            if (i + 1 >= app.arguments().size()) return 2;
            Store store;
            const auto id = store.saveProfile("导入订阅", {}, readFile(app.arguments()[i + 1]));
            auto settings = store.settings();
            if (app.arguments().contains("--activate") || settings["active"].toString().isEmpty()) { settings["active"] = id; store.setSettings(settings); }
            QTextStream(stdout) << id << '\n';
            return 0;
        }
        if (app.arguments().contains("--eval")) {
            const int i = app.arguments().indexOf("--eval");
            if (i + 1 >= app.arguments().size()) return 2;
            QFile input;
            input.open(stdin, QIODevice::ReadOnly);
            const auto doc = QJsonDocument::fromJson(input.readAll());
            QJSEngine js;
            const auto code = readFile(app.arguments()[i + 1]);
            auto result = js.evaluate(QString::fromUtf8(code));
            if (result.isError()) throw std::runtime_error(result.toString().toStdString());
            auto main = js.globalObject().property("main");
            if (!main.isCallable()) throw std::runtime_error("Override must define function main(config)");
            auto arg = js.toScriptValue(doc.object().toVariantMap());
            result = main.call({arg});
            if (result.isError() || !result.isObject() || result.isArray()) throw std::runtime_error("Override must return a configuration object");
            const auto out = QJsonDocument::fromVariant(result.toVariant());
            if (!out.isObject()) throw std::runtime_error("Override returned invalid data");
            QTextStream(stdout) << out.toJson(QJsonDocument::Compact);
            return 0;
        }
        if (app.arguments().contains("--command")) {
            const int i = app.arguments().indexOf("--command");
            if (i + 1 >= app.arguments().size()) return 2;
            QLocalSocket socket;
            bool connected = false;
            for (int retry = 0; retry < 20; ++retry) {
                socket.abort(); socket.connectToServer(socketName());
                if (socket.waitForConnected(200)) { connected = true; break; }
                QThread::msleep(100);
            }
            if (!connected) throw std::runtime_error(socket.errorString().toStdString());
            socket.write(QJsonDocument(QJsonObject{{"action", app.arguments()[i + 1]}}).toJson(QJsonDocument::Compact) + '\n');
            socket.waitForBytesWritten(1000);
            const auto action = app.arguments()[i + 1];
            if (!socket.waitForReadyRead(action == "start" || action == "restart" ? 140000 : 20000)) throw std::runtime_error(socket.errorString().toStdString());
            QTextStream(stdout) << socket.readAll();
            return 0;
        }
        Daemon daemon;
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, signalFds) == 0) {
            auto *notifier = new QSocketNotifier(signalFds[1], QSocketNotifier::Read, &app);
            QObject::connect(notifier, &QSocketNotifier::activated, &app, [&app](int) { app.quit(); });
            ::signal(SIGTERM, stopSignal);
            ::signal(SIGINT, stopSignal);
        }
        return app.exec();
    } catch (const std::exception &e) { QTextStream(stderr) << e.what() << '\n'; return 1; }
}
