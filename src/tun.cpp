#include "tun.h"
#include <QFileInfo>
#include <QFile>
#include <QHostAddress>
#include <QRegularExpression>
#include <QProcess>
#include <QSharedPointer>
#include <QTimer>
#include <QSet>
#include <sys/stat.h>
#include <unistd.h>
#include <stdexcept>
namespace Party {
YAML::Node defaultTunConfig()
{
    return parseConfig("enable: false\ndevice: Mihomo\nstack: mixed\nauto-route: true\nauto-redirect: false\nauto-detect-interface: true\ndns-hijack: [any:53]\nroute-exclude-address: []\nmtu: 1500\n");
}
YAML::Node defaultTunDns()
{
    return parseConfig("enable: true\nipv6: false\nenhanced-mode: fake-ip\nfake-ip-range: 198.18.0.1/16\nfake-ip-filter: ['*', '+.lan', '+.local', 'time.*.com', 'ntp.*.com', '+.market.xiaomi.com']\nuse-hosts: false\nuse-system-hosts: false\nrespect-rules: false\ndefault-nameserver: [tls://223.5.5.5]\nnameserver: [https://doh.pub/dns-query, https://dns.alidns.com/dns-query]\ndirect-nameserver: []\nfallback: []\nfallback-filter: {geoip: true, geoip-code: CN, ipcidr: [240.0.0.0/4, 0.0.0.0/32], domain: ['+.google.com', '+.facebook.com', '+.youtube.com']}\n");
}
QJsonObject validateTunOptions(const QJsonObject &options)
{
    const QSet<QString> booleans{"enable", "auto-route", "auto-redirect", "auto-detect-interface", "strict-route"};
    const QSet<QString> known = booleans + QSet<QString>{"device", "stack", "mtu", "dns-hijack", "route-exclude-address"};
    auto fail = [](const QString &message) { throw std::runtime_error(message.toStdString()); };
    QJsonObject out;
    for (auto it = options.begin(); it != options.end(); ++it) {
        if (!known.contains(it.key())) fail("不支持的虚拟网卡设置：" + it.key());
        if (booleans.contains(it.key())) { if (!it.value().isBool()) fail("虚拟网卡开关格式无效：" + it.key()); out[it.key()] = it.value(); }
        else if (it.key() == "device") {
            const auto device = it.value().toString().trimmed();
            if (!QRegularExpression("^[a-zA-Z0-9_.-]{1,15}$").match(device).hasMatch()) fail("虚拟网卡名称须为 1–15 个字母、数字、点、下划线或连字符");
            out[it.key()] = device;
        } else if (it.key() == "stack") {
            const auto stack = it.value().toString().toLower();
            if (!QStringList{"mixed", "gvisor", "system", "mips"}.contains(stack)) fail("无效的 TUN 协议栈");
            out[it.key()] = stack;
        } else if (it.key() == "mtu") {
            const double n = it.value().toDouble();
            if (!it.value().isDouble() || n < 1 || n > 65535 || n != int(n)) fail("MTU 必须在 1–65535 之间");
            out[it.key()] = int(n);
        } else {
            if (!it.value().isArray()) fail("虚拟网卡列表格式无效：" + it.key());
            if (it.value().toArray().size() > 512) fail("虚拟网卡列表最多支持 512 项");
            QJsonArray entries; QSet<QString> seen;
            for (const auto &v : it.value().toArray()) {
                if (!v.isString()) fail("虚拟网卡列表项必须是文本");
                auto entry = v.toString().trimmed(); if (entry.isEmpty()) continue;
                if (entry.size() > 256 || entry.contains('\n') || entry.contains('\r')) fail("虚拟网卡列表项无效");
                if (it.key() == "route-exclude-address") {
                    const auto subnet = QHostAddress::parseSubnet(entry);
                    if (!entry.contains('/') || subnet.first.isNull() || subnet.second < 0) fail("无效的路由排除网段：" + entry + "；请使用 IPv4/IPv6 CIDR 格式");
                    entry = subnet.first.toString() + '/' + QString::number(subnet.second);
                }
                if (!seen.contains(entry)) { seen.insert(entry); entries.append(entry); }
            }
            out[it.key()] = entries;
        }
    }
    return out;
}
bool hasTunPermission(const QString &core)
{
    if (::geteuid() == 0) return true;
    struct stat info {};
    const auto name = QFile::encodeName(core);
    return ::stat(name.constData(), &info) == 0 && S_ISREG(info.st_mode) && info.st_uid == 0 && (info.st_mode & S_ISUID);
}
bool processRunsAsRoot(qint64 pid)
{
    if (pid <= 0) return false;
    QFile status(QString("/proc/%1/status").arg(pid));
    if (!status.open(QIODevice::ReadOnly)) return false;
    for (const auto &line : status.readAll().split('\n')) {
        if (!line.startsWith("Uid:")) continue;
        const auto fields = line.simplified().split(' ');
        bool ok = false;
        const auto effectiveUid = fields.value(2).toUInt(&ok);
        return ok && effectiveUid == 0;
    }
    return false;
}
bool processHasTunPermission(qint64 pid)
{
    if (pid <= 0) return false;
    QFile status(QString("/proc/%1/status").arg(pid));
    if (!status.open(QIODevice::ReadOnly)) return false;
    for (const auto &line : status.readAll().split('\n')) {
        if (!line.startsWith("CapEff:")) continue;
        bool ok = false;
        const auto capabilities = line.mid(7).trimmed().toULongLong(&ok, 16);
        // CAP_NET_ADMIN is required by TUNSETIFF and route configuration.
        return ok && (capabilities & (quint64(1) << 12));
    }
    return false;
}
static void pkexec(QObject *context, const QStringList &arguments, std::function<void(QString)> done)
{
    auto *process = new QProcess(context);
    auto completed = QSharedPointer<bool>::create(false);
    auto finish = [process, completed, done](bool success) {
        if (*completed) return;
        *completed = true;
        const auto detail = QString::fromUtf8(process->readAllStandardError()).trimmed();
        process->deleteLater(); done(success ? QString() : (detail.isEmpty() ? QString("管理员认证取消或内核授权失败") : detail));
    };
    QObject::connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), context,
                     [finish](int code, QProcess::ExitStatus status) { finish(code == 0 && status == QProcess::NormalExit); });
    QObject::connect(process, &QProcess::errorOccurred, context, [finish](QProcess::ProcessError e) { if (e == QProcess::FailedToStart) finish(false); });
    QTimer::singleShot(120000, process, [process] { if (process->state() != QProcess::NotRunning) process->kill(); });
    process->start("pkexec", arguments);
}
void requestTunPermission(const QString &core, QObject *context, std::function<void(QString)> done)
{
    // Match Clash Party's Linux chown + setuid flow, restricted to our installed core.
    const QFileInfo file(core);
    if (file.canonicalFilePath() != "/usr/lib/dcc-proxy/mihomo" || !file.isFile() || !file.isExecutable()) {
        done("自动授权仅支持随包内核 /usr/lib/dcc-proxy/mihomo；自定义内核请自行授权"); return;
    }
    pkexec(context, {"/usr/bin/chown", "root:root", "/usr/lib/dcc-proxy/mihomo"}, [context, core, done](QString error) {
        if (!error.isEmpty()) { done(error); return; }
        pkexec(context, {"/usr/bin/chmod", "u+s", "/usr/lib/dcc-proxy/mihomo"}, [core, done](QString error) {
            if (error.isEmpty() && !hasTunPermission(core)) error = "内核授权未生效，请检查文件系统是否禁用了 setuid";
            done(error);
        });
    });
}
}
