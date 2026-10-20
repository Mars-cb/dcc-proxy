#include "panel.h"
#include "nodeflags.h"
#include "nodelist.h"
#include "tun.h"
#include "subscriptionnodes.h"
#include <QCryptographicHash>
#include <DSwitchButton>
#include <DPasswordEdit>
#include <QResizeEvent>
#include <QTextCursor>
#include <QRegularExpression>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QFrame>
#include <QTabBar>
#include <QFormLayout>
#include <QLabel>
#include <QComboBox>
#include <QLineEdit>
#include <QListWidget>
#include <QTableWidget>
#include <QHeaderView>
#include <QPlainTextEdit>
#include <QTabWidget>
#include <QSpinBox>
#include <QCheckBox>
#include <QPushButton>
#include <QFileDialog>
#include <QInputDialog>
#include <QMessageBox>
#include <QDialogButtonBox>
#include <QJsonDocument>
#include <QUrlQuery>
#include <QSignalBlocker>
#include <QProcess>
#include <QFile>
#include <QDir>
#include <QDateTime>
#include <QElapsedTimer>
#include <QScrollArea>
#include <QStandardPaths>
#include <QApplication>
#include <QPalette>
#include <QNetworkProxy>
#include <QSharedPointer>
#include <QScrollBar>
#include <algorithm>

namespace Party {
class MessageArea : public QScrollArea {
public:
    explicit MessageArea(QLabel *label) : m_label(label) {
        setObjectName("proxy-message-area"); setFrameShape(QFrame::NoFrame);
        setWidgetResizable(true); setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        m_label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        m_label->setAlignment(Qt::AlignLeft | Qt::AlignTop); setWidget(m_label);
        setAutoFillBackground(false); viewport()->setAutoFillBackground(false); m_label->setAutoFillBackground(false);
        fit();
    }
    void fit() {
        const int line = m_label->fontMetrics().lineSpacing();
        const int height = qBound(line + 4, m_label->heightForWidth(qMax(100, viewport()->width() - 20)) + 4, line * 3 + 8);
        if (this->height() != height) setFixedHeight(height);
        m_label->setToolTip(m_label->text());
    }
protected:
    void resizeEvent(QResizeEvent *event) override { QScrollArea::resizeEvent(event); fit(); }
private:
    QLabel *m_label;
};
static QString sectionHelp(const QString &section)
{
    static const QMap<QString, QString> help{
        {"dns", QObject::tr("DNS：设置域名解析服务器。示例使用系统 DNS，通常无需单独开放 DNS 监听端口。nameserver 是解析器列表；启用 TUN 时会联动启用 DNS。")},
        {"sniffer", QObject::tr("嗅探：从 HTTP/TLS 流量中识别域名，用于域名规则匹配。示例默认关闭，将 enable 改为 true 后启用；HTTP 80/8080、TLS 443 是待识别的目标端口。")},
        {"hosts", QObject::tr("Hosts：为域名指定固定 IP。示例将 example.test 指向文档专用地址 192.0.2.10，请替换为你实际需要的域名和 IP。")},
        {"listeners", QObject::tr("监听器：新增独立代理入口。示例仅在本机 127.0.0.1:17892 开放 HTTP 代理，使用 DIRECT 策略；端口必须空闲，可按需改为已有策略组名。常用混合/HTTP/SOCKS 端口也可直接在设置页修改。")},
        {"tunnels", QObject::tr("隧道：把本地端口转发到指定目标。示例把本机 TCP 18081 转发到 127.0.0.1:18080，使用 DIRECT；目标端口需有实际服务。格式为 协议,本地监听,目标地址,策略。")}
    };
    return help.value(section) + QObject::tr("\n只修改当前订阅的该部分；保存后验证并重新加载，取消不生效。YAML 注意缩进；覆写和专用设置可能覆盖这里的值。");
}
static QByteArray sectionExample(const QString &section)
{
    static const QMap<QString, QByteArray> examples{
        {"dns", "dns:\n  enable: true\n  enhanced-mode: redir-host\n  nameserver:\n    - system\n"},
        {"sniffer", "sniffer:\n  enable: false\n  sniff:\n    HTTP:\n      ports: [80, 8080]\n      override-destination: true\n    TLS:\n      ports: [443]\n"},
        {"hosts", "hosts:\n  example.test: 192.0.2.10\n"},
        {"listeners", "listeners:\n  - name: local-http-example\n    type: http\n    listen: 127.0.0.1\n    port: 17892\n    proxy: DIRECT\n"},
        {"tunnels", "tunnels:\n  - tcp,127.0.0.1:18081,127.0.0.1:18080,DIRECT\n"}
    };
    return examples.value(section);
}
static QByteArray yamlOverrideExample()
{
    return "# 映射递归合并；列表整体替换；null 删除字段。\n"
           "# 此示例添加 hosts 映射并关闭局域网访问，不替换订阅节点和规则。\n"
           "allow-lan: false\nhosts:\n  example.test: 192.0.2.10\n"
           "# DNS 列表示例（取消注释后会替换原 nameserver 列表）：\n"
           "# dns:\n#   nameserver:\n#     - 10.20.0.10\n#     - system\n";
}
static void labelText(QLabel *label, const QString &text)
{
    if (label->text() != text) {
        label->setText(text);
        if (label->parentWidget() && label->parentWidget()->parentWidget())
            if (auto *area = dynamic_cast<MessageArea *>(label->parentWidget()->parentWidget())) area->fit();
    }
}
static QStringList textEntries(const QString &text)
{
    return text.split(QRegularExpression("[\\n\\r,;，；]"), QString::SkipEmptyParts);
}
static QAbstractButton *nativeSwitch(const QString &name)
{
    auto *control = new Dtk::Widget::DSwitchButton;
    control->setObjectName(name);
    return control;
}

static QPushButton *button(const QString &text, QHBoxLayout *row, QObject *context, std::function<void()> action)
{
    auto *b = new QPushButton(text);
    row->addWidget(b);
    QObject::connect(b, &QPushButton::clicked, context, std::move(action));
    return b;
}
static QTableWidget *table(const QStringList &headers, QVBoxLayout *layout)
{
    auto *t = new QTableWidget;
    t->setColumnCount(headers.size());
    t->setHorizontalHeaderLabels(headers);
    t->setSelectionBehavior(QAbstractItemView::SelectRows);
    t->setSelectionMode(QAbstractItemView::SingleSelection);
    t->setEditTriggers(QAbstractItemView::NoEditTriggers);
    t->verticalHeader()->hide();
    t->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    t->horizontalHeader()->setStretchLastSection(true);
    t->setAlternatingRowColors(true);
    layout->addWidget(t, 1);
    return t;
}
static void cell(QTableWidget *t, int r, int c, const QString &text)
{
    auto *item = new QTableWidgetItem(text);
    item->setToolTip(text);
    t->setItem(r, c, item);
}
static QString sizeText(double n)
{
    const QStringList units{"B", "KiB", "MiB", "GiB", "TiB"};
    int i = 0;
    while (n >= 1024 && i < units.size() - 1) { n /= 1024; ++i; }
    return QString::number(n, 'f', i ? 1 : 0) + ' ' + units[i];
}
Panel::Panel(QWidget *parent) : QWidget(parent), m_api(this), m_download(this)
{
    setObjectName("dcc-proxy-panel");
    m_api.setCallbackWrapper([this](Api::Done done) { return scopedReply(std::move(done)); });
    m_download.setProxy(QNetworkProxy::NoProxy);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 12, 16, 16);
    auto *header = new QHBoxLayout;
    auto *icon = new QLabel;
    icon->setPixmap(QIcon(":/dcc-proxy/proxy.svg").pixmap(42, 42));
    header->addWidget(icon);
    auto *title = new QLabel(tr("私人代理"));
    m_pageTitle = title;
    auto font = title->font(); font.setPointSize(font.pointSize() + 4); font.setBold(true); title->setFont(font);
    header->addWidget(title);
    header->addStretch();
    button(tr("刷新"), header, this, [this] { refresh(); })->setObjectName("refresh-proxy-panel");
    layout->addLayout(header);
    m_status = new QLabel;
    m_status->setObjectName("proxy-status");
    m_status->setWordWrap(true);
    m_status->setTextFormat(Qt::PlainText);
    m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_messageArea = new MessageArea(m_status); m_messageArea->hide(); layout->addWidget(m_messageArea);
    m_tabs = new QTabWidget;
    m_tabs->setDocumentMode(true);
    m_tabs->setUsesScrollButtons(true);
    m_tabs->tabBar()->setExpanding(false);
    layout->addWidget(m_tabs, 1);
    m_tabs->addTab(overviewPage(), tr("概览"));
    m_tabs->addTab(proxiesPage(), tr("代理组"));
    m_tabs->addTab(profilesPage(), tr("订阅"));
    m_tabs->addTab(connectionsPage(), tr("连接"));
    m_tabs->addTab(rulesPage(), tr("规则"));
    m_tabs->addTab(providersPage(), tr("资源"));
    auto *logsPage = new QWidget;
    auto *logsLayout = new QVBoxLayout(logsPage);
    auto *logsRow = new QHBoxLayout;
    m_pauseLogs = new QCheckBox(tr("暂停")); m_pauseLogs->setObjectName("pause-logs");
    logsRow->addWidget(m_pauseLogs);
    m_logLevel = new QComboBox; m_logLevel->setObjectName("log-level");
    m_logLevel->addItems({"info", "warning", "error", "debug", "silent"});
    logsRow->addWidget(m_logLevel); logsRow->addStretch(); logsLayout->addLayout(logsRow);
    auto *logActions = new QHBoxLayout;
    button(tr("清空"), logActions, this, [this] { m_logs->clear(); });
    button(tr("导出"), logActions, this, [this] {
        const auto path = QFileDialog::getSaveFileName(this, tr("导出日志"), "mihomo.log");
        if (!path.isEmpty()) guarded([&] { writeFile(path, m_logs->toPlainText().toUtf8()); });
    });
    button(tr("后台日志"), logActions, this, [this] {
        guarded([&] { editDocument(tr("后台日志"), readFile(dataDir() + "/daemon.log"), {}); });
    });
    logActions->addStretch(); logsLayout->addLayout(logActions);
    m_logStatus = new QLabel(tr("显示后台最近记录；实时日志会在新连接产生时追加。")); m_logStatus->setWordWrap(true); logsLayout->addWidget(m_logStatus);
    m_logs = new QPlainTextEdit; m_logs->setObjectName("core-logs");
    m_logs->setPlaceholderText(tr("暂无日志。启动内核并通过本插件访问网页后，新连接会显示在这里。"));
    m_logs->setReadOnly(true); m_logs->setMaximumBlockCount(2000);
    logsLayout->addWidget(m_logs);
    m_tabs->addTab(logsPage, tr("日志"));
    m_tabs->addTab(overridePage(), tr("覆写"));
    m_tabs->addTab(toolsPage(), tr("工具"));
    m_tabs->addTab(settingsPage(), tr("设置"));
    m_tabs->addTab(tunPage(), tr("虚拟网卡"));
    loadTunSettings();
    connect(&m_api, &Api::configurationChanged, this, [this] {
        m_reads.clear(); m_polling = false; m_pageMessages.clear();
        m_providers->setRowCount(0);
        ++m_testEpoch; m_testing.clear(); m_testQueue.clear(); m_testPages.clear(); m_activeTests = 0;
        m_testResults.clear(); m_testErrors.clear(); m_nodeProviders.clear(); m_providerProxyData = {};
        m_testGroup->setEnabled(true); renderNodes();
    });
    loadSettings();
    reloadProfiles();
    connect(&m_refreshTimer, &QTimer::timeout, this, &Panel::refresh);
    m_refreshTimer.setInterval(3000);
    connect(m_tabs, &QTabWidget::currentChanged, this, [this](int index) {
        resetPageStatus();
        if (m_externalNavigation) m_pageTitle->setText(m_tabs->tabText(index));
        emit pageChanged(index);
        if (index == 6) loadLogHistory();
        syncStreams(); refresh();
    });
    connect(m_pauseLogs, &QCheckBox::toggled, this, [this] { syncStreams(); });
    connect(m_logLevel, &QComboBox::currentTextChanged, this, [this] {
        m_api.stopStreams(); syncStreams();
        if (m_connected) patchConfig({{"log-level", m_logLevel->currentText()}});
    });
    connect(&m_api, &Api::streamData, this, [this](const QString &path, const QJsonObject &data) {
        if (path.startsWith("/logs")) {
            labelText(m_logStatus, tr("实时日志已连接，后续连接记录会自动追加。"));
            if (!m_pauseLogs->isChecked()) m_logs->appendPlainText(QDateTime::currentDateTime().toString("HH:mm:ss ") + data["type"].toString() + " " + data["payload"].toString());
        } else if (path == "/traffic") {
            labelText(m_traffic, tr("上传 %1/s    下载 %2/s").arg(sizeText(data["up"].toDouble()), sizeText(data["down"].toDouble())));
        }
    });
    connect(&m_api, &Api::streamError, this, [this](const QString &path, const QString &error) {
        if (path.startsWith("/logs") && isVisible() && m_tabs->currentIndex() == 6 && !m_pauseLogs->isChecked())
            labelText(m_logStatus, tr("实时日志连接中断：%1；将自动重连。后台历史仍可查看。").arg(error));
    });
}
void Panel::guarded(std::function<void()> action)
{
    try { action(); } catch (const std::exception &e) { message(QString::fromUtf8(e.what()), true); }
}
void Panel::message(const QString &text, bool error)
{
    const auto page = notificationPage();
    m_pageMessages[page] = qMakePair(text, error);
    if (!m_tabs || m_tabs->currentIndex() == page) showMessage(text, error);
}
int Panel::notificationPage() const
{
    return m_notificationContext >= 0 ? m_notificationContext : (m_tabs ? m_tabs->currentIndex() : 0);
}
Api::Done Panel::scopedReply(Api::Done done)
{
    return scopedCallback(std::move(done));
}
void Panel::requestService(const QJsonObject &command, QObject *context, Api::Done done)
{
    Party::serviceRequest(command, context, scopedReply(std::move(done)));
}
void Panel::showMessage(const QString &text, bool error)
{
    labelText(m_status, text); m_messageArea->fit();
    m_messageArea->setVisible(!text.isEmpty());
    auto p = m_status->palette();
    p.setColor(QPalette::WindowText, error ? QColor("#d9534f") : palette().color(QPalette::WindowText));
    if (m_status->palette() != p) m_status->setPalette(p);
}
void Panel::resetPageStatus()
{
    const int page = m_tabs->currentIndex();
    if (m_pageMessages.contains(page)) { const auto value = m_pageMessages[page]; showMessage(value.first, value.second); return; }
    if (page == 0 && !m_serviceState["error"].toString().isEmpty()) { showMessage(m_serviceState["error"].toString(), true); return; }
    const bool preview = (page == 1 || page == 4) && m_previewing;
    showMessage(preview ? tr("当前为订阅预览") : QString(), false);
}
QWidget *Panel::overviewPage()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    m_overview = new QLabel(tr("内核离线")); m_overview->setTextFormat(Qt::PlainText); m_overview->setWordWrap(true);
    layout->addWidget(new MessageArea(m_overview));
    m_traffic = new QLabel(tr("上传 —    下载 —")); layout->addWidget(m_traffic);
    m_statistics = new QLabel(tr("连接 —    总上传 —    总下载 —    内存 —"));
    m_statistics->setObjectName("overview-statistics");
    m_statistics->setWordWrap(true); layout->addWidget(m_statistics);
    m_overview->setObjectName("overview-core");
    auto *controls = new QHBoxLayout;
    auto *coreLabel = new QLabel(tr("代理服务")); controls->addWidget(coreLabel);
    m_coreSwitch = nativeSwitch("core-switch"); m_coreSwitch->setAccessibleName(tr("代理服务"));
    controls->addWidget(m_coreSwitch);
    connect(m_coreSwitch, &QAbstractButton::clicked, this, [this](bool on) { runService(on ? "start" : "stop"); });
    m_restart = button(tr("重启服务"), controls, this, [this] { runService("restart-service"); });
    m_restart->setObjectName("restart-proxy-service");
    m_restart->setToolTip(tr("重新加载后台程序并重启 Mihomo 内核，代理连接会短暂中断"));
    controls->addStretch(); layout->addLayout(controls);
    auto *form = new QFormLayout;
    m_mode = new QComboBox; m_mode->setObjectName("proxy-mode");
    m_mode->addItem(tr("规则"), "rule"); m_mode->addItem(tr("全局"), "global"); m_mode->addItem(tr("直连"), "direct");
    form->addRow(tr("代理模式"), m_mode);
    connect(m_mode, QOverload<int>::of(&QComboBox::activated), this, [this](int) { patchConfig({{"mode", m_mode->currentData().toString()}}); });
    layout->addLayout(form);
    auto *advanced = new QGridLayout;
    int sectionIndex = 0;
    for (const auto &section : {"dns", "sniffer", "hosts", "listeners", "tunnels"}) {
        auto *b = new QPushButton(QString::fromLatin1(section).toUpper());
        advanced->addWidget(b, sectionIndex / 3, sectionIndex % 3); ++sectionIndex;
        connect(b, &QPushButton::clicked, this, [this, section] { editSectionForm(QString::fromLatin1(section)); });
    }
    layout->addLayout(advanced);
    auto *note = new QLabel(tr("关闭控制中心后，后台内核会继续运行。虚拟网卡请在“虚拟网卡”页面设置。"));
    note->setWordWrap(true); layout->addWidget(note);
    layout->addStretch();
    return page;
}
QWidget *Panel::tunPage()
{
    auto *scroll = new QScrollArea; scroll->setWidgetResizable(true); scroll->setFrameShape(QFrame::NoFrame);
    auto *page = new QWidget; scroll->setWidget(page); auto *layout = new QVBoxLayout(page);
    m_tunStatus = new QLabel(tr("虚拟网卡未启用")); m_tunStatus->setWordWrap(true); m_tunStatus->setTextFormat(Qt::PlainText); layout->addWidget(m_tunStatus);
    auto *form = new QFormLayout;
    m_tunPageSwitch = nativeSwitch("tun-page-enable"); form->addRow(tr("启用虚拟网卡"), m_tunPageSwitch);
    connect(m_tunPageSwitch, &QAbstractButton::clicked, this, [this](bool on) { setTunOptions({{"enable", on}}); });
    m_tunDevice = new QLineEdit; m_tunDevice->setObjectName("tun-device"); form->addRow(tr("网卡名称"), m_tunDevice);
    m_tunStack = new QComboBox; m_tunStack->setObjectName("tun-stack");
    for (const auto &stack : {"mixed", "gvisor", "system", "mips"}) m_tunStack->addItem(stack, stack);
    form->addRow(tr("协议栈"), m_tunStack);
    const QList<QPair<QString, QString>> flags{{"auto-route", tr("自动设置路由")}, {"auto-redirect", tr("自动重定向（Linux）")}, {"auto-detect-interface", tr("自动检测出口网卡")}, {"strict-route", tr("严格路由")}};
    for (const auto &flag : flags) { auto *control = nativeSwitch("tun-" + flag.first); m_tunFlags.insert(flag.first, control); form->addRow(flag.second, control); }
    m_tunMtu = new QSpinBox; m_tunMtu->setObjectName("tun-mtu"); m_tunMtu->setRange(1, 65535); form->addRow("MTU", m_tunMtu);
    m_tunDns = new QLineEdit; m_tunDns->setObjectName("tun-dns-hijack"); m_tunDns->setPlaceholderText("any:53"); form->addRow(tr("DNS 劫持（逗号分隔）"), m_tunDns);
    m_tunExclude = new QPlainTextEdit; m_tunExclude->setObjectName("tun-route-exclude"); m_tunExclude->setMaximumHeight(120); m_tunExclude->setPlaceholderText("192.168.0.0/16\nfc00::/7"); form->addRow(tr("路由排除网段（CIDR）"), m_tunExclude);
    layout->addLayout(form);
    auto *row = new QHBoxLayout;
    button(tr("保存并应用"), row, this, [this] {
        QJsonObject options{{"device", m_tunDevice->text()}, {"stack", m_tunStack->currentData().toString()}, {"mtu", m_tunMtu->value()}, {"dns-hijack", QJsonArray::fromStringList(textEntries(m_tunDns->text()))}, {"route-exclude-address", QJsonArray::fromStringList(textEntries(m_tunExclude->toPlainText()))}};
        for (auto it = m_tunFlags.begin(); it != m_tunFlags.end(); ++it) options[it.key()] = it.value()->isChecked();
        // The daemon preserves its current enable value when a settings-only patch omits it.
        setTunOptions(options);
    })->setObjectName("save-tun");
    button(tr("内核授权"), row, this, [this] {
        if (m_tunBusy) return;
        guarded([this] {
            if (!localController(m_store.settings()["endpoint"].toString())) { message(tr("远程控制器模式下不能为本机内核授权"), true); return; }
            const auto core = m_store.settings()["core"].toString();
            if (hasTunPermission(core)) { message(tr("内核文件已授权；首次启用时会自动重启未获得权限的内核进程")); return; }
            m_tunBusy = true; m_tunPageSwitch->setEnabled(false);
            requestTunPermission(core, this, scopedCallback(std::function<void(QString)>([this](QString error) { m_tunBusy = false; message(error.isEmpty() ? tr("内核授权成功，可启用虚拟网卡") : error, !error.isEmpty()); refresh(); })));
        });
    })->setObjectName("authorize-tun");
    row->addStretch(); layout->addLayout(row);
    auto *hint = new QLabel(tr("启用时会联动开启内核 DNS，关闭虚拟网卡后保留 DNS。自动路由开启后，应用无需设置系统代理；系统代理绕过请通过 DIRECT 规则或路由排除配置。修改后点击保存并应用。")); hint->setWordWrap(true); layout->addWidget(hint); layout->addStretch();
    return scroll;
}
void Panel::loadTunSettings()
{
    guarded([this] {
        auto tun = defaultTunConfig(); const auto path = dataDir() + "/controlled.yaml";
        if (QFile::exists(path)) { const auto config = parseConfig(readFile(path)); if (config["tun"]) tun = mergeYaml(tun, config["tun"]); }
        const auto options = yamlToJson(tun).toObject();
        m_tunDevice->setText(options["device"].toString()); m_tunStack->setCurrentIndex(m_tunStack->findData(options["stack"].toString())); m_tunMtu->setValue(options["mtu"].toInt(1500));
        for (auto it = m_tunFlags.begin(); it != m_tunFlags.end(); ++it) it.value()->setChecked(options[it.key()].toBool());
        auto entries = [](const QJsonValue &v) { QStringList out; for (const auto &entry : v.toArray()) out.append(entry.toString()); return out; };
        m_tunDns->setText(entries(options["dns-hijack"]).join(", ")); m_tunExclude->setPlainText(entries(options["route-exclude-address"]).join('\n'));
    });
}
void Panel::updateSwitches(const QJsonObject &state)
{
    m_serviceState = state;
    if (m_backendVersion) m_backendVersion->setText(tr("界面 %1 · 后台 %2%3").arg(QStringLiteral(DCC_PROXY_VERSION), state["version"].toString(tr("旧版/未知")), state["version"].toString() == QStringLiteral(DCC_PROXY_VERSION) ? QString() : tr("（待正常启动加载新版，未自动重启）")));
    const bool local = localController(m_store.settings()["endpoint"].toString());
    const bool busy = state["busy"].toBool() || m_coreBusy;
    if (!m_coreBusy) {
        const QSignalBlocker blocker(m_coreSwitch); m_coreSwitch->setChecked(local && state["running"].toBool()); m_coreSwitch->setEnabled(!busy && local); m_restart->setEnabled(!busy && local);
    }
    if (!m_tunBusy) {
        const auto tun = state["tun"].toObject();
        { const QSignalBlocker blocker(m_tunPageSwitch); m_tunPageSwitch->setChecked(tun["enable"].toBool()); m_tunPageSwitch->setEnabled(!busy && local); }
        labelText(m_tunStatus, tun["enable"].toBool() ? tr("虚拟网卡 %1 已启用").arg(tun["device"].toString()) : tr("虚拟网卡未启用"));
    }
}
void Panel::setTunOptions(const QJsonObject &options)
{
    if (m_tunBusy || m_coreBusy) return;
    if (!localController(m_store.settings()["endpoint"].toString())) { message(tr("远程控制器模式下不能修改本机 TUN"), true); return; }
    guarded([this, options] {
        const auto validated = validateTunOptions(options);
        m_tunBusy = true; m_tunPageSwitch->setEnabled(false);
        auto apply = [this, validated](QString error) {
            if (!error.isEmpty()) { m_tunBusy = false; message(error, true); refresh(); return; }
            backgroundCommand({{"action", "tun"}, {"options", validated}}, [this](const QJsonValue &state, const QString &) {
                m_tunBusy = false;
                if (state.toObject().contains("running")) updateSwitches(state.toObject());
                loadTunSettings();
            });
        };
        if (validated["enable"].toBool() && !hasTunPermission(m_store.settings()["core"].toString())) {
            message(tr("启用虚拟网卡需要管理员授权内核，请完成系统认证。")); requestTunPermission(m_store.settings()["core"].toString(), this, scopedCallback(std::function<void(QString)>(apply)));
        } else apply({});
    });
}

