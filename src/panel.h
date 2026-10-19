#pragma once
#include "api.h"
#include "config.h"
#include <QWidget>
#include <QTimer>
#include <QJsonArray>
#include <QElapsedTimer>
#include <QQueue>
#include <QDateTime>
#include <QMap>
#include <utility>
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
class QVBoxLayout;
class QAbstractButton;

namespace Party {
class NodeList;
class MessageArea;
class Panel : public QWidget {
    Q_OBJECT
public:
    explicit Panel(QWidget *parent = nullptr);
    void openPage(const QString &page);
    void setExternalNavigation(bool enabled);
    QSize minimumSizeHint() const override { return QSize(460, 420); }
signals:
    void pageChanged(int index);
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
    QWidget *groupsPage();
    void renderGroups();
    void renderNodeList(NodeList *list, const QString &group);
    void chooseNode(const QString &group, const QString &name);
    QWidget *profilesPage();
    QWidget *connectionsPage();
    QWidget *rulesPage();
    QString websiteRuleProfile() const;
    QString websiteRulePolicy() const;
    void updateWebsiteRule();
    void addWebsiteRule();
    void saveChange(const QJsonObject &change, std::function<void()> after = {});
    void saveProfileCandidate(const QString &name, const QString &url, const QByteArray &bytes, const QString &id = {}, const QJsonObject &metadata = {});
    void applySavedProfile(const QString &id);
    void editCustomRules();
    void editManagedOverrides();
    void editSectionForm(const QString &section);
    void diagnose();
    void subStore();
    void restoreArchive(const QByteArray &bytes);
    QWidget *providersPage();
    void updateProviders(const QString &kind, const QJsonObject &providers);
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
    void testNodes(bool group, const QString &node = {}, const QString &policy = {});
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
    void loadPortSettings();
    void patchConfig(const QJsonObject &patch);
    void editSection(const QString &section);
    void editDocument(const QString &title, const QByteArray &data, std::function<void(QByteArray)> save, const QString &help = {}, const QByteArray &example = {});
    void fetchProfiles(const QUrl &url, const QString &name, const QString &id = {});
    void syncStreams();
    void loadLogHistory();
    void resourceAction(bool health);
    void systemProxy(const QString &mode);
    void webdav(bool upload);
    void message(const QString &text, bool error = false);
    void showMessage(const QString &text, bool error);
    int notificationPage() const;
    Api::Done scopedReply(Api::Done done);
    template<typename... Args> std::function<void(Args...)> scopedCallback(std::function<void(Args...)> done, int page = -1)
    {
        if (page < 0) page = notificationPage();
        const QPointer<Panel> guard(this);
        return [guard, page, done](Args... args) {
            if (!guard) return;
            const int previous = guard->m_notificationContext; guard->m_notificationContext = page;
            try { done(std::forward<Args>(args)...); }
            catch (...) { if (guard) guard->m_notificationContext = previous; throw; }
            if (guard) guard->m_notificationContext = previous;
        };
    }
    void requestService(const QJsonObject &command, QObject *context, Api::Done done);
    void resetPageStatus();
    void guarded(std::function<void()> action);
    Api m_api;
    Store m_store;
    QNetworkAccessManager m_download;
    QTimer m_refreshTimer;
    QTabWidget *m_tabs = nullptr;
    QLabel *m_pageTitle;
    bool m_externalNavigation = false;
    int m_notificationContext = -1;
    QMap<int, QPair<QString, bool>> m_pageMessages;
    QLabel *m_status;
    QLabel *m_backendVersion = nullptr;
    MessageArea *m_messageArea;
    QLabel *m_overview;
    QLabel *m_traffic;
    QLabel *m_statistics;
    QAbstractButton *m_coreSwitch;
    QPushButton *m_restart;
    bool m_coreBusy = false;
    bool m_configBusy = false;
    QJsonObject m_serviceState;
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
    QComboBox *m_groupsSource;
    QLabel *m_groupsHint;
    QVBoxLayout *m_groupsLayout = nullptr;
    struct GroupCard { QWidget *frame; QPushButton *header; QLabel *route; QPushButton *test; QPushButton *automatic; NodeList *nodes; };
    QMap<QString, GroupCard> m_groupCards;
    QStringList m_groupOrder;
    QString m_groupScope;
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
    QLineEdit *m_website = nullptr;
    QLabel *m_websitePreview = nullptr;
    QPushButton *m_addWebsiteRule = nullptr;
    QTableWidget *m_providers;
    QPlainTextEdit *m_logs;
    QLabel *m_logStatus;
    bool m_logHistoryLoaded = false;
    QCheckBox *m_pauseLogs;
    QComboBox *m_logLevel;
    QLineEdit *m_endpoint;
    QLineEdit *m_secret;
    QLineEdit *m_corePath;
    QMap<QString, QSpinBox *> m_proxyPorts;
    QPushButton *m_applyPorts;
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
    struct NodeTest { QString name; QString query; QString provider; int timeout; };
    QQueue<NodeTest> m_testQueue;
    QSet<int> m_testPages;
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
