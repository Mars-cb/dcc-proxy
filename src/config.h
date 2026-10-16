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
QStringList defaultProxyBypass();
QStringList normalizeProxyBypass(const QStringList &entries);
QByteArray proxyPac(int port, const QStringList &entries);
void setSystemProxy(const QString &mode, int port, const QString &pacUrl = {}, const QStringList &bypass = defaultProxyBypass());
void restoreSystemProxy();

class Store {
public:
    explicit Store(QString root = dataDir());
    QString root() const { return m_root; }
    QJsonObject settings() const;
    void setSettings(const QJsonObject &settings);
    QJsonArray profiles() const;
    QString saveProfile(const QString &name, const QString &url, const QByteArray &data,
                        const QString &id = {});
    void removeProfile(const QString &id);
    void updateProfileOptions(const QString &id, const QJsonObject &options);
    QByteArray profile(const QString &id) const;
    QString profilePath(const QString &id) const;
    QByteArray runtime(const QString &id) const;
private:
    QString m_root;
};
int localProxyPort(const Store &store);
}
