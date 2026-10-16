#include "geodata.h"
#include "config.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSet>
#include <stdexcept>
namespace Party {
static bool readVarint(const QByteArray &data, int &position, quint64 &value)
{
    value = 0;
    for (int shift = 0; shift < 64 && position < data.size(); shift += 7) {
        const auto byte = quint8(data[position++]);
        if (shift == 63 && byte > 1) return false;
        value |= quint64(byte & 127) << shift;
        if (!(byte & 128)) return true;
    }
    return false;
}
static bool usableDatabase(const QString &path, bool mmdb)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || !file.size()) return false;
    if (mmdb) {
        file.seek(qMax<qint64>(0, file.size() - 128 * 1024));
        return file.readAll().contains(QByteArray::fromHex("abcdef") + "MaxMind.com");
    }
    if (file.size() > 32 * 1024 * 1024) return false;
    // GeoIPList and GeoSiteList contain repeated length-delimited entry records.
    // Check record boundaries to catch interrupted downloads without rejecting
    // custom country/site lists merely because their checksum differs.
    const auto data = file.readAll(); int position = 0, records = 0;
    while (position < data.size()) {
        quint64 tag, length;
        if (!readVarint(data, position, tag) || tag != 10 ||
            !readVarint(data, position, length) || !length || length > quint64(data.size() - position)) return false;
        position += int(length); ++records;
    }
    return records > 0;
}
int seedGeoData(const QString &work, const QString &resourceDirectory)
{
    const auto directory = resourceDirectory.isEmpty()
        ? qEnvironmentVariable("DCC_PROXY_GEODATA_DIR", "/usr/share/dcc-proxy") : resourceDirectory;
    const auto manifestPath = directory + "/geodata.json";
    if (!QFile::exists(manifestPath)) return 0; // Development / explicitly unbundled builds.
    const auto files = readJson(manifestPath)["files"].toArray();
    const QSet<QString> allowed{"Country.mmdb", "GeoIP.dat", "GeoSite.dat", "ASN.mmdb"};
    QSet<QString> provided;
    for (const auto &entry : files) provided.insert(entry.toObject()["file"].toString());
    if (provided != allowed || files.size() != allowed.size())
        throw std::runtime_error("随包地理数据库清单无效，请重新安装 dcc-proxy");
    if (!QDir().mkpath(work)) throw std::runtime_error("无法创建内核数据目录");
    int copied = 0;
    for (const auto &entry : files) {
        const auto info = entry.toObject(); const auto name = info["file"].toString();
        QStringList existing;
        for (const auto &file : QDir(work).entryList(QDir::Files)) {
            const bool alias = name == "Country.mmdb" && (file.compare("geoip.db", Qt::CaseInsensitive) == 0 || file.compare("geoip.metadb", Qt::CaseInsensitive) == 0);
            if (alias || file.compare(name, Qt::CaseInsensitive) == 0) existing.append(work + '/' + file);
        }
        // Use the same sorted-first filename precedence as Mihomo.
        if (!existing.isEmpty() && usableDatabase(existing.first(), name.endsWith(".mmdb"))) continue;
        const auto bytes = readFile(directory + '/' + name);
        const auto digest = QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
        if (digest != info["sha256"].toString() || bytes.size() != info["size"].toInt())
            throw std::runtime_error(("随包数据库校验失败：" + name + "，请重新安装 dcc-proxy").toStdString());
        // Keep interrupted/corrupt downloads available for inspection.
        for (const auto &file : existing) {
            const auto backup = file + ".invalid-" + QDateTime::currentDateTimeUtc().toString("yyyyMMddHHmmsszzz");
            if (!QFile::rename(file, backup)) throw std::runtime_error(("无法备份无效数据库：" + file).toStdString());
        }
        writeFile(work + '/' + name, bytes); ++copied;
    }
    return copied;
}
}
