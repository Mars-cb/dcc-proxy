#include "config.h"
#include "tun.h"
#include "subscriptionnodes.h"
#include <DSwitchButton>
#include <QLocalServer>
#include <QLocalSocket>
#include "api.h"
#include "panel.h"
#include "nodeflags.h"
#include "nodelist.h"
#include "plugin.h"
#include <QToolButton>
#include <QHBoxLayout>
#include <QtTest>
#include <QTemporaryDir>
#include <QJsonDocument>
#include <QTcpServer>
#include <QTcpSocket>
#include <QFile>
#include <QListWidget>
#include <QPushButton>
#include <QComboBox>
#include <QLineEdit>
#include <QTableWidget>
#include <QPersistentModelIndex>
#include <QLabel>
#include <QJSEngine>
#include <QProcess>
#include <DPasswordEdit>
#include <DIconButton>
#include <QNetworkProxy>
#include <QDialog>
#include <QDialogButtonBox>
#include <QMessageBox>
#include <QSslSocket>
#include <QSslError>
#include <QTabWidget>
#include <QScrollArea>
#include <QScrollBar>
#include <QSslCertificate>
#include <QSslKey>
#include <QSslConfiguration>
#include <QPlainTextEdit>
#include <QCheckBox>
#include <QSpinBox>
#include <QUrlQuery>
using namespace Party;
static QString testCore()
{
    const auto bundled = QStringLiteral(PARTY_TEST_CORE);
    return bundled.isEmpty() ? (QFile::exists("/usr/lib/dcc-proxy/mihomo") ? QString("/usr/lib/dcc-proxy/mihomo") : QString()) : bundled;
}
class TestFrame : public DCC_NAMESPACE::FrameProxyInterface {
public:
    QWidget host;
    QPointer<QWidget> page;
    int pushes = 0;
    QList<QPointer<QWidget>> stack;
    QHBoxLayout layout{&host};
    void pushWidget(DCC_NAMESPACE::ModuleInterface *const module, QWidget *const w, PushType) override {
        while (stack.size() > 1) popWidget(module);
        ++pushes; stack.append(w); page = w; layout.addWidget(w, stack.size() == 1 ? 1 : 3); w->show();
    }
    void popWidget(DCC_NAMESPACE::ModuleInterface *const) override {
        if (stack.isEmpty()) return;
        const auto retired = stack.takeLast();
        if (retired) { layout.removeWidget(retired); retired->setParent(nullptr); retired->hide(); retired->deleteLater(); }
        page = stack.isEmpty() ? nullptr : stack.last();
    }
    void setModuleVisible(DCC_NAMESPACE::ModuleInterface *const, const bool) override {}
    void showModulePage(const QString &, const QString &, bool) override {}
    void setModuleSubscriptVisible(const QString &, bool) override {}
    void setRemoveableDeviceStatus(QString, bool) override {}
    bool getRemoveableDeviceStatus(QString) const override { return false; }
    void setSearchPath(DCC_NAMESPACE::ModuleInterface *const) const override {}
    void addChildPageTrans(const QString &, const QString &) override {}
    void setModuleVisible(const QString &, bool) override {}
    void setWidgetVisible(const QString &, const QString &, bool) override {}
    void setDetailVisible(const QString &, const QString &, const QString &, bool) override {}
    void updateSearchData(const QString &) override {}
    QString moduleDisplayName(const QString &) const override { return {}; }
};
class TestTlsServer : public QTcpServer {
public:
    QSslCertificate certificate;
    QSslKey key;
protected:
    void incomingConnection(qintptr descriptor) override {
        auto *socket = new QSslSocket(this);
        socket->setLocalCertificate(certificate); socket->setPrivateKey(key);
        socket->setPeerVerifyMode(QSslSocket::VerifyNone);
        socket->setProtocol(QSsl::TlsV1_2);
        connect(socket, QOverload<QAbstractSocket::SocketError>::of(&QSslSocket::error), socket, [socket](QAbstractSocket::SocketError) { qWarning() << "Test TLS server error:" << socket->errorString(); });
        connect(socket, QOverload<const QList<QSslError> &>::of(&QSslSocket::sslErrors), socket, [](const QList<QSslError> &errors) { for (const auto &e : errors) qWarning() << "Test TLS server:" << e.errorString(); });
        if (!socket->setSocketDescriptor(descriptor)) { delete socket; return; }
        addPendingConnection(socket); socket->startServerEncryption();
    }
};
static NodeList *policyNodes(Panel &panel, const QString &name)
{
    for (auto *list : panel.findChildren<NodeList *>("policy-group-nodes")) if (list->property("proxyGroup").toString() == name) return list;
    return nullptr;
}
static QPushButton *policyButton(Panel &panel, const QString &object, const QString &name)
{
    for (auto *button : panel.findChildren<QPushButton *>(object)) if (button->property("proxyGroup").toString() == name) return button;
    return nullptr;
}
class PartyTests : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { QNetworkProxy::setApplicationProxy(QNetworkProxy::NoProxy); }
    void javascriptOverrideDnsAndProfileName()
    {
        QTemporaryDir dir; Store store(dir.path());
        const auto id = store.saveProfile("订阅名称", {}, "dns: {enable: true, nameserver: [1.1.1.1, 8.8.8.8]}\nrules: [\"MATCH,DIRECT\"]\n");
        auto settings = store.settings(); settings["scriptEnabled"] = true; store.setSettings(settings);
        const QByteArray script = R"JS(function main(config, profileName) {
            if (!config.dns) config.dns = {};
            if (!Array.isArray(config.dns.nameserver)) config.dns.nameserver = [];
            const newDns = "10.20.0.10";
            if (!config.dns.nameserver.includes(newDns)) config.dns.nameserver.unshift(newDns);
            config["profile-name-fixture"] = profileName;
            return config;
        })JS";
        writeFile(dir.path() + "/override.js", script);
        auto runtime = parseConfig(store.runtime(id));
        QCOMPARE(runtime["dns"]["nameserver"].size(), size_t(3));
        QCOMPARE(runtime["dns"]["nameserver"][0].as<std::string>(), std::string("10.20.0.10"));
        QCOMPARE(runtime["dns"]["nameserver"][1].as<std::string>(), std::string("1.1.1.1"));
        QCOMPARE(runtime["dns"]["nameserver"][2].as<std::string>(), std::string("8.8.8.8"));
        QCOMPARE(QString::fromStdString(runtime["profile-name-fixture"].as<std::string>()), QString("订阅名称"));
        // Reapplying must preserve existing nameservers without inserting twice.
        store.saveProfile("订阅名称", {}, emitYaml(runtime), id);
        runtime = parseConfig(store.runtime(id));
        QCOMPARE(runtime["dns"]["nameserver"].size(), size_t(3));
        for (const QByteArray yaml : {QByteArray("rules: [\"MATCH,DIRECT\"]\n"), QByteArray("dns: {nameserver: invalid}\nrules: [\"MATCH,DIRECT\"]\n")}) {
            store.saveProfile("订阅名称", {}, yaml, id);
            runtime = parseConfig(store.runtime(id));
            QCOMPARE(runtime["dns"]["nameserver"].size(), size_t(1));
            QCOMPARE(runtime["dns"]["nameserver"][0].as<std::string>(), std::string("10.20.0.10"));
        }
        writeFile(dir.path() + "/override.js", "function main(c) { if (![NaN].includes(NaN) || ![1,2].includes(2,-1) || [1].includes(1,Infinity) || !Array.prototype.includes.call({0: 'x',length: 1}, 'x')) throw Error('includes semantics'); return c; }");
        QVERIFY(!store.runtime(id).isEmpty());
    }
    void javascriptOverrideErrors()
    {
        QTemporaryDir dir; Store store(dir.path());
        const auto id = store.saveProfile("Fixture", {}, "rules: [\"MATCH,DIRECT\"]\n");
        auto settings = store.settings(); settings["scriptEnabled"] = true; store.setSettings(settings);
        const QList<QPair<QByteArray, QString>> cases = {
            {"function main(c) {\n c.missing.call();\n}", "TypeError"},
            {"function main(c) { throw 'explicit failure'; }", "explicit failure"},
            {"function main(c) {", "SyntaxError"},
            {"function main(c) { return null; }", "must return a configuration object"},
            {"function main(c) { return []; }", "must return a configuration object"},
            {"function main(c) {}", "must return a configuration object"},
            {"function main(c) { c.circular = c; return c; }", "TypeError"},
            {"function main(c) { return {then: function() {}}; }", "synchronously"}
        };
        for (const auto &test : cases) {
            writeFile(dir.path() + "/override.js", test.first);
            bool failed = false;
            try { store.runtime(id); }
            catch (const std::runtime_error &error) {
                failed = true; const auto message = QString::fromUtf8(error.what());
                QVERIFY2(message.contains(test.second), qPrintable(message));
                if (test.second == "TypeError" || test.second == "SyntaxError") QVERIFY2(message.contains("override.js"), qPrintable(message));
            }
            QVERIFY2(failed, test.first.constData());
        }
        QProcess process; process.start(serviceExecutable(), {"--eval", dir.path() + "/override.js"});
        QVERIFY(process.waitForStarted()); process.write("[]"); process.closeWriteChannel(); QVERIFY(process.waitForFinished());
        QCOMPARE(process.exitCode(), 1); QVERIFY(process.readAllStandardError().contains("input must be a JSON configuration object"));
    }
    void proxyPortsRuntimeAndValidation()
    {
        QTemporaryDir dir; Store store(dir.path());
        const auto id = store.saveProfile("Fixture", {}, "mixed-port: 16666\nport: 17891\nsocks-port: 17892\nrules: [\"MATCH,DIRECT\"]\n");
        writeFile(dir.path() + "/controlled.yaml", "mixed-port: 27890\nport: 27891\nsocks-port: 0\n");
        writeFile(dir.path() + "/override.yaml", "mixed-port: 37890\nport: 37891\n");
        writeFile(dir.path() + "/override.js", "function main(c) { c['mixed-port'] = 47890; return c; }");
        auto settings = store.settings(); settings["scriptEnabled"] = true; store.setSettings(settings);
        const auto config = parseConfig(store.runtime(id));
        QCOMPARE(config["mixed-port"].as<int>(), 27890);
        QCOMPARE(config["port"].as<int>(), 27891);
        QCOMPARE(config["socks-port"].as<int>(), 0);
        QCOMPARE(validateProxyPorts({{"mixed-port", 27890}, {"port", 0}})["mixed-port"].toInt(), 27890);
        for (const auto &ports : QList<QJsonObject>{{}, {{"mixed-port", 0}}, {{"port", -1}}, {{"port", 65536}},
            {{"port", 1.5}}, {{"port", "16666"}}, {{"unknown", 8}}, {{"mixed-port", 8}, {"port", 8}}})
            QVERIFY_EXCEPTION_THROWN(validateProxyPorts(ports), std::runtime_error);
    }
    void proxyPortsUiSavesAndRestores()
    {
        QTemporaryDir dir; const auto oldEnv = qgetenv("DCC_PROXY_DATA_DIR"); qputenv("DCC_PROXY_DATA_DIR", dir.path().toUtf8());
        Store store; writeFile(dir.path() + "/controlled.yaml", "mixed-port: 27890\nport: 27891\n");
        QLocalServer ipc; QVERIFY(ipc.listen(dir.path() + "/service.sock"));
        QJsonObject requested; bool fail = false;
        connect(&ipc, &QLocalServer::newConnection, this, [&] {
            while (auto *socket = ipc.nextPendingConnection()) {
                auto buffer = QSharedPointer<QByteArray>::create(); connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QLocalSocket::readyRead, socket, [&, socket, buffer] {
                    buffer->append(socket->readAll()); if (!buffer->contains('\n')) return;
                    const auto command = QJsonDocument::fromJson(*buffer).object();
                    QJsonObject result{{"running", false}};
                    if (command["action"].toString() == "ports") {
                        requested = command["options"].toObject();
                        if (fail) result["error"] = "端口已占用";
                        else writeFile(dir.path() + "/controlled.yaml", QJsonDocument(requested).toJson());
                    }
                    QTimer::singleShot(30, socket, [socket, result] { socket->write(QJsonDocument(result).toJson(QJsonDocument::Compact) + '\n'); socket->flush(); socket->disconnectFromServer(); });
                });
            }
        });
        {
            Panel panel; panel.show();
            auto *mixed = panel.findChild<QSpinBox *>("proxy-mixed-port"); auto *http = panel.findChild<QSpinBox *>("proxy-port");
            auto *apply = panel.findChild<QPushButton *>("apply-proxy-ports"); QVERIFY(mixed && http && apply);
            QCOMPARE(mixed->value(), 27890); QCOMPARE(http->value(), 27891);
            QCOMPARE(panel.findChild<QSpinBox *>("proxy-socks-port")->value(), 0);
            mixed->setValue(37890); QTest::mouseClick(apply, Qt::LeftButton); QVERIFY(!apply->isEnabled());
            QTRY_COMPARE(requested["mixed-port"].toInt(), 37890); QTRY_VERIFY(apply->isEnabled()); QCOMPARE(mixed->value(), 37890);
            fail = true; mixed->setValue(47890); QTest::mouseClick(apply, Qt::LeftButton);
            QTRY_COMPARE(requested["mixed-port"].toInt(), 47890); QTRY_VERIFY(apply->isEnabled()); QCOMPARE(mixed->value(), 37890);
            panel.hide();
        }
        if (oldEnv.isNull()) qunsetenv("DCC_PROXY_DATA_DIR"); else qputenv("DCC_PROXY_DATA_DIR", oldEnv);
    }
    void threeProxyModesApplyReadBackAndPersist()
    {
        QTemporaryDir dir; const auto oldEnv = qgetenv("DCC_PROXY_DATA_DIR"); qputenv("DCC_PROXY_DATA_DIR", dir.path().toUtf8());
        Store store; const auto id = store.saveProfile("Modes", {}, "mode: rule\nrules: [\"MATCH,DIRECT\"]\n");
        QTcpServer server; QVERIFY(server.listen(QHostAddress::LocalHost));
        auto settings = store.settings(); settings["active"] = id; settings["endpoint"] = QString("http://127.0.0.1:%1").arg(server.serverPort()); store.setSettings(settings);
        QString current = "rule"; int patches = 0;
        connect(&server, &QTcpServer::newConnection, this, [&] {
            while (auto *socket = server.nextPendingConnection()) {
                auto buffer = QSharedPointer<QByteArray>::create(); connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, socket, [&, socket, buffer] {
                    buffer->append(socket->readAll()); const int end = buffer->indexOf("\r\n\r\n"); if (end < 0) return;
                    const auto length = QRegularExpression("Content-Length: (\\d+)", QRegularExpression::CaseInsensitiveOption).match(QString::fromUtf8(buffer->left(end)));
                    if (length.hasMatch() && buffer->size() < end + 4 + length.captured(1).toInt()) return;
                    if (buffer->startsWith("PATCH /configs ")) {
                        current = QJsonDocument::fromJson(buffer->mid(end + 4)).object()["mode"].toString(); ++patches;
                    }
                    const auto body = buffer->startsWith("GET /configs ") ? QJsonDocument(QJsonObject{{"mode", current}, {"mixed-port", 16666}}).toJson(QJsonDocument::Compact) : QByteArray("{}");
                    socket->write("HTTP/1.1 200 OK\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body); socket->disconnectFromHost();
                });
            }
        });
        {
            Panel panel; panel.show(); auto *combo = panel.findChild<QComboBox *>("proxy-mode"); QVERIFY(combo);
            QTRY_VERIFY(panel.findChild<QLabel *>("overview-core")->text().contains("内核已连接"));
            QTRY_VERIFY(combo->isEnabled()); QCOMPARE(combo->count(), 3);
            for (const auto &pair : QList<QPair<QString, QString>>{{"规则", "rule"}, {"全局", "global"}, {"直连", "direct"}, {"规则", "rule"}, {"全局", "global"}}) {
                const int index = combo->findText(pair.first); QVERIFY(index >= 0); QCOMPARE(combo->itemData(index).toString(), pair.second);
                const int expected = patches + 1; combo->setCurrentIndex(index); QMetaObject::invokeMethod(combo, "activated", Q_ARG(int, index));
                QTRY_COMPARE(patches, expected); QCOMPARE(current, pair.second);
                QTRY_VERIFY(QFile::exists(dir.path() + "/controlled.yaml"));
                QTRY_COMPARE(QString::fromStdString(parseConfig(readFile(dir.path() + "/controlled.yaml"))["mode"].as<std::string>()), pair.second);
                QCOMPARE(QString::fromStdString(parseConfig(store.runtime(id))["mode"].as<std::string>()), pair.second);
                QTRY_COMPARE(combo->currentData().toString(), pair.second);
            }
            panel.hide();
        }
        {
            Panel reopened; reopened.show(); auto *combo = reopened.findChild<QComboBox *>("proxy-mode");
            QTRY_VERIFY(combo->isEnabled()); QTRY_COMPARE(combo->currentData().toString(), current); reopened.hide();
        }
        if (oldEnv.isNull()) qunsetenv("DCC_PROXY_DATA_DIR"); else qputenv("DCC_PROXY_DATA_DIR", oldEnv);
    }
    void allApiPagesAndActions()
    {
        QTemporaryDir dir; const auto oldEnv = qgetenv("DCC_PROXY_DATA_DIR"); qputenv("DCC_PROXY_DATA_DIR", dir.path().toUtf8());
        QTcpServer server; QVERIFY(server.listen(QHostAddress::LocalHost));
        Store store; auto settings = store.settings(); settings["endpoint"] = QString("http://127.0.0.1:%1").arg(server.serverPort()); store.setSettings(settings);
        QList<QByteArray> requests;
        connect(&server, &QTcpServer::newConnection, this, [&] {
            while (auto *socket = server.nextPendingConnection()) {
                auto buffer = QSharedPointer<QByteArray>::create(); connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, socket, [&, socket, buffer] {
                    buffer->append(socket->readAll()); const int end = buffer->indexOf("\r\n\r\n"); if (end < 0) return;
                    const auto match = QRegularExpression("Content-Length: (\\d+)", QRegularExpression::CaseInsensitiveOption).match(QString::fromUtf8(buffer->left(end)));
                    if (match.hasMatch() && buffer->size() < end + 4 + match.captured(1).toInt()) return;
                    requests.append(*buffer); QByteArray body = "{}";
                    if (buffer->startsWith("GET /configs ")) body = R"({"mixed-port":16666,"mode":"rule"})";
                    if (buffer->startsWith("GET /connections ")) body = R"({"connections":[{"id":"connection-1","metadata":{"host":"fixture.test","destinationPort":"80","network":"tcp"}}],"uploadTotal":1,"downloadTotal":2,"memory":3})";
                    if (buffer->startsWith("GET /rules ")) body = R"({"rules":[{"type":"Domain","payload":"fixture.test","proxy":"DIRECT","extra":{"disabled":false}}]})";
                    if (buffer->startsWith("GET /providers/proxies ")) body = R"({"providers":{"fixture /?#":{"vehicleType":"HTTP","proxies":[{"name":"DIRECT"}]}}})";
                    if (buffer->startsWith("GET /providers/rules ")) body = R"({"providers":{"rules /?#":{"vehicleType":"HTTP","ruleCount":1}}})";
                    if (buffer->startsWith("GET /logs?")) body = "{\"type\":\"info\",\"payload\":\"fixture log\"}\n";
                    if (buffer->startsWith("GET /traffic ")) body = "{\"up\":1024,\"down\":2048}\n";
                    socket->write("HTTP/1.1 200 OK\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body); socket->disconnectFromHost();
                });
            }
        });
        auto seen = [&](const QByteArray &prefix) { for (const auto &request : requests) if (request.startsWith(prefix)) return true; return false; };
        {
            Panel panel; panel.resize(1000, 900); panel.show();
            auto click = [&](const QString &text) { for (auto *b : panel.findChildren<QPushButton *>()) if (b->text() == text) { QTest::mouseClick(b, Qt::LeftButton); return true; } return false; };
            QTRY_VERIFY(seen("GET /traffic "));
            auto *mode = panel.findChild<QComboBox *>("proxy-mode"); QVERIFY(mode);
            mode->setCurrentIndex(mode->findData("direct")); QMetaObject::invokeMethod(mode, "activated", Q_ARG(int, mode->currentIndex()));
            QTRY_VERIFY(seen("PATCH /configs "));
            QTRY_VERIFY(QFile::exists(dir.path() + "/controlled.yaml"));
            QCOMPARE(parseConfig(readFile(dir.path() + "/controlled.yaml"))["mode"].as<std::string>(), std::string("direct"));
            panel.openPage("Connections"); auto *connections = panel.findChild<QTableWidget *>("connections"); QTRY_COMPARE(connections->rowCount(), 1);
            connections->selectRow(0); QVERIFY(click("关闭选中连接")); QTRY_VERIFY(seen("DELETE /connections/connection-1 "));
            QVERIFY(click("关闭全部连接")); QTRY_VERIFY(seen("DELETE /connections "));
            auto *search = panel.findChild<QLineEdit *>("connection-search"); search->setText("absent"); QTRY_COMPARE(connections->rowCount(), 0);
            search->clear(); QTRY_COMPARE(connections->rowCount(), 1);
            panel.openPage("Rules"); auto *rules = panel.findChild<QTableWidget *>("rules"); QTRY_COMPARE(rules->rowCount(), 1);
            rules->selectRow(0); QVERIFY(click("启用 / 禁用规则")); QTRY_VERIFY(seen("PATCH /rules/disable "));
            auto *ruleSearch = panel.findChild<QLineEdit *>("rule-search"); ruleSearch->setText("absent"); QTRY_COMPARE(rules->rowCount(), 0);
            ruleSearch->clear(); QTRY_COMPARE(rules->rowCount(), 1);
            panel.openPage("Providers"); auto *providers = panel.findChild<QTableWidget *>("providers"); QTRY_COMPARE(providers->rowCount(), 2);
            int proxyRow = -1, ruleRow = -1;
            for (int r = 0; r < providers->rowCount(); ++r) { if (providers->item(r, 1)->text().startsWith("fixture")) proxyRow = r; else ruleRow = r; }
            providers->selectRow(proxyRow); QVERIFY(click("更新资源")); QTRY_VERIFY(seen("PUT /providers/proxies/fixture%20%2F%3F%23 "));
            QTRY_COMPARE(providers->rowCount(), 2);
            for (int r = 0; r < providers->rowCount(); ++r) if (providers->item(r, 1)->text().startsWith("fixture")) providers->selectRow(r);
            QVERIFY(click("节点健康检查")); QTRY_VERIFY(seen("GET /providers/proxies/fixture%20%2F%3F%23/healthcheck "));
            QTRY_COMPARE(providers->rowCount(), 2);
            for (int r = 0; r < providers->rowCount(); ++r) if (providers->item(r, 1)->text().startsWith("rules")) ruleRow = r;
            providers->selectRow(ruleRow); QVERIFY(click("更新资源")); QTRY_VERIFY(seen("PUT /providers/rules/rules%20%2F%3F%23 "));
            QVERIFY(click("更新 GEO 数据")); QTRY_VERIFY(seen("POST /upgrade/geo "));
            panel.openPage("Logs"); auto *logs = panel.findChild<QPlainTextEdit *>("core-logs"); QTRY_VERIFY(logs->toPlainText().contains("fixture log"));
            auto *pause = panel.findChild<QCheckBox *>("pause-logs"); pause->setChecked(true); QVERIFY(click("清空")); QVERIFY(logs->toPlainText().isEmpty());
            auto *level = panel.findChild<QComboBox *>("log-level"); level->setCurrentText("debug"); pause->setChecked(false); QTRY_VERIFY(seen("GET /logs?level=debug "));
            panel.openPage("Tools"); QVERIFY(click("清除 dns 缓存")); QTRY_VERIFY(seen("POST /cache/dns/flush "));
            QVERIFY(click("清除 fakeip 缓存")); QTRY_VERIFY(seen("POST /cache/fakeip/flush "));
            QTimer closer; closer.setInterval(20); connect(&closer, &QTimer::timeout, this, [] { if (auto *d = qobject_cast<QDialog *>(QApplication::activeModalWidget())) d->reject(); }); closer.start();
            QVERIFY(click("DNS 查询")); QTRY_VERIFY(seen("GET /dns/query?name=example.com&type=A ")); QTest::qWait(80); closer.stop();
            panel.hide();
        }
        if (oldEnv.isNull()) qunsetenv("DCC_PROXY_DATA_DIR"); else qputenv("DCC_PROXY_DATA_DIR", oldEnv);
    }
    void webdavHttpsBackupRoundTrip()
    {
        QTemporaryDir dir; const auto oldEnv = qgetenv("DCC_PROXY_DATA_DIR"); qputenv("DCC_PROXY_DATA_DIR", dir.path().toUtf8());
        const auto cert = dir.path() + "/cert.pem", key = dir.path() + "/key.pem";
        QProcess openssl; openssl.start("openssl", {"req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", key, "-out", cert, "-days", "1", "-subj", "/CN=localhost", "-addext", "subjectAltName=DNS:localhost,IP:127.0.0.1"});
        QVERIFY(openssl.waitForFinished()); QCOMPARE(openssl.exitCode(), 0);
        TestTlsServer server; server.certificate = QSslCertificate(readFile(cert)); server.key = QSslKey(readFile(key), QSsl::Rsa);
        QVERIFY(!server.certificate.isNull()); QVERIFY(!server.key.isNull());
        const auto previousSsl = QSslConfiguration::defaultConfiguration(); auto ssl = previousSsl; ssl.setProtocol(QSsl::TlsV1_2); auto ca = ssl.caCertificates(); ca.append(server.certificate); ssl.setCaCertificates(ca); QSslConfiguration::setDefaultConfiguration(ssl);
        QVERIFY(server.listen(QHostAddress::LocalHost));
        QByteArray saved; bool authorized = false;
        connect(&server, &QTcpServer::newConnection, this, [&] {
            while (auto *socket = server.nextPendingConnection()) {
                auto buffer = QSharedPointer<QByteArray>::create(); connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, socket, [&, socket, buffer] {
                    buffer->append(socket->readAll()); const int end = buffer->indexOf("\r\n\r\n"); if (end < 0) return;
                    const auto match = QRegularExpression("Content-Length: (\\d+)", QRegularExpression::CaseInsensitiveOption).match(QString::fromUtf8(buffer->left(end)));
                    if (match.hasMatch() && buffer->size() < end + 4 + match.captured(1).toInt()) return;
                    authorized = buffer->contains("Authorization: Basic " + QByteArray("fixture:password").toBase64());
                    if (buffer->startsWith("PUT ")) saved = buffer->mid(end + 4);
                    const auto body = buffer->startsWith("GET ") ? saved : QByteArray();
                    socket->write("HTTP/1.1 200 OK\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body); socket->disconnectFromHost();
                });
            }
        });
        Store store; const auto id = store.saveProfile("WebDAV fixture", {}, "rules: [\"MATCH,DIRECT\"]\n");
        {
            Panel panel; panel.show(); panel.openPage("Settings");
            for (auto *manager : panel.findChildren<QNetworkAccessManager *>())
                connect(manager, &QNetworkAccessManager::sslErrors, this, [](QNetworkReply *, const QList<QSslError> &errors) { for (const auto &error : errors) qWarning() << "Test HTTPS client:" << error.errorString(); });
            panel.findChild<QLineEdit *>("webdav-url")->setText(QString("https://localhost:%1/backup.json").arg(server.serverPort()));
            panel.findChild<QLineEdit *>("webdav-user")->setText("fixture"); panel.findChild<QLineEdit *>("webdav-password")->setText("password");
            auto click = [&](const QString &text) { for (auto *b : panel.findChildren<QPushButton *>()) if (b->text() == text) { QTest::mouseClick(b, Qt::LeftButton); return true; } return false; };
            QVERIFY(click("上传备份")); QTRY_VERIFY2(!saved.isEmpty(), qPrintable(panel.findChild<QLabel *>("proxy-status")->text())); QVERIFY(authorized); QVERIFY(QJsonDocument::fromJson(saved).isObject());
            store.removeProfile(id); QCOMPARE(store.profiles().size(), 0);
            QTimer::singleShot(20, this, [] { if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) box->button(QMessageBox::Yes)->click(); });
            QVERIFY(click("下载并恢复")); QTRY_VERIFY2(store.profiles().size() == 1, qPrintable(panel.findChild<QLabel *>("proxy-status")->text())); QCOMPARE(store.profiles().first().toObject()["name"].toString(), QString("WebDAV fixture"));
            panel.hide();
        }
        QSslConfiguration::setDefaultConfiguration(previousSsl);
        if (oldEnv.isNull()) qunsetenv("DCC_PROXY_DATA_DIR"); else qputenv("DCC_PROXY_DATA_DIR", oldEnv);
    }
    void helpExamplesPasswordAndResponsiveLayout()
    {
        QTemporaryDir dir; const auto oldEnv = qgetenv("DCC_PROXY_DATA_DIR"); qputenv("DCC_PROXY_DATA_DIR", dir.path().toUtf8());
        Store store; const auto id = store.saveProfile("Examples", {}, "rules: [\"MATCH,DIRECT\"]\n");
        auto settings = store.settings(); settings["active"] = id; store.setSettings(settings);
        const auto original = store.profile(id);
        {
            Panel panel; panel.resize(800, 700); panel.show();
            auto click = [&](const QString &text) { for (auto *b : panel.findChildren<QPushButton *>()) if (b->text() == text) { QTest::mouseClick(b, Qt::LeftButton); return true; } return false; };
            for (const auto &section : QStringList{"dns", "sniffer", "hosts", "listeners", "tunnels"}) {
                QString help; QByteArray example;
                QTimer::singleShot(15, this, [&] {
                    auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
                    if (!dialog) return;
                    for (auto *button : dialog->findChildren<QPushButton *>()) if (button->text() == "高级 YAML 编辑") { button->click(); break; }
                });
                QTimer::singleShot(45, this, [&] {
                    auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
                    if (!dialog) return;
                    if (auto *label = dialog->findChild<QLabel *>("config-editor-help")) help = label->text();
                    if (auto *edit = dialog->findChild<QPlainTextEdit *>("config-document-editor")) example = edit->toPlainText().toUtf8();
                    dialog->reject();
                });
                QVERIFY(click(section.toUpper())); QVERIFY2(!help.isEmpty(), qPrintable(section)); QVERIFY(!example.isEmpty());
                QVERIFY(parseConfig(example)[section.toStdString()]); QCOMPARE(store.profile(id), original);
                const auto exampleFile = dir.path() + "/example.yaml";
                auto config = parseConfig(example); config["mixed-port"] = 16666; config["rules"].push_back("MATCH,DIRECT");
                writeFile(exampleFile, emitYaml(config));
                if (!testCore().isEmpty()) {
                    QProcess validator; validator.start(testCore(), {"-t", "-d", dir.path(), "-f", exampleFile});
                    QVERIFY(validator.waitForFinished(10000)); QVERIFY2(validator.exitCode() == 0, validator.readAllStandardOutput().constData());
                }
            }
            panel.openPage("Overrides");
            auto *yaml = panel.findChild<QPlainTextEdit *>("yaml-override-editor"); QVERIFY(yaml); QVERIFY(!yaml->toPlainText().trimmed().isEmpty());
            QVERIFY(panel.findChild<QLabel *>("yaml-override-help")->text().contains("列表整体替换"));
            QVERIFY(parseConfig(yaml->toPlainText().toUtf8())["hosts"]);
            QVERIFY(click("保存覆写")); QTRY_VERIFY(QFile::exists(dir.path() + "/override.yaml")); QVERIFY(parseConfig(readFile(dir.path() + "/override.yaml"))["hosts"]);
            panel.openPage("Settings"); auto *secret = panel.findChild<Dtk::Widget::DPasswordEdit *>("api-secret-field"); QVERIFY(secret);
            QVERIFY(secret->echoButtonIsVisible()); QCOMPARE(secret->echoMode(), QLineEdit::Password);
            QAbstractButton *eye = nullptr;
            for (auto *button : secret->findChildren<QAbstractButton *>()) {
                if (!button->inherits("QLineEditIconButton") && button->isVisible()) eye = button;
            }
            QVERIFY(eye);
            QTest::mouseClick(eye, Qt::LeftButton); QCOMPARE(secret->echoMode(), QLineEdit::Normal);
            QTest::mouseClick(eye, Qt::LeftButton); QCOMPARE(secret->echoMode(), QLineEdit::Password);
            auto *status = panel.findChild<QLabel *>("proxy-status"); status->setText(QString(5000, QChar('x')) + QString("\n长错误说明").repeated(100));
            panel.findChild<QScrollArea *>("proxy-message-area")->show();
            panel.resize(460, 520); QTest::qWait(50);
            auto *area = panel.findChild<QScrollArea *>("proxy-message-area"); auto *tabs = panel.findChild<QTabWidget *>(); QVERIFY(area && tabs);
            QVERIFY(!area->autoFillBackground()); QVERIFY(!area->viewport()->autoFillBackground()); QVERIFY(!status->autoFillBackground());
            QVERIFY2(area->height() <= status->fontMetrics().lineSpacing() * 3 + 8, qPrintable(QString::number(area->height())));
            QVERIFY(tabs->height() >= 250);
            QCOMPARE(panel.width(), 460);
            secret->setEchoMode(QLineEdit::Normal); panel.hide(); QCOMPARE(secret->echoMode(), QLineEdit::Password);
        }
        if (oldEnv.isNull()) qunsetenv("DCC_PROXY_DATA_DIR"); else qputenv("DCC_PROXY_DATA_DIR", oldEnv);
    }
    void logHistoryAppearsWithoutNewConnections()
    {
        QTemporaryDir dir; const auto oldEnv = qgetenv("DCC_PROXY_DATA_DIR"); qputenv("DCC_PROXY_DATA_DIR", dir.path().toUtf8());
        writeFile(dir.path() + "/daemon.log", "time=fixture level=info msg=\"core started\"\n");
        {
            Panel panel; panel.show(); panel.openPage("Logs");
            auto *logs = panel.findChild<QPlainTextEdit *>("core-logs"); QVERIFY(logs); QVERIFY(logs->toPlainText().contains("core started"));
            logs->clear(); panel.openPage("Overview"); panel.openPage("Logs"); QVERIFY(logs->toPlainText().isEmpty());
            panel.hide();
        }
        if (oldEnv.isNull()) qunsetenv("DCC_PROXY_DATA_DIR"); else qputenv("DCC_PROXY_DATA_DIR", oldEnv);
    }
    void realCoreLogsAndTraffic()
    {
        if (testCore().isEmpty()) QSKIP("Optional real-core test requires a bundled or installed core");
        QTemporaryDir dir; const auto oldEnv = qgetenv("DCC_PROXY_DATA_DIR"); qputenv("DCC_PROXY_DATA_DIR", dir.path().toUtf8());
        QTcpServer controllerProbe, mixedProbe, target; QVERIFY(controllerProbe.listen(QHostAddress::LocalHost)); QVERIFY(mixedProbe.listen(QHostAddress::LocalHost)); QVERIFY(target.listen(QHostAddress::LocalHost));
        const int controller = controllerProbe.serverPort(), mixed = mixedProbe.serverPort(); controllerProbe.close(); mixedProbe.close();
        Store store; const auto id = store.saveProfile("Real logs", {}, QString("mixed-port: %1\nlog-level: info\nrules: [\"MATCH,DIRECT\"]\n").arg(mixed).toUtf8());
        auto settings = store.settings(); settings["active"] = id; settings["endpoint"] = QString("http://127.0.0.1:%1").arg(controller); settings["core"] = testCore(); settings["autoSystemProxy"] = false; settings["updateMinutes"] = 0; store.setSettings(settings);
        QProcess daemon;
        const auto cleanup = QSharedPointer<int>(new int, [&](int *value) { daemon.terminate(); daemon.waitForFinished(5000); if (oldEnv.isNull()) qunsetenv("DCC_PROXY_DATA_DIR"); else qputenv("DCC_PROXY_DATA_DIR", oldEnv); delete value; });
        daemon.start(serviceExecutable(), {"--daemon"}); QVERIFY(daemon.waitForStarted()); QTRY_VERIFY(QFile::exists(dir.path() + "/service.sock"));
        QProcess starter; starter.start(serviceExecutable(), {"--command", "start"}); QVERIFY(starter.waitForFinished(20000));
        const auto result = QJsonDocument::fromJson(starter.readAllStandardOutput()).object(); QVERIFY2(result["running"].toBool() && result["error"].toString().isEmpty(), qPrintable(QString::fromUtf8(QJsonDocument(result).toJson())));
        connect(&target, &QTcpServer::newConnection, this, [&] { while (auto *socket = target.nextPendingConnection()) {
            connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            connect(socket, &QTcpSocket::readyRead, socket, [socket] { socket->readAll(); socket->write("HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok"); socket->disconnectFromHost(); });
        }});
        Panel panel; panel.show(); panel.openPage("Logs");
        auto *logs = panel.findChild<QPlainTextEdit *>("core-logs"); QVERIFY(logs);
        QTRY_VERIFY(logs->toPlainText().contains("Initial configuration") || logs->toPlainText().contains("Mixed(http+socks) proxy listening"));
        // Open the true Mihomo HTTP log stream before producing proxy traffic.
        QTest::qWait(300); const auto previous = logs->toPlainText();
        QNetworkAccessManager client; client.setProxy(QNetworkProxy(QNetworkProxy::HttpProxy, "127.0.0.1", mixed));
        auto *reply = client.get(QNetworkRequest(QUrl(QString("http://127.0.0.1:%1/log-fixture").arg(target.serverPort()))));
        QTRY_VERIFY(reply->isFinished()); QCOMPARE(reply->error(), QNetworkReply::NoError); QCOMPARE(reply->readAll(), QByteArray("ok")); reply->deleteLater();
        QTRY_VERIFY(logs->toPlainText() != previous); QTRY_VERIFY(logs->toPlainText().contains("DIRECT"));
        panel.findChild<QComboBox *>("log-level")->setCurrentText("debug");
        QTRY_VERIFY(QFile::exists(dir.path() + "/controlled.yaml"));
        QTRY_COMPARE(parseConfig(readFile(dir.path() + "/controlled.yaml"))["log-level"].as<std::string>(), std::string("debug"));
        panel.hide();
    }
    void clashTunDefaultsAndRuntime()
    {
        QTemporaryDir dir; Store store(dir.path());
        const auto id = store.saveProfile("TUN fixture", {}, "mixed-port: 16666\ntun: {enable: true, stack: system}\ndns: {enable: false}\nrules: [\"MATCH,DIRECT\"]\n");
        auto config = parseConfig(store.runtime(id));
        QVERIFY(!config["tun"]["enable"].as<bool>());
        QCOMPARE(config["tun"]["stack"].as<std::string>(), std::string("mixed"));
        QCOMPARE(config["tun"]["device"].as<std::string>(), std::string("Mihomo"));
        QVERIFY(config["tun"]["dns-hijack"].size() == 0);
        writeFile(dir.path() + "/controlled.yaml", "tun: {enable: true, mtu: 1400}\n");
        config = parseConfig(store.runtime(id));
        QVERIFY(config["tun"]["enable"].as<bool>());
        QVERIFY(config["dns"]["enable"].as<bool>());
        QCOMPARE(config["tun"]["mtu"].as<int>(), 1400);
        QVERIFY(config["tun"]["auto-route"].as<bool>());
        QVERIFY(config["tun"]["auto-detect-interface"].as<bool>());
        QVERIFY(!config["tun"]["auto-redirect"].as<bool>());
        QCOMPARE(config["tun"]["dns-hijack"][0].as<std::string>(), std::string("any:53"));
        QCOMPARE(config["dns"]["proxy-server-nameserver"][0].as<std::string>(), std::string("system"));
        for (const auto &options : QList<QJsonObject>{{{"mtu", 0}}, {{"mtu", 65536}}, {{"mtu", 1.5}}, {{"mtu", 1e30}}, {{"device", "bad/name"}}, {{"stack", "unknown"}}, {{"route-exclude-address", QJsonArray{"example.com"}}}, {{"enable", "true"}}})
            QVERIFY_EXCEPTION_THROWN(validateTunOptions(options), std::runtime_error);
        const auto validated = validateTunOptions({{"route-exclude-address", QJsonArray{"192.168.7.9/16", "fc00::/7"}}});
        QCOMPARE(validated["route-exclude-address"].toArray().first().toString(), QString("192.168.0.0/16"));
        QVERIFY(!validated.contains("enable"));
    }
    void overviewStableAndNativeSwitch()
    {
        QTemporaryDir dir; const auto oldEnv = qgetenv("DCC_PROXY_DATA_DIR"); qputenv("DCC_PROXY_DATA_DIR", dir.path().toUtf8());
        QTcpServer http; QVERIFY(http.listen(QHostAddress::LocalHost));
        QLocalServer ipc; QVERIFY(ipc.listen(dir.path() + "/service.sock"));
        bool running = true, failStart = false; int starts = 0, stops = 0;
        connect(&ipc, &QLocalServer::newConnection, this, [&] {
            while (auto *socket = ipc.nextPendingConnection()) {
                auto buffer = QSharedPointer<QByteArray>::create(); connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QLocalSocket::readyRead, socket, [&, socket, buffer] {
                    buffer->append(socket->readAll()); if (!buffer->contains('\n')) return;
                    const auto action = QJsonDocument::fromJson(*buffer).object()["action"].toString();
                    QJsonObject state;
                    if (action == "start") { ++starts; if (failStart) state["error"] = "fixture start failed"; else running = true; }
                    if (action == "stop") { ++stops; running = false; }
                    state["running"] = running; state["tun"] = QJsonObject{{"enable", false}};
                    QTimer::singleShot(action == "status" ? 0 : 100, socket, [socket, state] { socket->write(QJsonDocument(state).toJson(QJsonDocument::Compact) + '\n'); socket->flush(); socket->disconnectFromServer(); });
                });
            }
        });
        int connections = 0;
        connect(&http, &QTcpServer::newConnection, this, [&] {
            while (auto *socket = http.nextPendingConnection()) {
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, socket, [&, socket] {
                    const auto request = socket->readAll(); QByteArray body = "{}"; int delay = 0;
                    if (request.startsWith("GET /configs ")) body = "{\"mode\":\"rule\",\"mixed-port\":16666}";
                    if (request.startsWith("GET /connections ")) { ++connections; delay = 160; body = "{\"connections\":[{\"id\":\"fixture\"}],\"uploadTotal\":1024,\"downloadTotal\":2048,\"memory\":4096}"; }
                    QTimer::singleShot(delay, socket, [socket, body] { socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body); socket->disconnectFromHost(); });
                });
            }
        });
        Store store; auto settings = store.settings(); settings["endpoint"] = QString("http://127.0.0.1:%1").arg(http.serverPort()); store.setSettings(settings);
        {
            Panel panel; panel.resize(900, 700); panel.show();
            auto *restart = panel.findChild<QPushButton *>("restart-proxy-service"); QVERIFY(restart);
            QCOMPARE(restart->text(), QString("重启服务")); QVERIFY(restart->toolTip().contains("后台")); QVERIFY(restart->toolTip().contains("中断"));
            auto *control = panel.findChild<Dtk::Widget::DSwitchButton *>("core-switch"); QVERIFY(control);
            auto *core = panel.findChild<QLabel *>("overview-core"); auto *stats = panel.findChild<QLabel *>("overview-statistics"); QVERIFY(core); QVERIFY(stats);
            QTRY_VERIFY(core->text().contains("16666")); QTRY_VERIFY(stats->text().contains("连接 1")); QTRY_VERIFY(control->isChecked());
            const auto coreText = core->text(); const auto statsText = stats->text(); const auto statsPosition = stats->pos();
            auto *refresh = panel.findChild<QPushButton *>("refresh-proxy-panel"); QVERIFY(refresh);
            for (int i = 0; i < 3; ++i) {
                QTest::mouseClick(refresh, Qt::LeftButton); QTest::qWait(40);
                QCOMPARE(core->text(), coreText); QCOMPARE(stats->text(), statsText); QCOMPARE(stats->pos(), statsPosition);
                QTest::qWait(180); QCOMPARE(core->text(), coreText); QCOMPARE(stats->text(), statsText); QCOMPARE(stats->pos(), statsPosition);
            }
            QVERIFY(connections >= 4); QCOMPARE(panel.findChildren<QTableWidget *>().at(1)->rowCount(), 0);
            QTest::mouseClick(control, Qt::LeftButton); QVERIFY(!control->isEnabled()); QTRY_COMPARE(stops, 1); QTRY_VERIFY(control->isEnabled()); QVERIFY(!control->isChecked());
            failStart = true; QTest::mouseClick(control, Qt::LeftButton); QTRY_COMPARE(starts, 1); QTRY_VERIFY(control->isEnabled()); QVERIFY(!control->isChecked());
            QTRY_VERIFY(panel.findChild<QLabel *>("proxy-status")->text().contains("fixture start failed"));
            failStart = false; QTest::mouseClick(control, Qt::LeftButton); QTRY_COMPARE(starts, 2); QTRY_VERIFY(control->isEnabled()); QVERIFY(control->isChecked());
            auto *bypass = panel.findChild<QPlainTextEdit *>("proxy-bypass"); auto *save = panel.findChild<QPushButton *>("save-proxy-bypass"); QVERIFY(bypass); QVERIFY(save);
            const auto userInput = QString("ai.uniontech.com\nai.getdeepin.org\n*.uniontech.*\ndify.cicd.getdeepin.org\nagent-dev.uniontech.com\ndocs.qq.com");
            bypass->setPlainText(userInput); QVERIFY(!save->isEnabled()); QCOMPARE(bypass->toPlainText(), userInput);
            QVERIFY(panel.findChild<QLabel *>("proxy-bypass-error")->text().contains("*.uniontech.*"));
            bypass->setPlainText(QString(userInput).replace("*.uniontech.*", "*.uniontech.com")); QVERIFY(save->isEnabled());
            panel.hide();
        }
        if (oldEnv.isNull()) qunsetenv("DCC_PROXY_DATA_DIR"); else qputenv("DCC_PROXY_DATA_DIR", oldEnv);
    }
    void bypassValidation()
    {
        QCOMPARE(normalizeProxyBypass({" LOCALHOST ", "localhost", ".Example.COM", "*.example.com", "[::1]", "192.168.7.9/16", ""}),
                 QStringList({"localhost", "*.example.com", "::1", "192.168.0.0/16"}));
        QVERIFY(normalizeProxyBypass({}).isEmpty());
        QCOMPARE(normalizeProxyBypass({"例子.测试"}).first(), QString("xn--fsqu00a.xn--0zwm56d"));
        for (const auto &invalid : {"https://example.com", "192.168.0.0/99", "*.bad*.com", "a';return'DIRECT", "<local>", "a b.com"})
            QVERIFY_EXCEPTION_THROWN(normalizeProxyBypass({invalid}), std::runtime_error);
        QVERIFY_EXCEPTION_THROWN(normalizeProxyBypass(QString(513, QChar('x')).split('x')), std::runtime_error);
        QVERIFY_EXCEPTION_THROWN(proxyPac(0, {}), std::runtime_error);
    }
    void bypassPacRoutes()
    {
        QJSEngine js;
        auto source = proxyPac(16666, defaultProxyBypass() + QStringList{"*.example.com", "exact.test", "fc00::/7", "2001:db8::/32"});
        QVERIFY(!js.evaluate(QString::fromUtf8(source)).isError());
        js.evaluate("function dnsResolve(host) { return host==='intranet.test' ? '10.20.8.220' : '203.0.113.8'; }");
        auto route = [&](const QString &host) {
            return js.globalObject().property("FindProxyForURL").call({"https://" + host + "/", host}).toString();
        };
        for (const auto &host : {"localhost", "127.99.1.2", "192.168.8.1", "10.20.8.220", "172.16.0.1", "172.31.255.255", "[::1]", "fc00::1", "fdff:ffff::99", "2001:db8:1234::1", "x.EXAMPLE.com.", "exact.test", "intranet.test"})
            QCOMPARE(route(host), QString("DIRECT"));
        for (const auto &host : {"172.32.0.1", "192.169.0.1", "example.com", "badexample.com", "exact.test.evil", "2001:db9::1", "fe80::1", "public.test"})
            QVERIFY2(route(host).startsWith("PROXY 127.0.0.1:16666;"), host);
        js.evaluate("function dnsResolveEx(host) { return '2001:db8::7; 203.0.113.8'; }");
        QCOMPARE(route("ipv6-intranet.test"), QString("DIRECT"));
        QVERIFY(!js.evaluate(QString::fromUtf8(proxyPac(16666, {}))).isError());
        QVERIFY(route("localhost").startsWith("PROXY")); // Explicit empty list disables bypass.
        QVERIFY(route("10.20.8.220").startsWith("PROXY"));
    }
    void bypassSettingsPersist()
    {
        QTemporaryDir dir;
        const auto oldEnv = qgetenv("DCC_PROXY_DATA_DIR"); qputenv("DCC_PROXY_DATA_DIR", dir.path().toUtf8());
        {
            Store store;
            Panel panel;
            auto *edit = panel.findChild<QPlainTextEdit *>("proxy-bypass"); QVERIFY(edit);
            QVERIFY(panel.findChild<QCheckBox *>("auto-system-proxy")->isChecked());
            QCOMPARE(edit->toPlainText(), defaultProxyBypass().join('\n'));
            auto settings = store.settings(); settings["autoSystemProxy"] = false; settings["proxyBypass"] = QJsonArray{"*.example.com", "10.0.0.0/8"}; store.setSettings(settings);
            Panel restored;
            QVERIFY(!restored.findChild<QCheckBox *>("auto-system-proxy")->isChecked());
            QCOMPARE(restored.findChild<QPlainTextEdit *>("proxy-bypass")->toPlainText(), QString("*.example.com\n10.0.0.0/8"));
            settings["proxyBypass"] = QJsonArray(); store.setSettings(settings);
            Panel empty;
            QVERIFY(empty.findChild<QPlainTextEdit *>("proxy-bypass")->toPlainText().isEmpty());
        }
        if (oldEnv.isNull()) qunsetenv("DCC_PROXY_DATA_DIR"); else qputenv("DCC_PROXY_DATA_DIR", oldEnv);
    }
    void pluginPageSurvivesDeferredDeletion()
    {
        QTemporaryDir dir;
        const auto oldEnv = qgetenv("DCC_PROXY_DATA_DIR");
        qputenv("DCC_PROXY_DATA_DIR", dir.path().toUtf8());
        TestFrame frame; frame.host.resize(900, 650); frame.host.show();
        ProxyPlugin plugin; plugin.setFrameProxy(&frame);
        plugin.active();
        QPointer<QWidget> retired = frame.page;
        QVERIFY(retired);
        plugin.load("Proxies"); QCOMPARE(frame.page.data(), retired.data()); QCOMPARE(frame.pushes, 2);
        frame.popWidget(&plugin);
        QVERIFY(retired); // deleteLater has deliberately not run yet.
        QCOMPARE(plugin.load("Proxies"), 0);
        QPointer<QWidget> current = frame.page;
        QVERIFY(current); QVERIFY(current != retired);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QVERIFY(retired.isNull()); QVERIFY(current); QVERIFY(current->isVisible());
        QCOMPARE(current->findChild<QTabWidget *>()->currentIndex(), 1);
        frame.popWidget(&plugin);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        if (oldEnv.isNull()) qunsetenv("DCC_PROXY_DATA_DIR"); else qputenv("DCC_PROXY_DATA_DIR", oldEnv);
    }
    void nodeFlags()
    {
        QCOMPARE(nodeCountryCode(QString::fromUtf8("🇯🇵 日本 01")), QString("jp"));
        QCOMPARE(nodeCountryCode(QString::fromUtf8("🇭🇰 Japan")), QString("hk"));
        QCOMPARE(nodeCountryCode("香港 IPLC 01"), QString("hk"));
        QCOMPARE(nodeCountryCode("臺灣 02"), QString("tw"));
        QCOMPARE(nodeCountryCode("US01"), QString("us"));
        QCOMPARE(nodeCountryCode("[uk] London"), QString("gb"));
        QCOMPARE(nodeCountryCode("South Korea"), QString("kr"));
        QCOMPARE(nodeCountryCode("印度尼西亚 01"), QString("id"));
        QCOMPARE(nodeCountryCode("Australia"), QString("au"));
        QVERIFY(nodeCountryCode("自动选择").isEmpty());
        QVERIFY(nodeCountryCode("business").isEmpty());
        QCOMPARE(nodeDisplayName(QString::fromUtf8("🇺🇸 美国 01")), QString("美国 01"));
        const QStringList names{"香港", "日本", "新加坡", "美国", "德国", "巴西", "未知节点"};
        for (const auto &name : names) QVERIFY2(!QIcon(nodeFlagPath(name)).pixmap(28, 21).isNull(), qPrintable(name));
        // Every bundled country icon must render with the target Qt SVG implementation.
        QDir flags(":/dcc-proxy/flags");
        QVERIFY(flags.entryList({"*.svg"}, QDir::Files).size() > 240);
        for (const auto &file : flags.entryList({"*.svg"}, QDir::Files))
            QVERIFY2(!QIcon(flags.filePath(file)).pixmap(28, 21).isNull(), qPrintable(file));
    }
    void profileRoundTrip()
    {
        QTemporaryDir dir; Store store(dir.path());
        const auto id = store.saveProfile("日本", "https://example.com/sub", "mixed-port: 16666\nproxies: []\nrules: [\"MATCH,DIRECT\"]\n");
        QVERIFY(validId(id)); QCOMPARE(store.profiles().size(), 1);
        QVERIFY(store.profile(id).contains("16666"));
        store.saveProfile("新名称", {}, "mode: direct\n", id);
        QCOMPARE(store.profiles().size(), 1); QCOMPARE(store.profiles()[0].toObject()["name"].toString(), QString("新名称"));
        QVERIFY_EXCEPTION_THROWN(store.profile("../../etc/passwd"), std::runtime_error);
        auto s = store.settings(); s["active"] = id; store.setSettings(s);
        QVERIFY_EXCEPTION_THROWN(store.removeProfile(id), std::runtime_error);
        s["active"] = ""; store.setSettings(s); store.removeProfile(id); QCOMPARE(store.profiles().size(), 0);
    }
    void yamlMergeAndSafety()
    {
        auto result = mergeYaml(parseConfig("dns: {enable: true, nameserver: [a]}\nremove: true\n"), parseConfig("dns: {nameserver: [b]}\nremove: null\n"));
        QVERIFY(result["dns"]["enable"].as<bool>()); QCOMPARE(result["dns"]["nameserver"][0].as<std::string>(), std::string("b")); QVERIFY(!result["remove"]);
        QVERIFY_EXCEPTION_THROWN(parseConfig("a: 1\na: 2\n"), std::runtime_error);
        QVERIFY_EXCEPTION_THROWN(parseConfig("a: &a {b: *a}\n"), std::runtime_error);
        QVERIFY_EXCEPTION_THROWN(validateConfig(parseConfig("rule-providers: {bad: {type: exec, command: id}}\n")), std::runtime_error);
        QVERIFY_EXCEPTION_THROWN(parseConfig("a: 1\n---\nb: 2"), std::runtime_error);
    }
    void uriSubscriptions()
    {
        const QByteArray uri = "hysteria2://test%3Apassword@example.com:443?sni=example.net&insecure=1&obfs=salamander&obfs-password=sample#%E6%97%A5%E6%9C%AC\n";
        const auto a = parseConfig(normalizeSubscription(uri));
        const auto b = parseConfig(normalizeSubscription(uri.toBase64()));
        QCOMPARE(a["proxies"].size(), size_t(1)); QCOMPARE(b["proxies"].size(), size_t(1));
        QCOMPARE(b["proxies"][0]["type"].as<std::string>(), std::string("hysteria2"));
        QCOMPARE(b["proxies"][0]["password"].as<std::string>(), std::string("test:password"));
        QCOMPARE(b["proxies"][0]["name"].as<std::string>(), std::string("日本"));
        QVERIFY(b["proxies"][0]["skip-cert-verify"].as<bool>());
        QCOMPARE(b["proxy-groups"].size(), size_t(2));
        QVERIFY_EXCEPTION_THROWN(normalizeSubscription("<html>not a subscription</html>"), std::runtime_error);
    }
    void anytlsSubscriptions()
    {
        QByteArray mixed;
        for (int i = 0; i < 8; ++i) mixed += "hy2://sample@example.com:443#HY2-" + QByteArray::number(i) + '\n';
        mixed += "anytls://pa%3Ass%40word%2F%2B@example.com:8443/?sni=tls.example.net&insecure=1&alpn=h2,http%2F1.1&fp=chrome#%E6%97%A5%E6%9C%AC\n";
        for (const auto &bytes : {mixed, mixed.toBase64(), mixed.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals)}) {
            QTemporaryDir dir; Store store(dir.path());
            const auto id = store.saveProfile("Mixed subscription", {}, bytes);
            const auto yaml = store.profile(id);
            const auto config = parseConfig(yaml);
            QCOMPARE(config["proxies"].size(), size_t(9));
            for (int i = 0; i < 8; ++i) QCOMPARE(config["proxies"][i]["type"].as<std::string>(), std::string("hysteria2"));
            const auto node = config["proxies"][8];
            QCOMPARE(node["type"].as<std::string>(), std::string("anytls"));
            QCOMPARE(node["server"].as<std::string>(), std::string("example.com"));
            QCOMPARE(node["port"].as<int>(), 8443);
            QCOMPARE(node["password"].as<std::string>(), std::string("pa:ss@word/+"));
            QCOMPARE(node["name"].as<std::string>(), std::string("日本"));
            QCOMPARE(node["sni"].as<std::string>(), std::string("tls.example.net"));
            QCOMPARE(node["client-fingerprint"].as<std::string>(), std::string("chrome"));
            QVERIFY(node["skip-cert-verify"].as<bool>()); QVERIFY(node["udp"].as<bool>());
            QCOMPARE(node["alpn"][0].as<std::string>(), std::string("h2"));
            QCOMPARE(node["alpn"][1].as<std::string>(), std::string("http/1.1"));
            QCOMPARE(config["proxy-groups"][1]["proxies"].size(), size_t(9));
            QCOMPARE(subscriptionNodes(yaml).proxies["日本"].toObject()["type"].toString(), QString("AnyTLS"));
            QVERIFY(!subscriptionNodes(yaml).proxies["日本"].toObject().contains("password"));
            if (!testCore().isEmpty()) {
                const auto path = dir.path() + "/imported.yaml"; writeFile(path, yaml);
                QProcess validator; validator.start(testCore(), {"-t", "-d", dir.path(), "-f", path});
                QVERIFY(validator.waitForFinished(10000));
                QCOMPARE(validator.exitStatus(), QProcess::NormalExit); QCOMPARE(validator.exitCode(), 0);
            }
        }
        const auto defaults = parseConfig(normalizeSubscription("anytls://sample@[2001:db8::1]/?insecure=0#IPv6"))["proxies"][0];
        QCOMPARE(defaults["server"].as<std::string>(), std::string("2001:db8::1"));
        QCOMPARE(defaults["port"].as<int>(), 443);
        QCOMPARE(defaults["password"].as<std::string>(), std::string("sample"));
        QVERIFY(!defaults["skip-cert-verify"]); QVERIFY(!defaults["sni"]);
    }
    void groupRoutesAndWebsiteRules()
    {
        const QJsonObject proxies{{"GLOBAL", QJsonObject{{"all", QJsonArray{"AI", "主选择", "隐藏"}}, {"hidden", true}}},
            {"AI", QJsonObject{{"all", QJsonArray{"主选择", "日本03"}}, {"now", "主选择"}, {"type", "Selector"}}},
            {"主选择", QJsonObject{{"all", QJsonArray{"日本03"}}, {"now", "日本03"}, {"type", "Selector"}}},
            {"日本03", QJsonObject{{"type", "Hysteria2"}}}, {"隐藏", QJsonObject{{"all", QJsonArray{"DIRECT"}}, {"hidden", true}}}};
        QCOMPARE(proxyGroupNames(proxies), QStringList({"AI", "主选择"}));
        QCOMPARE(proxyGroupRoute(proxies, "AI"), QString("AI → 主选择 → 日本03"));
        auto cycle = proxies; auto group = cycle["主选择"].toObject(); group["now"] = "AI"; cycle["主选择"] = group;
        QVERIFY(proxyGroupRoute(cycle, "AI").endsWith("循环引用"));
        group["type"] = "LoadBalance"; cycle["主选择"] = group; QVERIFY(proxyGroupRoute(cycle, "主选择").contains("负载均衡"));
        QCOMPARE(websiteRule("https://Example.COM/path?q=1#x", "AI"), QString("DOMAIN-SUFFIX,example.com,AI"));
        QCOMPARE(websiteRule("*.example.com", "主选择"), QString("DOMAIN-SUFFIX,example.com,主选择"));
        QCOMPARE(websiteRule("http://192.0.2.1:8080/p", "DIRECT"), QString("IP-CIDR,192.0.2.1/32,DIRECT,no-resolve"));
        QCOMPARE(websiteRule("https://[2001:db8::1]/", "AI"), QString("IP-CIDR6,2001:db8::1/128,AI,no-resolve"));
        QVERIFY(websiteRule("https://例子.测试/path", "AI").startsWith("DOMAIN-SUFFIX,xn--"));
        for (const auto &invalid : {"", "a,b,DIRECT", "ftp://example.com", "https://user:password@example.com", "https://*.example.com", "-bad.example"})
            QVERIFY_EXCEPTION_THROWN(websiteRule(invalid, "AI"), std::runtime_error);
        QVERIFY_EXCEPTION_THROWN(websiteRule("example.com", "bad,policy"), std::runtime_error);
        const auto yaml = prependWebsiteRule("proxy-groups: [{name: AI, type: select, proxies: [DIRECT]}]\nrules: [\"DOMAIN-SUFFIX,example.com,DIRECT\", \"DOMAIN-SUFFIX,other.com,DIRECT\", \"MATCH,AI\"]\n", websiteRule("example.com", "AI"));
        const auto config = parseConfig(yaml); QCOMPARE(config["rules"].size(), size_t(3));
        QCOMPARE(QString::fromStdString(config["rules"][0].as<std::string>()), QString("DOMAIN-SUFFIX,example.com,AI"));
        QCOMPARE(QString::fromStdString(config["rules"][2].as<std::string>()), QString("MATCH,AI"));
        QVERIFY(subscriptionNodes("proxy-groups: [{name: 隐藏, type: select, hidden: true, proxies: [DIRECT]}]\n").proxies["隐藏"].toObject()["hidden"].toBool());
    }
    void websiteRuleFormAndOverrideRollback()
    {
        QTemporaryDir dir; const auto oldEnv = qgetenv("DCC_PROXY_DATA_DIR"); qputenv("DCC_PROXY_DATA_DIR", dir.path().toUtf8());
        {
            Store store; const auto id = store.saveProfile("网址测试", {}, "proxy-groups: [{name: 主选择, type: select, proxies: [DIRECT]}]\nrules: [\"MATCH,主选择\"]\n");
            QTcpServer probe; QVERIFY(probe.listen(QHostAddress::LocalHost));
            auto settings = store.settings(); settings["endpoint"] = QString("http://127.0.0.1:%1").arg(probe.serverPort()); probe.close(); store.setSettings(settings);
            Panel panel; panel.resize(650, 700); panel.show(); panel.openPage("Rules");
            auto *url = panel.findChild<QLineEdit *>("website-rule-url"); auto *add = panel.findChild<QPushButton *>("add-website-rule");
            auto *preview = panel.findChild<QLabel *>("website-rule-preview"); QVERIFY(url); QVERIFY(add); QVERIFY(preview);
            url->setText("https://example.com/path"); QVERIFY(add->isEnabled()); QVERIFY(preview->text().contains("DOMAIN-SUFFIX,example.com,主选择"));
            const auto original = store.profile(id);
            QTest::mouseClick(add, Qt::LeftButton);
            QTRY_VERIFY(!store.customizations()["rules"].toObject()[id].toArray().isEmpty());
            QCOMPARE(store.profile(id), original);
            QCOMPARE(QString::fromStdString(parseConfig(store.runtime(id))["rules"][0].as<std::string>()), QString("DOMAIN-SUFFIX,example.com,主选择"));
            QTRY_VERIFY(url->text().isEmpty()); QVERIFY(!QFile::exists(dir.path()+"/service.sock"));
            const auto previous = store.profile(id); writeFile(dir.path()+"/override.yaml", "rules: [\"MATCH,DIRECT\"]\n");
            url->setText("other.example"); QTest::mouseClick(add, Qt::LeftButton);
            QTRY_COMPARE(store.customizations()["rules"].toObject()[id].toArray().size(), 2);
            QCOMPARE(store.profile(id), previous);
            QVERIFY(QString::fromStdString(parseConfig(store.runtime(id))["rules"][0].as<std::string>()).contains("other.example"));
            panel.hide();
        }
        if (oldEnv.isNull()) qunsetenv("DCC_PROXY_DATA_DIR"); else qputenv("DCC_PROXY_DATA_DIR", oldEnv);
    }
    void policyGroupsSwitchIndependently()
    {
        QTemporaryDir dir; const auto oldEnv = qgetenv("DCC_PROXY_DATA_DIR"); qputenv("DCC_PROXY_DATA_DIR", dir.path().toUtf8());
        QTcpServer server; QVERIFY(server.listen(QHostAddress::LocalHost)); Store store;
        auto settings = store.settings(); settings["endpoint"] = QString("http://127.0.0.1:%1").arg(server.serverPort()); store.setSettings(settings);
        QString selected = "主选择"; int switches = 0, delays = 0;
        connect(&server, &QTcpServer::newConnection, this, [&] {
            while (auto *socket = server.nextPendingConnection()) {
                auto buffer = QSharedPointer<QByteArray>::create(); connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, socket, [&, socket, buffer] {
                    buffer->append(socket->readAll()); const int end = buffer->indexOf("\r\n\r\n"); if (end < 0) return;
                    const auto length = QRegularExpression("Content-Length: (\\d+)", QRegularExpression::CaseInsensitiveOption).match(QString::fromUtf8(buffer->left(end)));
                    if (length.hasMatch() && buffer->size() < end + 4 + length.captured(1).toInt()) return;
                    QByteArray body = "{}"; int wait = 0;
                    if (buffer->startsWith("PUT /proxies/AI ")) { selected = QJsonDocument::fromJson(buffer->mid(end+4)).object()["name"].toString(); ++switches; }
                    if (buffer->startsWith("GET /configs ")) body = "{\"mode\":\"rule\"}";
                    if (buffer->startsWith("GET /proxies ")) {
                        QJsonObject proxies{{"GLOBAL", QJsonObject{{"all", QJsonArray{"AI", "主选择"}}, {"hidden", true}}},
                            {"AI", QJsonObject{{"type", "Selector"}, {"all", QJsonArray{"主选择", "美国"}}, {"now", selected}}},
                            {"主选择", QJsonObject{{"type", "Selector"}, {"all", QJsonArray{"日本03", "美国"}}, {"now", "日本03"}}},
                            {"日本03", QJsonObject{{"type", "Hysteria2"}}}, {"美国", QJsonObject{{"type", "Hysteria2"}}}};
                        body = QJsonDocument(QJsonObject{{"proxies", proxies}}).toJson(QJsonDocument::Compact);
                    }
                    if (buffer->contains("/delay?")) { ++delays; wait = 180; body = "{\"delay\":123}"; }
                    buffer->clear();
                    QTimer::singleShot(wait, socket, [socket, body] { socket->write("HTTP/1.1 200 OK\r\nContent-Length: "+QByteArray::number(body.size())+"\r\nConnection: close\r\n\r\n"+body); socket->disconnectFromHost(); });
                });
            }
        });
        {
            Panel panel; panel.resize(750, 800); panel.show(); panel.openPage("Proxies");
            QTRY_VERIFY(policyNodes(panel, "AI")); auto *nodes = policyNodes(panel, "AI"); QTRY_VERIFY(nodes->isVisible());
            QCOMPARE(nodes->count(), 2); auto *header = policyButton(panel, "proxy-group-header", "AI"); QVERIFY(header); QVERIFY(header->isChecked());
            bool route = false; for (auto *label : panel.findChildren<QLabel *>("proxy-group-route")) if (label->text() == "AI → 主选择 → 日本03") route = true; QVERIFY(route);
            const QPersistentModelIndex stable(nodes->model()->index(0, 0));
            QTest::mouseClick(nodes->viewport(), Qt::LeftButton, Qt::NoModifier, nodes->delayRect(nodes->item(1)).center());
            QVERIFY(nodes->item(1)->data(NodeTestingRole).toBool()); QTRY_COMPARE(delays, 1); QTRY_VERIFY(!nodes->item(1)->data(NodeTestingRole).toBool());
            QCOMPARE(selected, QString("主选择")); QCOMPARE(switches, 0);
            QTest::mouseClick(nodes->viewport(), Qt::LeftButton, Qt::NoModifier, nodes->visualItemRect(nodes->item(1)).topLeft()+QPoint(50, 24));
            QTRY_COMPARE(selected, QString("美国")); QTRY_VERIFY(nodes->item(1)->data(NodeActiveRole).toBool()); QCOMPARE(switches, 1);
            QVERIFY(stable.isValid());
            route = false; for (auto *label : panel.findChildren<QLabel *>("proxy-group-route")) if (label->text() == "主选择 → 日本03") route = true; QVERIFY(route);
            QTest::mouseClick(header, Qt::LeftButton); QVERIFY(!nodes->isVisible());
            QTest::mouseClick(panel.findChild<QPushButton *>("refresh-proxy-panel"), Qt::LeftButton); QTest::qWait(80);
            QVERIFY(stable.isValid()); QVERIFY(!header->isChecked()); QVERIFY(!nodes->isVisible()); panel.hide();
        }
        if (oldEnv.isNull()) qunsetenv("DCC_PROXY_DATA_DIR"); else qputenv("DCC_PROXY_DATA_DIR", oldEnv);
    }
    void subscriptionPreviewData()
    {
        const auto preview = subscriptionNodes("proxies:\n  - {name: 日本A, type: ss, server: private.example, password: hidden}\n  - {name: 香港B, type: trojan, password: hidden}\nproxy-providers:\n  inline: {type: inline, payload: [{name: 美国C, type: vless}]}\n  remote: {type: http, url: https://example.com/provider}\nproxy-groups:\n  - {name: 选择, type: select, include-all: true}\n  - {name: 香港, type: url-test, include-all: true, filter: 香港}\n");
        QCOMPARE(preview.remoteProviders, 1);
        QCOMPARE(preview.proxies["日本A"].toObject()["type"].toString(), QString("Shadowsocks"));
        QCOMPARE(preview.proxies["选择"].toObject()["all"].toArray().size(), 3);
        QCOMPARE(preview.proxies["香港"].toObject()["all"].toArray(), QJsonArray{"香港B"});
        QVERIFY(!preview.proxies["日本A"].toObject().contains("password"));
        QVERIFY(!preview.proxies["选择"].toObject().contains("now"));
        QCOMPARE(subscriptionNodes("proxies: [{name: direct-only, type: direct}]\n").remoteProviders, 0);
    }
    void offlineSubscriptionChoice()
    {
        QTemporaryDir dir; const auto oldEnv = qgetenv("DCC_PROXY_DATA_DIR"); qputenv("DCC_PROXY_DATA_DIR", dir.path().toUtf8());
        {
            Store store;
            const auto first = store.saveProfile("first", {}, "proxies: [{name: 日本A, type: ss}]\n");
            const auto second = store.saveProfile("second", {}, "proxies: [{name: 香港B, type: trojan}, {name: 美国C, type: vless}]\n");
            auto settings = store.settings(); QTcpServer probe; QVERIFY(probe.listen(QHostAddress::LocalHost));
            settings["endpoint"] = QString("http://127.0.0.1:%1").arg(probe.serverPort()); probe.close(); store.setSettings(settings);
            Panel panel; panel.resize(900, 700); panel.show(); panel.openPage("Proxies");
            auto *nodes = policyNodes(panel, "__dcc_subscription_nodes__"); auto *source = panel.findChild<QComboBox *>("group-subscription-source"); QVERIFY(nodes); QVERIFY(source);
            QTRY_COMPARE(nodes->count(), 1); QCOMPARE(nodes->item(0)->data(Qt::UserRole).toString(), QString("日本A"));
            QVERIFY(!nodes->item(0)->data(NodeActiveRole).toBool());
            QVERIFY(!panel.findChild<QPushButton *>("test-proxy-group")->isEnabled());
            source->setCurrentIndex(source->findData(second)); nodes = policyNodes(panel, "__dcc_subscription_nodes__"); QVERIFY(nodes); QTRY_COMPARE(nodes->count(), 2);
            const QPersistentModelIndex card(nodes->model()->index(0, 0)); QVERIFY(card.isValid());
            QTest::mouseClick(panel.findChild<QPushButton *>("refresh-proxy-panel"), Qt::LeftButton); QTest::qWait(150);
            QVERIFY(card.isValid()); QCOMPARE(nodes->count(), 2);
            QTest::mouseClick(nodes->viewport(), Qt::LeftButton, Qt::NoModifier, nodes->visualItemRect(nodes->item(0)).topLeft()+QPoint(50,24));
            QVERIFY(!nodes->item(0)->data(NodeActiveRole).toBool());
            QVERIFY(panel.findChild<QLabel *>("proxy-status")->text().contains("订阅预览"));
            QVERIFY(!QFile::exists(dir.path()+"/service.sock")); QVERIFY(store.settings()["active"].toString().isEmpty());
            source->setCurrentIndex(source->findData(first)); nodes = policyNodes(panel, "__dcc_subscription_nodes__"); QVERIFY(nodes); QTRY_COMPARE(nodes->count(), 1); panel.hide();
        }
        if (oldEnv.isNull()) qunsetenv("DCC_PROXY_DATA_DIR"); else qputenv("DCC_PROXY_DATA_DIR", oldEnv);
    }
    void subscriptionUrlImport()
    {
        QTemporaryDir dir;
        const auto oldEnv = qgetenv("DCC_PROXY_DATA_DIR");
        qputenv("DCC_PROXY_DATA_DIR", dir.path().toUtf8());
        QTcpServer server; QVERIFY(server.listen(QHostAddress::LocalHost));
        Store store;
        auto settings = store.settings();
        settings["endpoint"] = QString("http://127.0.0.1:%1").arg(server.serverPort());
        store.setSettings(settings);
        QByteArray subscriptionRequest;
        const QByteArray subscription = QByteArray("hy2://sample@example.com:443#%E6%97%A5%E6%9C%AC").toBase64();
        connect(&server, &QTcpServer::newConnection, this, [&] {
            while (auto *s = server.nextPendingConnection()) {
                auto buffer = QSharedPointer<QByteArray>::create();
                connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
                connect(s, &QTcpSocket::readyRead, s, [&, s, buffer] {
                    *buffer += s->readAll();
                    if (!buffer->contains("\r\n\r\n")) return;
                    const bool isSubscription = buffer->startsWith("GET /subscription ");
                    if (isSubscription) subscriptionRequest = *buffer;
                    const auto bytes = isSubscription ? subscription : QByteArray("{}");
                    s->write("HTTP/1.1 200 OK\r\nContent-Length: " + QByteArray::number(bytes.size()) +
                             "\r\nSubscription-Userinfo: upload=1; download=2; total=1024; expire=2000000000\r\n"
                             "Profile-Update-Interval: 12\r\nConnection: close\r\n\r\n" + bytes);
                    s->disconnectFromHost();
                });
            }
        });
        {
            Panel panel; panel.resize(900, 650); panel.show(); panel.openPage("Profiles");
            auto *url = panel.findChild<QLineEdit *>("subscription-url");
            auto *import = panel.findChild<QPushButton *>("import-subscription-url");
            auto *profiles = panel.findChild<QTableWidget *>("profiles");
            QVERIFY(url); QVERIFY(import); QVERIFY(profiles);
            const auto source = QString("http://127.0.0.1:%1/subscription").arg(server.serverPort());
            url->setText(source); QTest::mouseClick(import, Qt::LeftButton);
            QTRY_COMPARE(store.profiles().size(), 1);
            QTRY_COMPARE(profiles->rowCount(), 1);
            const auto profile = store.profiles()[0].toObject();
            QCOMPARE(profile["url"].toString(), source);
            QCOMPARE(profiles->item(0, 1)->text(), source);
            QCOMPARE(parseConfig(store.profile(profile["id"].toString()))["proxies"].size(), size_t(1));
            QVERIFY(subscriptionRequest.contains("User-Agent: Mozilla/5.0"));
            QCOMPARE(profile["suggestedUpdateMinutes"].toInt(), 720);
            panel.openPage("Proxies");
            auto *nodes = policyNodes(panel, "__dcc_subscription_nodes__"); QVERIFY(nodes); QTRY_COMPARE(nodes->count(), 1);
            QCOMPARE(panel.findChild<QComboBox *>("node-subscription-source")->currentData().toString(), profile["id"].toString());
            QVERIFY(!panel.findChild<QPushButton *>("test-proxy-group")->isEnabled());
            QVERIFY(!nodes->item(0)->data(NodeActiveRole).toBool());
            QVERIFY(!QFile::exists(dir.path() + "/service.sock"));
            panel.hide();
        }
        if (oldEnv.isNull()) qunsetenv("DCC_PROXY_DATA_DIR"); else qputenv("DCC_PROXY_DATA_DIR", oldEnv);
    }
    void durableCustomizationsAndExactRestore()
    {
        QTemporaryDir dir; Store store(dir.path());
        const auto id = store.saveProfile("Persistent", "https://example.invalid/sub", "rules: [\"MATCH,DIRECT\"]\n");
        const auto clean = backup(dir.path());
        QJsonObject custom{{"rules", QJsonObject{{id, QJsonArray{"DOMAIN-SUFFIX,keep.example,DIRECT"}}}},
            {"overrides", QJsonArray{QJsonObject{{"id", newId()}, {"name", "Scoped"}, {"kind", "yaml"}, {"profile", id}, {"content", "mode: direct\n"}, {"enabled", true}}}}};
        store.setCustomizations(custom);
        store.saveProfile("Persistent", "https://example.invalid/sub", "rules: [\"MATCH,REJECT\"]\n", id);
        const auto runtime = parseConfig(store.runtime(id));
        QCOMPARE(runtime["rules"][0].as<std::string>(), std::string("DOMAIN-SUFFIX,keep.example,DIRECT"));
        QCOMPARE(runtime["mode"].as<std::string>(), std::string("direct"));
        auto ordered = custom["overrides"].toArray();
        ordered.append(QJsonObject{{"id", newId()}, {"kind", "yaml"}, {"profile", id}, {"content", "mode: global\n"}});
        ordered.append(QJsonObject{{"id", newId()}, {"kind", "javascript"}, {"profile", id}, {"content", "function main(config) { if (config.mode !== 'global') throw new Error('wrong override order'); config.mode = 'rule'; return config; }"}});
        ordered.append(QJsonObject{{"id", newId()}, {"kind", "yaml"}, {"profile", id}, {"enabled", false}, {"content", "mode: direct\n"}});
        custom["overrides"] = ordered; store.setCustomizations(custom);
        QCOMPARE(parseConfig(store.runtime(id))["mode"].as<std::string>(), std::string("rule"));
        const auto other = store.saveProfile("Other", {}, "rules: [\"MATCH,DIRECT\"]\n");
        QVERIFY(!parseConfig(store.runtime(other))["mode"]);
        writeFile(dir.path() + "/override.yaml", "rules: [\"MATCH,REJECT\"]\n");
        restore(dir.path(), clean);
        QVERIFY(!QFile::exists(dir.path() + "/override.yaml")); QVERIFY(!QFile::exists(dir.path() + "/customizations.json"));
        QVERIFY(!QFile::exists(store.profilePath(other))); QCOMPARE(store.profiles().size(), 1);
        QVERIFY(QFile::exists(dir.path() + "/before-restore.json"));
    }
    void completeCandidateValidationBeforeCommit()
    {
        if (testCore().isEmpty()) QSKIP("Requires the bundled core");
        QTemporaryDir dir; Store store(dir.path());
        const auto id = store.saveProfile("Candidate", {}, "rules: [\"MATCH,DIRECT\"]\n");
        auto settings = store.settings(); settings["core"] = testCore(); store.setSettings(settings);
        const auto initial = configFingerprint(dir.path());
        QJsonObject bad{{"files", QJsonObject{{"override.yaml", QString::fromLatin1(QByteArray("mixed-port: invalid\n").toBase64())}}}};
        QVERIFY_EXCEPTION_THROWN(validateStoreChange(dir.path(), bad), std::runtime_error);
        QCOMPARE(configFingerprint(dir.path()), initial); QVERIFY(!QFile::exists(dir.path() + "/override.yaml"));
        auto scriptedSettings = settings; scriptedSettings["scriptEnabled"] = true;
        QJsonObject badScript{{"files", QJsonObject{
            {"settings.json", QString::fromLatin1(QJsonDocument(scriptedSettings).toJson().toBase64())},
            {"override.js", QString::fromLatin1(QByteArray("function main(config) { throw new Error('candidate script rejected'); }").toBase64())}}}};
        QVERIFY_EXCEPTION_THROWN(validateStoreChange(dir.path(), badScript), std::runtime_error);
        QCOMPARE(configFingerprint(dir.path()), initial); QVERIFY(!QFile::exists(dir.path() + "/override.js"));
        QJsonObject custom{{"rules", QJsonObject{{id, QJsonArray{"DOMAIN-SUFFIX,valid.example,DIRECT"}}}}};
        QJsonObject change{{"files", QJsonObject{{"customizations.json", QString::fromLatin1(QJsonDocument(custom).toJson().toBase64())}}}};
        const auto validated = validateStoreChange(dir.path(), change); QVERIFY(validated["valid"].toBool());
        change["fingerprint"] = validated["fingerprint"];
        settings["updateMinutes"] = 0; store.setSettings(settings);
        QVERIFY_EXCEPTION_THROWN(commitStoreChange(dir.path(), change), std::runtime_error);
        QVERIFY(!QFile::exists(dir.path() + "/customizations.json"));
        change["fingerprint"] = validateStoreChange(dir.path(), change)["fingerprint"];
        commitStoreChange(dir.path(), change);
        QVERIFY(store.runtime(id).contains("valid.example"));
    }
    void extendedUriFormatsAndTransports()
    {
        QJsonObject vmess{{"v", "2"}, {"ps", "VMess WS"}, {"add", "example.com"}, {"port", "443"},
            {"id", "00000000-0000-4000-8000-000000000001"}, {"aid", "0"}, {"net", "ws"}, {"host", "cdn.example.com"}, {"path", "/websocket"}, {"tls", "tls"}, {"sni", "tls.example.com"}};
        const QByteArray vmessUri = "vmess://" + QJsonDocument(vmess).toJson(QJsonDocument::Compact).toBase64();
        auto b64 = [](const QByteArray &bytes) { return bytes.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals); };
        const QByteArray ssr = "ssr://" + b64("example.com:443:auth_sha1_v4:aes-128-cfb:tls1.2_ticket_auth:" + b64("pass:word") + "/?remarks=" + b64("SSR sample") + "&obfsparam=" + b64("cdn.example.com"));
        const QByteArray legacy = "ss://" + b64("aes-128-gcm:pa:ss@example.com:8388") + "#Legacy";
        const QByteArray vless = "vless://00000000-0000-4000-8000-000000000001@example.com:443?security=tls&type=h2&host=cdn.example.com&path=%2Fstream#H2";
        const auto yaml = normalizeSubscription(vmessUri + '\n' + ssr + '\n' + legacy + '\n' + vless);
        const auto config = parseConfig(yaml); QCOMPARE(config["proxies"].size(), size_t(4));
        QCOMPARE(config["proxies"][0]["ws-opts"]["headers"]["Host"].as<std::string>(), std::string("cdn.example.com"));
        QCOMPARE(config["proxies"][1]["password"].as<std::string>(), std::string("pass:word"));
        QCOMPARE(config["proxies"][1]["obfs-param"].as<std::string>(), std::string("cdn.example.com"));
        QCOMPARE(config["proxies"][2]["password"].as<std::string>(), std::string("pa:ss"));
        QCOMPARE(config["proxies"][3]["h2-opts"]["path"].as<std::string>(), std::string("/stream"));
        if (!testCore().isEmpty()) {
            QTemporaryDir dir; writeFile(dir.path() + "/config.yaml", yaml);
            QProcess core; core.start(testCore(), {"-t", "-d", dir.path(), "-f", dir.path() + "/config.yaml"});
            QVERIFY(core.waitForFinished(10000)); QVERIFY2(core.exitCode() == 0, core.readAllStandardOutput().constData());
        }
        QVERIFY_EXCEPTION_THROWN(normalizeSubscription("vmess://invalid"), std::runtime_error);
        const auto exported = parseConfig(normalizeSubscription("proxies: [{name: FromSubStore, type: ss, server: example.com, port: 443, cipher: aes-128-gcm, password: sample}]\n"));
        QVERIFY(exported["proxy-groups"].IsSequence()); QVERIFY(exported["rules"].IsSequence());
    }
    void remoteControllerPreservesManagedIdentity()
    {
        QTemporaryDir dir; Store store(dir.path()); const auto id = store.saveProfile("Local", {}, "rules: [\"MATCH,DIRECT\"]\n");
        auto settings = store.settings(); settings["endpoint"] = "https://remote.example"; settings["secret"] = "remote-token";
        settings["managedEndpoint"] = "http://127.0.0.1:29999"; settings["managedSecret"] = "local-token"; store.setSettings(settings);
        QVERIFY(!localController(settings["endpoint"].toString()));
        const auto runtime = parseConfig(store.runtime(id));
        QCOMPARE(runtime["external-controller"].as<std::string>(), std::string("127.0.0.1:29999"));
        QCOMPARE(runtime["secret"].as<std::string>(), std::string("local-token"));
        QCOMPARE(store.settings()["secret"].toString(), QString("remote-token"));
    }
    void structuredDnsFormPreservesAdvancedFields()
    {
        if (testCore().isEmpty()) QSKIP("Requires a bundled core");
        QTemporaryDir dir; const auto oldEnv = qgetenv("DCC_PROXY_DATA_DIR"); qputenv("DCC_PROXY_DATA_DIR", dir.path().toUtf8());
        {
            Store store; const auto id = store.saveProfile("DNS form", {}, "dns: {enable: true, nameserver: [1.1.1.1], fake-ip-filter: ['+.lan'], prefer-h3: true}\nrules: [\"MATCH,DIRECT\"]\n");
            QTcpServer port; QVERIFY(port.listen(QHostAddress::LocalHost)); const int endpoint = port.serverPort(); port.close();
            auto settings = store.settings(); settings["core"] = testCore(); settings["endpoint"] = QString("http://127.0.0.1:%1").arg(endpoint); store.setSettings(settings);
            const auto original = store.profile(id);
            Panel panel; panel.show();
            bool formSeen = false;
            QTimer::singleShot(30, this, [&] {
                auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget()); if (!dialog) return;
                auto *edit = dialog->findChild<QPlainTextEdit *>("dns-nameserver"); if (!edit) { dialog->reject(); return; }
                formSeen = true; edit->setPlainText("9.9.9.9");
                dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save)->click();
            });
            bool clicked = false; for (auto *button : panel.findChildren<QPushButton *>()) if (button->text() == "DNS") { button->click(); clicked = true; break; }
            QVERIFY(clicked); QVERIFY(formSeen);
            QTRY_COMPARE(store.customizations()["overrides"].toArray().size(), 1);
            const auto runtime = parseConfig(store.runtime(id));
            QCOMPARE(runtime["dns"]["nameserver"][0].as<std::string>(), std::string("9.9.9.9"));
            QCOMPARE(runtime["dns"]["fake-ip-filter"][0].as<std::string>(), std::string("+.lan"));
            QVERIFY(runtime["dns"]["prefer-h3"].as<bool>()); QCOMPARE(store.profile(id), original);
            QVERIFY(!QFile::exists(dir.path() + "/service.sock")); panel.hide();
        }
        if (oldEnv.isNull()) qunsetenv("DCC_PROXY_DATA_DIR"); else qputenv("DCC_PROXY_DATA_DIR", oldEnv);
    }
    void diagnosticDoesNotMutateConfiguration()
    {
        if (testCore().isEmpty()) QSKIP("Requires a bundled core");
        QTemporaryDir dir; const auto oldEnv = qgetenv("DCC_PROXY_DATA_DIR"); qputenv("DCC_PROXY_DATA_DIR", dir.path().toUtf8());
        {
            QTcpServer server; QVERIFY(server.listen(QHostAddress::LocalHost)); int mutations = 0;
            connect(&server, &QTcpServer::newConnection, this, [&] {
                while (auto *socket = server.nextPendingConnection()) {
                    auto buffer = QSharedPointer<QByteArray>::create(); connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                    connect(socket, &QTcpSocket::readyRead, socket, [&, socket, buffer] {
                        *buffer += socket->readAll(); if (!buffer->contains("\r\n\r\n")) return;
                        if (!buffer->startsWith("GET ")) ++mutations;
                        QByteArray body = "{}";
                        if (buffer->startsWith("GET /configs ")) body = QJsonDocument(QJsonObject{{"mode", "direct"}, {"mixed-port", server.serverPort()}}).toJson();
                        if (buffer->startsWith("GET /version ")) body = "{\"version\":\"fixture\"}";
                        if (buffer->startsWith("GET /dns/query?")) body = "{\"Answer\":[{\"data\":\"127.0.0.1\"}]}";
                        socket->write("HTTP/1.1 200 OK\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body); socket->disconnectFromHost();
                    });
                }
            });
            Store store; store.saveProfile("Diagnostic", {}, "rules: [\"MATCH,DIRECT\"]\n");
            auto settings = store.settings(); settings["core"] = testCore(); settings["endpoint"] = QString("http://127.0.0.1:%1").arg(server.serverPort()); store.setSettings(settings);
            QLocalServer ipc; QVERIFY(ipc.listen(dir.path() + "/service.sock")); int controlChanges = 0;
            connect(&ipc, &QLocalServer::newConnection, this, [&] {
                while (auto *socket = ipc.nextPendingConnection()) {
                    connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
                    connect(socket, &QLocalSocket::readyRead, socket, [&, socket] {
                        const auto command = QJsonDocument::fromJson(socket->readAll()).object(); if (command["action"].toString() != "status") ++controlChanges;
                        socket->write(QJsonDocument(QJsonObject{{"running", true}, {"version", QStringLiteral(DCC_PROXY_VERSION)}, {"pid", 123}}).toJson(QJsonDocument::Compact) + '\n'); socket->flush(); socket->disconnectFromServer();
                    });
                }
            });
            Panel panel; panel.show(); panel.openPage("Tools"); const auto before = configFingerprint(dir.path());
            panel.findChild<QPushButton *>("diagnose-network")->click();
            auto *target = panel.findChild<QLineEdit *>("diagnostic-target"); QVERIFY(target); target->setText(QString("http://127.0.0.1:%1/test").arg(server.serverPort()));
            auto *run = panel.findChild<QPushButton *>("diagnostic-run"); run->click();
            auto *report = panel.findChild<QPlainTextEdit *>("diagnostic-report"); QVERIFY(report);
            QTRY_VERIFY(report->toPlainText().contains("实际代理请求：收到 HTTP 200"));
            QTRY_VERIFY(report->toPlainText().contains("磁盘完整候选配置：校验通过")); QTRY_VERIFY(run->isEnabled());
            QCOMPARE(mutations, 0); QCOMPARE(controlChanges, 0); QCOMPARE(configFingerprint(dir.path()), before); panel.hide();
        }
        if (oldEnv.isNull()) qunsetenv("DCC_PROXY_DATA_DIR"); else qputenv("DCC_PROXY_DATA_DIR", oldEnv);
    }
    void substoreListsAndImportsConvertedNodes()
    {
        if (testCore().isEmpty()) QSKIP("Requires a bundled core");
        QTemporaryDir dir; const auto oldEnv = qgetenv("DCC_PROXY_DATA_DIR"); qputenv("DCC_PROXY_DATA_DIR", dir.path().toUtf8());
        {
            QTcpServer server; QVERIFY(server.listen(QHostAddress::LocalHost)); QStringList requests;
            connect(&server, &QTcpServer::newConnection, this, [&] {
                while (auto *socket = server.nextPendingConnection()) {
                    auto buffer = QSharedPointer<QByteArray>::create(); connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                    connect(socket, &QTcpSocket::readyRead, socket, [&, socket, buffer] {
                        *buffer += socket->readAll(); if (!buffer->contains("\r\n\r\n")) return;
                        requests << QString::fromUtf8(buffer->split(' ').value(1)); QByteArray body = "{}";
                        if (buffer->startsWith("GET /api/subs ")) body = "{\"status\":\"success\",\"data\":[{\"name\":\"Sample\",\"source\":\"remote\"}]}";
                        else if (buffer->startsWith("GET /api/collections ")) body = "{\"status\":\"success\",\"data\":{\"Combo\":{\"name\":\"Combo\",\"subscriptions\":[\"Sample\"]}}}";
                        else if (buffer->startsWith("GET /download/sub/Sample?target=ClashMeta ")) body = "proxies: [{name: FromSubStore, type: anytls, server: example.com, port: 443, password: sample}]\n";
                        socket->write("HTTP/1.1 200 OK\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body); socket->disconnectFromHost();
                    });
                }
            });
            Store store; auto settings = store.settings(); settings["core"] = testCore(); settings["substoreUrl"] = QString("http://127.0.0.1:%1").arg(server.serverPort()); store.setSettings(settings);
            Panel panel; panel.show(); panel.openPage("Tools"); panel.findChild<QPushButton *>("manage-substore")->click();
            auto *table = panel.findChild<QTableWidget *>("substore-items"); QVERIFY(table);
            panel.findChild<QPushButton *>("substore-refresh")->click(); QTRY_COMPARE(table->rowCount(), 2);
            for (int row = 0; row < table->rowCount(); ++row) if (table->item(row, 1)->text() == "Sample") table->setCurrentCell(row, 0);
            panel.findChild<QPushButton *>("substore-import")->click(); QTRY_COMPARE(store.profiles().size(), 1);
            const auto imported = parseConfig(store.profile(store.profiles()[0].toObject()["id"].toString()));
            QCOMPARE(imported["proxies"][0]["type"].as<std::string>(), std::string("anytls")); QVERIFY(imported["proxy-groups"].IsSequence());
            QVERIFY(requests.contains("/download/sub/Sample?target=ClashMeta")); QVERIFY(!QFile::exists(dir.path() + "/service.sock")); panel.hide();
        }
        if (oldEnv.isNull()) qunsetenv("DCC_PROXY_DATA_DIR"); else qputenv("DCC_PROXY_DATA_DIR", oldEnv);
    }
    void backupRestore()
    {
        QTemporaryDir a, b; Store store(a.path());
        auto id = store.saveProfile("test", {}, "mode: rule\n");
        auto bytes = backup(a.path()); restore(b.path(), bytes); Store restored(b.path());
        QCOMPARE(restored.profile(id), store.profile(id));
        auto doc = QJsonDocument::fromJson(bytes).object(); auto files = doc["files"].toObject(); files["../escape"] = "dGVzdA=="; doc["files"] = files;
        QVERIFY_EXCEPTION_THROWN(restore(b.path(), QJsonDocument(doc).toJson()), std::runtime_error);
        QCOMPARE(restored.profile(id), store.profile(id));
    }
    void subscriptionWithoutMixedPort()
    {
        QTemporaryDir dir; Store store(dir.path());
        const auto id = store.saveProfile("full YAML", {}, "port: 7890\nsocks-port: 7891\ngeo-auto-update: true\nrules: [\"MATCH,DIRECT\"]\n");
        auto config = parseConfig(store.runtime(id));
        QCOMPARE(config["mixed-port"].as<int>(), 16666);
        QCOMPARE(config["port"].as<int>(), 7890);
        QCOMPARE(config["socks-port"].as<int>(), 7891);
        QVERIFY(!config["geo-auto-update"].as<bool>());
        writeFile(dir.path() + "/controlled.yaml", "mixed-port: 18890\n");
        QCOMPARE(parseConfig(store.runtime(id))["mixed-port"].as<int>(), 18890);
        writeFile(dir.path() + "/controlled.yaml", "mixed-port: 0\n");
        QCOMPARE(parseConfig(store.runtime(id))["mixed-port"].as<int>(), 0);
    }
    void runtimeController()
    {
        QTemporaryDir d; Store store(d.path());
        auto id = store.saveProfile("test", {}, "mixed-port: 16666\nsecret: subscription-secret\nexternal-controller: 0.0.0.0:9090\n");
        auto s = store.settings(); s["secret"] = "private-token"; store.setSettings(s);
        const auto node = parseConfig(store.runtime(id));
        QCOMPARE(node["external-controller"].as<std::string>(), std::string("127.0.0.1:19999"));
        QCOMPARE(node["secret"].as<std::string>(), std::string("private-token"));
        QVERIFY(node["profile"]["store-selected"].as<bool>());
        QVERIFY(node["unified-delay"].as<bool>());
        store.saveProfile("test", {}, "unified-delay: false\n", id);
        QVERIFY(parseConfig(store.runtime(id))["unified-delay"].as<bool>());
        writeFile(d.path() + "/controlled.yaml", "unified-delay: false\n");
        QVERIFY(!parseConfig(store.runtime(id))["unified-delay"].as<bool>());
    }
    void runtimeNodeDnsCompatibility()
    {
        QTemporaryDir dir; Store store(dir.path());
        const QByteArray data = "dns: {enable: true, nameserver: [tls://example.invalid:853], enhanced-mode: fake-ip}\n";
        const auto id = store.saveProfile("dns", {}, data);
        auto runtime = parseConfig(store.runtime(id));
        QCOMPARE(runtime["dns"]["proxy-server-nameserver"][0].as<std::string>(), std::string("system"));
        QCOMPARE(runtime["dns"]["nameserver"][0].as<std::string>(), std::string("tls://example.invalid:853"));
        QCOMPARE(runtime["dns"]["enhanced-mode"].as<std::string>(), std::string("fake-ip"));
        store.saveProfile("dns", {}, "dns: {enable: true, proxy-server-nameserver: [https://example.net/dns-query]}\n", id);
        runtime = parseConfig(store.runtime(id));
        QCOMPARE(runtime["dns"]["proxy-server-nameserver"][0].as<std::string>(), std::string("https://example.net/dns-query"));
        store.saveProfile("dns", {}, data, id);
        auto settings = store.settings(); settings["systemNodeDns"] = false; store.setSettings(settings);
        QVERIFY(!parseConfig(store.runtime(id))["dns"]["proxy-server-nameserver"]);
        settings["systemNodeDns"] = true; store.setSettings(settings);
        store.saveProfile("dns", {}, "dns: {enable: false}\n", id);
        QVERIFY(!parseConfig(store.runtime(id))["dns"]["proxy-server-nameserver"]);
    }
    void endpointAndEncoding()
    {
        QVERIFY(Api::validEndpoint(QUrl("http://127.0.0.1:19999")));
        QVERIFY(Api::validEndpoint(QUrl("https://example.com:9090")));
        QVERIFY(!Api::validEndpoint(QUrl("http://example.com:9090")));
        QVERIFY(!Api::validEndpoint(QUrl("https://token@example.com")));
        QVERIFY(!Api::validEndpoint(QUrl("https://example.com/path")));
        QCOMPARE(Api::segment("组 /?#日本"), QString("%E7%BB%84%20%2F%3F%23%E6%97%A5%E6%9C%AC"));
    }
    void nodeTestsAnimateAndKeepCardsStable()
    {
        QTemporaryDir dir; const auto oldEnv = qgetenv("DCC_PROXY_DATA_DIR");
        qputenv("DCC_PROXY_DATA_DIR", dir.path().toUtf8());
        QTcpServer server; QVERIFY(server.listen(QHostAddress::LocalHost));
        Store store; auto settings = store.settings();
        settings["endpoint"] = QString("http://127.0.0.1:%1").arg(server.serverPort()); store.setSettings(settings);
        int delayRequests = 0, groupRequests = 0, switchRequests = 0, providerRequests = 0;
        QStringList testedUrls; QList<int> testedTimeouts;
        connect(&server, &QTcpServer::newConnection, this, [&] {
            while (auto *s = server.nextPendingConnection()) {
                auto buffer = QSharedPointer<QByteArray>::create();
                connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
                connect(s, &QTcpSocket::readyRead, s, [&, s, buffer] {
                    *buffer += s->readAll(); if (!buffer->contains("\r\n\r\n")) return;
                    const auto request = *buffer; buffer->clear();
                    if (request.startsWith("PUT /proxies/")) ++switchRequests;
                    if (request.startsWith("GET /group/")) ++groupRequests;
                    QByteArray body = "{}", status = "200 OK"; int wait = 0;
                    if (request.contains("/delay?") || request.contains("/healthcheck?")) {
                        ++delayRequests;
                        if (request.contains("/healthcheck?")) ++providerRequests;
                        const QUrlQuery query(QUrl::fromEncoded(request.split(' ').value(1)));
                        testedUrls << query.queryItemValue("url", QUrl::FullyDecoded);
                        testedTimeouts << query.queryItemValue("timeout").toInt();
                        if (request.contains(Api::segment("香港").toUtf8())) { wait = 900; status = "504 Gateway Timeout"; body = "{\"message\":\"context deadline exceeded\"}"; }
                        else { wait = 250; body = "{\"delay\":245}"; }
                    } else if (request.startsWith("GET /proxies ")) {
                        QJsonObject proxies{{"组", QJsonObject{{"type", "Selector"}, {"all", QJsonArray{"日本", "香港"}}, {"now", "日本"}, {"testUrl", "https://group.example/204"}}},
                            {"日本", QJsonObject{{"type", "Hysteria2"}, {"history", QJsonArray{QJsonObject{{"delay", 0}}}}}},
                            {"香港", QJsonObject{{"type", "Hysteria2"}, {"history", QJsonArray{QJsonObject{{"delay", 0}}}}}}};
                        body = QJsonDocument(QJsonObject{{"proxies", proxies}}).toJson(QJsonDocument::Compact);
                    } else if (request.startsWith("GET /providers/proxies ")) {
                        QJsonObject provider{{"proxies", QJsonArray{QJsonObject{{"name", "日本"}, {"type", "Hysteria2"}}}}};
                        body = QJsonDocument(QJsonObject{{"providers", QJsonObject{{"订阅 /", provider}}}}).toJson(QJsonDocument::Compact);
                    } else if (request.startsWith("GET /configs ")) body = "{\"mode\":\"rule\"}";
                    QTimer::singleShot(wait, s, [s, body, status] {
                        s->write("HTTP/1.1 " + status + "\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
                        s->disconnectFromHost();
                    });
                });
            }
        });
        {
            Panel panel; panel.resize(900, 650); panel.show(); panel.openPage("Proxies");
            QTRY_VERIFY(policyNodes(panel, "组"));
            auto *nodes = policyNodes(panel, "组");
            auto *group = policyButton(panel, "test-policy-group", "组");
            auto *refresh = panel.findChild<QPushButton *>("refresh-proxy-panel");
            QVERIFY(nodes); QVERIFY(group); QVERIFY(refresh);
            auto *testUrl = panel.findChild<QLineEdit *>("node-test-url");
            QCOMPARE(panel.findChild<QSpinBox *>("node-test-timeout")->value(), 5000);
            QCOMPARE(panel.findChild<QSpinBox *>("node-test-concurrency")->value(), 50);
            QVERIFY(testUrl->text().isEmpty());
            QVERIFY(!panel.findChild<QPushButton *>("test-selected-node"));
            QVERIFY(!panel.findChild<QPushButton *>("select-node"));
            QTRY_COMPARE(nodes->count(), 2);
            QTRY_VERIFY(panel.findChild<QLabel *>("overview-core")->text().contains("内核已连接"));
            QVERIFY(panel.findChild<QLabel *>("proxy-status")->text().isEmpty());
            QPersistentModelIndex persistent(nodes->model()->index(0, 0));
            auto *first = nodes->item(0);
            nodes->setCurrentRow(0);
            QTest::mouseClick(nodes->viewport(), Qt::LeftButton, Qt::NoModifier, qobject_cast<NodeList *>(nodes)->delayRect(first).center());
            QVERIFY(first->data(NodeTestingRole).toBool()); QVERIFY(!nodes->item(1)->data(NodeTestingRole).toBool());
            const auto angle = nodes->property("node-spinner-angle").toInt();
            QTRY_VERIFY(nodes->property("node-spinner-angle").toInt() != angle);
            QTest::mouseClick(refresh, Qt::LeftButton);
            QTRY_VERIFY(!first->data(NodeTestingRole).toBool());
            QCOMPARE(first->data(NodeDelayRole).toInt(), 245);
            QVERIFY(first->data(NodeActiveRole).toBool()); QVERIFY(persistent.isValid()); QCOMPARE(nodes->item(0), first);
            QCOMPARE(testedUrls.first(), QString("https://group.example/204"));
            QCOMPARE(testedTimeouts.first(), 5000);
            testUrl->setText("https://custom.example/204");
            QTest::mouseClick(group, Qt::LeftButton);
            QVERIFY(first->data(NodeTestingRole).toBool()); QVERIFY(nodes->item(1)->data(NodeTestingRole).toBool());
            QTRY_VERIFY(!first->data(NodeTestingRole).toBool());
            QVERIFY(nodes->item(1)->data(NodeTestingRole).toBool());
            QCOMPARE(first->data(NodeDelayRole).toInt(), 245);
            QTRY_VERIFY(!nodes->item(1)->data(NodeTestingRole).toBool()); QTRY_VERIFY(group->isEnabled());
            QCOMPARE(nodes->item(1)->data(NodeDelayRole).toInt(), 0);
            QVERIFY(nodes->item(1)->data(NodeTestErrorRole).toString().contains("deadline"));
            QCOMPARE(delayRequests, 3); QCOMPARE(groupRequests, 0);
            QCOMPARE(providerRequests, 2);
            QCOMPARE(testedUrls.last(), QString("https://custom.example/204"));
            QCOMPARE(store.settings()["delayTestUrl"].toString(), QString("https://custom.example/204"));
            // Testing an unselected failed node must neither select it nor send a switch request.
            QTest::mouseClick(nodes->viewport(), Qt::LeftButton, Qt::NoModifier, qobject_cast<NodeList *>(nodes)->delayRect(nodes->item(1)).center());
            QVERIFY(nodes->item(1)->data(NodeTestingRole).toBool());
            QCOMPARE(nodes->currentRow(), 0); QVERIFY(first->data(NodeActiveRole).toBool());
            QTRY_VERIFY(!nodes->item(1)->data(NodeTestingRole).toBool());
            QCOMPARE(nodes->currentRow(), 0); QCOMPARE(switchRequests, 0); QCOMPARE(delayRequests, 4);
            QTest::mouseClick(refresh, Qt::LeftButton); QTest::qWait(80);
            QVERIFY(persistent.isValid()); QCOMPARE(nodes->item(0), first); QCOMPARE(first->data(NodeDelayRole).toInt(), 245);
            // Additional clicks must join the running batch, keep their own URL, and never select nodes.
            testUrl->setText("https://parallel-a.example/204");
            QTest::mouseClick(nodes->viewport(), Qt::LeftButton, Qt::NoModifier, nodes->delayRect(first).center());
            QVERIFY(first->data(NodeTestingRole).toBool());
            testUrl->setText("https://parallel-b.example/204");
            QTest::mouseClick(nodes->viewport(), Qt::LeftButton, Qt::NoModifier, nodes->delayRect(nodes->item(1)).center());
            QVERIFY(first->data(NodeTestingRole).toBool()); QVERIFY(nodes->item(1)->data(NodeTestingRole).toBool());
            QTest::mouseClick(nodes->viewport(), Qt::LeftButton, Qt::NoModifier, nodes->delayRect(first).center());
            QTRY_COMPARE(delayRequests, 6);
            QCOMPARE(testedUrls[testedUrls.size() - 2], QString("https://parallel-a.example/204"));
            QCOMPARE(testedUrls.last(), QString("https://parallel-b.example/204"));
            QCOMPARE(switchRequests, 0);
            QTRY_VERIFY(!first->data(NodeTestingRole).toBool());
            QVERIFY(nodes->item(1)->data(NodeTestingRole).toBool());
            QTRY_VERIFY(!nodes->item(1)->data(NodeTestingRole).toBool());
            QCOMPARE(delayRequests, 6); QCOMPARE(nodes->currentRow(), 0);
            panel.hide();
        }
        if (oldEnv.isNull()) qunsetenv("DCC_PROXY_DATA_DIR"); else qputenv("DCC_PROXY_DATA_DIR", oldEnv);
    }
    void selectionUsesRealHttp()
    {
        QTcpServer server; QVERIFY(server.listen(QHostAddress::LocalHost));
        QByteArray request;
        connect(&server, &QTcpServer::newConnection, this, [&] {
            auto *s = server.nextPendingConnection();
            connect(s, &QTcpSocket::readyRead, s, [&, s] {
                request += s->readAll();
                if (request.contains("\r\n\r\n") && request.contains("节点")) {
                    s->write("HTTP/1.1 204 No Content\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"); s->disconnectFromHost();
                }
            });
        });
        Api api; QVERIFY(api.configure(QString("http://127.0.0.1:%1").arg(server.serverPort()), "test-secret"));
        bool complete = false; QString error;
        api.request("PUT", "/proxies/" + Api::segment("组/日本"), {{"name", "节点"}}, [&](const QJsonValue &, const QString &e) { error = e; complete = true; });
        QTRY_VERIFY(complete); QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY(request.startsWith("PUT /proxies/%E7%BB%84%2F%E6%97%A5%E6%9C%AC HTTP/1.1"));
        QVERIFY(request.contains("Authorization: Bearer test-secret"));
    }
    void restartButtonReplacesDaemonAndCore()
    {
        if (testCore().isEmpty()) QSKIP("Requires the bundled core");
        QTemporaryDir dir; const auto previous = qgetenv("DCC_PROXY_DATA_DIR"); qputenv("DCC_PROXY_DATA_DIR", dir.path().toUtf8());
        QProcess original;
        struct Cleanup {
            QByteArray previous;
            QProcess *original;
            ~Cleanup() {
                QLocalSocket socket; socket.connectToServer(socketName());
                if (socket.waitForConnected(500)) { socket.write("{\"action\":\"quit\"}\n"); socket.waitForBytesWritten(500); socket.waitForReadyRead(2000); }
                if (original->state() != QProcess::NotRunning) original->waitForFinished(5000);
                if (previous.isNull()) qunsetenv("DCC_PROXY_DATA_DIR"); else qputenv("DCC_PROXY_DATA_DIR", previous);
            }
        } cleanup{previous, &original};
        QTcpServer controller, listener; QVERIFY(controller.listen(QHostAddress::LocalHost)); QVERIFY(listener.listen(QHostAddress::LocalHost));
        const auto controllerPort = controller.serverPort(), mixedPort = listener.serverPort(); controller.close(); listener.close();
        Store store; const auto id = store.saveProfile("Restart button", {}, QString("mixed-port: %1\nmode: direct\nrules: [\"MATCH,DIRECT\"]\n").arg(mixedPort).toUtf8());
        auto settings = store.settings(); settings["core"] = testCore(); settings["endpoint"] = QString("http://127.0.0.1:%1").arg(controllerPort); settings["active"] = id; settings["autoStart"] = false; settings["autoSystemProxy"] = false; settings["updateMinutes"] = 0; store.setSettings(settings);
        auto env = QProcessEnvironment::systemEnvironment(); env.insert("DBUS_SESSION_BUS_ADDRESS", "unix:path=/nonexistent"); env.insert("DBUS_SYSTEM_BUS_ADDRESS", "unix:path=/nonexistent"); original.setProcessEnvironment(env); original.start(serviceExecutable(), {"--daemon"});
        QVERIFY(original.waitForStarted()); const auto firstDaemon = original.processId();
        QTRY_VERIFY(QFile::exists(socketName()));
        QJsonObject started; bool completed = false;
        serviceRequest({{"action", "start"}}, this, [&](const QJsonValue &value, const QString &) { started = value.toObject(); completed = true; });
        QTRY_VERIFY_WITH_TIMEOUT(completed, 20000); QVERIFY2(started["running"].toBool(), qPrintable(started["error"].toString()));
        {
            Panel panel; panel.show(); auto *restart = panel.findChild<QPushButton *>("restart-proxy-service"); QVERIFY(restart); QTRY_VERIFY(restart->isEnabled());
            QTest::mouseClick(restart, Qt::LeftButton); QVERIFY(!restart->isEnabled());
            QTRY_VERIFY_WITH_TIMEOUT(restart->isEnabled(), 30000);
            QJsonObject current; completed = false;
            serviceRequest({{"action", "status"}}, this, [&](const QJsonValue &value, const QString &) { current = value.toObject(); completed = true; });
            QTRY_VERIFY(completed); QVERIFY(current["running"].toBool()); QVERIFY(current["daemonPid"].toDouble() != firstDaemon); QVERIFY(current["pid"] != started["pid"]);
            QCOMPARE(current["version"].toString(), QStringLiteral(DCC_PROXY_VERSION));
            QVERIFY(!QFile::exists(dir.path() + "/system-proxy-before.json"));
            panel.hide();
        }
    }
    void providersRefreshKeepsRowsAndScopesMessages()
    {
        QTemporaryDir dir; const auto previous = qgetenv("DCC_PROXY_DATA_DIR"); qputenv("DCC_PROXY_DATA_DIR", dir.path().toUtf8());
        QTcpServer server; QVERIFY(server.listen(QHostAddress::LocalHost));
        Store store; auto settings = store.settings(); settings["endpoint"] = QString("http://127.0.0.1:%1").arg(server.serverPort()); store.setSettings(settings);
        int phase = 0, proxyReads = 0, ruleReads = 0, actions = 0, replies = 0, cacheActions = 0;
        bool failAction = false;
        connect(&server, &QTcpServer::newConnection, this, [&] {
            while (auto *socket = server.nextPendingConnection()) {
                auto bytes = QSharedPointer<QByteArray>::create(); auto handled = QSharedPointer<bool>::create(false);
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, socket, [&, socket, bytes, handled] {
                    *bytes += socket->readAll(); if (*handled || !bytes->contains("\r\n\r\n")) return; *handled = true;
                    QByteArray body = "{}", status = "200 OK"; int delay = 0; bool resource = false;
                    if (bytes->startsWith("GET /configs ")) body = R"({"mode":"rule","mixed-port":16666})";
                    if (bytes->startsWith("GET /providers/proxies ")) {
                        ++proxyReads; resource = true; QJsonObject providers;
                        for (int i = 0; i < (phase == 3 ? 29 : 30); ++i) providers[QString("p%1").arg(i, 2, 10, QChar('0'))] = QJsonObject{{"vehicleType", "HTTP"}, {"proxies", QJsonArray{QJsonObject{{"name", "DIRECT"}}}}};
                        body = QJsonDocument(QJsonObject{{"providers", providers}}).toJson();
                        if (phase) delay = 200;
                        if (phase == 2) { status = "500 Internal Server Error"; body = R"({"message":"fixture failure"})"; }
                    }
                    if (bytes->startsWith("GET /providers/rules ")) {
                        ++ruleReads; resource = true;
                        QJsonObject providers;
                        if (phase != 3) providers["p00"] = QJsonObject{{"vehicleType", "HTTP"}, {"ruleCount", phase == 2 ? 99 : 7}};
                        body = QJsonDocument(QJsonObject{{"providers", providers}}).toJson();
                        if (phase) delay = 650;
                    }
                    if (bytes->startsWith("PUT /providers/proxies/")) {
                        ++actions; delay = 350;
                        if (failAction) { status = "500 Internal Server Error"; body = R"({"message":"resource action failed"})"; }
                    }
                    if (bytes->startsWith("POST /cache/dns/flush ")) ++cacheActions;
                    QTimer::singleShot(delay, socket, [&, socket, body, status, resource] {
                        socket->write("HTTP/1.1 " + status + "\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body); socket->disconnectFromHost();
                        if (resource) ++replies;
                    });
                });
            }
        });
        {
            Panel panel; panel.resize(750, 480); panel.show(); panel.openPage("Providers");
            auto *table = panel.findChild<QTableWidget *>("providers"); auto *status = panel.findChild<QLabel *>("proxy-status");
            auto *refresh = panel.findChild<QPushButton *>("refresh-proxy-panel");
            QTRY_COMPARE(table->rowCount(), 31); QTRY_COMPARE(replies, 2);
            auto *messages = panel.findChild<QScrollArea *>("proxy-message-area"); QVERIFY(messages);
            QVERIFY(status->text().isEmpty()); QVERIFY(messages->isHidden());
            int selected = -1;
            for (int row = 0; row < table->rowCount(); ++row) if (table->item(row, 1)->text() == "p15") selected = row;
            QVERIFY(selected >= 0); table->selectRow(selected); table->scrollToItem(table->item(selected, 0), QAbstractItemView::PositionAtCenter);
            QTest::qWait(30); const int scroll = table->verticalScrollBar()->value(); QVERIFY(scroll > 0);
            auto *item = table->item(selected, 1); QPersistentModelIndex persistent(table->model()->index(selected, 1));
            QSignalSpy removed(table->model(), SIGNAL(rowsRemoved(QModelIndex,int,int)));
            phase = 1; const int reads = proxyReads + ruleReads; QTest::mouseClick(refresh, Qt::LeftButton);
            QTRY_COMPARE(proxyReads + ruleReads, reads + 2);
            QCOMPARE(table->rowCount(), 31); QCOMPARE(table->item(selected, 1), item);
            QTest::mouseClick(refresh, Qt::LeftButton); QTest::qWait(80); QCOMPARE(proxyReads + ruleReads, reads + 2);
            QTRY_COMPARE(replies, 4); QVERIFY(persistent.isValid()); QCOMPARE(table->item(selected, 1), item);
            QCOMPARE(table->currentRow(), selected); QCOMPARE(table->verticalScrollBar()->value(), scroll); QCOMPARE(removed.count(), 0);
            phase = 2; QTest::mouseClick(refresh, Qt::LeftButton); QTRY_COMPARE(replies, 6);
            QTRY_VERIFY([&] { for (int row = 0; row < table->rowCount(); ++row) if (table->item(row, 0)->data(Qt::UserRole).toJsonObject()["kind"] == "rules") return table->item(row, 4)->text() == "99"; return false; }());
            QCOMPARE(table->rowCount(), 31); QCOMPARE(removed.count(), 0); QVERIFY(persistent.isValid());
            phase = 3; QTest::mouseClick(refresh, Qt::LeftButton); QTRY_COMPARE(replies, 8); QTRY_COMPARE(table->rowCount(), 29);
            QVERIFY(persistent.isValid()); QCOMPARE(table->item(table->currentRow(), 1)->text(), QString("p15"));
            for (const auto &page : {"Tools", "Settings", "Profiles", "Logs", "Overrides", "Tun", "Providers", "Overview"}) {
                panel.openPage(page); QVERIFY(status->text().isEmpty()); QVERIFY(messages->isHidden());
            }
            panel.openPage("Providers");
            QPushButton *update = nullptr;
            for (auto *button : panel.findChildren<QPushButton *>()) if (button->text() == "更新资源") update = button;
            QVERIFY(update); QTest::mouseClick(update, Qt::LeftButton); QTRY_COMPARE(actions, 1);
            panel.openPage("Tools"); QVERIFY(!status->text().contains("正在更新资源"));
            QVERIFY(status->text().isEmpty()); QVERIFY(messages->isHidden());
            QPushButton *clearCache = nullptr;
            for (auto *button : panel.findChildren<QPushButton *>()) if (button->text() == "清除 dns 缓存") clearCache = button;
            QVERIFY(clearCache); QTest::mouseClick(clearCache, Qt::LeftButton); QTRY_COMPARE(cacheActions, 1);
            QTRY_VERIFY(status->text().contains("缓存已清除"));
            QVERIFY(messages->isVisible());
            QVERIFY(!messages->autoFillBackground()); QVERIFY(!messages->viewport()->autoFillBackground()); QVERIFY(!status->autoFillBackground());
            QTest::qWait(600); QVERIFY(status->text().contains("缓存已清除"));
            panel.openPage("Providers"); QVERIFY(status->text().contains("操作完成"));
            QTest::mouseClick(update, Qt::LeftButton); QTRY_COMPARE(actions, 2); QTRY_VERIFY(status->text().contains("操作完成"));
            panel.openPage("Tools"); QVERIFY(status->text().contains("缓存已清除"));
            panel.openPage("Settings"); QVERIFY(!status->text().contains("缓存已清除")); QVERIFY(!status->text().contains("操作完成"));
            QVERIFY(status->text().isEmpty()); QVERIFY(messages->isHidden());
            panel.openPage("Providers"); QVERIFY(status->text().contains("操作完成"));
            failAction = true; QTest::mouseClick(update, Qt::LeftButton); QTRY_COMPARE(actions, 3);
            panel.openPage("Tools"); QTest::qWait(600); QVERIFY(status->text().contains("缓存已清除"));
            panel.openPage("Providers"); QVERIFY(status->text().contains("resource action failed"));
            QCOMPARE(status->palette().color(QPalette::WindowText), QColor("#d9534f"));
            panel.openPage("Tools"); QVERIFY(status->text().contains("缓存已清除"));
            QVERIFY(status->palette().color(QPalette::WindowText) != QColor("#d9534f"));
            panel.hide();
        }
        if (previous.isNull()) qunsetenv("DCC_PROXY_DATA_DIR"); else qputenv("DCC_PROXY_DATA_DIR", previous);
    }
    void nativeSecondaryPagesNavigateWithoutTabs()
    {
        QTemporaryDir dir; const auto previous = qgetenv("DCC_PROXY_DATA_DIR"); qputenv("DCC_PROXY_DATA_DIR", dir.path().toUtf8());
        TestFrame frame; frame.host.resize(980, 650); frame.host.show();
        ProxyPlugin plugin; plugin.setFrameProxy(&frame); plugin.active();
        auto *list = plugin.moduleWidget()->findChild<QAbstractItemView *>("proxy-secondary-pages"); QVERIFY(list);
        auto *panel = qobject_cast<Panel *>(frame.page.data()); QVERIFY(panel);
        auto *tabs = panel->findChild<QTabWidget *>(); QVERIFY(tabs); QVERIFY(tabs->tabBar()->isHidden());
        QCOMPARE(list->model()->rowCount(), 11); QCOMPARE(frame.stack.size(), 2); QCOMPARE(frame.pushes, 2);
        for (int row = 0; row < list->model()->rowCount(); ++row) QVERIFY(list->model()->index(row, 0).data(Qt::ToolTipRole).toString().isEmpty());
        QCOMPARE(list->iconSize(), QSize(32, 32));
        QTRY_COMPARE(list->viewport()->geometry().topLeft(), QPoint(10, 10));
        for (const auto &page : {QString("Tools"), QString("Settings"), QString("Tun"), QString("Overview")}) {
            QCOMPARE(plugin.load(page), 0); QCOMPARE(frame.page.data(), static_cast<QWidget *>(panel));
            QCOMPARE(list->currentIndex().data(Qt::UserRole).toString(), page);
            QTRY_VERIFY(list->viewport()->rect().intersects(list->visualRect(list->currentIndex())));
        }
        const auto tools = list->model()->index(8, 0); list->scrollTo(tools); QTest::qWait(30);
        QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::NoModifier, list->visualRect(tools).center());
        QCOMPARE(tabs->currentIndex(), 8); QCOMPARE(frame.pushes, 2);
        QPointer<Panel> retired = panel; frame.popWidget(&plugin); QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QVERIFY(retired.isNull()); plugin.load("Tun"); QCOMPARE(frame.stack.size(), 2); QVERIFY(frame.page);
        frame.popWidget(&plugin); frame.popWidget(&plugin); QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        plugin.load("Settings"); QCOMPARE(frame.stack.size(), 2);
        QVERIFY(!QFile::exists(dir.path() + "/service.sock"));
        if (previous.isNull()) qunsetenv("DCC_PROXY_DATA_DIR"); else qputenv("DCC_PROXY_DATA_DIR", previous);
    }
    void panelSwitchReadsBackAndPreservesFailure()
    {
        QTemporaryDir dir;
        const auto oldEnv = qgetenv("DCC_PROXY_DATA_DIR");
        qputenv("DCC_PROXY_DATA_DIR", dir.path().toUtf8());
        QTcpServer server; QVERIFY(server.listen(QHostAddress::LocalHost));
        Store store;
        auto settings = store.settings();
        settings["endpoint"] = QString("http://127.0.0.1:%1").arg(server.serverPort());
        store.setSettings(settings);
        QString now = "DIRECT";
        bool failSwitch = false;
        int switches = 0;
        connect(&server, &QTcpServer::newConnection, this, [&] {
            while (auto *socket = server.nextPendingConnection()) {
                auto buffer = QSharedPointer<QByteArray>::create();
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, socket, [&, socket, buffer] {
                    buffer->append(socket->readAll());
                    const int headerEnd = buffer->indexOf("\r\n\r\n"); if (headerEnd < 0) return;
                    int length = 0;
                    for (auto line : buffer->left(headerEnd).split('\n')) if (line.toLower().startsWith("content-length:")) length = line.mid(15).trimmed().toInt();
                    if (buffer->size() < headerEnd + 4 + length) return;
                    QByteArray body;
                    QByteArray status = "200 OK";
                    if (buffer->startsWith("PUT /proxies/")) {
                        ++switches;
                        if (failSwitch) { status = "500 Internal Server Error"; body = "{\"message\":\"switch rejected\"}"; }
                        else { now = QJsonDocument::fromJson(buffer->mid(headerEnd + 4, length)).object()["name"].toString(); status = "204 No Content"; }
                    } else if (buffer->startsWith("GET /proxies ")) {
                        QJsonObject proxies{{"组 /日本", QJsonObject{{"type", "Selector"}, {"all", QJsonArray{"DIRECT", "REJECT"}}, {"now", now}}},
                                            {"DIRECT", QJsonObject{{"type", "Direct"}}}, {"REJECT", QJsonObject{{"type", "Reject"}}}};
                        body = QJsonDocument(QJsonObject{{"proxies", proxies}}).toJson(QJsonDocument::Compact);
                    } else if (buffer->startsWith("GET /configs ")) body = "{\"mode\":\"rule\",\"mixed-port\":16666}";
                    else body = "{}";
                    socket->write("HTTP/1.1 " + status + "\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
                    socket->disconnectFromHost();
                });
            }
        });
        {
            Panel panel; panel.resize(900, 650); panel.show(); panel.openPage("Proxies");
            QTRY_VERIFY(policyNodes(panel, "组 /日本")); auto *nodes = policyNodes(panel, "组 /日本");
            QVERIFY(nodes);
            QTRY_COMPARE(nodes->count(), 2);
            QTRY_VERIFY(panel.findChild<QLabel *>("overview-core")->text().contains("内核已连接"));
            QVERIFY(panel.findChild<QLabel *>("proxy-status")->text().isEmpty());
            QVERIFY(!nodes->item(0)->icon().pixmap(28, 21).isNull());
            QCOMPARE(nodes->viewMode(), QListView::IconMode);
            QTRY_COMPARE(nodes->visualItemRect(nodes->item(0)).top(), nodes->visualItemRect(nodes->item(1)).top());
            QVERIFY(nodes->visualItemRect(nodes->item(1)).left() > nodes->visualItemRect(nodes->item(0)).left());
            const int wideGrid = nodes->gridSize().width();
            panel.resize(530, 650);
            QTRY_VERIFY(nodes->gridSize().width() != wideGrid);
            QTRY_COMPARE(nodes->visualItemRect(nodes->item(0)).top(), nodes->visualItemRect(nodes->item(1)).top());
            QTest::mouseClick(nodes->viewport(), Qt::LeftButton, Qt::NoModifier, nodes->visualItemRect(nodes->item(1)).topLeft() + QPoint(50, 24));
            QTRY_COMPARE(now, QString("REJECT"));
            QTRY_VERIFY(nodes->item(1)->text().startsWith(QString::fromUtf8("✓")));
            failSwitch = true;
            QTest::mouseClick(nodes->viewport(), Qt::LeftButton, Qt::NoModifier, nodes->visualItemRect(nodes->item(0)).topLeft() + QPoint(50, 24));
            QTRY_COMPARE(switches, 2);
            QTRY_VERIFY(panel.findChild<QLabel *>("proxy-status")->text().contains("切换失败"));
            QCOMPARE(now, QString("REJECT"));
            QVERIFY(nodes->item(1)->text().startsWith(QString::fromUtf8("✓")));
            QVERIFY(!QIcon(":/dcc-proxy/proxy.svg").pixmap(42, 42).isNull());
            panel.hide();
        }
        if (oldEnv.isNull()) qunsetenv("DCC_PROXY_DATA_DIR"); else qputenv("DCC_PROXY_DATA_DIR", oldEnv);
    }
};
QTEST_MAIN(PartyTests)
#include "test_party.moc"
