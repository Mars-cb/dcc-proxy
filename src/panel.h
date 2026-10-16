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
    void backgroundCommand(const QJsonObject &command);
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
    QComboBox *m_mode;
    QCheckBox *m_tun;
    QComboBox *m_group;
    QLineEdit *m_nodeSearch;
    QComboBox *m_sort;
    QCheckBox *m_hideUnavailable;
    NodeList *m_nodes;
    QPushButton *m_testGroup;
    QLineEdit *m_testUrl;
    QSpinBox *m_testTimeout;
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
    int m_testTimeoutMs = 10000;
    int m_activeTests = 0;
    int m_testPassed = 0;
    int m_testFailed = 0;
    quint64 m_testEpoch = 0;
};
}
