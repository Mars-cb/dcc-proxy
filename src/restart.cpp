#include "restart.h"
#include "config.h"
#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDir>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QLockFile>
#include <QProcess>
#include <QThread>
#include <stdexcept>
#include <unistd.h>
namespace Party {
[[noreturn]] static void failRestart(const QString &message) { throw std::runtime_error(message.toStdString()); }
static QJsonObject request(const QJsonObject &command, int timeout = 3000)
{
    QLocalSocket socket; socket.connectToServer(socketName());
    if (!socket.waitForConnected(1000)) failRestart(socket.errorString());
    socket.write(QJsonDocument(command).toJson(QJsonDocument::Compact) + '\n');
    if (!socket.waitForBytesWritten(1000)) failRestart(socket.errorString());
    QByteArray bytes; QElapsedTimer elapsed; elapsed.start();
    while (!bytes.contains('\n') && elapsed.elapsed() < timeout) {
        bytes += socket.readAll();
        if (bytes.contains('\n')) break;
        if (!socket.waitForReadyRead(qMax(1, timeout - int(elapsed.elapsed())))) { bytes += socket.readAll(); break; }
        if (bytes.size() > 1024 * 1024) failRestart("后台响应过大");
    }
    const auto doc = QJsonDocument::fromJson(bytes.left(bytes.indexOf('\n')));
    if (!doc.isObject()) failRestart("无法读取后台状态，未执行强制终止");
    return doc.object();
}
static QByteArray procFile(qint64 pid, const char *name)
{
    QFile file(QString("/proc/%1/%2").arg(pid).arg(name));
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}
static qint64 daemonPid()
{
    QLockFile lock(dataDir() + "/daemon.lock"); qint64 pid = 0; QString host, app;
    if (!lock.getLockInfo(&pid, &host, &app)) return 0;
    if (!QFileInfo::exists(QString("/proc/%1").arg(pid))) return 0;
    const auto args = procFile(pid, "cmdline").split('\0');
    bool owned = false;
    for (const auto &line : procFile(pid, "status").split('\n'))
        if (line.startsWith("Uid:")) owned = line.simplified().split(' ').value(1).toUInt() == ::getuid();
    if (!owned || !args.contains("--daemon") || QFileInfo(QString::fromLocal8Bit(args.value(0))).canonicalFilePath() != QFileInfo(serviceExecutable()).canonicalFilePath())
        failRestart("后台进程归属无法核实，未停止任何进程");
    return pid;
}
static QByteArray systemctl(const QStringList &arguments, int timeout = 15000, bool *success = nullptr)
{
    if (success) *success = false;
    QProcess process; process.start("systemctl", QStringList{"--user"} + arguments);
    if (!process.waitForStarted(2000)) return {};
    if (!process.waitForFinished(timeout)) { process.kill(); process.waitForFinished(2000); failRestart("systemd 操作超时，请检查后台服务状态"); }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) return {};
    if (success) *success = true;
    return process.readAllStandardOutput().trimmed();
}
// Older running daemons cannot export their in-memory proxy mode. Read only the
// desktop state and require our existing ownership snapshot before resuming it.
static QJsonObject legacyProxyState(const Store &store)
{
    QJsonObject state{{"mode", "none"}, {"bypass", QJsonArray::fromStringList(defaultProxyBypass())}};
    if (!QFileInfo::exists(store.root() + "/system-proxy-before.json")) return state;
    const auto snapshot = readJson(store.root() + "/system-proxy-before.json");
    QString mode, pac; QStringList bypass; int port = 0;
    if (snapshot["backend"].toString() == "dbus") {
        QDBusInterface network(snapshot["service"].toString(), snapshot["path"].toString(), snapshot["interface"].toString(), QDBusConnection::sessionBus()); network.setTimeout(2000);
        auto get = [&](const QString &method, const QVariantList &args = QVariantList()) {
            const auto reply = network.callWithArgumentList(QDBus::Block, method, args);
            if (reply.type() == QDBusMessage::ErrorMessage) failRestart("无法确认当前系统代理模式，原服务未停止：" + reply.errorMessage());
            return reply.arguments();
        };
        mode = get("GetProxyMethod").value(0).toString(); pac = get("GetAutoProxy").value(0).toString();
        bypass = get("GetProxyIgnoreHosts").value(0).toString().split(',', QString::SkipEmptyParts);
        const auto proxy = get("GetProxy", {"http"});
        if (proxy.value(0).toString() == "127.0.0.1") port = proxy.value(1).toString().toInt();
    } else {
        QString schema;
        for (const auto &entry : snapshot["values"].toArray()) if (entry.toObject()["key"].toString() == "mode") schema = entry.toObject()["schema"].toString();
        if (schema.isEmpty()) failRestart("系统代理快照不完整，原服务未停止");
        auto get = [&](const QString &s, const QString &key) {
            QProcess process; process.start("gsettings", {"get", s, key});
            if (!process.waitForFinished(3000) || process.exitCode() != 0) failRestart("无法确认当前系统代理模式，原服务未停止");
            auto value = QString::fromUtf8(process.readAllStandardOutput()).trimmed();
            if (value.startsWith('\'') && value.endsWith('\'')) value = value.mid(1, value.size() - 2);
            return value;
        };
        mode = get(schema, "mode"); pac = get(schema, "autoconfig-url");
        if (get(schema + ".http", "host") == "127.0.0.1") port = get(schema + ".http", "port").toInt();
        for (const auto &entry : store.settings()["proxyBypass"].toArray()) bypass << entry.toString();
        if (!store.settings().contains("proxyBypass")) bypass = defaultProxyBypass();
    }
    if (mode == "auto" || (mode == "manual" && port == localProxyPort(store))) {
        state["mode"] = mode; state["pac"] = pac; state["bypass"] = QJsonArray::fromStringList(bypass);
    }
    return state;
}
QJsonObject restartServiceProcesses(const QString &token)
{
    Store store;
    if (!validId(token) || !localController(store.settings()["endpoint"].toString())) failRestart("仅支持重启本机代理服务");
    QLockFile operation(store.root() + "/service-restart.lock");
    if (!operation.tryLock(0)) failRestart("另一个服务重启正在进行");
    QJsonObject old; const auto pid = daemonPid();
    if (pid) {
        old = request({{"action", "status"}});
        if (old["busy"].toBool() || !old["orphan"].toObject().isEmpty()) failRestart("后台正在操作或存在残留内核，请先处理；原服务未停止");
    }
    auto id = old["running"].toBool() ? old["profile"].toString() : store.settings()["active"].toString();
    if (id.isEmpty() && !store.profiles().isEmpty()) id = store.profiles().first().toObject()["id"].toString();
    if (!validId(id)) failRestart("请先导入并选择一个订阅，原服务未停止");
    // Validation happens while the old process still serves traffic.
    QString name;
    for (const auto &entry : store.profiles()) if (entry.toObject()["id"].toString() == id) name = entry.toObject()["name"].toString();
    const auto validated = validateStoreChange(store.root(), {{"id", id}, {"name", name}, {"source", QString::fromLatin1(store.profile(id).toBase64())}});
    const auto fingerprint = validated["fingerprint"].toString().toLatin1();
    QJsonObject proxy;
    if (pid && old["capabilities"].toArray().contains("restart-state")) {
        const auto exported = request({{"action", "restart-state"}});
        if (!exported["error"].toString().isEmpty()) failRestart(exported["error"].toString());
        proxy = exported["proxy"].toObject();
    } else proxy = legacyProxyState(store);
    QJsonObject handoff{{"token", token}, {"created", double(QDateTime::currentSecsSinceEpoch())}, {"profile", id},
        {"runtime", QString::fromLatin1(store.runtime(id).toBase64())}, {"proxy", proxy}};
    if (old["running"].toBool()) handoff["fallback"] = QString::fromLatin1(readFile(store.root() + "/work/config.yaml").toBase64());
    if (fingerprint != configFingerprint(store.root())) failRestart("校验期间配置发生变化，原服务未停止，请重试");
    const auto marker = store.root() + "/service-restart.json";
    writeFile(marker, QJsonDocument(handoff).toJson());
    bool replaced = false;
    try {
        if (pid) {
            const auto latest = request({{"action", "status"}});
            if (daemonPid() != pid || latest["busy"].toBool() || latest["pid"] != old["pid"] || latest["profile"] != old["profile"] || fingerprint != configFingerprint(store.root()))
                failRestart("后台或配置状态发生变化，原服务未停止，请重试");
        }
        const bool managed = pid && !qEnvironmentVariableIsSet("DCC_PROXY_DATA_DIR") && systemctl({"show", "--property=MainPID", "--value", "dde-mihomo-daemon.service"}).toLongLong() == pid;
        if (managed) {
            // Let systemd retain ownership and clean up its own process group.
            replaced = true;
            bool success = false;
            systemctl({"restart", "dde-mihomo-daemon.service"}, 20000, &success);
            if (!success) failRestart("systemd 重启失败，请检查用户服务状态；未另行启动重复后台");
        } else {
            if (pid) {
                const auto latest = request({{"action", "status"}});
                if (daemonPid() != pid || latest["busy"].toBool()) failRestart("后台状态发生变化，原服务未停止，请重试");
                const auto stopped = request({{"action", "quit"}});
                if (!stopped["error"].toString().isEmpty()) failRestart(stopped["error"].toString());
                QElapsedTimer wait; wait.start();
                while (QFileInfo::exists(store.root() + "/daemon.lock") && wait.elapsed() < 15000) QThread::msleep(100);
                if (QFileInfo::exists(store.root() + "/daemon.lock")) failRestart("后台未正常退出，未强制终止，请检查服务状态");
            }
            QProcess daemon;
            daemon.setProgram(serviceExecutable()); daemon.setArguments({"--daemon"}); daemon.setWorkingDirectory(store.root());
            daemon.setStandardInputFile(QProcess::nullDevice()); daemon.setStandardOutputFile(QProcess::nullDevice()); daemon.setStandardErrorFile(QProcess::nullDevice());
            if (!daemon.startDetached()) failRestart("新版后台启动失败，请检查日志");
            replaced = true;
        }
        QElapsedTimer wait; wait.start();
        while (wait.elapsed() < 130000) {
            QThread::msleep(200);
            QJsonObject state;
            try { state = request({{"action", "status"}}, 1500); } catch (...) { continue; }
            const auto restart = state["serviceRestart"].toObject();
            if (restart["token"].toString() != token || !restart["complete"].toBool()) continue;
            state["message"] = "后台与 Mihomo 内核已重启";
            if (!restart["error"].toString().isEmpty()) state["error"] = restart["error"];
            return state;
        }
        failRestart("服务重启等待超时，请查看后台日志；未执行强制终止");
    } catch (...) {
        if (!replaced) QFile::remove(marker);
        throw;
    }
}
}
