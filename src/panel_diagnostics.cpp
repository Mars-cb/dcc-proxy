#include "panel.h"
#include <QDialog>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QCheckBox>
#include <QTableWidget>
#include <QHeaderView>
#include <QInputDialog>
#include <QMessageBox>
#include <QJsonDocument>
#include <QNetworkProxy>
#include <QNetworkRequest>
#include <QProcess>
#include <QHostInfo>
#include <QTcpSocket>
#include <QPointer>
#include <QTimer>
#include <QUrlQuery>
#include <QFileInfo>
#include <QSharedPointer>
#include <QRegularExpression>
#include <stdexcept>
namespace Party {
void Panel::diagnose()
{
    auto *dialog = new QDialog(this); dialog->setAttribute(Qt::WA_DeleteOnClose); dialog->setWindowTitle(tr("只读网络诊断")); dialog->resize(740, 580);
    auto *layout = new QVBoxLayout(dialog);
    auto *hint = new QLabel(tr("检查配置、后台、控制器、本地监听、DNS 和指定目标。诊断只发送查询请求，不切换节点、不重启内核、不修改系统代理。")); hint->setWordWrap(true); layout->addWidget(hint);
    auto *target = new QLineEdit("https://www.gstatic.com/generate_204"); target->setObjectName("diagnostic-target"); layout->addWidget(target);
    auto *run = new QPushButton(tr("开始检查")); run->setObjectName("diagnostic-run"); layout->addWidget(run);
    auto *report = new QPlainTextEdit; report->setObjectName("diagnostic-report"); report->setReadOnly(true); layout->addWidget(report, 1);
    auto *recover = new QPushButton(tr("恢复已确认归属的残留内核")); recover->setVisible(false); layout->addWidget(recover);
    connect(recover, &QPushButton::clicked, dialog, scopedCallback(std::function<void()>([this, dialog] {
        if (QMessageBox::question(dialog, tr("恢复残留内核"), tr("将停止已核实属于本插件的残留内核并重新启动，期间代理会短暂中断。是否继续？")) == QMessageBox::Yes)
            backgroundCommand({{"action", "recover-orphan"}});
    })));
    connect(run, &QPushButton::clicked, dialog, [this, dialog, report, run, target, recover] {
        const QUrl url(target->text().trimmed());
        if (!url.isValid() || !QStringList{"http", "https"}.contains(url.scheme()) || url.host().isEmpty() || !url.userInfo().isEmpty()) { report->setPlainText(tr("测试目标须为无嵌入凭据的 HTTP/HTTPS 地址")); return; }
        run->setEnabled(false); report->clear(); recover->hide();
        report->appendPlainText(tr("界面版本：%1").arg(QStringLiteral(DCC_PROXY_VERSION)));
        const bool local = localController(m_store.settings()["endpoint"].toString());
        report->appendPlainText(local ? tr("对象：本机代理") : tr("对象：远程控制器；跳过本机代理流量和恢复操作"));
        auto pending = QSharedPointer<int>::create(0);
        auto done = [pending, run] { if (--*pending == 0) run->setEnabled(true); };
        ++*pending;
        requestService({{"action", "status"}}, dialog, [report, recover, local, done](const QJsonValue &value, const QString &error) {
            const auto state = value.toObject();
            if (!error.isEmpty()) report->appendPlainText(QObject::tr("后台：不可达，%1").arg(error));
            else {
                report->appendPlainText(QObject::tr("后台：版本 %1，内核 %2，PID %3").arg(state["version"].toString(QObject::tr("旧版/未知")), state["running"].toBool() ? QObject::tr("运行") : QObject::tr("停止"), QString::number(state["pid"].toInt())));
                if (state["version"].toString() != QStringLiteral(DCC_PROXY_VERSION)) report->appendPlainText(QObject::tr("版本待生效：后台未自动重启，现有代理继续运行；新后台将在正常启动后加载。"));
                const auto orphan = state["orphan"].toObject();
                if (local && orphan["verified"].toBool()) { report->appendPlainText(QObject::tr("发现已核实归属的残留内核 PID %1，恢复需要明确操作。").arg(orphan["pid"].toInt())); recover->show(); }
                if (!state["error"].toString().isEmpty()) report->appendPlainText(QObject::tr("后台错误：") + state["error"].toString());
            }
            done();
        });
        ++*pending;
        m_api.get("/version", [guard = QPointer<QDialog>(dialog), report, done](const QJsonValue &value, const QString &error) { if (!guard) return; report->appendPlainText(error.isEmpty() ? QObject::tr("控制器鉴权：通过，内核版本 %1").arg(value.toObject()["version"].toString()) : QObject::tr("控制器鉴权/连接失败：%1").arg(error)); done(); });
        ++*pending;
        QHostInfo::lookupHost(url.host(), dialog, [report, done](const QHostInfo &info) {
            QStringList addresses; for (const auto &address : info.addresses()) addresses << address.toString();
            report->appendPlainText(info.error() == QHostInfo::NoError ? QObject::tr("系统 DNS：%1").arg(addresses.join(", ")) : QObject::tr("系统 DNS 失败：%1").arg(info.errorString())); done();
        });
        ++*pending; QUrlQuery query; query.addQueryItem("name", url.host()); query.addQueryItem("type", "A");
        m_api.get("/dns/query?" + query.toString(QUrl::FullyEncoded), [guard = QPointer<QDialog>(dialog), report, done](const QJsonValue &value, const QString &error) {
            if (!guard) return;
            report->appendPlainText(error.isEmpty() ? QObject::tr("内核 DNS：响应成功，回答 %1 条").arg(value.toObject()["Answer"].toArray().size()) : QObject::tr("内核 DNS 失败：%1").arg(error)); done();
        });
        if (local) {
            ++*pending;
            m_api.get("/configs", [this, guard = QPointer<QDialog>(dialog), dialog, report, pending, done, url](const QJsonValue &value, const QString &error) {
                if (!guard) return;
                if (!error.isEmpty()) { report->appendPlainText(QObject::tr("代理流量检查跳过：无法读取正在运行的端口。")); done(); return; }
                const int port = value.toObject()["mixed-port"].toInt();
                if (port < 1 || port > 65535) { report->appendPlainText(QObject::tr("混合代理未开启；进程存在不代表具备本地 HTTP 代理监听。")); done(); return; }
                auto *probe = new QTcpSocket(dialog); probe->setProxy(QNetworkProxy::NoProxy);
                auto finished = QSharedPointer<bool>::create(false);
                auto finish = [probe, finished, report, done](bool success) { if (*finished) return; *finished = true; report->appendPlainText(success ? QObject::tr("本地代理 TCP 监听：可连接") : QObject::tr("本地代理 TCP 监听：不可连接")); probe->deleteLater(); done(); };
                connect(probe, &QTcpSocket::connected, dialog, [finish] { finish(true); });
                connect(probe, QOverload<QAbstractSocket::SocketError>::of(&QTcpSocket::error), dialog, [finish](QAbstractSocket::SocketError) { finish(false); });
                QTimer::singleShot(3000, probe, [finish] { finish(false); }); probe->connectToHost(QHostAddress::LocalHost, quint16(port));
                ++*pending;
                auto *manager = new QNetworkAccessManager(dialog); manager->setProxy(QNetworkProxy(QNetworkProxy::HttpProxy, "127.0.0.1", quint16(port)));
                QNetworkRequest request(url); request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
                auto *reply = manager->get(request); QTimer::singleShot(12000, reply, &QNetworkReply::abort);
                connect(reply, &QNetworkReply::readyRead, dialog, [reply] { if (reply->bytesAvailable() > 256 * 1024) reply->abort(); });
                connect(reply, &QNetworkReply::finished, dialog, [reply, manager, report, done] {
                    const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                    report->appendPlainText(code >= 200 && code < 400 ? QObject::tr("实际代理请求：收到 HTTP %1").arg(code) : QObject::tr("实际代理请求失败：HTTP %1，%2").arg(code).arg(reply->errorString())); manager->deleteLater(); done();
                });
            });
            if (!m_store.profiles().isEmpty()) {
                ++*pending; auto *process = new QProcess(dialog);
                connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), dialog, [process, report, done](int code, QProcess::ExitStatus status) {
                    report->appendPlainText(code == 0 && status == QProcess::NormalExit ? QObject::tr("磁盘完整候选配置：校验通过（未保存、未应用）") : QObject::tr("磁盘完整配置校验失败：%1").arg(QString::fromUtf8(process->readAllStandardError()).right(1600))); process->deleteLater(); done();
                });
                connect(process, &QProcess::errorOccurred, dialog, [process, report, done](QProcess::ProcessError error) { if (error == QProcess::FailedToStart) { report->appendPlainText(QObject::tr("配置校验程序不可用")); process->deleteLater(); done(); } });
                QTimer::singleShot(125000, process, [process] { if (process->state() != QProcess::NotRunning) process->kill(); });
                process->start(serviceExecutable(), {"--validate-store", m_store.root()}); process->write("{}"); process->closeWriteChannel();
            }
        }
    }); dialog->show();
}
void Panel::subStore()
{
    const int page = notificationPage();
    auto *dialog = new QDialog(this); dialog->setAttribute(Qt::WA_DeleteOnClose); dialog->setWindowTitle(tr("Sub-Store 订阅与组合")); dialog->resize(760, 570);
    auto *layout = new QVBoxLayout(dialog); auto *form = new QFormLayout; layout->addLayout(form);
    auto *base = new QLineEdit(m_store.settings()["substoreUrl"].toString("http://127.0.0.1:3001")); base->setObjectName("substore-service-url"); form->addRow(tr("已有 Sub-Store 服务地址"), base);
    auto *token = new QLineEdit; token->setEchoMode(QLineEdit::Password); form->addRow(tr("Bearer 令牌（可选，本次会话）"), token);
    auto *proxy = new QCheckBox(tr("远程服务通过当前本机代理访问")); proxy->setChecked(true); form->addRow(proxy);
    auto *note = new QLabel(tr("连接你已有的 Sub-Store 服务，可管理订阅/组合并导入转换结果。不会安装、启动新服务，也不会切换当前代理。")); note->setWordWrap(true); layout->addWidget(note);
    auto *table = new QTableWidget; table->setObjectName("substore-items"); table->setColumnCount(3); table->setHorizontalHeaderLabels({tr("类型"), tr("名称"), tr("成员数")}); table->setSelectionBehavior(QAbstractItemView::SelectRows); table->setSelectionMode(QAbstractItemView::SingleSelection); table->setEditTriggers(QAbstractItemView::NoEditTriggers); table->horizontalHeader()->setStretchLastSection(true); layout->addWidget(table, 1);
    auto *status = new QLabel; status->setWordWrap(true); status->setTextFormat(Qt::PlainText); layout->addWidget(status);
    using Done = std::function<void(QByteArray, QString)>;
    auto request = [this, dialog, base, token, proxy, page](const QByteArray &method, const QString &path, const QByteArray &body, Done done) {
        QUrl url(base->text().trimmed());
        if (!url.isValid() || url.host().isEmpty() || !url.userInfo().isEmpty() || url.hasQuery() || url.hasFragment() || (url.scheme() != "https" && !localController(url.toString()))) { done({}, tr("服务地址须为本机 HTTP 或远程 HTTPS，无嵌入凭据/查询参数")); return; }
        url.setPath(url.path().replace(QRegularExpression("/+$"), "") + path.section('?', 0, 0)); if (path.contains('?')) url.setQuery(path.section('?', 1));
        auto *manager = new QNetworkAccessManager(dialog);
        const bool loopback = QStringList{"127.0.0.1", "localhost", "::1"}.contains(url.host()); const int port = localProxyPort(m_store);
        manager->setProxy(!loopback && proxy->isChecked() && port > 0 ? QNetworkProxy(QNetworkProxy::HttpProxy, "127.0.0.1", quint16(port)) : QNetworkProxy(QNetworkProxy::NoProxy));
        QNetworkRequest req(url); req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json"); req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
        if (!token->text().isEmpty()) req.setRawHeader("Authorization", "Bearer " + token->text().toUtf8());
        auto *reply = manager->sendCustomRequest(req, method, body); QTimer::singleShot(30000, reply, &QNetworkReply::abort);
        connect(reply, &QNetworkReply::readyRead, dialog, [reply] { if (reply->bytesAvailable() > 8 * 1024 * 1024) reply->abort(); });
        connect(reply, &QNetworkReply::finished, dialog, scopedCallback(std::function<void()>([reply, manager, done] {
            const auto bytes = reply->readAll(); const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const auto object = QJsonDocument::fromJson(bytes).object();
            const auto error = reply->error() != QNetworkReply::NoError || code < 200 || code >= 300 ? QObject::tr("Sub-Store 请求失败（HTTP %1）：%2").arg(code).arg(reply->errorString()) :
                (object["status"].toString() == "failed" || object["status"].toString() == "error" ? QString::fromUtf8(QJsonDocument(object["error"].toObject()).toJson(QJsonDocument::Compact)) : QString());
            manager->deleteLater(); done(bytes, error);
        }), page));
    };
    auto loaded = QSharedPointer<QString>::create();
    auto refresh = [this, dialog, table, status, base, token, request, loaded] {
        table->setRowCount(0); *loaded = base->text().trimmed() + '\n' + token->text(); status->setText(tr("正在读取服务…"));
        const auto identity = *loaded; auto remaining = QSharedPointer<int>::create(2);
        for (const auto &kind : {QString("sub"), QString("collection")}) request("GET", "/api/" + (kind == "sub" ? QString("subs") : QString("collections")), {}, [this, dialog, table, status, kind, remaining, loaded, identity, base, token](QByteArray bytes, QString error) {
            Q_UNUSED(dialog)
            if (*loaded != identity || identity != base->text().trimmed() + '\n' + token->text()) return;
            if (!error.isEmpty()) { status->setText(error); loaded->clear(); return; }
            const auto root = QJsonDocument::fromJson(bytes).object(); auto data = root["data"];
            QJsonArray entries = data.toArray(); if (data.isObject()) for (const auto &value : data.toObject()) entries.append(value);
            for (const auto &entry : entries) {
                auto item = entry.toObject(); const int row = table->rowCount(); table->insertRow(row); item["_kind"] = kind;
                auto *type = new QTableWidgetItem(kind == "sub" ? tr("订阅") : tr("组合")); type->setData(Qt::UserRole, item); table->setItem(row, 0, type);
                table->setItem(row, 1, new QTableWidgetItem(item["name"].toString())); table->setItem(row, 2, new QTableWidgetItem(kind == "collection" ? QString::number(item["subscriptions"].toArray().size()) : QString()));
            }
            if (--*remaining == 0) { status->setText(tr("已加载 %1 项").arg(table->rowCount())); auto settings = m_store.settings(); settings["substoreUrl"] = base->text().trimmed(); m_store.setSettings(settings); }
        });
    };
    auto selection = [table, loaded, base, token]() { const int row = table->currentRow(); return row >= 0 && *loaded == base->text().trimmed() + '\n' + token->text() ? table->item(row, 0)->data(Qt::UserRole).toJsonObject() : QJsonObject(); };
    auto *row = new QHBoxLayout; layout->addLayout(row);
    auto button = [&](const QString &text, std::function<void()> action) { auto *b = new QPushButton(text); row->addWidget(b); connect(b, &QPushButton::clicked, dialog, scopedCallback(std::move(action), page)); return b; };
    button(tr("刷新列表"), refresh)->setObjectName("substore-refresh");
    auto edit = [this, dialog, selection, request, status, refresh](bool create) {
        auto item = selection(); if (!create && item.isEmpty()) { status->setText(tr("请先刷新并选择一项")); return; }
        const auto oldName = item["name"].toString(); QString kind = item["_kind"].toString(); item.remove("_kind");
        if (create) {
            bool ok = false; const auto type = QInputDialog::getItem(dialog, tr("新增类型"), tr("类型"), {tr("订阅"), tr("组合")}, 0, false, &ok); if (!ok) return;
            kind = type == tr("订阅") ? "sub" : "collection";
            item = {{"name", "new-subscription"}, {"process", QJsonArray()}};
            if (kind == "sub") { item["source"] = "remote"; item["url"] = "https://example.com/subscription"; }
            else item["subscriptions"] = QJsonArray();
        }
        editDocument(tr("Sub-Store JSON 配置"), QJsonDocument(item).toJson(), [request, status, refresh, create, kind, oldName](QByteArray bytes) {
            const auto object = QJsonDocument::fromJson(bytes).object();
            if (object["name"].toString().isEmpty() || object["name"].toString().contains('/') || (kind == "collection" && !object["subscriptions"].isArray())) throw std::runtime_error("请填写有效名称，组合需要 subscriptions 数组");
            const auto path = create ? "/api/" + (kind == "sub" ? QString("subs") : QString("collections")) : "/api/" + kind + '/' + Api::segment(oldName);
            request(create ? QByteArray("POST") : QByteArray("PATCH"), path, QJsonDocument(object).toJson(), [status, refresh](QByteArray, QString error) { if (error.isEmpty()) refresh(); else status->setText(error); });
        });
    };
    button(tr("新增"), [edit] { edit(true); }); button(tr("编辑"), [edit] { edit(false); });
    button(tr("删除"), [dialog, selection, request, status, refresh] {
        const auto item = selection(); if (item.isEmpty()) return;
        if (QMessageBox::question(dialog, QObject::tr("删除 Sub-Store 项目"), QObject::tr("删除服务中的 %1？").arg(item["name"].toString())) != QMessageBox::Yes) return;
        request("DELETE", "/api/" + item["_kind"].toString() + '/' + Api::segment(item["name"].toString()), {}, [status, refresh](QByteArray, QString error) { if (error.isEmpty()) refresh(); else status->setText(error); });
    });
    button(tr("转换并导入"), [this, selection, request, status, base, token, proxy] {
        const auto item = selection(); if (item.isEmpty()) { status->setText(tr("请先刷新并选择一项")); return; }
        const auto path = "/download/" + item["_kind"].toString() + '/' + Api::segment(item["name"].toString()) + "?target=ClashMeta";
        QUrl source(base->text().trimmed()); source.setPath(source.path().replace(QRegularExpression("/+$"), "") + path.section('?', 0, 0)); source.setQuery("target=ClashMeta");
        const auto auth = token->text(); const bool useProxy = proxy->isChecked();
        request("GET", path, {}, [this, status, item, source, auth, useProxy](QByteArray bytes, QString error) {
            if (!error.isEmpty()) { status->setText(error); return; }
            QJsonObject metadata{{"useProxy", useProxy}}; if (!auth.isEmpty()) metadata["authToken"] = "Bearer " + auth;
            saveProfileCandidate(item["name"].toString(), source.toString(), bytes, {}, metadata); status->setText(tr("转换结果已送交本地完整校验，导入结果见主界面提示"));
        });
    })->setObjectName("substore-import"); dialog->show();
}
}
