#include "config.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QCryptographicHash>
#include <unistd.h>
namespace Party {
static QByteArray procFile(qint64 pid, const QString &name)
{
    QFile file(QString("/proc/%1/%2").arg(pid).arg(name));
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}
QJsonObject coreIdentity(qint64 pid, const QString &core, const QString &work)
{
    if (pid <= 0) return {};
    const auto stat = procFile(pid, "stat");
    const auto fields = stat.mid(stat.lastIndexOf(')') + 2).simplified().split(' ');
    if (fields.size() < 20 || fields[0] == "Z") return {};
    bool owned = false;
    for (const auto &line : procFile(pid, "status").split('\n'))
        if (line.startsWith("Uid:")) owned = line.simplified().split(' ').value(1).toUInt() == ::getuid();
    const auto rawArgs = procFile(pid, "cmdline");
    const auto args = rawArgs.split('\0');
    const int d = args.indexOf("-d"), f = args.indexOf("-f");
    if (!owned || d < 0 || f < 0 || QString::fromLocal8Bit(args.value(d + 1)) != work ||
        QString::fromLocal8Bit(args.value(f + 1)) != work + "/config.yaml") return {};
    const auto actual = QFileInfo(QString::fromLocal8Bit(args.value(0))).canonicalFilePath();
    if (actual.isEmpty() || actual != QFileInfo(core).canonicalFilePath()) return {};
    return {{"pid", double(pid)}, {"start", QString::fromLatin1(fields[19])}, {"core", actual},
            {"work", work}, {"arguments", QString::fromLatin1(rawArgs.toBase64())}};
}
QJsonObject findOrphanCore(const QString &root, const QString &core)
{
    const auto work = root + "/work";
    auto recorded = readJson(work + "/core-instance.json");
    const auto current = coreIdentity(qint64(recorded["pid"].toDouble()), core, work);
    if (!current.isEmpty() && current["start"] == recorded["start"] && current["arguments"] == recorded["arguments"]) {
        recorded["verified"] = true; return recorded;
    }
    // Legacy daemons have no record. Match our exact executable, real UID and both owned paths.
    for (const auto &name : QDir("/proc").entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        bool numeric = false; const auto pid = name.toLongLong(&numeric); if (!numeric) continue;
        auto identity = coreIdentity(pid, core, work);
        if (!identity.isEmpty()) { identity["verified"] = true; identity["legacy"] = true; return identity; }
    }
    return {};
}
}
