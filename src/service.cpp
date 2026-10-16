#include "config.h"
#include "api.h"
#include "tun.h"
#include "geodata.h"
#include <QNetworkInterface>
#include <QDBusConnection>
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
#include <QElapsedTimer>
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
    Q_OBJECT
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
        connect(&m_core, &QProcess::readyRead, this, [this] {
            const auto bytes = m_core.readAll(); log(bytes); m_coreLogBuffer += bytes;
            int end;
            while ((end = m_coreLogBuffer.indexOf('\n')) >= 0) {
                const auto line = m_coreLogBuffer.left(end); m_coreLogBuffer.remove(0, end + 1);
                if (line.contains("Start TUN listening error") || line.contains("configure tun interface: operation not permitted")) {
                    m_tunError = QString::fromUtf8(line).right(1200);
                    if (!m_busy) disableFailedTun(m_tunError);
                }
            }
            if (m_coreLogBuffer.size() > 8192) m_coreLogBuffer = m_coreLogBuffer.right(4096);
        });
        m_resumeTimer.setSingleShot(true); m_resumeTimer.setInterval(5000);
        connect(&m_resumeTimer, &QTimer::timeout, this, [this] {
            if (m_core.state() == QProcess::Running && m_expectedTun.value("enable").toBool())
                repairTun([this](QJsonObject result) { if (!result["error"].toString().isEmpty()) log(result["error"].toString().toUtf8() + '\n'); });
        });
        QDBusConnection::systemBus().connect("org.freedesktop.login1", "/org/freedesktop/login1", "org.freedesktop.login1.Manager",
                                             "PrepareForSleep", this, SLOT(prepareForSleep(bool)));
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
                {"profile", m_profile}, {"error", m_error}, {"busy", m_busy},
                {"tun", QJsonObject{{"enable", m_tunEnabled && QNetworkInterface::interfaceFromName(m_tunDevice).isValid()},
                                     {"device", m_tunDevice}, {"permission", hasTunPermission(m_store.settings()["core"].toString())},
                                     {"processPermission", processHasTunPermission(m_core.processId())}}}};
    }
    void stopCore(bool restoreProxy = true)
    {
        m_resumeTimer.stop(); m_tunEnabled = false;
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
    void completeStart(Answer answer, int attempt = 0, bool preserveProxy = false)
    {
        const auto settings = m_store.settings();
        QUrl url(settings["endpoint"].toString()); url.setPath("/configs");
        QNetworkRequest request(url);
        request.setRawHeader("Authorization", "Bearer " + settings["secret"].toString().toUtf8());
        auto *reply = m_network.get(request);
        QTimer::singleShot(500, reply, &QNetworkReply::abort);
        connect(reply, &QNetworkReply::finished, this, [this, reply, answer, attempt, preserveProxy] {
            reply->deleteLater();
            const auto document = QJsonDocument::fromJson(reply->readAll());
            const bool ready = m_core.state() == QProcess::Running && reply->error() == QNetworkReply::NoError && document.isObject();
            if (!ready) {
                if (m_core.state() == QProcess::Running && m_readyTimer.elapsed() < 10000) {
                    QTimer::singleShot(100, this, [this, answer, attempt, preserveProxy] { completeStart(answer, attempt + 1, preserveProxy); });
                    return;
                }
                m_busy = false; m_wanted = false; ++m_restartEpoch;
                stopCore(!preserveProxy);
                m_error = "内核启动失败：控制器未就绪，请检查日志和端口占用";
                answer({{"error", m_error}});
                return;
            }
            try {
                auto current = m_store.settings();
                if (m_expectedTun.value("enable").toBool()) {
                    const bool tunReady = document.object()["tun"].toObject()["enable"].toBool() &&
                        QNetworkInterface::interfaceFromName(m_tunDevice).isValid();
                    if (!tunReady && m_tunError.isEmpty() && m_readyTimer.elapsed() < 10000) {
                        QTimer::singleShot(100, this, [this, answer, attempt, preserveProxy] { completeStart(answer, attempt + 1, preserveProxy); });
                        return;
                    }
                    if (!tunReady) {
                        m_busy = false; disableFailedTun(m_tunError.isEmpty() ? "虚拟网卡未创建成功，请检查内核日志和权限" : m_tunError);
                        auto result = status(); result["error"] = m_error; answer(result); return;
                    }
                    m_tunEnabled = true;
                } else m_tunEnabled = false;
                const bool automatic = !preserveProxy && current.value("autoSystemProxy").toBool(true);
                current["active"] = m_profile;
                m_store.setSettings(current);
                const int port = document.object()["mixed-port"].toInt();
                if (m_expectedMixedPort > 0) {
                    QTcpSocket probe;
                    probe.connectToHost(QHostAddress::LocalHost, quint16(m_expectedMixedPort));
                    if (port != m_expectedMixedPort || !probe.waitForConnected(100)) {
                        if (m_readyTimer.elapsed() < 10000) {
                            QTimer::singleShot(100, this, [this, answer, attempt, preserveProxy] { completeStart(answer, attempt + 1, preserveProxy); });
                            return;
                        }
                        throw std::runtime_error("内核混合端口未就绪，请检查端口占用和监听地址");
                    }
                }
                if (automatic) {
                    if (port < 1 || port > 65535) throw std::runtime_error("自动启用系统代理需要配置有效的 mixed-port");
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
                if (!preserveProxy) {
                    try { restoreSystemProxy(); } catch (...) { }
                    m_systemProxyMode.clear(); m_pacServer.close();
                }
                m_error = (preserveProxy ? "虚拟网卡操作失败：" : "内核已启动，但系统代理启用失败：") + QString::fromUtf8(e.what());
                auto result = status(); result["error"] = m_error; answer(result);
            }
        });
    }
    void startCore(const QString &id, Answer answer, bool recovery = false, bool preserveProxy = false, bool hotReload = false)
    {
        if (m_busy) { answer({{"error", "A core operation is in progress"}}); return; }
        const auto settings = m_store.settings();
        QString core = settings["core"].toString();
        if (!QFileInfo(core).isExecutable() || !QFileInfo(core).isFile()) throw std::runtime_error("Select an executable Mihomo core in Settings");
        const QString work = dataDir() + "/work";
        QDir().mkpath(work);
        const int seeded = seedGeoData(work);
        if (seeded) log(QString("已从安装包准备 %1 个地理数据库，无需在线下载\n").arg(seeded).toUtf8());
        const QString candidate = work + "/candidate.yaml";
        auto runtime = parseConfig(m_store.runtime(id));
        if (runtime["tun"]["enable"].as<bool>() && !hasTunPermission(core)) {
            disableFailedTun("TUN 权限不足，已关闭虚拟网卡开关；请在虚拟网卡页面授权内核后启用");
            runtime = parseConfig(m_store.runtime(id));
        }
        if (runtime["tun"]["enable"].as<bool>()) {
            const auto device = QString::fromStdString(runtime["tun"]["device"].as<std::string>());
            const bool owned = m_core.state() == QProcess::Running && m_expectedTun["enable"].toBool() && m_tunDevice == device;
            if (!owned && QNetworkInterface::interfaceFromName(device).isValid())
                throw std::runtime_error(("虚拟网卡 " + device + " 已存在，请更换名称，避免影响其他代理程序").toStdString());
        }
        writeFile(candidate, emitYaml(runtime));
        m_busy = true;
        auto *validator = new QProcess(this);
        validator->setProcessChannelMode(QProcess::MergedChannels);
        validator->setWorkingDirectory(work);
        auto env = QProcessEnvironment::systemEnvironment();
        env.insert("SAFE_PATHS", dataDir());
        validator->setProcessEnvironment(env);
        QTimer::singleShot(120000, validator, [validator] { if (validator->state() != QProcess::NotRunning) validator->kill(); });
        auto complete = QSharedPointer<bool>::create(false);
        auto finish = [this, validator, complete, core, work, candidate, id, answer, env, recovery, preserveProxy, hotReload](bool success) {
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
                m_expectedMixedPort = mixedPort;
                m_tunError.clear(); m_coreLogBuffer.clear();
                if (hotReload && m_core.state() == QProcess::Running) {
                    m_expectedTun = yamlToJson(config["tun"]).toObject();
                    m_tunDevice = m_expectedTun["device"].toString();
                    reloadRuntime(answer);
                    return;
                }
                stopCore(false);
                m_expectedTun = yamlToJson(config["tun"]).toObject();
                m_tunDevice = m_expectedTun["device"].toString();
                m_profile = id;
                m_error.clear();
                if (!recovery) m_failures = 0;
                m_wanted = true;
                m_core.setProcessEnvironment(env);
                m_core.setWorkingDirectory(work);
                m_readyTimer.start();
                m_core.start(core, {"-d", work, "-f", work + "/config.yaml"});
                if (!m_core.waitForStarted(2000)) throw std::runtime_error(m_core.errorString().toStdString());
                completeStart(answer, 0, preserveProxy);
            } catch (const std::exception &e) {
                m_busy = false; m_wanted = false; ++m_restartEpoch; stopCore(!preserveProxy);
                m_error = QString::fromUtf8(e.what()); answer({{"error", m_error}});
            }
        };
        connect(validator, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [finish](int code, QProcess::ExitStatus state) { finish(code == 0 && state == QProcess::NormalExit); });
        connect(validator, &QProcess::errorOccurred, this, [finish](QProcess::ProcessError e) { if (e == QProcess::FailedToStart) finish(false); });
        validator->start(core, {"-t", "-d", work, "-f", candidate});
    }
    void disableFailedTun(const QString &reason)
    {
        m_tunEnabled = false; m_expectedTun["enable"] = false; m_error = reason;
        const auto path = dataDir() + "/controlled.yaml";
        auto controlled = QFile::exists(path) ? parseConfig(readFile(path)) : YAML::Node(YAML::NodeType::Map);
        controlled["tun"]["enable"] = false;
        writeFile(path, emitYaml(controlled));
        log(reason.toUtf8() + '\n');
    }
    void configureTun(const QJsonObject &input, Answer answer)
    {
        if (m_busy) { answer({{"error", "内核正在操作，请稍候"}}); return; }
        const auto patch = validateTunOptions(input);
        const auto path = dataDir() + "/controlled.yaml";
        const bool existed = QFile::exists(path);
        const auto oldBytes = existed ? readFile(path) : QByteArray();
        auto controlled = existed ? parseConfig(oldBytes) : YAML::Node(YAML::NodeType::Map);
        auto tun = defaultTunConfig(); if (controlled["tun"]) tun = mergeYaml(tun, controlled["tun"]);
        tun = mergeYaml(tun, parseConfig(QJsonDocument(patch).toJson()));
        const bool enable = tun["enable"].as<bool>();
        QString id = m_store.settings()["active"].toString();
        if (id.isEmpty() && !m_store.profiles().isEmpty()) id = m_store.profiles().first().toObject()["id"].toString();
        if (enable && id.isEmpty()) { answer({{"error", "请先导入订阅，再启用虚拟网卡"}}); return; }
        if (enable && !hasTunPermission(m_store.settings()["core"].toString())) {
            answer({{"error", "启用虚拟网卡需要先授权内核"}, {"needsAuthorization", true}}); return;
        }
        const bool running = m_core.state() == QProcess::Running;
        // chmod/setuid affects the next exec, never an already-running process.
        const bool needsPrivilegeRestart = enable && running && !processHasTunPermission(m_core.processId());
        if (enable) {
            const auto device = QString::fromStdString(tun["device"].as<std::string>());
            const bool owned = running && m_expectedTun["enable"].toBool() && m_tunDevice == device;
            if (!owned && QNetworkInterface::interfaceFromName(device).isValid()) {
                answer({{"error", "虚拟网卡 " + device + " 已被其他程序使用，请更换名称"}}); return;
            }
        }
        controlled["tun"] = tun;
        if (enable) controlled["dns"]["enable"] = true;
        writeFile(path, emitYaml(controlled));
        if (!running && !enable) { answer({{"message", "虚拟网卡设置已保存"}, {"tun", yamlToJson(tun)}}); return; }
        auto restore = [path, existed, oldBytes] { if (existed) writeFile(path, oldBytes); else QFile::remove(path); };
        auto complete = [this, answer, restore, running, id, enable](QJsonObject result) {
            if (result["error"].toString().isEmpty()) {
                result["message"] = enable ? "虚拟网卡已启用，DNS 已联动开启" : "虚拟网卡已关闭，原路由已恢复";
                answer(result); return;
            }
            const auto error = result["error"].toString();
            try {
                restore();
                if (running) {
                    startCore(id, [this, answer, error](QJsonObject recovered) {
                        const auto recoveryError = recovered["error"].toString();
                        m_error = error + (recoveryError.isEmpty() ? QString() : "\n恢复原配置失败：" + recoveryError);
                        recovered["error"] = m_error; answer(recovered);
                    }, false, true, true);
                } else { m_error = error; answer(result); }
            } catch (const std::exception &e) { m_busy = false; m_error = error + "\n恢复配置失败：" + QString::fromUtf8(e.what()); answer({{"error", m_error}}); }
        };
        try { startCore(id, complete, false, true, !needsPrivilegeRestart); }
        catch (...) { restore(); throw; }
    }
    void repairTun(Answer answer)
    {
        if (m_busy) { answer({{"error", "内核正在操作，请稍候"}}); return; }
        if (m_core.state() != QProcess::Running || !m_expectedTun["enable"].toBool()) { answer({{"error", "虚拟网卡未启用"}}); return; }
        m_busy = true; m_tunError.clear(); m_error.clear();
        reloadRuntime([answer](QJsonObject result) { if (result["error"].toString().isEmpty()) result["message"] = "虚拟网卡、路由与 DNS 配置已重新加载"; answer(result); });
    }
    void reloadRuntime(Answer answer)
    {
        m_readyTimer.start();
        m_busy = true; m_error.clear();
        const auto settings = m_store.settings(); QUrl url(settings["endpoint"].toString()); url.setPath("/configs"); url.setQuery("force=true");
        QNetworkRequest request(url); request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
        request.setRawHeader("Authorization", "Bearer " + settings["secret"].toString().toUtf8());
        auto *reply = m_network.put(request, QJsonDocument(QJsonObject{{"path", dataDir() + "/work/config.yaml"}}).toJson());
        QTimer::singleShot(5000, reply, &QNetworkReply::abort);
        connect(reply, &QNetworkReply::finished, this, [this, reply, answer] {
            reply->deleteLater();
            if (reply->error() != QNetworkReply::NoError) { m_busy = false; m_error = "重建虚拟网卡失败：" + reply->errorString(); answer({{"error", m_error}}); return; }
            completeStart(answer, 0, true);
        });
    }
    void handle(const QJsonObject &command, Answer answer)
    {
        const auto action = command["action"].toString();
        if (action == "status") { answer(status()); return; }
        if (action == "tun") { if (!command["options"].isObject()) throw std::runtime_error("无效的虚拟网卡配置"); configureTun(command["options"].toObject(), answer); return; }
        if (action == "repair-tun") { repairTun(answer); return; }
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
private slots:
    void prepareForSleep(bool sleeping) { if (sleeping) m_resumeTimer.stop(); else m_resumeTimer.start(); }
private:
    QTimer m_resumeTimer;
    QElapsedTimer m_readyTimer;
    int m_expectedMixedPort = 0;
    QJsonObject m_expectedTun;
    bool m_tunEnabled = false;
    QString m_tunDevice;
    QString m_tunError;
    QByteArray m_coreLogBuffer;
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

#include "service.moc"
