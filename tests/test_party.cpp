#include "config.h"
#include "api.h"
#include "panel.h"
#include "nodeflags.h"
#include "nodelist.h"
#include "plugin.h"
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
#include <QPlainTextEdit>
using namespace Party;
class TestFrame : public DCC_NAMESPACE::FrameProxyInterface {
public:
    QWidget host;
    QPointer<QWidget> page;
    int pushes = 0;
    void pushWidget(DCC_NAMESPACE::ModuleInterface *const, QWidget *const w, PushType) override { ++pushes; page = w; w->setParent(&host); w->show(); }
    void popWidget(DCC_NAMESPACE::ModuleInterface *const) override { if (page) { page->setParent(nullptr); page->hide(); page->deleteLater(); page = nullptr; } }
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
class PartyTests : public QObject {
    Q_OBJECT
private slots:
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
        auto source = proxyPac(17890, defaultProxyBypass() + QStringList{"*.example.com", "exact.test", "fc00::/7", "2001:db8::/32"});
        QVERIFY(!js.evaluate(QString::fromUtf8(source)).isError());
        js.evaluate("function dnsResolve(host) { return host==='intranet.test' ? '10.20.8.220' : '203.0.113.8'; }");
        auto route = [&](const QString &host) {
            return js.globalObject().property("FindProxyForURL").call({"https://" + host + "/", host}).toString();
        };
        for (const auto &host : {"localhost", "127.99.1.2", "192.168.8.1", "10.20.8.220", "172.16.0.1", "172.31.255.255", "[::1]", "fc00::1", "fdff:ffff::99", "2001:db8:1234::1", "x.EXAMPLE.com.", "exact.test", "intranet.test"})
            QCOMPARE(route(host), QString("DIRECT"));
        for (const auto &host : {"172.32.0.1", "192.169.0.1", "example.com", "badexample.com", "exact.test.evil", "2001:db9::1", "fe80::1", "public.test"})
            QVERIFY2(route(host).startsWith("PROXY 127.0.0.1:17890;"), host);
        js.evaluate("function dnsResolveEx(host) { return '2001:db8::7; 203.0.113.8'; }");
        QCOMPARE(route("ipv6-intranet.test"), QString("DIRECT"));
        QVERIFY(!js.evaluate(QString::fromUtf8(proxyPac(17890, {}))).isError());
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
            QCOMPARE(edit->toPlainText(), defaultProxyBypass().join('\n'));
            auto settings = store.settings(); settings["proxyBypass"] = QJsonArray{"*.example.com", "10.0.0.0/8"}; store.setSettings(settings);
            Panel restored;
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
        QPointer<QWidget> retired = plugin.moduleWidget();
        QVERIFY(retired);
        plugin.load("Proxies"); QCOMPARE(plugin.moduleWidget(), retired.data()); QCOMPARE(frame.pushes, 1);
        frame.popWidget(&plugin);
        QVERIFY(retired); // deleteLater has deliberately not run yet.
        QCOMPARE(plugin.load("Proxies"), 0);
        QPointer<QWidget> current = plugin.moduleWidget();
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
        const auto id = store.saveProfile("日本", "https://example.com/sub", "mixed-port: 17890\nproxies: []\nrules: [MATCH,DIRECT]\n");
        QVERIFY(validId(id)); QCOMPARE(store.profiles().size(), 1);
        QVERIFY(store.profile(id).contains("17890"));
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
            QVERIFY(subscriptionRequest.contains("User-Agent: mihomo.party/v2.0.3 (clash.meta)"));
            QCOMPARE(profile["suggestedUpdateMinutes"].toInt(), 720);
            panel.hide();
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
    void runtimeController()
    {
        QTemporaryDir d; Store store(d.path());
        auto id = store.saveProfile("test", {}, "mixed-port: 17890\nsecret: subscription-secret\nexternal-controller: 0.0.0.0:9090\n");
        auto s = store.settings(); s["secret"] = "private-token"; store.setSettings(s);
        const auto node = parseConfig(store.runtime(id));
        QCOMPARE(node["external-controller"].as<std::string>(), std::string("127.0.0.1:19090"));
        QCOMPARE(node["secret"].as<std::string>(), std::string("private-token"));
        QVERIFY(node["profile"]["store-selected"].as<bool>());
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
        QVERIFY(Api::validEndpoint(QUrl("http://127.0.0.1:19090")));
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
        int delayRequests = 0, groupRequests = 0, switchRequests = 0;
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
                    if (request.contains("/delay?")) {
                        ++delayRequests;
                        if (request.contains(Api::segment("香港").toUtf8())) { wait = 900; status = "504 Gateway Timeout"; body = "{\"message\":\"context deadline exceeded\"}"; }
                        else { wait = 250; body = "{\"delay\":245}"; }
                    } else if (request.startsWith("GET /proxies ")) {
                        QJsonObject proxies{{"组", QJsonObject{{"type", "Selector"}, {"all", QJsonArray{"日本", "香港"}}, {"now", "日本"}}},
                            {"日本", QJsonObject{{"type", "Hysteria2"}, {"history", QJsonArray{QJsonObject{{"delay", 0}}}}}},
                            {"香港", QJsonObject{{"type", "Hysteria2"}, {"history", QJsonArray{QJsonObject{{"delay", 0}}}}}}};
                        body = QJsonDocument(QJsonObject{{"proxies", proxies}}).toJson(QJsonDocument::Compact);
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
            auto *nodes = panel.findChild<QListWidget *>("proxy-nodes");
            auto *group = panel.findChild<QPushButton *>("test-proxy-group");
            auto *refresh = panel.findChild<QPushButton *>("refresh-proxy-panel");
            QVERIFY(nodes); QVERIFY(group); QVERIFY(refresh);
            QVERIFY(!panel.findChild<QPushButton *>("test-selected-node"));
            QVERIFY(!panel.findChild<QPushButton *>("select-node"));
            QTRY_COMPARE(nodes->count(), 2);
            QTRY_VERIFY(panel.findChild<QLabel *>("proxy-status")->text().contains("已连接"));
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
            QTest::mouseClick(group, Qt::LeftButton);
            QVERIFY(first->data(NodeTestingRole).toBool()); QVERIFY(nodes->item(1)->data(NodeTestingRole).toBool());
            QTRY_VERIFY(!first->data(NodeTestingRole).toBool());
            QVERIFY(nodes->item(1)->data(NodeTestingRole).toBool());
            QCOMPARE(first->data(NodeDelayRole).toInt(), 245);
            QTRY_VERIFY(!nodes->item(1)->data(NodeTestingRole).toBool()); QTRY_VERIFY(group->isEnabled());
            QCOMPARE(nodes->item(1)->data(NodeDelayRole).toInt(), 0);
            QVERIFY(nodes->item(1)->data(NodeTestErrorRole).toString().contains("deadline"));
            QCOMPARE(delayRequests, 3); QCOMPARE(groupRequests, 0);
            // Testing an unselected failed node must neither select it nor send a switch request.
            QTest::mouseClick(nodes->viewport(), Qt::LeftButton, Qt::NoModifier, qobject_cast<NodeList *>(nodes)->delayRect(nodes->item(1)).center());
            QVERIFY(nodes->item(1)->data(NodeTestingRole).toBool());
            QCOMPARE(nodes->currentRow(), 0); QVERIFY(first->data(NodeActiveRole).toBool());
            QTRY_VERIFY(!nodes->item(1)->data(NodeTestingRole).toBool());
            QCOMPARE(nodes->currentRow(), 0); QCOMPARE(switchRequests, 0); QCOMPARE(delayRequests, 4);
            QTest::mouseClick(refresh, Qt::LeftButton); QTest::qWait(80);
            QVERIFY(persistent.isValid()); QCOMPARE(nodes->item(0), first); QCOMPARE(first->data(NodeDelayRole).toInt(), 245);
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
                    } else if (buffer->startsWith("GET /configs ")) body = "{\"mode\":\"rule\",\"mixed-port\":17890}";
                    else body = "{}";
                    socket->write("HTTP/1.1 " + status + "\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
                    socket->disconnectFromHost();
                });
            }
        });
        {
            Panel panel; panel.resize(900, 650); panel.show(); panel.openPage("Proxies");
            auto *nodes = panel.findChild<QListWidget *>("proxy-nodes");
            QVERIFY(nodes);
            QTRY_COMPARE(nodes->count(), 2);
            QTRY_VERIFY(panel.findChild<QLabel *>("proxy-status")->text().contains("已连接"));
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