QWidget *Panel::proxiesPage()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    m_nodeSource = new QComboBox; m_nodeSource->setObjectName("node-subscription-source");
    m_nodeSource->addItem(tr("自动（运行时显示实时节点）"), QString()); layout->addWidget(m_nodeSource);
    m_nodeHint = new QLabel; m_nodeHint->setObjectName("node-source-status"); m_nodeHint->setWordWrap(true); layout->addWidget(m_nodeHint);
    connect(m_nodeSource, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this] {
        ++m_testEpoch; m_testing.clear(); m_testQueue.clear(); m_activeTests = 0;
        m_testResults.clear(); m_testErrors.clear(); refreshProxies();
    });
    m_group = new QComboBox; m_group->setObjectName("proxy-group"); layout->addWidget(m_group);
    auto *filters = new QHBoxLayout;
    m_nodeSearch = new QLineEdit; m_nodeSearch->setObjectName("node-search"); m_nodeSearch->setPlaceholderText(tr("搜索节点")); filters->addWidget(m_nodeSearch, 1);
    m_sort = new QComboBox; m_sort->addItems({tr("配置顺序"), tr("延迟排序"), tr("名称排序")}); filters->addWidget(m_sort);
    m_hideUnavailable = new QCheckBox(tr("隐藏不可用")); filters->addWidget(m_hideUnavailable);
    layout->addLayout(filters);
    m_nodes = new NodeList; m_nodes->setObjectName("proxy-nodes"); layout->addWidget(m_nodes, 1);
    auto *test = new QHBoxLayout;
    m_testUrl = new QLineEdit; m_testUrl->setPlaceholderText(tr("留空使用策略组地址或 https://www.gstatic.com/generate_204")); m_testUrl->setObjectName("node-test-url"); test->addWidget(m_testUrl, 1);
    m_testTimeout = new QSpinBox; m_testTimeout->setObjectName("node-test-timeout"); m_testTimeout->setRange(1, 30000); m_testTimeout->setValue(5000); m_testTimeout->setSuffix(" ms"); m_testTimeout->setToolTip(tr("每个节点的测速超时")); test->addWidget(m_testTimeout);
    m_testConcurrency = new QSpinBox; m_testConcurrency->setObjectName("node-test-concurrency"); m_testConcurrency->setRange(1, 256); m_testConcurrency->setValue(50); m_testConcurrency->setSuffix(tr(" 并发")); test->addWidget(m_testConcurrency);
    m_testGroup = button(tr("测整个策略组"), test, this, [this] { testNodes(true); }); m_testGroup->setObjectName("test-proxy-group");
    layout->addLayout(test);
    auto *actions = new QHBoxLayout;
    m_restoreAutomatic = button(tr("恢复自动选择"), actions, this, [this] {
        if (m_previewing || !m_connected) return;
        const QString group = m_group->currentData().toString();
        if (group.isEmpty()) return;
        m_api.request("DELETE", "/proxies/" + Api::segment(group), {}, [this](const QJsonValue &, const QString &error) {
            if (!error.isEmpty()) message(error, true); else refreshProxies();
        });
    });
    actions->addStretch(); layout->addLayout(actions);
    connect(m_group, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this] { renderNodes(); });
    connect(m_nodeSearch, &QLineEdit::textChanged, this, [this] { renderNodes(); });
    connect(m_sort, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this] { renderNodes(); });
    connect(m_hideUnavailable, &QCheckBox::toggled, this, [this] { renderNodes(); });
    connect(m_nodes, &QListWidget::itemClicked, this, [this] { selectNode(); });
    connect(m_nodes, &QListWidget::itemActivated, this, [this] { selectNode(); });
    connect(m_nodes, &NodeList::delayClicked, this, [this](const QString &name) { testNodes(false, name); });
    m_nodeSource->hide(); m_nodeHint->hide(); m_group->hide(); m_nodes->hide();
    m_testGroup->hide(); m_restoreAutomatic->hide();
    layout->insertWidget(4, groupsPage(), 1);
    return page;
}
QWidget *Panel::groupsPage()
{
    auto *page = new QWidget; auto *layout = new QVBoxLayout(page);
    m_groupsSource = new QComboBox; m_groupsSource->setObjectName("group-subscription-source"); layout->addWidget(m_groupsSource);
    m_groupsHint = new QLabel; m_groupsHint->setObjectName("group-source-status"); m_groupsHint->setWordWrap(true); layout->addWidget(m_groupsHint);
    auto *help = new QLabel(tr("按订阅保留原有策略组。点击组名展开节点，点击卡片切换此组，点击延迟单独测速。规则模式按匹配的策略组分流；切换主节点不会覆盖其他组的独立选择。箭头显示实际出口路径。"));
    help->setWordWrap(true); layout->addWidget(help);
    auto *scroll = new QScrollArea; scroll->setObjectName("proxy-groups-scroll"); scroll->setWidgetResizable(true); scroll->setFrameShape(QFrame::NoFrame);
    auto *body = new QWidget; m_groupsLayout = new QVBoxLayout(body); m_groupsLayout->setContentsMargins(0, 0, 0, 0); m_groupsLayout->addStretch(); scroll->setWidget(body); layout->addWidget(scroll, 1);
    connect(m_groupsSource, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this] {
        const int index = m_nodeSource->findData(m_groupsSource->currentData());
        if (index >= 0) m_nodeSource->setCurrentIndex(index);
    });
    return page;
}
void Panel::renderGroups()
{
    if (!m_groupsLayout) return;
    const auto selectedSource = m_nodeSource->currentData().toString();
    const auto scope = selectedSource.isEmpty() ? m_store.settings()["active"].toString() : selectedSource;
    const auto names = proxyGroupNames(m_proxyData);
    if (scope != m_groupScope || names != m_groupOrder) {
        for (const auto &card : m_groupCards) delete card.frame;
        m_groupCards.clear(); m_groupOrder = names; m_groupScope = scope;
        const auto saved = m_store.settings()["expandedProxyGroups"].toObject()[scope].toArray();
        for (const auto &name : names) {
            auto *frame = new QFrame; frame->setFrameShape(QFrame::StyledPanel); frame->setProperty("proxyGroup", name);
            auto *layout = new QVBoxLayout(frame); auto *row = new QHBoxLayout;
            auto *header = new QPushButton; header->setCheckable(true); header->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed); header->setObjectName("proxy-group-header"); header->setProperty("proxyGroup", name); row->addWidget(header, 1);
            auto *automatic = new QPushButton(tr("恢复自动")); automatic->setObjectName("automatic-policy-group"); row->addWidget(automatic);
            auto *test = new QPushButton(tr("测速")); test->setObjectName("test-policy-group"); test->setProperty("proxyGroup", name); row->addWidget(test); layout->addLayout(row);
            auto *route = new QLabel; route->setWordWrap(true); route->setTextFormat(Qt::PlainText); route->setTextInteractionFlags(Qt::TextSelectableByMouse); route->setObjectName("proxy-group-route"); layout->addWidget(route);
            auto *nodes = new NodeList; nodes->setObjectName("policy-group-nodes"); nodes->setProperty("proxyGroup", name); nodes->setMinimumWidth(0); layout->addWidget(nodes);
            const bool expanded = saved.contains(name) || (!m_store.settings()["expandedProxyGroups"].toObject().contains(scope) && name == names.value(0)); header->setChecked(expanded); nodes->setVisible(expanded);
            m_groupCards.insert(name, {frame, header, route, test, automatic, nodes}); m_groupsLayout->insertWidget(m_groupsLayout->count() - 1, frame);
            connect(header, &QPushButton::toggled, this, [this, name, scope, nodes](bool expanded) {
                nodes->setVisible(expanded);
                if (expanded) { const int index = m_group->findData(name); if (index >= 0) m_group->setCurrentIndex(index); }
                guarded([&] {
                    auto settings = m_store.settings(); auto groups = settings["expandedProxyGroups"].toObject(); auto saved = groups[scope].toArray();
                    if (expanded && !saved.contains(name)) saved.append(name);
                    if (!expanded) for (int i = saved.size() - 1; i >= 0; --i) if (saved[i].toString() == name) saved.removeAt(i);
                    groups[scope] = saved; settings["expandedProxyGroups"] = groups; m_store.setSettings(settings);
                });
                renderGroups();
            });
            connect(nodes, &QListWidget::itemClicked, this, [this, name](QListWidgetItem *item) { chooseNode(name, item->data(Qt::UserRole).toString()); });
            connect(nodes, &QListWidget::itemActivated, this, [this, name](QListWidgetItem *item) { chooseNode(name, item->data(Qt::UserRole).toString()); });
            connect(nodes, &NodeList::delayClicked, this, [this, name](const QString &node) { testNodes(false, node, name); });
            connect(test, &QPushButton::clicked, this, [this, name] { testNodes(true, {}, name); });
            connect(automatic, &QPushButton::clicked, this, [this, name] {
                if (m_previewing || !m_connected) return;
                m_api.request("DELETE", "/proxies/" + Api::segment(name), {}, [this](const QJsonValue &, const QString &error) {
                    if (!error.isEmpty()) message(error, true); else refreshProxies();
                });
            });
        }
    }
    QString subscription;
    for (const auto &v : m_store.profiles()) if (v.toObject()["id"].toString() == scope) subscription = v.toObject()["name"].toString();
    labelText(m_groupsHint, (m_previewing ? tr("订阅预览：%1 · 未连接时也可查看分组；启动该配置后可切换和测速。") : tr("当前订阅：%1 · 实时出口" )).arg(subscription.isEmpty() ? tr("外部内核") : subscription));
    for (const auto &name : names) {
        const auto card = m_groupCards.value(name); const auto info = m_proxyData[name].toObject();
        const auto title = QString(card.header->isChecked() ? "▾  " : "▸  ") + info["displayName"].toString(name) + "  ·  " + info["type"].toString() + tr("  ·  %1 个").arg(info["all"].toArray().size());
        if (card.header->text() != title) card.header->setText(title);
        card.header->setToolTip(title);
        labelText(card.route, proxyGroupRoute(m_proxyData, name));
        card.test->setEnabled(m_connected && !m_previewing);
        card.automatic->setVisible(info["type"].toString() == "URLTest" || info["type"].toString() == "Fallback");
        card.automatic->setEnabled(m_connected && !m_previewing);
        if (card.header->isChecked()) {
            renderNodeList(card.nodes, name);
            const int columns = qMax(1, card.nodes->viewport()->width() / qMax(1, card.nodes->gridSize().width()));
            card.nodes->setFixedHeight(qBound(100, ((card.nodes->count() + columns - 1) / columns) * 88 + 8, 360));
        }
    }
}
void Panel::reloadNodeSources()
{
    const QSignalBlocker blocker(m_nodeSource);
    const auto current = m_nodeSource->currentData().toString();
    const auto profiles = m_store.profiles();
    bool same = m_nodeSource->count() == profiles.size() + 1;
    for (int i = 0; same && i < profiles.size(); ++i)
        same = m_nodeSource->itemData(i + 1).toString() == profiles[i].toObject()["id"].toString();
    if (!same) { m_nodeSource->clear(); m_nodeSource->addItem(tr("自动（运行时显示实时节点）"), QString()); }
    for (int i = 0; i < profiles.size(); ++i) {
        const auto profile = profiles[i].toObject(); const auto label = tr("订阅预览：%1").arg(profile["name"].toString());
        if (!same) m_nodeSource->addItem(label, profile["id"].toString());
        else if (m_nodeSource->itemText(i + 1) != label) m_nodeSource->setItemText(i + 1, label);
    }
    const int index = m_nodeSource->findData(current); m_nodeSource->setCurrentIndex(index >= 0 ? index : 0);
    {
        const QSignalBlocker block(m_groupsSource); m_groupsSource->clear();
        for (int i = 0; i < m_nodeSource->count(); ++i) m_groupsSource->addItem(m_nodeSource->itemText(i), m_nodeSource->itemData(i));
        m_groupsSource->setCurrentIndex(m_nodeSource->currentIndex());
    }
    if (!m_connected || !m_nodeSource->currentData().toString().isEmpty()) previewProfile(m_nodeSource->currentData().toString());
}
void Panel::showProfileNodes(const QString &id)
{
    const QSignalBlocker blocker(m_nodeSource);
    const int index = m_nodeSource->findData(id);
    if (index >= 0) m_nodeSource->setCurrentIndex(index);
    previewProfile(id);
}
void Panel::previewProfile(const QString &requested)
{
    guarded([this, requested] {
        const auto profiles = m_store.profiles(); QString id = requested;
        if (id.isEmpty()) id = m_store.settings()["active"].toString();
        if (id.isEmpty() && !profiles.isEmpty()) id = profiles.first().toObject()["id"].toString();
        QJsonObject profile;
        for (const auto &value : profiles) if (value.toObject()["id"].toString() == id) { profile = value.toObject(); break; }
        if (profile.isEmpty() && !profiles.isEmpty() && requested.isEmpty()) { profile = profiles.first().toObject(); id = profile["id"].toString(); }
        if (profile.isEmpty()) {
            m_previewing = true; m_previewFingerprint.clear(); m_proxyData = {};
            resetPageStatus();
            labelText(m_nodeHint, tr("导入订阅后即可查看节点，无需启动内核或开启系统代理。")); updateProxyGroups(); return;
        }
        const auto bytes = m_store.profile(id);
        const auto fingerprint = id.toUtf8() + QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
        if (m_previewing && fingerprint == m_previewFingerprint) { renderNodes(); return; }
        const auto preview = subscriptionNodes(bytes);
        ++m_testEpoch; m_testing.clear(); m_testQueue.clear(); m_activeTests = 0;
        m_testResults.clear(); m_testErrors.clear(); m_nodeProviders.clear(); m_providerProxyData = {};
        m_previewing = true; m_previewFingerprint = fingerprint; m_proxyData = preview.proxies;
        resetPageStatus();
        auto hint = tr("订阅预览：%1 · 启动并使用此配置后可切换节点和测速；无需开启系统代理。").arg(profile["name"].toString());
        if (preview.remoteProviders) hint += tr(" %1 个远程节点提供者需加载后显示。").arg(preview.remoteProviders);
        labelText(m_nodeHint, hint); updateProxyGroups();
    });
}

