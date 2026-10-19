#pragma once
#include <QJsonObject>
#include <QJsonArray>
#include <QString>
#include <QStringList>
#include <yaml-cpp/yaml.h>

namespace Party {
QString dataDir();
QString socketName();
QString serviceExecutable();
QByteArray readFile(const QString &path);
void writeFile(const QString &path, const QByteArray &data);
QJsonObject readJson(const QString &path);
QString newId();
bool validId(const QString &id);
YAML::Node parseConfig(const QByteArray &data);
QByteArray normalizeSubscription(const QByteArray &data);
void validateConfig(const YAML::Node &node);
YAML::Node mergeYaml(const YAML::Node &base, const YAML::Node &overrides);
QByteArray emitYaml(const YAML::Node &node);
QJsonValue yamlToJson(const YAML::Node &node);
QByteArray backup(const QString &root);
void restore(const QString &root, const QByteArray &bytes);
QStringList proxyPortKeys();
QJsonObject validateProxyPorts(const QJsonObject &ports);
QStringList defaultProxyBypass();
QStringList normalizeProxyBypass(const QStringList &entries);
QByteArray proxyPac(int port, const QStringList &entries);
void setSystemProxy(const QString &mode, int port, const QString &pacUrl = {}, const QStringList &bypass = defaultProxyBypass());
void restoreSystemProxy(bool force = false);

class Store {
public:
    explicit Store(QString root = dataDir());
    QString root() const { return m_root; }
    QJsonObject settings() const;
    QJsonObject managedSettings() const;
    void setSettings(const QJsonObject &settings);
    QJsonArray profiles() const;
    QString saveProfile(const QString &name, const QString &url, const QByteArray &data,
                        const QString &id = {});
    void removeProfile(const QString &id);
    void updateProfileOptions(const QString &id, const QJsonObject &options);
    QByteArray profile(const QString &id) const;
    QString profilePath(const QString &id) const;
    QByteArray runtime(const QString &id) const;
    QJsonObject customizations() const;
    void setCustomizations(const QJsonObject &value);
private:
    QString m_root;
};
int localProxyPort(const Store &store);
QStringList managedConfigFiles(const QString &root);
QByteArray configFingerprint(const QString &root);
QJsonObject validateStoreChange(const QString &root, const QJsonObject &change);
void commitStoreChange(const QString &root, const QJsonObject &change);
bool localController(const QString &endpoint);
void checkCustomizations(const QJsonObject &value);
YAML::Node extendedSubscriptionNode(const QString &line);
void applyUriTransport(YAML::Node &node, QString type, const QString &host, const QString &path, const QString &service);
QJsonObject coreIdentity(qint64 pid, const QString &core, const QString &work);
QJsonObject findOrphanCore(const QString &root, const QString &core);
}
