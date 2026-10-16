#pragma once
#include "api.h"
#include "config.h"
#include <QWidget>
#include <QTimer>
#include <QJsonArray>
#include <QElapsedTimer>
#include <QQueue>
#include <QDateTime>
class QLabel;
class QComboBox;
class QLineEdit;
class QListWidget;
class QTableWidget;
class QPlainTextEdit;
class QTabWidget;
class QSpinBox;
class QCheckBox;
class QPushButton;
class QAbstractButton;

namespace Party {
class NodeList;
class Panel : public QWidget {
    Q_OBJECT
public:
    explicit Panel(QWidget *parent = nullptr);
    void openPage(const QString &page);
    QSize minimumSizeHint() const override { return QSize(460, 420); }
protected:
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
private:
    QWidget *overviewPage();
    QWidget *tunPage();
    void loadTunSettings();
    void setTunOptions(const QJsonObject &options);
    void updateSwitches(const QJsonObject &state);
    QWidget *proxiesPage();
    QWidget *profilesPage();
    QWidget *connectionsPage();
    QWidget *rulesPage();
    QWidget *providersPage();
    QWidget *overridePage();
    QWidget *settingsPage();
    QWidget *toolsPage();
    void refresh();
    void refreshProxies();
    void reloadNodeSources();
    void previewProfile(const QString &id = {});
    void showProfileNodes(const QString &id);
    void updateProxyGroups();
    void renderNodes();
    void selectNode();
    void testNodes(bool group, const QString &node = {});
    void runNodeTests();
    void reloadProfiles();
    void editProfile(bool create);
    void importProfile(bool remote);
    void editSubscriptionOptions();
    void updateProfile();
    void activateProfile();
    QJsonObject selectedProfile() const;
    void runService(const QString &action, const QString &profile = {});
    void backgroundCommand(const QJsonObject &command, Api::Done extra = {});
    void saveSettings();
    void loadSettings();
    void patchConfig(const QJsonObject &patch);
    void editSection(const QString &section);
    void editDocument(const QString &title, const QByteArray &data, std::function<void(QByteArray)> save);
    void fetchProfiles(const QUrl &url, const QString &name, const QString &id = {});
    void syncStreams();
    void resourceAction(bool health);
    void systemProxy(const QString &mode);
    void webdav(bool upload);
    void message(const QString &text, bool error = false);
    void guarded(std::function<void()> action);
    Api m_api;
    Store m_store;
    QNetworkAccessManager m_download;
    QTimer m_refreshTimer;
    QTabWidget *m_tabs;
    QLabel *m_status;
    QLabel *m_overview;
    QLabel *m_traffic;
    QLabel *m_statistics;
    QAbstractButton *m_coreSwitch;
    QPushButton *m_restart;
    bool m_coreBusy = false;
    bool m_tunBusy = false;
    QAbstractButton *m_tunPageSwitch;
    QLabel *m_tunStatus;
    QLineEdit *m_tunDevice;
    QComboBox *m_tunStack;
    QSpinBox *m_tunMtu;
    QLineEdit *m_tunDns;
    QPlainTextEdit *m_tunExclude;
    QMap<QString, QAbstractButton *> m_tunFlags;
    QComboBox *m_mode;
    QComboBox *m_group;
    QComboBox *m_nodeSource;
    QLabel *m_nodeHint;
    QPushButton *m_restoreAutomatic;
    bool m_previewing = false;
    QByteArray m_previewFingerprint;
    QLineEdit *m_nodeSearch;
    QComboBox *m_sort;
    QCheckBox *m_hideUnavailable;
    NodeList *m_nodes;
    QPushButton *m_testGroup;
    QLineEdit *m_testUrl;
    QSpinBox *m_testTimeout;
    QSpinBox *m_testConcurrency;
    QCheckBox *m_unifiedDelay;
    QMap<QString, QString> m_nodeProviders;
    QJsonObject m_providerProxyData;
    QTableWidget *m_profiles;
    QTableWidget *m_connections;
    QLineEdit *m_connectionSearch;
    QTableWidget *m_rules;
    QLineEdit *m_ruleSearch;
    QTableWidget *m_providers;
    QPlainTextEdit *m_logs;
    QCheckBox *m_pauseLogs;
    QComboBox *m_logLevel;
    QLineEdit *m_endpoint;
    QLineEdit *m_secret;
    QLineEdit *m_corePath;
    QCheckBox *m_autoStart;
    QCheckBox *m_systemNodeDns;
    QCheckBox *m_autoSystemProxy;
    QPlainTextEdit *m_proxyBypass;
    QSpinBox *m_updateMinutes;
    QCheckBox *m_overrideEnabled;
    QCheckBox *m_scriptEnabled;
    QPlainTextEdit *m_yamlOverride;
    QPlainTextEdit *m_jsOverride;
    QLineEdit *m_davUrl;
    QLineEdit *m_davUser;
    QLineEdit *m_davPassword;
    QJsonObject m_proxyData;
    QJsonArray m_connectionData;
    QJsonArray m_rulesData;
    bool m_connected = false;
    bool m_polling = false;
    bool m_selecting = false;
    QSet<QString> m_reads;
    QSet<QString> m_testing;
    QQueue<QString> m_testQueue;
    QMap<QString, QPair<QDateTime, int>> m_testResults;
    QMap<QString, QString> m_testErrors;
    QString m_testQuery;
    int m_testTimeoutMs = 5000;
    int m_activeTests = 0;
    int m_testPassed = 0;
    int m_testFailed = 0;
    quint64 m_testEpoch = 0;
};
}