void Panel::refreshProxies()
{
    { const QSignalBlocker block(m_groupsSource); m_groupsSource->setCurrentIndex(m_groupsSource->findData(m_nodeSource->currentData())); }
    const auto preview = m_nodeSource->currentData().toString();
    if (!preview.isEmpty() || !m_connected) { previewProfile(preview); return; }
    if (m_reads.contains("proxies")) return;
    m_reads.insert("proxies");
    if (!m_reads.contains("proxy-providers")) {
        m_reads.insert("proxy-providers");
        m_api.get("/providers/proxies", [this](const QJsonValue &value, const QString &error) {
            m_reads.remove("proxy-providers");
            if (!error.isEmpty() || !m_nodeSource->currentData().toString().isEmpty() || !m_connected) return;
            m_nodeProviders.clear(); m_providerProxyData = {};
            const auto providers = value.toObject()["providers"].toObject();
            for (auto it = providers.begin(); it != providers.end(); ++it)
                for (const auto &v : it.value().toObject()["proxies"].toArray()) {
                    const auto proxy = v.toObject(); const auto name = proxy["name"].toString();
                    if (!name.isEmpty()) { m_nodeProviders[name] = it.key(); m_providerProxyData[name] = proxy; if (!m_proxyData.contains(name)) m_proxyData[name] = proxy; }
                }
            renderNodes();
        });
    }
    m_api.get("/proxies", [this](const QJsonValue &value, const QString &error) {
        m_reads.remove("proxies");
        if (!m_nodeSource->currentData().toString().isEmpty()) return;
        if (!error.isEmpty()) { m_connected = false; previewProfile(); return; }
        m_previewing = false; m_previewFingerprint.clear();
        resetPageStatus();
        labelText(m_nodeHint, tr("实时节点 · 测速和切换不依赖系统代理开关"));
        m_proxyData = value.toObject()["proxies"].toObject();
        for (auto it = m_providerProxyData.begin(); it != m_providerProxyData.end(); ++it)
            if (!m_proxyData.contains(it.key())) m_proxyData[it.key()] = it.value();
        updateProxyGroups();
    });
}
void Panel::updateProxyGroups()
{
        const auto current = m_group->currentData().toString();
        QSignalBlocker block(m_group);
        const auto groups = proxyGroupNames(m_proxyData);
        bool sameGroups = m_group->count() == groups.size();
        for (int i = 0; sameGroups && i < groups.size(); ++i) sameGroups = m_group->itemData(i).toString() == groups[i];
        if (!sameGroups) m_group->clear();
        for (int i = 0; i < groups.size(); ++i) {
            const auto p = m_proxyData[groups[i]].toObject();
            const auto display = p["displayName"].toString(nodeDisplayName(groups[i]));
            const auto label = display + "  ·  " + p["type"].toString() + (m_previewing ? QString() : "  ·  " + nodeDisplayName(p["now"].toString()));
            if (!sameGroups) m_group->addItem(label, groups[i]);
            else if (m_group->itemText(i) != label) m_group->setItemText(i, label);
        }
        int index = m_group->findData(current);
        m_group->setCurrentIndex(index >= 0 ? index : (m_group->count() ? 0 : -1));
        renderNodes();
        updateWebsiteRule();

}

