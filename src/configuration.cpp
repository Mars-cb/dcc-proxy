#include "config.h"
#include "geodata.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLockFile>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QElapsedTimer>
#include <QUrl>
#include <QSet>
#include <stdexcept>

namespace Party {
static void reject(const QString &error) { throw std::runtime_error(error.toStdString()); }
bool localController(const QString &endpoint)
{
    const QUrl url(endpoint);
    return url.scheme() == "http" && QStringList{"127.0.0.1", "localhost", "::1"}.contains(url.host());
}
QJsonObject Store::managedSettings() const
{
    auto out = settings();
    if (!localController(out["endpoint"].toString())) {
        out["endpoint"] = out["managedEndpoint"].toString("http://127.0.0.1:19999");
        out["secret"] = out["managedSecret"].toString();
        if (!out["managedCore"].toString().isEmpty()) out["core"] = out["managedCore"];
        // Preserve the running local controller when connecting a remote UI for the first time.
        const auto path = m_root + "/work/config.yaml";
        if (out["managedSecret"].isUndefined() && QFileInfo::exists(path)) {
            const auto config = parseConfig(readFile(path));
            if (config["external-controller"]) out["endpoint"] = "http://" + QString::fromStdString(config["external-controller"].as<std::string>());
            if (config["secret"]) out["secret"] = QString::fromStdString(config["secret"].as<std::string>());
        }
    }
    return out;
}
QStringList managedConfigFiles(const QString &root)
{
    QStringList files{"settings.json", "profiles.json", "override.yaml", "override.js", "controlled.yaml", "customizations.json"};
    for (const auto &name : QDir(root + "/profiles").entryList({"*.yaml"}, QDir::Files))
        if (validId(name.left(name.size() - 5))) files << "profiles/" + name;
    return files;
}
static bool allowedFile(const QString &name)
{
    return QStringList{"settings.json", "profiles.json", "override.yaml", "override.js", "controlled.yaml", "customizations.json"}.contains(name) ||
        (name.startsWith("profiles/") && name.endsWith(".yaml") && validId(name.mid(9, name.size() - 14)));
}
QByteArray configFingerprint(const QString &root)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    auto files = managedConfigFiles(root); files.sort();
    for (const auto &file : files) {
        hash.addData(file.toUtf8()); hash.addData("\0", 1);
        const bool exists = QFileInfo::exists(root + '/' + file);
        hash.addData(exists ? "1" : "0", 1);
        if (exists) { hash.addData(readFile(root + '/' + file)); hash.addData("\0", 1); }
    }
    return hash.result().toHex();
}
void checkCustomizations(const QJsonObject &value)
{
    if ((!value["rules"].isUndefined() && !value["rules"].isObject()) ||
        (!value["overrides"].isUndefined() && !value["overrides"].isArray())) reject("自定义规则或覆写格式无效");
    const auto rules = value["rules"].toObject();
    for (auto it = rules.begin(); it != rules.end(); ++it) {
        if (!validId(it.key()) || !it.value().isArray() || it.value().toArray().size() > 2048) reject("自定义规则的订阅或数量无效");
        for (const auto &entry : it.value().toArray())
            if (!entry.isString() || entry.toString().trimmed().isEmpty() || entry.toString().contains('\n') || entry.toString().contains('\r')) reject("每条自定义规则必须是单行文本");
    }
    QSet<QString> ids;
    if (value["overrides"].toArray().size() > 128) reject("覆写最多支持 128 项");
    for (const auto &entry : value["overrides"].toArray()) {
        const auto item = entry.toObject(); const auto id = item["id"].toString(); const auto scope = item["profile"].toString();
        if (!validId(id) || ids.contains(id) || (!scope.isEmpty() && !validId(scope)) ||
            !QStringList{"yaml", "javascript"}.contains(item["kind"].toString()) || !item["content"].isString() ||
            (!item["enabled"].isUndefined() && !item["enabled"].isBool())) reject("覆写的编号、类型或关联订阅无效");
        ids.insert(id);
        if (item["kind"].toString() == "yaml") validateConfig(parseConfig(item["content"].toString().toUtf8()));
    }
}
QJsonObject Store::customizations() const { return readJson(m_root + "/customizations.json"); }
void Store::setCustomizations(const QJsonObject &value)
{
    checkCustomizations(value);
    writeFile(m_root + "/customizations.json", QJsonDocument(value).toJson());
}
static void applyChange(Store &store, const QJsonObject &change)
{
    const auto files = change["files"].toObject();
    for (auto it = files.begin(); it != files.end(); ++it) {
        if (!allowedFile(it.key())) reject("不支持修改此配置文件");
        const auto path = store.root() + '/' + it.key();
        if (it.value().isNull()) {
            if (QFileInfo::exists(path) && !QFile::remove(path)) reject("无法删除配置文件：" + it.key());
        } else {
            if (!it.value().isString()) reject("配置内容格式无效");
            const auto bytes = QByteArray::fromBase64(it.value().toString().toLatin1());
            if (it.key().endsWith(".yaml")) validateConfig(parseConfig(bytes));
            if (it.key().endsWith(".json") && !QJsonDocument::fromJson(bytes).isObject()) reject("JSON 配置无效");
            writeFile(path, bytes);
        }
    }
    if (change.contains("source")) {
        if (!validId(change["id"].toString())) reject("订阅编号无效");
        store.saveProfile(change["name"].toString(), change["url"].toString(),
                          QByteArray::fromBase64(change["source"].toString().toLatin1()), change["id"].toString());
        if (change["metadata"].isObject()) store.updateProfileOptions(change["id"].toString(), change["metadata"].toObject());
    }
    checkCustomizations(store.customizations());
}
QJsonObject validateStoreChange(const QString &root, const QJsonObject &change)
{
    const auto fingerprint = configFingerprint(root);
    QTemporaryDir stage; if (!stage.isValid()) reject("无法创建配置校验目录");
    for (const auto &file : managedConfigFiles(root))
        if (QFileInfo::exists(root + '/' + file)) writeFile(stage.path() + '/' + file, readFile(root + '/' + file));
    Store candidate(stage.path()); applyChange(candidate, change);
    const auto core = candidate.managedSettings()["core"].toString();
    if (!QFileInfo(core).isExecutable()) reject("请先选择可用的 Mihomo 内核，再校验配置");
    const auto work = stage.path() + "/work"; QDir().mkpath(work); seedGeoData(work);
    for (const auto &file : {"Country.mmdb", "GeoIP.dat", "GeoSite.dat", "ASN.mmdb"})
        if (!QFileInfo::exists(work + '/' + file) && QFileInfo::exists(root + "/work/" + file))
            QFile::copy(root + "/work/" + file, work + '/' + file);
    QStringList profiles;
    if (change.contains("source")) profiles << change["id"].toString();
    else for (const auto &item : candidate.profiles()) profiles << item.toObject()["id"].toString();
    QElapsedTimer budget; budget.start();
    for (const auto &id : profiles) {
        if (budget.elapsed() >= 115000) reject("配置校验超时，原配置未修改");
        const auto path = stage.path() + "/candidate.yaml"; writeFile(path, candidate.runtime(id));
        QProcess process; process.setProcessChannelMode(QProcess::MergedChannels);
        auto env = QProcessEnvironment::systemEnvironment();
        env.insert("SAFE_PATHS", stage.path() + QDir::listSeparator() + root); process.setProcessEnvironment(env);
        process.start(core, {"-t", "-d", work, "-f", path});
        if (!process.waitForStarted(2000)) reject("无法启动内核进行配置校验");
        if (!process.waitForFinished(qMax(1000, int(115000 - budget.elapsed())))) {
            process.kill(); process.waitForFinished(2000); reject("配置校验超时，原配置未修改");
        }
        if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
            reject("完整配置校验失败，原配置未修改：\n" + QString::fromUtf8(process.readAll()).right(3000));
    }
    return {{"valid", true}, {"fingerprint", QString::fromLatin1(fingerprint)}, {"profiles", profiles.size()}};
}
void commitStoreChange(const QString &root, const QJsonObject &change)
{
    QLockFile lock(root + "/configuration.lock");
    if (!lock.tryLock(1000)) reject("配置正在保存，请稍后重试");
    if (change["fingerprint"].toString().toLatin1() != configFingerprint(root)) reject("校验期间配置发生变化，请重新保存；原配置已保留");
    const auto before = backup(root);
    writeFile(root + "/before-change.json", before);
    if (change["restore"].toBool()) writeFile(root + "/before-restore.json", before);
    try { Store store(root); applyChange(store, change); }
    catch (...) {
        try { restore(root, before); } catch (...) { reject("保存失败且自动恢复失败，请使用 before-change.json 恢复"); }
        throw;
    }
}
}