void Panel::renderNodes()
{
    m_testGroup->setEnabled(m_connected && !m_previewing && m_testing.isEmpty());
    m_restoreAutomatic->setEnabled(m_connected && !m_previewing);
    renderNodeList(m_nodes, m_group->currentData().toString());
    renderGroups();
}
void Panel::renderNodeList(NodeList *list, const QString &groupName)
{
    const auto *selected = list->currentItem();
    const QString selectedName = selected ? selected->data(Qt::UserRole).toString() : QString();
    const auto group = m_proxyData[groupName].toObject();
    struct Node { QString name; int delay; QJsonObject info; };
    QList<Node> nodes;
    for (const auto &value : group["all"].toArray()) {
        const auto name = value.toString();
        const auto info = m_proxyData[name].toObject();
        if (!name.contains(m_nodeSearch->text(), Qt::CaseInsensitive)) continue;
        if (m_hideUnavailable->isChecked() && info.contains("alive") && !info["alive"].toBool()) continue;
        const auto history = info["history"].toArray();
        int delay = history.isEmpty() ? -1 : history.last().toObject()["delay"].toInt();
        if (m_testResults.contains(name)) {
            const auto measured = m_testResults.value(name);
            const auto historyTime = history.isEmpty() ? QDateTime() : QDateTime::fromString(history.last().toObject()["time"].toString(), Qt::ISODateWithMs);
            if (!historyTime.isValid() || historyTime <= measured.first) delay = measured.second;
        }
        nodes.append({name, delay, info});
    }
    if (m_sort->currentIndex() == 1) std::stable_sort(nodes.begin(), nodes.end(), [](const Node &a, const Node &b) {
        return (a.delay > 0 ? a.delay : 1000000) < (b.delay > 0 ? b.delay : 1000000);
    });
    if (m_sort->currentIndex() == 2) std::sort(nodes.begin(), nodes.end(), [](const Node &a, const Node &b) { return a.name.localeAwareCompare(b.name) < 0; });
    bool sameNodes = list->count() == nodes.size();
    for (int row = 0; sameNodes && row < nodes.size(); ++row)
        sameNodes = list->item(row)->data(Qt::UserRole).toString() == nodes[row].name;
    const int scroll = list->verticalScrollBar()->value();
    if (!sameNodes) { list->setUpdatesEnabled(false); list->clear(); }
    for (int row = 0; row < nodes.size(); ++row) {
        const auto &node = nodes[row];
        const bool active = !m_previewing && group["now"].toString() == node.name;
        const bool testing = m_testing.contains(node.name);
        const auto delay = testing ? tr("测速中") : (node.delay < 0 ? tr("未测速") : (node.delay == 0 ? tr("超时") : tr("%1 ms").arg(node.delay)));
        auto *item = sameNodes ? list->item(row) : new QListWidgetItem(QIcon(nodeFlagPath(node.name)), QString(), list);
        item->setText((active ? QString::fromUtf8("✓  ") : "    ") + nodeDisplayName(node.name) + "    " + node.info["type"].toString() + "    " + delay);
        item->setData(Qt::UserRole, node.name);
        item->setData(NodeActiveRole, active);
        item->setData(NodeTypeRole, node.info["type"].toString());
        item->setData(NodeDelayRole, node.delay);
        item->setData(NodeDisplayNameRole, nodeDisplayName(node.name));
        item->setData(NodeTestingRole, testing);
        item->setData(NodeTestErrorRole, m_testErrors.value(node.name));
        item->setToolTip(node.name + "\n" + (m_previewing ? tr("订阅预览；启动并使用该配置后可切换节点和测速，无需开启系统代理。") : tr("点击卡片使用节点；点击延迟或状态测速。")) + (m_testErrors.value(node.name).isEmpty() ? QString() : "\n" + m_testErrors.value(node.name)));
        auto font = item->font(); font.setBold(active); item->setFont(font);
        if (!sameNodes && (node.name == selectedName || (selectedName.isEmpty() && active))) list->setCurrentItem(item);
    }
    if (!sameNodes) {
        list->doItemsLayout(); list->verticalScrollBar()->setValue(scroll); list->setUpdatesEnabled(true);
    }
    list->syncAnimation();
}
void Panel::selectNode()
{
    if (!m_nodes->currentItem()) return;
    chooseNode(m_group->currentData().toString(), m_nodes->currentItem()->data(Qt::UserRole).toString());
}
void Panel::chooseNode(const QString &group, const QString &name)
{
    const int index = m_group->findData(group); if (index >= 0) m_group->setCurrentIndex(index);
    if (m_previewing) { message(tr("当前为订阅预览，启动并使用该配置后可切换节点；无需开启系统代理。")); return; }
    if (!m_connected || m_selecting) return;
    const auto info = m_proxyData[group].toObject();
    if (info["type"].toString() == "Relay" || info["type"].toString() == "LoadBalance" || !info["all"].isArray() || info["now"].toString() == name) return;
    m_selecting = true;
    m_api.request("PUT", "/proxies/" + Api::segment(group), {{"name", name}}, [this, group, name](const QJsonValue &, const QString &error) {
        m_selecting = false;
        if (!error.isEmpty()) { message(tr("节点切换失败：%1").arg(error), true); renderNodes(); return; }
        message(tr("已切换 %1 → %2").arg(group, name));
        refreshProxies();
    });
}
void Panel::testNodes(bool all, const QString &node, const QString &policy)
{
    if (m_previewing) { message(tr("当前为订阅预览，启动并使用该配置后可测速；无需开启系统代理。")); return; }
    if (!m_connected) return;
    const auto policyName = policy.isEmpty() ? m_group->currentData().toString() : policy;
    QString target = m_testUrl->text().trimmed();
    if (target.isEmpty()) {
        target = m_proxyData[policyName].toObject()["testUrl"].toString();
        if (target.isEmpty()) {
            try {
                const auto runtime = parseConfig(m_store.runtime(m_store.settings()["active"].toString()));
                if (runtime["proxy-groups"]) for (const auto &group : runtime["proxy-groups"])
                    if (group["name"] && group["url"] && QString::fromStdString(group["name"].as<std::string>()) == policyName)
                        target = QString::fromStdString(group["url"].as<std::string>());
            } catch (...) { }
        }
    }
    if (target.isEmpty()) target = "https://www.gstatic.com/generate_204";
    const QUrl testUrl(target);
    if (testUrl.scheme() != "https" && testUrl.scheme() != "http") { message(tr("请输入 HTTP 或 HTTPS 测试地址"), true); return; }
    QStringList names;
    if (all) for (const auto &v : m_proxyData[policyName].toObject()["all"].toArray()) names << v.toString();
    else if (!node.isEmpty()) names << node;
    names.removeDuplicates();
    if (names.isEmpty()) return;
    if (m_testing.isEmpty()) { ++m_testEpoch; m_testPassed = 0; m_testFailed = 0; m_testPages.clear(); }
    m_testPages.insert(notificationPage());
    m_testTimeoutMs = m_testTimeout->value();
    guarded([&] {
        auto s = m_store.settings(); s["delayTestUrl"] = m_testUrl->text().trimmed();
        s["delayTestTimeout"] = m_testTimeoutMs; s["delayTestConcurrency"] = m_testConcurrency->value();
        m_store.setSettings(s);
    });
    QUrlQuery query; query.addQueryItem("url", testUrl.toString()); query.addQueryItem("timeout", QString::number(m_testTimeoutMs));
    m_testQuery = query.toString(QUrl::FullyEncoded);
    for (const auto &name : names) {
        if (m_testing.contains(name)) continue;
        m_testing.insert(name); m_testQueue.enqueue({name, m_testQuery, m_nodeProviders.value(name), m_testTimeoutMs}); m_testErrors.remove(name);
    }
    m_testGroup->setEnabled(false);
    message(tr("正在测速 %1 个节点…").arg(names.size()));
    renderNodes(); runNodeTests();
}
void Panel::runNodeTests()
{
    // Individual requests preserve successful results when another group member times out.
    while (m_activeTests < m_testConcurrency->value() && !m_testQueue.isEmpty()) {
        const auto task = m_testQueue.dequeue();
        const auto name = task.name;
        const auto epoch = m_testEpoch;
        ++m_activeTests;
        const auto provider = task.provider;
        const auto path = provider.isEmpty() ? "/proxies/" + Api::segment(name) + "/delay?" + task.query :
            "/providers/proxies/" + Api::segment(provider) + '/' + Api::segment(name) + "/healthcheck?" + task.query;
        m_api.get(path, [this, name, epoch](const QJsonValue &value, const QString &error) {
            if (epoch != m_testEpoch) return;
            --m_activeTests; m_testing.remove(name);
            const int delay = error.isEmpty() ? value.toObject()["delay"].toInt() : 0;
            m_testResults[name] = qMakePair(QDateTime::currentDateTimeUtc(), delay);
            m_testErrors[name] = error;
            if (delay > 0) ++m_testPassed; else ++m_testFailed;
            renderNodes();
            if (m_testing.isEmpty()) {
                m_testGroup->setEnabled(true);
                for (const int page : m_testPages) scopedCallback(std::function<void()>([this] {
                    message(tr("测速完成：%1 个成功，%2 个失败").arg(m_testPassed).arg(m_testFailed), m_testPassed == 0);
                }), page)();
                refreshProxies();
            } else runNodeTests();
        }, task.timeout + 2000);
    }
}
QWidget *Panel::profilesPage()
{
    auto *page = new QWidget; auto *layout = new QVBoxLayout(page);
    auto *urlRow = new QHBoxLayout;
    auto *urlInput = new QLineEdit;
    urlInput->setObjectName("subscription-url");
    urlInput->setPlaceholderText(tr("粘贴 Clash / Mihomo 订阅 URL"));
    urlRow->addWidget(urlInput, 1);
    auto importUrl = [this, urlInput] {
        const QUrl url(urlInput->text().trimmed());
        if (url.isEmpty()) { message(tr("请先输入订阅 URL"), true); urlInput->setFocus(); return; }
        fetchProfiles(url, url.host());
    };
    auto *urlImport = button(tr("URL 导入"), urlRow, this, importUrl);
    urlImport->setObjectName("import-subscription-url");
    connect(urlInput, &QLineEdit::returnPressed, this, importUrl);
    layout->addLayout(urlRow);
    auto *row = new QHBoxLayout;
    button(tr("导入文件"), row, this, [this] { importProfile(false); });
    button(tr("新建"), row, this, [this] { editProfile(true); });
    button(tr("更新"), row, this, [this] { updateProfile(); });
    button(tr("编辑"), row, this, [this] { editProfile(false); });
    row->addStretch(); layout->addLayout(row);
    m_profiles = table({tr("名称"), tr("来源"), tr("更新时间")}, layout); m_profiles->setObjectName("profiles");
    m_profiles->horizontalHeader()->setStretchLastSection(false);
    m_profiles->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    auto *actions = new QHBoxLayout;
    button(tr("订阅设置"), actions, this, [this] { editSubscriptionOptions(); });
    button(tr("使用此配置"), actions, this, [this] { activateProfile(); });
    button(tr("导出"), actions, this, [this] {
        const auto p = selectedProfile(); if (p.isEmpty()) return;
        const auto path = QFileDialog::getSaveFileName(this, tr("导出配置"), p["name"].toString() + ".yaml");
        if (!path.isEmpty()) guarded([&] { writeFile(path, m_store.profile(p["id"].toString())); });
    });
    button(tr("删除"), actions, this, [this] {
        const auto p = selectedProfile(); if (p.isEmpty()) return;
        if (QMessageBox::question(this, tr("删除配置"), tr("删除 %1？").arg(p["name"].toString())) != QMessageBox::Yes) return;
        guarded([&] { m_store.removeProfile(p["id"].toString()); reloadProfiles(); });
    });
    actions->addStretch(); layout->addLayout(actions);
    return page;
}
void Panel::editSubscriptionOptions()
{
    const auto p = selectedProfile();
    if (p.isEmpty()) { message(tr("请先选择一个订阅")); return; }
    QDialog dialog(this); dialog.setWindowTitle(tr("订阅设置")); dialog.resize(560, 420);
    auto *layout = new QVBoxLayout(&dialog); auto *form = new QFormLayout;
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    auto *name = new QLineEdit(p["name"].toString()); form->addRow(tr("名称"), name);
    auto *url = new QLineEdit(p["url"].toString()); form->addRow(tr("订阅 URL"), url);
    auto *agent = new QLineEdit(p["userAgent"].toString()); agent->setPlaceholderText("Mozilla/5.0"); form->addRow("User-Agent", agent);
    auto *token = new QLineEdit(p["authToken"].toString()); token->setEchoMode(QLineEdit::Password); form->addRow(tr("Authorization 令牌"), token);
    auto *interval = new QSpinBox; interval->setRange(0, 10080); interval->setSuffix(tr(" 分钟")); interval->setSpecialValueText(tr("跟随全局")); interval->setValue(p["updateMinutes"].toInt()); form->addRow(tr("更新周期"), interval);
    auto *timeout = new QSpinBox; timeout->setRange(5, 180); timeout->setSuffix(tr(" 秒")); timeout->setValue(p["timeoutSeconds"].toInt(30)); form->addRow(tr("请求超时"), timeout);
    auto *automatic = new QCheckBox(tr("自动更新")); automatic->setChecked(p["autoUpdate"].toBool(true)); form->addRow(automatic);
    auto *useProxy = new QCheckBox(tr("通过本插件或系统代理下载")); useProxy->setChecked(p["useProxy"].toBool()); form->addRow(useProxy);
    layout->addLayout(form);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel); layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        try {
            if (!url->text().isEmpty() && (QUrl(url->text()).scheme() != "http" && QUrl(url->text()).scheme() != "https")) throw std::runtime_error("Invalid subscription URL");
            QJsonObject options{{"name", name->text()}, {"url", url->text()}, {"userAgent", agent->text()}, {"authToken", token->text()},
                                {"timeoutSeconds", timeout->value()}, {"autoUpdate", automatic->isChecked()}, {"useProxy", useProxy->isChecked()}};
            if (interval->value() > 0) options["updateMinutes"] = interval->value();
            else options["updateMinutes"] = m_store.settings()["updateMinutes"].toInt(360);
            m_store.updateProfileOptions(p["id"].toString(), options); reloadProfiles(); dialog.accept();
        } catch (const std::exception &e) { QMessageBox::warning(&dialog, tr("无法保存"), QString::fromUtf8(e.what())); }
    });
    dialog.exec();
}
void Panel::reloadProfiles()
{
    guarded([&] {
        const auto current = selectedProfile()["id"].toString();
        const auto list = m_store.profiles();
        const auto active = m_store.settings()["active"].toString();
        m_profiles->setRowCount(list.size());
        for (int i = 0; i < list.size(); ++i) {
            const auto p = list[i].toObject();
            cell(m_profiles, i, 0, (p["id"].toString() == active ? QString::fromUtf8("✓ ") : "") + p["name"].toString());
            m_profiles->item(i, 0)->setData(Qt::UserRole, p);
            cell(m_profiles, i, 1, p["url"].toString().isEmpty() ? tr("本地") : p["url"].toString());
            cell(m_profiles, i, 2, QDateTime::fromString(p["updated"].toString(), Qt::ISODate).toLocalTime().toString("yyyy-MM-dd HH:mm"));
            if (current == p["id"].toString()) m_profiles->selectRow(i);
        }
        if (m_profiles->currentRow() < 0 && !list.isEmpty()) m_profiles->selectRow(0);
        reloadNodeSources();
    });
}
QJsonObject Panel::selectedProfile() const
{
    if (!m_profiles || m_profiles->currentRow() < 0) return {};
    const auto *item = m_profiles->item(m_profiles->currentRow(), 0);
    return item ? item->data(Qt::UserRole).toJsonObject() : QJsonObject();
}
void Panel::importProfile(bool remote)
{
    if (remote) {
        bool ok = false;
        const auto url = QInputDialog::getText(this, tr("导入订阅"), tr("Clash / Mihomo 订阅 URL"), QLineEdit::Normal, {}, &ok);
        if (!ok || url.isEmpty()) return;
        const auto name = QInputDialog::getText(this, tr("订阅名称"), tr("名称"), QLineEdit::Normal, QUrl(url).host(), &ok);
        if (ok) fetchProfiles(QUrl(url), name);
    } else {
        const auto path = QFileDialog::getOpenFileName(this, tr("导入配置"), {}, "YAML (*.yaml *.yml);;All (*)");
        if (path.isEmpty()) return;
        guarded([&] { saveProfileCandidate(QFileInfo(path).completeBaseName(), {}, readFile(path)); });
    }
}
void Panel::editProfile(bool create)
{
    auto p = selectedProfile();
    if (!create && p.isEmpty()) return;
    guarded([&] {
        const auto name = create ? QInputDialog::getText(this, tr("新建配置"), tr("名称")) : p["name"].toString();
        if (name.isEmpty()) return;
        const auto data = create ? QByteArray("mixed-port: 16666\nmode: rule\nlog-level: info\nproxies: []\nproxy-groups:\n  - name: PROXY\n    type: select\n    proxies: [DIRECT]\nrules:\n  - MATCH,PROXY\n") : m_store.profile(p["id"].toString());
        editDocument(name, data, [this, p, name](QByteArray text) {
            saveProfileCandidate(name, p["url"].toString(), text, p["id"].toString());
        });
    });
}
void Panel::updateProfile()
{
    const auto p = selectedProfile();
    if (p["url"].toString().isEmpty()) { message(tr("本地配置无需更新")); return; }
    fetchProfiles(QUrl(p["url"].toString()), p["name"].toString(), p["id"].toString());
}
void Panel::fetchProfiles(const QUrl &url, const QString &name, const QString &id)
{
    if (!url.isValid() || (url.scheme() != "http" && url.scheme() != "https")) { message(tr("订阅 URL 无效"), true); return; }
    QJsonObject options;
    guarded([&] {
        for (const auto &p : m_store.profiles()) if (p.toObject()["id"].toString() == id) options = p.toObject();
    });
    message(tr("正在下载订阅…"));
    fetchSubscription(url, options, localProxyPort(m_store), this, scopedCallback(SubscriptionDone([this, url, name, id, options](const QByteArray &bytes, const QJsonObject &metadata, const QString &error) {
        if (!error.isEmpty()) { message(error, true); return; }
        guarded([&] {
            saveProfileCandidate(name, url.toString(), bytes, id, metadata);
        });
    })));
}
void Panel::activateProfile()
{
    const auto p = selectedProfile();
    if (!p.isEmpty()) runService("start", p["id"].toString());
}
void Panel::runService(const QString &action, const QString &profile)
{
    backgroundCommand({{"action", action}, {"profile", profile}});
}
void Panel::backgroundCommand(const QJsonObject &command, Api::Done extra)
{
    const QString action = command["action"].toString();
    if (!localController(m_store.settings()["endpoint"].toString())) { message(tr("远程控制器模式：本地代理、内核和 TUN 操作已禁用。"), true); return; }
    const bool coreOperation = action == "start" || action == "stop" || action == "restart" || action == "restart-service" || action == "ports";
    if (coreOperation && m_coreBusy) { message(tr("代理服务正在操作，请稍候"), true); return; }
    if (coreOperation) { m_coreBusy = true; m_coreSwitch->setEnabled(false); m_restart->setEnabled(false); }
    auto done = scopedReply([this, coreOperation, extra](const QJsonValue &value, const QString &error) {
        if (coreOperation) m_coreBusy = false;
        if (value.isObject() && value.toObject().contains("running")) updateSwitches(value.toObject());
        if (extra) extra(value, error);
        if (coreOperation && error.isEmpty() && value.toObject()["running"].toBool()) {
            QSignalBlocker blocker(m_nodeSource); m_nodeSource->setCurrentIndex(0);
        }
        if (!error.isEmpty()) { message(error, true); refresh(); return; }
        const auto text = value.toObject()["message"].toString();
        message(text.isEmpty() ? (value.toObject()["running"].toBool() ? tr("内核已启动") : tr("内核已停止")) : text);
        reloadProfiles(); refresh();
    });
    if (action == "restart-service") {
        const auto token = newId();
        const auto path = dataDir() + "/restart-result-" + token + ".json";
        if (!QProcess::startDetached(serviceExecutable(), {"--restart-service", token}, dataDir())) { done({}, tr("无法启动服务重启程序，原服务未停止")); return; }
        message(tr("正在校验配置并重启后台与内核，期间代理连接会短暂中断…"));
        // The detached coordinator survives closing this page or control center.
        auto *timer = new QTimer(this); auto elapsed = QSharedPointer<QElapsedTimer>::create(); elapsed->start();
        connect(timer, &QTimer::timeout, this, [timer, elapsed, path, done] {
            if (QFileInfo::exists(path)) {
                timer->stop(); timer->deleteLater();
                try { const auto result = readJson(path); QFile::remove(path); done(result, result["error"].toString()); }
                catch (const std::exception &e) { done({}, QString::fromUtf8(e.what())); }
            } else if (elapsed->elapsed() > 280000) {
                timer->stop(); timer->deleteLater(); done({}, QObject::tr("等待服务重启超时，请检查后台日志；未执行强制终止"));
            }
        }); timer->start(200); return;
    }
    requestService({{"action", "status"}}, this, [this, command, done, action](const QJsonValue &state, const QString &error) {
        if (error.isEmpty() || state.toObject().contains("running")) { requestService(command, this, done); return; }
        if (action == "stop") { done(QJsonObject{{"running", false}}, {}); return; }
        auto launch = [this, command, done](bool managed) {
            if (!managed && !QProcess::startDetached(serviceExecutable(), {"--daemon"}, dataDir())) { done({}, tr("无法启动 dde-mihomo-daemon")); return; }
            QTimer::singleShot(800, this, [this, command, done] { requestService(command, this, done); });
        };
        if (qEnvironmentVariableIsSet("DCC_PROXY_DATA_DIR")) { launch(false); return; }
        auto *process = new QProcess(this);
        auto complete = QSharedPointer<bool>::create(false);
        auto finish = [process, complete, launch](bool ok) {
            if (*complete) return;
            *complete = true; process->deleteLater(); launch(ok);
        };
        connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this, [finish](int code, QProcess::ExitStatus state) { finish(code == 0 && state == QProcess::NormalExit); });
        connect(process, &QProcess::errorOccurred, this, [finish](QProcess::ProcessError e) { if (e == QProcess::FailedToStart) finish(false); });
        QTimer::singleShot(4000, process, [process] { if (process->state() != QProcess::NotRunning) process->kill(); });
        process->start("systemctl", {"--user", "start", "dde-mihomo-daemon.service"});
    });
}
QWidget *Panel::connectionsPage()
{
    auto *page = new QWidget; auto *layout = new QVBoxLayout(page);
    m_connectionSearch = new QLineEdit; m_connectionSearch->setObjectName("connection-search"); m_connectionSearch->setPlaceholderText(tr("搜索域名、进程、规则、节点")); layout->addWidget(m_connectionSearch);
    m_connections = table({tr("目标"), tr("进程"), tr("类型"), tr("规则"), tr("代理链"), tr("上传"), tr("下载")}, layout); m_connections->setObjectName("connections");
    auto *row = new QHBoxLayout;
    button(tr("关闭选中连接"), row, this, [this] {
        const int r = m_connections->currentRow(); if (r < 0) return;
        const auto id = m_connections->item(r, 0)->data(Qt::UserRole).toString();
        m_api.request("DELETE", "/connections/" + Api::segment(id), {}, [this](const QJsonValue &, const QString &error) { if (!error.isEmpty()) message(error, true); refresh(); });
    });
    button(tr("关闭全部连接"), row, this, [this] {
        m_api.request("DELETE", "/connections", {}, [this](const QJsonValue &, const QString &error) { if (!error.isEmpty()) message(error, true); refresh(); });
    });
    row->addStretch(); layout->addLayout(row);
    connect(m_connectionSearch, &QLineEdit::textChanged, this, [this] { refresh(); });
    return page;
}
QWidget *Panel::rulesPage()
{
    auto *page = new QWidget; auto *layout = new QVBoxLayout(page);
    auto *addRow = new QHBoxLayout;
    m_website = new QLineEdit; m_website->setObjectName("website-rule-url"); m_website->setPlaceholderText(tr("填写网址，例如 https://example.com/path 或 *.example.com")); addRow->addWidget(m_website, 1);
    m_addWebsiteRule = button(tr("添加网址规则"), addRow, this, [this] { addWebsiteRule(); }); m_addWebsiteRule->setObjectName("add-website-rule"); layout->addLayout(addRow);
    m_websitePreview = new QLabel; m_websitePreview->setWordWrap(true); m_websitePreview->setTextFormat(Qt::PlainText); m_websitePreview->setObjectName("website-rule-preview");
    m_websitePreview->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred); layout->addWidget(m_websitePreview);
    connect(m_website, &QLineEdit::textChanged, this, [this] { updateWebsiteRule(); });
    connect(m_website, &QLineEdit::returnPressed, this, [this] { if (m_addWebsiteRule->isEnabled()) addWebsiteRule(); });
    connect(m_group, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this] { updateWebsiteRule(); });

    m_ruleSearch = new QLineEdit; m_ruleSearch->setObjectName("rule-search"); m_ruleSearch->setPlaceholderText(tr("搜索规则")); layout->addWidget(m_ruleSearch);
    m_rules = table({tr("类型"), tr("匹配内容"), tr("目标策略"), tr("状态")}, layout); m_rules->setObjectName("rules");
    auto *row = new QHBoxLayout;
    button(tr("自定义规则管理"), row, this, [this] { editCustomRules(); })->setObjectName("manage-custom-rules");
    button(tr("高级规则编辑"), row, this, [this] { editSection("rules"); });
    button(tr("启用 / 禁用规则"), row, this, [this] {
        const int r = m_rules->currentRow(); if (r < 0) return;
        const auto rule = m_rules->item(r, 0)->data(Qt::UserRole).toJsonObject();
        m_api.request("PATCH", "/rules/disable", {{QString::number(rule["index"].toInt()), !rule["extra"].toObject()["disabled"].toBool()}},
            [this](const QJsonValue &, const QString &error) { if (!error.isEmpty()) message(error, true); refresh(); });
    });
    row->addStretch(); layout->addLayout(row);
    connect(m_ruleSearch, &QLineEdit::textChanged, this, [this] { refresh(); });
    return page;
}
QString Panel::websiteRuleProfile() const
{
    auto id = m_nodeSource->currentData().toString();
    if (id.isEmpty()) id = m_store.settings()["active"].toString();
    if (id.isEmpty() && !m_store.profiles().isEmpty()) id = m_store.profiles().first().toObject()["id"].toString();
    return id;
}
QString Panel::websiteRulePolicy() const
{
    if (m_mode->currentData().toString() == "direct") return "DIRECT";
    if (!m_previewing && m_mode->currentData().toString() == "global") return "GLOBAL";
    const auto current = m_group->currentData().toString();
    if (!current.isEmpty() && m_proxyData[current].toObject()["type"].toString() != "Subscription") return current;
    for (const auto &name : proxyGroupNames(m_proxyData))
        if (name != "GLOBAL" && m_proxyData[name].toObject()["type"].toString() != "Subscription") return name;
    return "DIRECT";
}
void Panel::updateWebsiteRule()
{
    if (!m_websitePreview) return;
    m_addWebsiteRule->setEnabled(false);
    if (websiteRuleProfile().isEmpty()) { labelText(m_websitePreview, tr("先导入订阅即可添加网址规则。")); return; }
    if (m_website->text().trimmed().isEmpty()) {
        labelText(m_websitePreview, tr("只需填写网址。自动提取域名并关联当前策略组 %1；切换该组内的节点后，规则继续跟随该组。网址规则优先于订阅规则，在规则模式下生效。").arg(websiteRulePolicy())); return;
    }
    try { labelText(m_websitePreview, websiteRule(m_website->text(), websiteRulePolicy())); m_addWebsiteRule->setEnabled(true); }
    catch (const std::exception &e) { labelText(m_websitePreview, QString::fromUtf8(e.what())); }
}
void Panel::addWebsiteRule()
{
    guarded([&] {
        const auto id = websiteRuleProfile(); const auto rule = websiteRule(m_website->text(), websiteRulePolicy());
        auto custom = m_store.customizations(); auto map = custom["rules"].toObject(); auto rules = map[id].toArray();
        const auto fields = rule.split(','); const auto prefix = fields.value(0) + ',' + fields.value(1) + ',';
        QJsonArray updated; updated.append(rule);
        for (const auto &entry : rules) if (!entry.toString().startsWith(prefix)) updated.append(entry);
        map[id] = updated; custom["rules"] = map;
        saveChange({{"files", QJsonObject{{"customizations.json", QString::fromLatin1(QJsonDocument(custom).toJson().toBase64())}}}}, [this, id] {
            m_website->clear(); applySavedProfile(id);
        });
    });
}
QWidget *Panel::providersPage()
{
    auto *page = new QWidget; auto *layout = new QVBoxLayout(page);
    m_providers = table({tr("类型"), tr("名称"), tr("来源"), tr("更新时间"), tr("条目数")}, layout); m_providers->setObjectName("providers");
    auto *row = new QHBoxLayout;
    button(tr("更新资源"), row, this, [this] { resourceAction(false); });
    button(tr("节点健康检查"), row, this, [this] { resourceAction(true); });
    button(tr("更新 GEO 数据"), row, this, [this] {
        m_api.request("POST", "/upgrade/geo", {}, [this](const QJsonValue &, const QString &error) { message(error.isEmpty() ? tr("GEO 数据已更新") : error, !error.isEmpty()); });
    });
    row->addStretch(); layout->addLayout(row); return page;
}
void Panel::updateProviders(const QString &kind, const QJsonObject &providers)
{
    const int vertical = m_providers->verticalScrollBar()->value();
    const int horizontal = m_providers->horizontalScrollBar()->value();
    // Keep each endpoint's rows until its successful replacement arrives.
    // Reusing items preserves selection and avoids repainting unchanged content.
    for (int row = m_providers->rowCount() - 1; row >= 0; --row) {
        const auto key = m_providers->item(row, 0)->data(Qt::UserRole).toJsonObject();
        if (key["kind"].toString() == kind && !providers.contains(key["name"].toString())) m_providers->removeRow(row);
    }
    for (auto it = providers.begin(); it != providers.end(); ++it) {
        const QJsonObject key{{"kind", kind}, {"name", it.key()}};
        int row = 0;
        while (row < m_providers->rowCount() && m_providers->item(row, 0)->data(Qt::UserRole).toJsonObject() != key) ++row;
        if (row == m_providers->rowCount()) m_providers->insertRow(row);
        const auto provider = it.value().toObject();
        const QStringList values{kind == "proxies" ? tr("节点") : tr("规则"), it.key(), provider["vehicleType"].toString(), provider["updatedAt"].toString(),
            QString::number(kind == "proxies" ? provider["proxies"].toArray().size() : provider["ruleCount"].toInt())};
        for (int column = 0; column < values.size(); ++column) {
            auto *item = m_providers->item(row, column);
            if (!item) cell(m_providers, row, column, values[column]);
            else if (item->text() != values[column]) { item->setText(values[column]); item->setToolTip(values[column]); }
        }
        if (m_providers->item(row, 0)->data(Qt::UserRole).toJsonObject() != key) m_providers->item(row, 0)->setData(Qt::UserRole, key);
    }
    m_providers->verticalScrollBar()->setValue(vertical);
    m_providers->horizontalScrollBar()->setValue(horizontal);
}
void Panel::resourceAction(bool health)
{
    const int r = m_providers->currentRow(); if (r < 0) return;
    const auto p = m_providers->item(r, 0)->data(Qt::UserRole).toJsonObject();
    const auto kind = p["kind"].toString();
    if (health && kind != "proxies") return;
    const auto path = "/providers/" + kind + '/' + Api::segment(p["name"].toString()) + (health ? "/healthcheck" : "");
    message(health ? tr("正在进行节点健康检查…") : tr("正在更新资源…"));
    m_api.request(health ? "GET" : "PUT", path, {}, [this](const QJsonValue &, const QString &error) { message(error.isEmpty() ? tr("操作完成") : error, !error.isEmpty()); refresh(); });
}
QWidget *Panel::overridePage()
{
    auto *page = new QWidget; auto *layout = new QVBoxLayout(page);
    m_overrideEnabled = new QCheckBox(tr("启用 YAML 覆写")); m_overrideEnabled->setToolTip(tr("映射递归合并，列表整体替换，null 删除字段")); layout->addWidget(m_overrideEnabled);
    auto *help = new QLabel(tr("YAML 覆写会合并到订阅配置：同名映射递归合并，列表整体替换，null 删除字段。例：hosts 添加域名到 IP 的映射；dns.nameserver 替换 DNS 列表；allow-lan: false 禁止局域网接入。节点和 rules 列表只在明确需要整体替换时填写。保存后重启内核生效。"));
    help->setObjectName("yaml-override-help"); help->setWordWrap(true); help->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred); layout->addWidget(help);
    m_yamlOverride = new QPlainTextEdit; m_yamlOverride->setObjectName("yaml-override-editor"); m_yamlOverride->setPlaceholderText("mixed-port: 16666\nallow-lan: false"); layout->addWidget(m_yamlOverride, 1);
    m_scriptEnabled = new QCheckBox(tr("启用 JavaScript 覆写")); m_scriptEnabled->setToolTip(tr("隔离执行，限时 4 秒")); layout->addWidget(m_scriptEnabled);
    m_jsOverride = new QPlainTextEdit; m_jsOverride->setObjectName("javascript-override-editor"); m_jsOverride->setPlaceholderText("function main(config) {\n  return config;\n}"); layout->addWidget(m_jsOverride, 1);
    auto *row = new QHBoxLayout;
    auto *exampleRow = new QHBoxLayout;
    button(tr("填入 YAML 示例"), exampleRow, this, [this] {
        if (!m_yamlOverride->toPlainText().trimmed().isEmpty() && m_yamlOverride->toPlainText().trimmed() != "{}" &&
            QMessageBox::question(this, tr("填入示例"), tr("替换当前编辑区内容？保存前不会影响已生效配置。")) != QMessageBox::Yes) return;
        m_yamlOverride->setPlainText(QString::fromUtf8(yamlOverrideExample()));
    })->setObjectName("yaml-override-example");
    exampleRow->addStretch(); layout->insertLayout(2, exampleRow);
    button(tr("保存覆写"), row, this, [this] {
        guarded([&] {
            const auto yaml = m_yamlOverride->toPlainText().trimmed().isEmpty() ? QByteArray("{}\n") : m_yamlOverride->toPlainText().toUtf8();
            parseConfig(yaml);
            auto settings = m_store.settings(); settings["overrideEnabled"] = m_overrideEnabled->isChecked(); settings["scriptEnabled"] = m_scriptEnabled->isChecked();
            QJsonObject files{{"override.yaml", QString::fromLatin1(yaml.toBase64())},
                              {"override.js", QString::fromLatin1(m_jsOverride->toPlainText().toUtf8().toBase64())},
                              {"settings.json", QString::fromLatin1(QJsonDocument(settings).toJson().toBase64())}};
            saveChange({{"files", files}});
        });
    });
    button(tr("管理有序覆写"), row, this, [this] { editManagedOverrides(); })->setObjectName("manage-overrides");
    button(tr("预览生效配置"), row, this, [this] { guarded([&] { editDocument(tr("生效配置"), m_store.runtime(m_store.settings()["active"].toString()), {}); }); });
    row->addStretch(); layout->addLayout(row); return page;
}
QWidget *Panel::settingsPage()
{
    auto *page = new QWidget; auto *layout = new QVBoxLayout(page); auto *form = new QFormLayout;
    m_endpoint = new QLineEdit; m_endpoint->setObjectName("controller-endpoint"); form->addRow(tr("控制器地址"), m_endpoint);
    auto *secretField = new Dtk::Widget::DPasswordEdit; secretField->setObjectName("api-secret-field");
    secretField->setEchoMode(QLineEdit::Password); secretField->setEchoButtonIsVisible(true);
    m_secret = secretField->lineEdit(); m_secret->setObjectName("api-secret"); form->addRow(tr("API 密钥"), secretField);
    auto *coreRow = new QHBoxLayout; m_corePath = new QLineEdit; coreRow->addWidget(m_corePath);
    button(tr("选择"), coreRow, this, [this] {
        const auto path = QFileDialog::getOpenFileName(this, tr("选择 Mihomo / Smart 内核")); if (!path.isEmpty()) m_corePath->setText(path);
    }); form->addRow(tr("内核路径"), coreRow);
    const QMap<QString, QString> portLabels = {{"mixed-port", tr("混合端口（HTTP / SOCKS）")},
        {"port", tr("HTTP 端口")}, {"socks-port", tr("SOCKS 端口")},
        {"redir-port", tr("重定向端口")}, {"tproxy-port", tr("透明代理端口")}};
    for (const auto &key : proxyPortKeys()) {
        auto *spin = new QSpinBox; spin->setObjectName("proxy-" + key);
        spin->setRange(key == "mixed-port" ? 1 : 0, 65535);
        if (key != "mixed-port") spin->setSpecialValueText(tr("关闭"));
        spin->setValue(key == "mixed-port" ? 16666 : 0);
        m_proxyPorts.insert(key, spin); form->addRow(portLabels[key], spin);
    }
    m_applyPorts = new QPushButton(tr("保存并应用端口")); m_applyPorts->setObjectName("apply-proxy-ports");
    connect(m_applyPorts, &QPushButton::clicked, this, [this] {
        guarded([&] {
            QJsonObject ports; for (auto it = m_proxyPorts.begin(); it != m_proxyPorts.end(); ++it) ports[it.key()] = it.value()->value();
            validateProxyPorts(ports); m_applyPorts->setEnabled(false);
            backgroundCommand({{"action", "ports"}, {"options", ports}}, [this](const QJsonValue &, const QString &) {
                m_applyPorts->setEnabled(true); loadPortSettings();
            });
        });
    });
    form->addRow(m_applyPorts);
    auto *portHint = new QLabel(tr("其它端口填 0 关闭。运行中修改会重启本插件内核，并同步已启用的系统代理；失败恢复原配置。"));
    portHint->setWordWrap(true); form->addRow(portHint);
    m_unifiedDelay = new QCheckBox(tr("统一延迟（与 Clash Party 一致，重启内核后生效）")); m_unifiedDelay->setObjectName("unified-delay"); form->addRow(m_unifiedDelay);
    m_autoSystemProxy = new QCheckBox(tr("启动内核时自动启用系统代理")); m_autoSystemProxy->setObjectName("auto-system-proxy"); form->addRow(m_autoSystemProxy);
    m_systemNodeDns = new QCheckBox(tr("节点域名使用系统 DNS（重启内核后生效）")); form->addRow(m_systemNodeDns);
    m_autoStart = new QCheckBox(tr("登录后自动启动上次使用的配置")); form->addRow(m_autoStart);
    m_updateMinutes = new QSpinBox; m_updateMinutes->setRange(0, 10080); m_updateMinutes->setSuffix(tr(" 分钟")); m_updateMinutes->setSpecialValueText(tr("关闭自动更新")); form->addRow(tr("订阅更新周期"), m_updateMinutes);
    layout->addLayout(form);
    auto *row = new QHBoxLayout;
    m_backendVersion = new QLabel; m_backendVersion->setObjectName("backend-version"); m_backendVersion->setWordWrap(true); layout->addWidget(m_backendVersion);
    button(tr("保存并连接"), row, this, [this] { saveSettings(); });
    button(tr("编辑完整配置"), row, this, [this] { editProfile(false); });
    button(tr("运行配置"), row, this, [this] { m_api.get("/configs", [this](const QJsonValue &value, const QString &error) { if (!error.isEmpty()) message(error, true); else editDocument(tr("运行配置"), QJsonDocument(value.toObject()).toJson(), {}); }); });
    row->addStretch(); layout->addLayout(row);
    auto *proxy = new QHBoxLayout;
    button(tr("启用系统代理"), proxy, this, [this] { systemProxy("manual"); });
    button(tr("关闭系统代理"), proxy, this, [this] { systemProxy("none"); });
    button(tr("PAC"), proxy, this, [this] { systemProxy("auto"); });
    proxy->addStretch(); layout->addLayout(proxy);
    auto *bypassLabel = new QLabel(tr("代理绕过")); layout->addWidget(bypassLabel);
    auto *bypassHint = new QLabel(tr("每行一项，支持域名、*.example.com、IP 和 CIDR 网段。匹配的地址直连，适用于系统代理和内置 PAC。TUN 和应用手动指定代理时请在规则中设置 DIRECT。"));
    bypassHint->setWordWrap(true); layout->addWidget(bypassHint);
    m_proxyBypass = new QPlainTextEdit;
    m_proxyBypass->setObjectName("proxy-bypass");
    m_proxyBypass->setMinimumHeight(112); m_proxyBypass->setMaximumHeight(160);
    m_proxyBypass->setPlaceholderText(defaultProxyBypass().join('\n')); layout->addWidget(m_proxyBypass);
    auto *bypassError = new QLabel; bypassError->setObjectName("proxy-bypass-error");
    bypassError->setWordWrap(true); bypassError->setTextFormat(Qt::PlainText);
    auto errorPalette = bypassError->palette(); errorPalette.setColor(QPalette::WindowText, QColor("#d9534f")); bypassError->setPalette(errorPalette);
    layout->addWidget(bypassError);
    auto *bypassRow = new QHBoxLayout;
    auto *saveBypass = button(tr("保存并应用绕过"), bypassRow, this, [this] {
        guarded([&] {
            const auto bypass = normalizeProxyBypass(textEntries(m_proxyBypass->toPlainText()));
            m_proxyBypass->setPlainText(bypass.join('\n'));
            backgroundCommand({{"action", "bypass"}, {"entries", QJsonArray::fromStringList(bypass)}});
        });
    });
    saveBypass->setObjectName("save-proxy-bypass");
    connect(m_proxyBypass, &QPlainTextEdit::textChanged, this, [this, bypassError, saveBypass] {
        try { normalizeProxyBypass(textEntries(m_proxyBypass->toPlainText())); labelText(bypassError, {}); saveBypass->setEnabled(true); }
        catch (const std::exception &e) { labelText(bypassError, QString::fromUtf8(e.what())); saveBypass->setEnabled(false); }
    });
    button(tr("填入默认绕过"), bypassRow, this, [this] { m_proxyBypass->setPlainText(defaultProxyBypass().join('\n')); });
    bypassRow->addStretch(); layout->addLayout(bypassRow);
    auto *backupRow = new QHBoxLayout;
    button(tr("本地备份"), backupRow, this, [this] {
        const auto path = QFileDialog::getSaveFileName(this, tr("备份"), "dcc-proxy-backup.json");
        if (!path.isEmpty()) guarded([&] { writeFile(path, backup(dataDir())); message(tr("备份完成（包含订阅和密钥，请妥善保存）")); });
    });
    button(tr("恢复备份"), backupRow, this, [this] {
        const auto path = QFileDialog::getOpenFileName(this, tr("恢复备份")); if (path.isEmpty()) return;
        if (QMessageBox::question(this, tr("恢复备份"), tr("覆盖本地配置？当前配置将自动备份。")) != QMessageBox::Yes) return;
        guarded([&] { restoreArchive(readFile(path)); });
    }); backupRow->addStretch(); layout->addLayout(backupRow);
    auto *davForm = new QFormLayout;
    m_davUrl = new QLineEdit; m_davUrl->setObjectName("webdav-url"); m_davUrl->setPlaceholderText("https://server/path/dcc-proxy-backup.json"); davForm->addRow(tr("WebDAV 文件 URL"), m_davUrl);
    m_davUser = new QLineEdit; m_davUser->setObjectName("webdav-user"); davForm->addRow(tr("用户名"), m_davUser);
    m_davPassword = new QLineEdit; m_davPassword->setObjectName("webdav-password"); m_davPassword->setEchoMode(QLineEdit::Password); davForm->addRow(tr("密码"), m_davPassword); layout->addLayout(davForm);
    auto *davRow = new QHBoxLayout;
    button(tr("上传备份"), davRow, this, [this] { webdav(true); });
    button(tr("下载并恢复"), davRow, this, [this] { webdav(false); });
    davRow->addStretch(); layout->addLayout(davRow);
    layout->addStretch();
    auto *scroll = new QScrollArea; scroll->setWidgetResizable(true); scroll->setFrameShape(QFrame::NoFrame); scroll->setWidget(page); return scroll;
}
void Panel::loadSettings()
{
    guarded([&] {
        const auto s = m_store.settings(); m_endpoint->setText(s["endpoint"].toString()); m_secret->setText(s["secret"].toString()); m_corePath->setText(s["core"].toString());
        m_testUrl->setText(s["delayTestUrl"].toString());
        m_testTimeout->setValue(s["delayTestTimeout"].toInt(5000));
        m_testConcurrency->setValue(s["delayTestConcurrency"].toInt(50));
        const auto controlledPath = dataDir() + "/controlled.yaml";
        const auto controlled = QFile::exists(controlledPath) ? parseConfig(readFile(controlledPath)) : YAML::Node(YAML::NodeType::Map);
        m_unifiedDelay->setChecked(!controlled["unified-delay"] || controlled["unified-delay"].as<bool>());
        loadPortSettings();
        { QSignalBlocker block(m_logLevel); m_logLevel->setCurrentText(controlled["log-level"] ? QString::fromStdString(controlled["log-level"].as<std::string>()) : "info"); }
        m_autoSystemProxy->setChecked(s.value("autoSystemProxy").toBool(true));
        m_systemNodeDns->setChecked(s.value("systemNodeDns").toBool(true));
        QStringList bypass = defaultProxyBypass();
        if (s["proxyBypass"].isArray()) {
            bypass.clear();
            for (const auto &v : s["proxyBypass"].toArray()) bypass.append(v.toString());
        }
        m_proxyBypass->setPlainText(bypass.join('\n'));
        m_autoStart->setChecked(s["autoStart"].toBool()); m_updateMinutes->setValue(s["updateMinutes"].toInt(360));
        m_overrideEnabled->setChecked(s["overrideEnabled"].toBool(true)); m_scriptEnabled->setChecked(s["scriptEnabled"].toBool());
        m_davUrl->setText(s["webdavUrl"].toString()); m_davUser->setText(s["webdavUser"].toString());
        m_api.configure(m_endpoint->text(), m_secret->text());
        if (QFile::exists(dataDir() + "/override.yaml")) m_yamlOverride->setPlainText(QString::fromUtf8(readFile(dataDir() + "/override.yaml")));
        if (m_yamlOverride->toPlainText().trimmed().isEmpty() || m_yamlOverride->toPlainText().trimmed() == "{}")
            m_yamlOverride->setPlainText(QString::fromUtf8(yamlOverrideExample()));
        if (QFile::exists(dataDir() + "/override.js")) m_jsOverride->setPlainText(QString::fromUtf8(readFile(dataDir() + "/override.js")));
    });
}
void Panel::loadPortSettings()
{
    guarded([&] {
        YAML::Node config(YAML::NodeType::Map);
        const auto work = dataDir() + "/work/config.yaml";
        const auto id = m_store.settings()["active"].toString();
        if (QFile::exists(work)) config = parseConfig(readFile(work));
        else if (validId(id)) config = parseConfig(m_store.profile(id));
        const auto path = dataDir() + "/controlled.yaml";
        if (QFile::exists(path)) config = mergeYaml(config, parseConfig(readFile(path)));
        for (auto it = m_proxyPorts.begin(); it != m_proxyPorts.end(); ++it) {
            const auto value = config[it.key().toStdString()];
            it.value()->setValue(value ? value.as<int>() : (it.key() == "mixed-port" ? 16666 : 0));
        }
    });
}
void Panel::saveSettings()
{
    guarded([&] {
        QString error;
        if (!m_api.configure(m_endpoint->text(), m_secret->text(), &error)) { message(error, true); return; }
        m_reads.clear(); m_polling = false; m_pageMessages.clear(); m_selecting = false;
        if (localController(m_endpoint->text()) && !QFileInfo(m_corePath->text()).isExecutable()) throw std::runtime_error("内核路径不是可执行文件，原设置已保留");
        auto s = m_store.settings();
        if (localController(s["endpoint"].toString())) { s["managedEndpoint"] = s["endpoint"]; s["managedSecret"] = s["secret"]; s["managedCore"] = s["core"]; }
        s["endpoint"] = m_endpoint->text(); s["secret"] = m_secret->text(); s["core"] = m_corePath->text();
        if (localController(s["endpoint"].toString())) { s["managedEndpoint"] = s["endpoint"]; s["managedSecret"] = s["secret"]; s["managedCore"] = s["core"]; }
        const auto controlledPath = dataDir() + "/controlled.yaml";
        auto controlled = QFile::exists(controlledPath) ? parseConfig(readFile(controlledPath)) : YAML::Node(YAML::NodeType::Map);
        controlled["unified-delay"] = m_unifiedDelay->isChecked(); writeFile(controlledPath, emitYaml(controlled));
        s["autoSystemProxy"] = m_autoSystemProxy->isChecked();
        s["systemNodeDns"] = m_systemNodeDns->isChecked();
        s["autoStart"] = m_autoStart->isChecked(); s["updateMinutes"] = m_updateMinutes->value();
        s["webdavUrl"] = m_davUrl->text(); s["webdavUser"] = m_davUser->text(); m_store.setSettings(s);
        const QString units = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation) + "/systemd/user/default.target.wants";
        const QString unit = units + "/dde-mihomo-daemon.service";
        if (localController(s["endpoint"].toString()) && m_autoStart->isChecked()) {
            QDir().mkpath(units);
            if (!QFile::exists(unit) && !QFile::link("/usr/lib/systemd/user/dde-mihomo-daemon.service", unit)) throw std::runtime_error("Cannot enable background service");
        } else if (localController(s["endpoint"].toString())) QFile::remove(unit);
        QProcess::startDetached("systemctl", {"--user", "daemon-reload"});
        message(tr("设置已保存")); refresh();
    });
}
void Panel::patchConfig(const QJsonObject &patch)
{
    m_api.request("PATCH", "/configs", patch, [this, patch](const QJsonValue &, const QString &error) {
        if (!error.isEmpty()) { message(error, true); refresh(); return; }
        if (!localController(m_store.settings()["endpoint"].toString())) { message(tr("远程内核设置已应用，本地配置未修改")); refresh(); return; }
        guarded([&] {
            const auto path = dataDir() + "/controlled.yaml";
            auto base = QFile::exists(path) ? parseConfig(readFile(path)) : YAML::Node(YAML::NodeType::Map);
            const auto delta = parseConfig(QJsonDocument(patch).toJson(QJsonDocument::Compact));
            writeFile(path, emitYaml(mergeYaml(base, delta)));
            if (m_serviceState["capabilities"].toArray().contains("checkpoint")) requestService({{"action", "checkpoint"}, {"patch", patch}}, this, [](const QJsonValue &, const QString &) {});
            message(tr("设置已应用并保存"));
        });
        refresh();
    });
}
void Panel::editDocument(const QString &title, const QByteArray &data, std::function<void(QByteArray)> save, const QString &help, const QByteArray &example)
{
    QDialog dialog(this); dialog.setWindowTitle(title); dialog.resize(700, 520);
    auto *layout = new QVBoxLayout(&dialog);
    if (!help.isEmpty()) { auto *label = new QLabel(help); label->setObjectName("config-editor-help"); label->setWordWrap(true); label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred); layout->addWidget(label); }
    if (!example.isEmpty()) {
        auto *row = new QHBoxLayout; button(tr("填入示例"), row, &dialog, [&dialog, example] {
            auto *edit = dialog.findChild<QPlainTextEdit *>("config-document-editor");
            if (edit && (edit->toPlainText().trimmed().isEmpty() || QMessageBox::question(&dialog, tr("填入示例"), tr("替换当前编辑区内容？保存后才会生效。")) == QMessageBox::Yes)) edit->setPlainText(QString::fromUtf8(example));
        }); row->addStretch(); layout->addLayout(row);
    }
    auto *edit = new QPlainTextEdit(QString::fromUtf8(data)); edit->setObjectName("config-document-editor"); edit->setReadOnly(!save); layout->addWidget(edit);
    auto *buttons = new QDialogButtonBox(save ? QDialogButtonBox::Save | QDialogButtonBox::Cancel : QDialogButtonBox::Close);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        try { if (save) save(edit->toPlainText().toUtf8()); dialog.accept(); }
        catch (const std::exception &e) { QMessageBox::warning(&dialog, tr("无法保存"), QString::fromUtf8(e.what())); }
    });
    dialog.exec();
}
void Panel::editSection(const QString &section)
{
    guarded([&] {
        const auto id = m_store.settings()["active"].toString();
        auto node = parseConfig(m_store.profile(id));
        YAML::Node wrapper; wrapper[section.toStdString()] = node[section.toStdString()];
        const bool empty = !node[section.toStdString()] || node[section.toStdString()].IsNull();
        editDocument(section.toUpper(), empty && !sectionExample(section).isEmpty() ? sectionExample(section) : emitYaml(wrapper), [this, id, section, node](QByteArray text) mutable {
            const auto edited = parseConfig(text);
            node[section.toStdString()] = edited[section.toStdString()];
            auto custom = m_store.customizations(); auto overrides = custom["overrides"].toArray();
            QJsonObject item{{"id", newId()}, {"name", section.toUpper() + tr(" 高级设置")}, {"profile", id}, {"kind", "yaml"}, {"enabled", true}};
            YAML::Node fragment; fragment[section.toStdString()] = node[section.toStdString()];
            item["content"] = QString::fromUtf8(emitYaml(fragment)); overrides.append(item); custom["overrides"] = overrides;
            saveChange({{"files", QJsonObject{{"customizations.json", QString::fromLatin1(QJsonDocument(custom).toJson().toBase64())}}}}, [this, id] { applySavedProfile(id); });
        }, sectionHelp(section), sectionExample(section));
    });
}
QWidget *Panel::toolsPage()
{
    auto *page = new QWidget; auto *layout = new QVBoxLayout(page);
    auto *diagnosticRow = new QHBoxLayout;
    button(tr("网络分层诊断"), diagnosticRow, this, [this] { diagnose(); })->setObjectName("diagnose-network");
    button(tr("Sub-Store 管理"), diagnosticRow, this, [this] { subStore(); })->setObjectName("manage-substore");
    diagnosticRow->addStretch(); layout->addLayout(diagnosticRow);
    auto *dnsRow = new QHBoxLayout; auto *domain = new QLineEdit("example.com"); dnsRow->addWidget(domain, 1);
    auto *dnsType = new QComboBox; dnsType->addItems({"A", "AAAA", "CNAME", "TXT", "MX", "HTTPS"}); dnsRow->addWidget(dnsType);
    button(tr("DNS 查询"), dnsRow, this, [this, domain, dnsType] {
        QUrlQuery q; q.addQueryItem("name", domain->text()); q.addQueryItem("type", dnsType->currentText());
        m_api.get("/dns/query?" + q.toString(QUrl::FullyEncoded), [this](const QJsonValue &value, const QString &error) {
            if (!error.isEmpty()) message(error, true); else editDocument(tr("DNS 查询结果"), QJsonDocument(value.toObject()).toJson(), {});
        });
    }); layout->addLayout(dnsRow);
    auto *cacheRow = new QHBoxLayout;
    for (const auto &kind : {"dns", "fakeip"}) button(tr("清除 %1 缓存").arg(kind), cacheRow, this, [this, kind] {
        m_api.request("POST", QString("/cache/%1/flush").arg(kind), {}, [this](const QJsonValue &, const QString &error) { message(error.isEmpty() ? tr("缓存已清除") : error, !error.isEmpty()); });
    }); cacheRow->addStretch(); layout->addLayout(cacheRow);
    auto *substoreRow = new QHBoxLayout;
    auto *subUrl = new QLineEdit; subUrl->setPlaceholderText(tr("Sub-Store 导出的 Clash / Mihomo 配置 URL")); substoreRow->addWidget(subUrl, 1);
    button(tr("从 Sub-Store 导入"), substoreRow, this, [this, subUrl] { fetchProfiles(QUrl(subUrl->text()), "Sub-Store"); }); layout->addLayout(substoreRow);
    auto *note = new QLabel(tr("完整的 DNS、嗅探、TUN、监听器和隧道配置可在概览页编辑。Sub-Store 支持导入其导出的完整 Mihomo 配置。")); note->setWordWrap(true); layout->addWidget(note);
    layout->addStretch(); return page;
}
void Panel::systemProxy(const QString &mode)
{
    m_api.get("/configs", [this, mode](const QJsonValue &value, const QString &error) {
        if (!error.isEmpty() && mode != "none") { message(error, true); return; }
        backgroundCommand({{"action", "proxy"}, {"mode", mode}, {"port", value.toObject()["mixed-port"].toInt()}});
    });
}
void Panel::webdav(bool upload)
{
    const QUrl url(m_davUrl->text());
    if (!url.isValid() || url.scheme() != "https" || !url.userInfo().isEmpty()) { message(tr("WebDAV 需要 HTTPS 文件 URL"), true); return; }
    if (!upload && QMessageBox::question(this, tr("恢复备份"), tr("下载并覆盖本地配置？")) != QMessageBox::Yes) return;
    guarded([&] {
        QNetworkRequest request(url);
        request.setRawHeader("Authorization", "Basic " + (m_davUser->text() + ':' + m_davPassword->text()).toUtf8().toBase64());
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
        auto *reply = upload ? m_download.put(request, backup(dataDir())) : m_download.get(request);
        QTimer::singleShot(20000, reply, &QNetworkReply::abort);
        connect(reply, &QNetworkReply::readyRead, this, [reply] { if (reply->bytesAvailable() > 32 * 1024 * 1024) reply->abort(); });
        connect(reply, &QNetworkReply::finished, this, scopedCallback(std::function<void()>([this, reply, upload] {
            reply->deleteLater();
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (reply->error() != QNetworkReply::NoError || status < 200 || status >= 300) { message(reply->errorString(), true); return; }
            guarded([&] { if (!upload) restoreArchive(reply->readAll()); else message(tr("备份上传完成")); });
        })));
    });
}
void Panel::refresh()
{
    if (!isVisible()) return;
    if (!m_polling) {
    m_polling = true;
    requestService({{"action", "status"}}, this, [this](const QJsonValue &state, const QString &) {
        if (state.toObject().contains("running")) updateSwitches(state.toObject());
        else if (!m_coreBusy) updateSwitches({{"running", false}});
    });
    m_api.get("/configs", [this](const QJsonValue &value, const QString &error) {
        const bool wasConnected = m_connected;
        m_polling = false; m_connected = error.isEmpty();
        if (m_connected != wasConnected) resetPageStatus();
        if ((m_tabs->currentIndex() == 1 || m_tabs->currentIndex() == 4) && (!m_connected || !wasConnected || m_previewing)) refreshProxies();
        if (!m_connected) {
            m_mode->setEnabled(false);
            requestService({{"action", "status"}}, this, [this](const QJsonValue &state, const QString &serviceError) {
                if (m_connected) return;
                if (!serviceError.isEmpty() && !state.toObject().contains("running")) { labelText(m_overview, tr("后台未启动。请导入订阅并打开“代理服务”开关。")); return; }
                const auto s = state.toObject();
                if (s["busy"].toBool()) labelText(m_overview, tr("正在验证配置并启动内核，请稍候…"));
                else if (!s["error"].toString().isEmpty()) labelText(m_overview, tr("内核启动失败：%1").arg(s["error"].toString()));
                else if (!s["running"].toBool()) labelText(m_overview, tr("后台已运行，内核未启动。导入订阅后可打开“代理服务”开关。"));
                else labelText(m_overview, tr("内核正在运行，等待控制器连接。"));
            });
            return;
        }
        const auto config = value.toObject();
        if (!wasConnected) { QSignalBlocker block(m_logLevel); m_logLevel->setCurrentText(config["log-level"].toString("info")); }
        labelText(m_overview, tr("内核已连接    混合端口 %1    HTTP %2    SOCKS %3    局域网 %4")
            .arg(config["mixed-port"].toInt()).arg(config["port"].toInt()).arg(config["socks-port"].toInt()).arg(config["allow-lan"].toBool() ? tr("允许") : tr("禁止")));
        { QSignalBlocker block(m_mode); m_mode->setCurrentIndex(m_mode->findData(config["mode"].toString())); }
        m_mode->setEnabled(!m_coreBusy && !m_configBusy); renderNodes(); updateWebsiteRule(); syncStreams();
    });
    }
    const int page = m_tabs->currentIndex();
    if (page == 1 || page == 4) refreshProxies();
    if (page == 2) reloadProfiles();
    auto read = [this](const QString &path, Api::Done done) {
        if (m_reads.contains(path)) return;
        m_reads.insert(path);
        m_api.get(path, [this, path, done](const QJsonValue &value, const QString &error) { m_reads.remove(path); done(value, error); });
    };
    if (page == 3 || page == 0) read("/connections", [this](const QJsonValue &value, const QString &error) {
        if (!error.isEmpty()) return;
        const auto obj = value.toObject();
        labelText(m_statistics, tr("连接 %1    总上传 %2    总下载 %3    内存 %4").arg(obj["connections"].toArray().size()).arg(sizeText(obj["uploadTotal"].toDouble()), sizeText(obj["downloadTotal"].toDouble()), sizeText(obj["memory"].toDouble())));
        if (m_tabs->currentIndex() != 3) return;
        const auto data = obj["connections"].toArray();
        QString selected;
        if (m_connections->currentRow() >= 0) selected = m_connections->item(m_connections->currentRow(), 0)->data(Qt::UserRole).toString();
        m_connections->setRowCount(0);
        for (const auto &v : data) {
            const auto c = v.toObject(); const auto md = c["metadata"].toObject();
            if (!QString::fromUtf8(QJsonDocument(c).toJson()).contains(m_connectionSearch->text(), Qt::CaseInsensitive)) continue;
            const int row = m_connections->rowCount(); m_connections->insertRow(row);
            cell(m_connections, row, 0, (md["host"].toString().isEmpty() ? md["destinationIP"].toString() : md["host"].toString()) + ':' + md["destinationPort"].toString());
            m_connections->item(row, 0)->setData(Qt::UserRole, c["id"].toString());
            cell(m_connections, row, 1, md["process"].toString()); cell(m_connections, row, 2, md["network"].toString() + '/' + md["type"].toString());
            cell(m_connections, row, 3, c["rule"].toString() + ' ' + c["rulePayload"].toString());
            QStringList chain; for (const auto &n : c["chains"].toArray()) chain << n.toString();
            std::reverse(chain.begin(), chain.end());
            cell(m_connections, row, 4, chain.join(" → ")); cell(m_connections, row, 5, sizeText(c["upload"].toDouble())); cell(m_connections, row, 6, sizeText(c["download"].toDouble()));
            if (c["id"].toString() == selected) m_connections->selectRow(row);
        }
    });
    if (page == 4) read("/rules", [this](const QJsonValue &value, const QString &error) {
        if (!error.isEmpty()) return;
        m_rules->setRowCount(0);
        const auto data = value.toObject()["rules"].toArray();
        for (int i = 0; i < data.size(); ++i) {
            auto c = data[i].toObject(); if (!c.contains("index")) c["index"] = i;
            if (!QString::fromUtf8(QJsonDocument(c).toJson()).contains(m_ruleSearch->text(), Qt::CaseInsensitive)) continue;
            const int row = m_rules->rowCount(); m_rules->insertRow(row);
            cell(m_rules, row, 0, c["type"].toString()); m_rules->item(row, 0)->setData(Qt::UserRole, c);
            cell(m_rules, row, 1, c["payload"].toString()); cell(m_rules, row, 2, c["proxy"].toString());
            cell(m_rules, row, 3, c["extra"].toObject()["disabled"].toBool() ? tr("禁用") : tr("启用"));
        }
    });
    if (page == 5) {
        for (const auto &kind : {"proxies", "rules"}) read(QString("/providers/%1").arg(kind), [this, kind](const QJsonValue &value, const QString &error) {
            if (!error.isEmpty() || !value.toObject()["providers"].isObject()) return;
            updateProviders(kind, value.toObject()["providers"].toObject());
        });
    }
}
void Panel::loadLogHistory()
{
    if (m_logHistoryLoaded) return;
    QFile file(dataDir() + "/daemon.log");
    if (!file.open(QIODevice::ReadOnly)) return;
    const bool tail = file.size() > 64 * 1024;
    if (tail) file.seek(file.size() - 64 * 1024);
    auto bytes = file.readAll(); if (tail) bytes = bytes.mid(bytes.indexOf('\n') + 1);
    auto text = QString::fromUtf8(bytes); text.remove(QRegularExpression("\x1b\\[[0-9;]*m"));
    if (!text.trimmed().isEmpty()) { m_logHistoryLoaded = true; m_logs->appendPlainText(text.trimmed()); m_logs->moveCursor(QTextCursor::End); }
}
void Panel::syncStreams()
{
    const bool active = isVisible() && m_connected;
    m_api.stream("/traffic", active && m_tabs->currentIndex() == 0);
    for (const auto &level : {"info", "warning", "error", "debug", "silent"}) {
        const auto path = QString("/logs?level=%1").arg(level);
        m_api.stream(path, active && m_tabs->currentIndex() == 6 && !m_pauseLogs->isChecked() && m_logLevel->currentText() == level);
    }
}
void Panel::showEvent(QShowEvent *event) { QWidget::showEvent(event); m_refreshTimer.start(); refresh(); syncStreams(); }
void Panel::hideEvent(QHideEvent *event) { if (auto *secret = findChild<Dtk::Widget::DPasswordEdit *>("api-secret-field")) secret->setEchoMode(QLineEdit::Password); m_refreshTimer.stop(); m_api.stopStreams(); QWidget::hideEvent(event); }
void Panel::openPage(const QString &page)
{
    const QStringList names{"Overview", "Proxies", "Profiles", "Connections", "Rules", "Providers", "Logs", "Overrides", "Tools", "Settings", "Tun"};
    const int index = names.indexOf(page == "Groups" ? "Proxies" : page); if (index >= 0) m_tabs->setCurrentIndex(index);
}
void Panel::setExternalNavigation(bool enabled)
{
    m_externalNavigation = enabled;
    m_tabs->tabBar()->setVisible(!enabled);
    m_pageTitle->setText(enabled ? m_tabs->tabText(m_tabs->currentIndex()) : tr("私人代理"));
}
}
