#include "panel.h"
#include "nodeflags.h"
#include "nodelist.h"
#include "tun.h"
#include <DSwitchButton>
#include <QRegularExpression>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
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
#include <QScrollArea>
#include <QStandardPaths>
#include <QApplication>
#include <QPalette>
#include <QNetworkProxy>
#include <QSharedPointer>
#include <QScrollBar>
#include <algorithm>

namespace Party {
static void labelText(QLabel *label, const QString &text)
{
    if (label->text() != text) label->setText(text);
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
    m_download.setProxy(QNetworkProxy::NoProxy);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 12, 16, 16);
    auto *header = new QHBoxLayout;
    auto *icon = new QLabel;
    icon->setPixmap(QIcon(":/dcc-proxy/proxy.svg").pixmap(42, 42));
    header->addWidget(icon);
    auto *title = new QLabel(tr("私人代理"));
    auto font = title->font(); font.setPointSize(font.pointSize() + 4); font.setBold(true); title->setFont(font);
    header->addWidget(title);
    header->addStretch();
    button(tr("刷新"), header, this, [this] { refresh(); })->setObjectName("refresh-proxy-panel");
    layout->addLayout(header);
    m_status = new QLabel(tr("尚未连接内核，请导入配置并启动，或在设置中连接现有内核。"));
    m_status->setObjectName("proxy-status");
    m_status->setWordWrap(true);
    m_status->setTextFormat(Qt::PlainText);
    m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(m_status);
    m_tabs = new QTabWidget;
    m_tabs->setDocumentMode(true);
    m_tabs->setUsesScrollButtons(true);
    m_tabs->tabBar()->setExpanding(false);
    layout->addWidget(m_tabs, 1);
    m_tabs->addTab(overviewPage(), tr("概览"));
    m_tabs->addTab(proxiesPage(), tr("节点"));
    m_tabs->addTab(profilesPage(), tr("订阅"));
    m_tabs->addTab(connectionsPage(), tr("连接"));
    m_tabs->addTab(rulesPage(), tr("规则"));
    m_tabs->addTab(providersPage(), tr("资源"));
    auto *logsPage = new QWidget;
    auto *logsLayout = new QVBoxLayout(logsPage);
    auto *logsRow = new QHBoxLayout;
    m_pauseLogs = new QCheckBox(tr("暂停"));
    logsRow->addWidget(m_pauseLogs);
    m_logLevel = new QComboBox;
    m_logLevel->addItems({"info", "warning", "error", "debug", "silent"});
    logsRow->addWidget(m_logLevel);
    button(tr("清空"), logsRow, this, [this] { m_logs->clear(); });
    button(tr("导出"), logsRow, this, [this] {
        const auto path = QFileDialog::getSaveFileName(this, tr("导出日志"), "mihomo.log");
        if (!path.isEmpty()) guarded([&] { writeFile(path, m_logs->toPlainText().toUtf8()); });
    });
    button(tr("后台日志"), logsRow, this, [this] {
        guarded([&] { editDocument(tr("后台日志"), readFile(dataDir() + "/daemon.log"), {}); });
    });
    logsRow->addStretch(); logsLayout->addLayout(logsRow);
    m_logs = new QPlainTextEdit;
    m_logs->setReadOnly(true); m_logs->setMaximumBlockCount(2000);
    logsLayout->addWidget(m_logs);
    m_tabs->addTab(logsPage, tr("日志"));
    m_tabs->addTab(overridePage(), tr("覆写"));
    m_tabs->addTab(toolsPage(), tr("工具"));
    m_tabs->addTab(settingsPage(), tr("设置"));
    m_tabs->addTab(tunPage(), tr("虚拟网卡"));
    loadTunSettings();
    connect(&m_api, &Api::configurationChanged, this, [this] {
        ++m_testEpoch; m_testing.clear(); m_testQueue.clear(); m_activeTests = 0;
        m_testResults.clear(); m_testErrors.clear(); m_nodeProviders.clear(); m_providerProxyData = {};
        m_testGroup->setEnabled(true); renderNodes();
    });
    loadSettings();
    reloadProfiles();
    connect(&m_refreshTimer, &QTimer::timeout, this, &Panel::refresh);
    m_refreshTimer.setInterval(3000);
    connect(m_tabs, &QTabWidget::currentChanged, this, [this] { syncStreams(); refresh(); });
    connect(m_pauseLogs, &QCheckBox::toggled, this, [this] { syncStreams(); });
    connect(m_logLevel, &QComboBox::currentTextChanged, this, [this] { m_api.stopStreams(); syncStreams(); });
    connect(&m_api, &Api::streamData, this, [this](const QString &path, const QJsonObject &data) {
        if (path.startsWith("/logs")) {
            if (!m_pauseLogs->isChecked()) m_logs->appendPlainText(QDateTime::currentDateTime().toString("HH:mm:ss ") + data["type"].toString() + " " + data["payload"].toString());
        } else if (path == "/traffic") {
            labelText(m_traffic, tr("上传 %1/s    下载 %2/s").arg(sizeText(data["up"].toDouble()), sizeText(data["down"].toDouble())));
        }
    });
    connect(&m_api, &Api::streamError, this, [this](const QString &, const QString &) { /* next refresh reconnects */ });
}
void Panel::guarded(std::function<void()> action)
{
    try { action(); } catch (const std::exception &e) { message(QString::fromUtf8(e.what()), true); }
}
void Panel::message(const QString &text, bool error)
{
    labelText(m_status, text);
    auto p = m_status->palette();
    p.setColor(QPalette::WindowText, error ? QColor("#d9534f") : palette().color(QPalette::WindowText));
    if (m_status->palette() != p) m_status->setPalette(p);
}
QWidget *Panel::overviewPage()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    m_overview = new QLabel(tr("内核离线")); m_overview->setTextFormat(Qt::PlainText); m_overview->setWordWrap(true);
    layout->addWidget(m_overview);
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
    m_restart = button(tr("重启"), controls, this, [this] { runService("restart"); });
    controls->addStretch(); layout->addLayout(controls);
    auto *form = new QFormLayout;
    m_mode = new QComboBox;
    m_mode->addItem(tr("规则"), "rule"); m_mode->addItem(tr("全局"), "global"); m_mode->addItem(tr("直连"), "direct");
    form->addRow(tr("代理模式"), m_mode);
    connect(m_mode, QOverload<int>::of(&QComboBox::activated), this, [this](int) { patchConfig({{"mode", m_mode->currentData().toString()}}); });
    layout->addLayout(form);
    auto *advanced = new QGridLayout;
    int sectionIndex = 0;
    for (const auto &section : {"dns", "sniffer", "hosts", "listeners", "tunnels"}) {
        auto *b = new QPushButton(QString::fromLatin1(section).toUpper());
        advanced->addWidget(b, sectionIndex / 3, sectionIndex % 3); ++sectionIndex;
        connect(b, &QPushButton::clicked, this, [this, section] { editSection(QString::fromLatin1(section)); });
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
            const auto core = m_store.settings()["core"].toString();
            if (hasTunPermission(core)) { message(tr("内核已具备虚拟网卡权限")); return; }
            m_tunBusy = true; m_tunPageSwitch->setEnabled(false);
            requestTunPermission(core, this, [this](QString error) { m_tunBusy = false; message(error.isEmpty() ? tr("内核授权成功，可启用虚拟网卡") : error, !error.isEmpty()); refresh(); });
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
    const bool busy = state["busy"].toBool();
    if (!m_coreBusy) {
        const QSignalBlocker blocker(m_coreSwitch); m_coreSwitch->setChecked(state["running"].toBool()); m_coreSwitch->setEnabled(!busy); m_restart->setEnabled(!busy);
    }
    if (!m_tunBusy) {
        const auto tun = state["tun"].toObject();
        { const QSignalBlocker blocker(m_tunPageSwitch); m_tunPageSwitch->setChecked(tun["enable"].toBool()); m_tunPageSwitch->setEnabled(!busy); }
        labelText(m_tunStatus, tun["enable"].toBool() ? tr("虚拟网卡 %1 已启用").arg(tun["device"].toString()) : tr("虚拟网卡未启用"));
    }
}
void Panel::setTunOptions(const QJsonObject &options)
{
    if (m_tunBusy || m_coreBusy) return;
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
            message(tr("启用虚拟网卡需要管理员授权内核，请完成系统认证。")); requestTunPermission(m_store.settings()["core"].toString(), this, apply);
        } else apply({});
    });
}

QWidget *Panel::proxiesPage()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
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
    button(tr("恢复自动选择"), actions, this, [this] {
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
    return page;
}
void Panel::refreshProxies()
{
    if (m_reads.contains("proxies")) return;
    m_reads.insert("proxies");
    if (!m_reads.contains("proxy-providers")) {
        m_reads.insert("proxy-providers");
        m_api.get("/providers/proxies", [this](const QJsonValue &value, const QString &error) {
            m_reads.remove("proxy-providers");
            if (!error.isEmpty()) return;
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
        if (!error.isEmpty()) { m_connected = false; message(error, true); return; }
        m_proxyData = value.toObject()["proxies"].toObject();
        for (auto it = m_providerProxyData.begin(); it != m_providerProxyData.end(); ++it)
            if (!m_proxyData.contains(it.key())) m_proxyData[it.key()] = it.value();
        const auto current = m_group->currentData().toString();
        QSignalBlocker block(m_group);
        QStringList names;
        for (const auto &entry : m_proxyData["GLOBAL"].toObject()["all"].toArray()) names << entry.toString();
        for (auto it = m_proxyData.begin(); it != m_proxyData.end(); ++it) if (!names.contains(it.key())) names << it.key();
        QStringList groups;
        for (const auto &name : names) {
            const auto p = m_proxyData[name].toObject();
            if (p["all"].isArray() && !p["hidden"].toBool()) groups << name;
        }
        bool sameGroups = m_group->count() == groups.size();
        for (int i = 0; sameGroups && i < groups.size(); ++i) sameGroups = m_group->itemData(i).toString() == groups[i];
        if (!sameGroups) m_group->clear();
        for (int i = 0; i < groups.size(); ++i) {
            const auto p = m_proxyData[groups[i]].toObject();
            const auto label = nodeDisplayName(groups[i]) + "  ·  " + p["type"].toString() + "  ·  " + nodeDisplayName(p["now"].toString());
            if (!sameGroups) m_group->addItem(label, groups[i]);
            else if (m_group->itemText(i) != label) m_group->setItemText(i, label);
        }
        int index = m_group->findData(current);
        m_group->setCurrentIndex(index >= 0 ? index : (m_group->count() ? 0 : -1));
        renderNodes();
    });
}
void Panel::renderNodes()
{
    const auto *selected = m_nodes->currentItem();
    const QString selectedName = selected ? selected->data(Qt::UserRole).toString() : QString();
    const auto group = m_proxyData[m_group->currentData().toString()].toObject();
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
    bool sameNodes = m_nodes->count() == nodes.size();
    for (int row = 0; sameNodes && row < nodes.size(); ++row)
        sameNodes = m_nodes->item(row)->data(Qt::UserRole).toString() == nodes[row].name;
    const int scroll = m_nodes->verticalScrollBar()->value();
    if (!sameNodes) { m_nodes->setUpdatesEnabled(false); m_nodes->clear(); }
    for (int row = 0; row < nodes.size(); ++row) {
        const auto &node = nodes[row];
        const bool active = group["now"].toString() == node.name;
        const bool testing = m_testing.contains(node.name);
        const auto delay = testing ? tr("测速中") : (node.delay < 0 ? tr("未测速") : (node.delay == 0 ? tr("超时") : tr("%1 ms").arg(node.delay)));
        auto *item = sameNodes ? m_nodes->item(row) : new QListWidgetItem(QIcon(nodeFlagPath(node.name)), QString(), m_nodes);
        item->setText((active ? QString::fromUtf8("✓  ") : "    ") + nodeDisplayName(node.name) + "    " + node.info["type"].toString() + "    " + delay);
        item->setData(Qt::UserRole, node.name);
        item->setData(NodeActiveRole, active);
        item->setData(NodeTypeRole, node.info["type"].toString());
        item->setData(NodeDelayRole, node.delay);
        item->setData(NodeDisplayNameRole, nodeDisplayName(node.name));
        item->setData(NodeTestingRole, testing);
        item->setData(NodeTestErrorRole, m_testErrors.value(node.name));
        item->setToolTip(node.name + "\n" + tr("点击卡片使用节点；点击延迟或状态测速。") + (m_testErrors.value(node.name).isEmpty() ? QString() : "\n" + m_testErrors.value(node.name)));
        auto font = item->font(); font.setBold(active); item->setFont(font);
        if (!sameNodes && (node.name == selectedName || (selectedName.isEmpty() && active))) m_nodes->setCurrentItem(item);
    }
    if (!sameNodes) {
        m_nodes->doItemsLayout(); m_nodes->verticalScrollBar()->setValue(scroll); m_nodes->setUpdatesEnabled(true);
    }
    m_nodes->syncAnimation();
}
void Panel::selectNode()
{
    if (!m_connected || m_selecting || !m_nodes->currentItem()) return;
    const auto name = m_nodes->currentItem()->data(Qt::UserRole).toString();
    const auto group = m_group->currentData().toString();
    const auto info = m_proxyData[group].toObject();
    if (info["type"].toString() == "LoadBalance" || !info["all"].isArray() || info["now"].toString() == name) return;
    m_selecting = true;
    m_api.request("PUT", "/proxies/" + Api::segment(group), {{"name", name}}, [this, group, name](const QJsonValue &, const QString &error) {
        m_selecting = false;
        if (!error.isEmpty()) { message(tr("节点切换失败：%1").arg(error), true); renderNodes(); return; }
        message(tr("已切换 %1 → %2").arg(group, name));
        refreshProxies();
    });
}
void Panel::testNodes(bool all, const QString &node)
{
    if (!m_connected || !m_testing.isEmpty()) return;
    QString target = m_testUrl->text().trimmed();
    if (target.isEmpty()) {
        target = m_proxyData[m_group->currentData().toString()].toObject()["testUrl"].toString();
        if (target.isEmpty()) {
            try {
                const auto runtime = parseConfig(m_store.runtime(m_store.settings()["active"].toString()));
                if (runtime["proxy-groups"]) for (const auto &group : runtime["proxy-groups"])
                    if (group["name"] && group["url"] && QString::fromStdString(group["name"].as<std::string>()) == m_group->currentData().toString())
                        target = QString::fromStdString(group["url"].as<std::string>());
            } catch (...) { }
        }
    }
    if (target.isEmpty()) target = "https://www.gstatic.com/generate_204";
    const QUrl testUrl(target);
    if (testUrl.scheme() != "https" && testUrl.scheme() != "http") { message(tr("请输入 HTTP 或 HTTPS 测试地址"), true); return; }
    QStringList names;
    if (all) for (const auto &v : m_proxyData[m_group->currentData().toString()].toObject()["all"].toArray()) names << v.toString();
    else if (!node.isEmpty()) names << node;
    names.removeDuplicates();
    if (names.isEmpty()) return;
    ++m_testEpoch;
    m_testPassed = 0; m_testFailed = 0;
    m_testTimeoutMs = m_testTimeout->value();
    guarded([&] {
        auto s = m_store.settings(); s["delayTestUrl"] = m_testUrl->text().trimmed();
        s["delayTestTimeout"] = m_testTimeoutMs; s["delayTestConcurrency"] = m_testConcurrency->value();
        m_store.setSettings(s);
    });
    QUrlQuery query; query.addQueryItem("url", testUrl.toString()); query.addQueryItem("timeout", QString::number(m_testTimeoutMs));
    m_testQuery = query.toString(QUrl::FullyEncoded);
    for (const auto &name : names) { m_testing.insert(name); m_testQueue.enqueue(name); m_testErrors.remove(name); }
    m_testGroup->setEnabled(false);
    message(tr("正在测速 %1 个节点…").arg(names.size()));
    renderNodes(); runNodeTests();
}
void Panel::runNodeTests()
{
    // Individual requests preserve successful results when another group member times out.
    while (m_activeTests < m_testConcurrency->value() && !m_testQueue.isEmpty()) {
        const auto name = m_testQueue.dequeue();
        const auto epoch = m_testEpoch;
        ++m_activeTests;
        const auto provider = m_nodeProviders.value(name);
        const auto path = provider.isEmpty() ? "/proxies/" + Api::segment(name) + "/delay?" + m_testQuery :
            "/providers/proxies/" + Api::segment(provider) + '/' + Api::segment(name) + "/healthcheck?" + m_testQuery;
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
                message(tr("测速完成：%1 个成功，%2 个失败").arg(m_testPassed).arg(m_testFailed), m_testPassed == 0);
                refreshProxies();
            } else runNodeTests();
        }, m_testTimeoutMs + 2000);
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
    button(tr("订阅设置"), row, this, [this] { editSubscriptionOptions(); });
    row->addStretch(); layout->addLayout(row);
    m_profiles = table({tr("名称"), tr("来源"), tr("更新时间")}, layout); m_profiles->setObjectName("profiles");
    m_profiles->horizontalHeader()->setStretchLastSection(false);
    m_profiles->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    auto *actions = new QHBoxLayout;
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
    auto *agent = new QLineEdit(p["userAgent"].toString()); agent->setPlaceholderText("mihomo.party/v2.0.3 (clash.meta)"); form->addRow("User-Agent", agent);
    auto *token = new QLineEdit(p["authToken"].toString()); token->setEchoMode(QLineEdit::Password); form->addRow(tr("Authorization 令牌"), token);
    auto *interval = new QSpinBox; interval->setRange(0, 10080); interval->setSuffix(tr(" 分钟")); interval->setSpecialValueText(tr("跟随全局")); interval->setValue(p["updateMinutes"].toInt()); form->addRow(tr("更新周期"), interval);
    auto *timeout = new QSpinBox; timeout->setRange(5, 180); timeout->setSuffix(tr(" 秒")); timeout->setValue(p["timeoutSeconds"].toInt(30)); form->addRow(tr("请求超时"), timeout);
    auto *automatic = new QCheckBox(tr("自动更新")); automatic->setChecked(p["autoUpdate"].toBool(true)); form->addRow(automatic);
    auto *useProxy = new QCheckBox(tr("通过正在运行的本地代理下载")); useProxy->setChecked(p["useProxy"].toBool()); form->addRow(useProxy);
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
        guarded([&] { m_store.saveProfile(QFileInfo(path).completeBaseName(), {}, readFile(path)); reloadProfiles(); message(tr("配置已导入")); });
    }
}
void Panel::editProfile(bool create)
{
    auto p = selectedProfile();
    if (!create && p.isEmpty()) return;
    guarded([&] {
        const auto name = create ? QInputDialog::getText(this, tr("新建配置"), tr("名称")) : p["name"].toString();
        if (name.isEmpty()) return;
        const auto data = create ? QByteArray("mixed-port: 17890\nmode: rule\nlog-level: info\nproxies: []\nproxy-groups:\n  - name: PROXY\n    type: select\n    proxies: [DIRECT]\nrules:\n  - MATCH,PROXY\n") : m_store.profile(p["id"].toString());
        editDocument(name, data, [this, p, name](QByteArray text) {
            m_store.saveProfile(name, p["url"].toString(), text, p["id"].toString()); reloadProfiles(); message(tr("已保存；使用此配置后生效"));
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
    fetchSubscription(url, options, localProxyPort(m_store), this, [this, url, name, id, options](const QByteArray &bytes, const QJsonObject &metadata, const QString &error) {
        if (!error.isEmpty()) { message(error, true); return; }
        guarded([&] {
            const auto savedId = m_store.saveProfile(name, url.toString(), bytes, id);
            m_store.updateProfileOptions(savedId, metadata);
            reloadProfiles();
            message(tr("订阅已更新"));
            if (!id.isEmpty() && m_store.settings()["active"].toString() == id) runService("restart", id);
        });
    });
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
    const bool coreOperation = action == "start" || action == "stop" || action == "restart";
    if (coreOperation) { m_coreBusy = true; m_coreSwitch->setEnabled(false); m_restart->setEnabled(false); }
    auto done = [this, coreOperation, extra](const QJsonValue &value, const QString &error) {
        if (coreOperation) m_coreBusy = false;
        if (value.isObject() && value.toObject().contains("running")) updateSwitches(value.toObject());
        if (extra) extra(value, error);
        if (!error.isEmpty()) { message(error, true); refresh(); return; }
        const auto text = value.toObject()["message"].toString();
        message(text.isEmpty() ? (value.toObject()["running"].toBool() ? tr("内核已启动") : tr("内核已停止")) : text);
        reloadProfiles(); refresh();
    };
    serviceRequest({{"action", "status"}}, this, [this, command, done, action](const QJsonValue &state, const QString &error) {
        if (error.isEmpty() || state.toObject().contains("running")) { serviceRequest(command, this, done); return; }
        if (action == "stop") { done(QJsonObject{{"running", false}}, {}); return; }
        auto launch = [this, command, done](bool managed) {
            if (!managed && !QProcess::startDetached(serviceExecutable(), {"--daemon"}, dataDir())) { done({}, tr("无法启动 dde-mihomo-daemon")); return; }
            QTimer::singleShot(800, this, [this, command, done] { serviceRequest(command, this, done); });
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
    m_connectionSearch = new QLineEdit; m_connectionSearch->setPlaceholderText(tr("搜索域名、进程、规则、节点")); layout->addWidget(m_connectionSearch);
    m_connections = table({tr("目标"), tr("进程"), tr("类型"), tr("规则"), tr("代理链"), tr("上传"), tr("下载")}, layout);
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
    m_ruleSearch = new QLineEdit; m_ruleSearch->setPlaceholderText(tr("搜索规则")); layout->addWidget(m_ruleSearch);
    m_rules = table({tr("类型"), tr("匹配内容"), tr("目标策略"), tr("状态")}, layout);
    auto *row = new QHBoxLayout;
    button(tr("编辑配置规则"), row, this, [this] { editSection("rules"); });
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
QWidget *Panel::providersPage()
{
    auto *page = new QWidget; auto *layout = new QVBoxLayout(page);
    m_providers = table({tr("类型"), tr("名称"), tr("来源"), tr("更新时间"), tr("条目数")}, layout);
    auto *row = new QHBoxLayout;
    button(tr("更新资源"), row, this, [this] { resourceAction(false); });
    button(tr("节点健康检查"), row, this, [this] { resourceAction(true); });
    button(tr("更新 GEO 数据"), row, this, [this] {
        m_api.request("POST", "/upgrade/geo", {}, [this](const QJsonValue &, const QString &error) { message(error.isEmpty() ? tr("GEO 数据已更新") : error, !error.isEmpty()); });
    });
    row->addStretch(); layout->addLayout(row); return page;
}
void Panel::resourceAction(bool health)
{
    const int r = m_providers->currentRow(); if (r < 0) return;
    const auto p = m_providers->item(r, 0)->data(Qt::UserRole).toJsonObject();
    const auto kind = p["kind"].toString();
    if (health && kind != "proxies") return;
    const auto path = "/providers/" + kind + '/' + Api::segment(p["name"].toString()) + (health ? "/healthcheck" : "");
    m_api.request(health ? "GET" : "PUT", path, {}, [this](const QJsonValue &, const QString &error) { message(error.isEmpty() ? tr("操作完成") : error, !error.isEmpty()); refresh(); });
}
QWidget *Panel::overridePage()
{
    auto *page = new QWidget; auto *layout = new QVBoxLayout(page);
    m_overrideEnabled = new QCheckBox(tr("启用 YAML 覆写")); m_overrideEnabled->setToolTip(tr("映射递归合并，列表整体替换，null 删除字段")); layout->addWidget(m_overrideEnabled);
    m_yamlOverride = new QPlainTextEdit; m_yamlOverride->setPlaceholderText("mixed-port: 17890\nallow-lan: false"); layout->addWidget(m_yamlOverride, 1);
    m_scriptEnabled = new QCheckBox(tr("启用 JavaScript 覆写")); m_scriptEnabled->setToolTip(tr("隔离执行，限时 4 秒")); layout->addWidget(m_scriptEnabled);
    m_jsOverride = new QPlainTextEdit; m_jsOverride->setPlaceholderText("function main(config) {\n  return config;\n}"); layout->addWidget(m_jsOverride, 1);
    auto *row = new QHBoxLayout;
    button(tr("保存覆写"), row, this, [this] {
        guarded([&] {
            const auto yaml = m_yamlOverride->toPlainText().trimmed().isEmpty() ? QByteArray("{}\n") : m_yamlOverride->toPlainText().toUtf8();
            parseConfig(yaml);
            writeFile(dataDir() + "/override.yaml", yaml);
            writeFile(dataDir() + "/override.js", m_jsOverride->toPlainText().toUtf8());
            auto s = m_store.settings(); s["overrideEnabled"] = m_overrideEnabled->isChecked(); s["scriptEnabled"] = m_scriptEnabled->isChecked(); m_store.setSettings(s);
            message(tr("覆写已保存；重启内核后生效"));
        });
    });
    button(tr("预览生效配置"), row, this, [this] { guarded([&] { editDocument(tr("生效配置"), m_store.runtime(m_store.settings()["active"].toString()), {}); }); });
    row->addStretch(); layout->addLayout(row); return page;
}
QWidget *Panel::settingsPage()
{
    auto *page = new QWidget; auto *layout = new QVBoxLayout(page); auto *form = new QFormLayout;
    m_endpoint = new QLineEdit; m_endpoint->setObjectName("controller-endpoint"); form->addRow(tr("控制器地址"), m_endpoint);
    m_secret = new QLineEdit; m_secret->setEchoMode(QLineEdit::Password); form->addRow(tr("API 密钥"), m_secret);
    auto *coreRow = new QHBoxLayout; m_corePath = new QLineEdit; coreRow->addWidget(m_corePath);
    button(tr("选择"), coreRow, this, [this] {
        const auto path = QFileDialog::getOpenFileName(this, tr("选择 Mihomo / Smart 内核")); if (!path.isEmpty()) m_corePath->setText(path);
    }); form->addRow(tr("内核路径"), coreRow);
    m_unifiedDelay = new QCheckBox(tr("统一延迟（与 Clash Party 一致，重启内核后生效）")); m_unifiedDelay->setObjectName("unified-delay"); form->addRow(m_unifiedDelay);
    m_autoSystemProxy = new QCheckBox(tr("启动内核时自动启用系统代理")); m_autoSystemProxy->setObjectName("auto-system-proxy"); form->addRow(m_autoSystemProxy);
    m_systemNodeDns = new QCheckBox(tr("节点域名使用系统 DNS（重启内核后生效）")); form->addRow(m_systemNodeDns);
    m_autoStart = new QCheckBox(tr("登录后自动启动上次使用的配置")); form->addRow(m_autoStart);
    m_updateMinutes = new QSpinBox; m_updateMinutes->setRange(0, 10080); m_updateMinutes->setSuffix(tr(" 分钟")); m_updateMinutes->setSpecialValueText(tr("关闭自动更新")); form->addRow(tr("订阅更新周期"), m_updateMinutes);
    layout->addLayout(form);
    auto *row = new QHBoxLayout;
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
        guarded([&] { restore(dataDir(), readFile(path)); loadSettings(); reloadProfiles(); message(tr("已恢复，重启内核后生效")); });
    }); backupRow->addStretch(); layout->addLayout(backupRow);
    auto *davForm = new QFormLayout;
    m_davUrl = new QLineEdit; m_davUrl->setPlaceholderText("https://server/path/dcc-proxy-backup.json"); davForm->addRow(tr("WebDAV 文件 URL"), m_davUrl);
    m_davUser = new QLineEdit; davForm->addRow(tr("用户名"), m_davUser);
    m_davPassword = new QLineEdit; m_davPassword->setEchoMode(QLineEdit::Password); davForm->addRow(tr("密码"), m_davPassword); layout->addLayout(davForm);
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
        if (QFile::exists(dataDir() + "/override.js")) m_jsOverride->setPlainText(QString::fromUtf8(readFile(dataDir() + "/override.js")));
    });
}
void Panel::saveSettings()
{
    guarded([&] {
        QString error;
        if (!m_api.configure(m_endpoint->text(), m_secret->text(), &error)) { message(error, true); return; }
        m_reads.clear(); m_polling = false; m_selecting = false;
        auto s = m_store.settings(); s["endpoint"] = m_endpoint->text(); s["secret"] = m_secret->text(); s["core"] = m_corePath->text();
        const auto controlledPath = dataDir() + "/controlled.yaml";
        auto controlled = QFile::exists(controlledPath) ? parseConfig(readFile(controlledPath)) : YAML::Node(YAML::NodeType::Map);
        controlled["unified-delay"] = m_unifiedDelay->isChecked(); writeFile(controlledPath, emitYaml(controlled));
        s["autoSystemProxy"] = m_autoSystemProxy->isChecked();
        s["systemNodeDns"] = m_systemNodeDns->isChecked();
        s["autoStart"] = m_autoStart->isChecked(); s["updateMinutes"] = m_updateMinutes->value();
        s["webdavUrl"] = m_davUrl->text(); s["webdavUser"] = m_davUser->text(); m_store.setSettings(s);
        const QString units = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation) + "/systemd/user/default.target.wants";
        const QString unit = units + "/dde-mihomo-daemon.service";
        if (m_autoStart->isChecked()) {
            QDir().mkpath(units);
            if (!QFile::exists(unit) && !QFile::link("/usr/lib/systemd/user/dde-mihomo-daemon.service", unit)) throw std::runtime_error("Cannot enable background service");
        } else QFile::remove(unit);
        QProcess::startDetached("systemctl", {"--user", "daemon-reload"});
        message(tr("设置已保存")); refresh();
    });
}
void Panel::patchConfig(const QJsonObject &patch)
{
    m_api.request("PATCH", "/configs", patch, [this, patch](const QJsonValue &, const QString &error) {
        if (!error.isEmpty()) { message(error, true); refresh(); return; }
        guarded([&] {
            const auto path = dataDir() + "/controlled.yaml";
            auto base = QFile::exists(path) ? parseConfig(readFile(path)) : YAML::Node(YAML::NodeType::Map);
            const auto delta = parseConfig(QJsonDocument(patch).toJson(QJsonDocument::Compact));
            writeFile(path, emitYaml(mergeYaml(base, delta)));
            message(tr("设置已应用并保存"));
        });
        refresh();
    });
}
void Panel::editDocument(const QString &title, const QByteArray &data, std::function<void(QByteArray)> save)
{
    QDialog dialog(this); dialog.setWindowTitle(title); dialog.resize(700, 520);
    auto *layout = new QVBoxLayout(&dialog); auto *edit = new QPlainTextEdit(QString::fromUtf8(data)); edit->setReadOnly(!save); layout->addWidget(edit);
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
        editDocument(section.toUpper(), emitYaml(wrapper), [this, id, section, node](QByteArray text) mutable {
            const auto edited = parseConfig(text);
            node[section.toStdString()] = edited[section.toStdString()];
            const auto list = m_store.profiles();
            for (const auto &p : list) if (p.toObject()["id"].toString() == id) {
                m_store.saveProfile(p.toObject()["name"].toString(), p.toObject()["url"].toString(), emitYaml(node), id); break;
            }
            runService("restart", id);
        });
    });
}
QWidget *Panel::toolsPage()
{
    auto *page = new QWidget; auto *layout = new QVBoxLayout(page);
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
        connect(reply, &QNetworkReply::finished, this, [this, reply, upload] {
            reply->deleteLater();
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (reply->error() != QNetworkReply::NoError || status < 200 || status >= 300) { message(reply->errorString(), true); return; }
            guarded([&] { if (!upload) { restore(dataDir(), reply->readAll()); loadSettings(); reloadProfiles(); } message(upload ? tr("备份上传完成") : tr("备份恢复完成")); });
        });
    });
}
void Panel::refresh()
{
    if (!isVisible()) return;
    if (!m_polling) {
    m_polling = true;
    serviceRequest({{"action", "status"}}, this, [this](const QJsonValue &state, const QString &) {
        if (state.toObject().contains("running")) updateSwitches(state.toObject());
        else if (!m_coreBusy) updateSwitches({{"running", false}});
    });
    m_api.get("/configs", [this](const QJsonValue &value, const QString &error) {
        const bool wasConnected = m_connected;
        m_polling = false; m_connected = error.isEmpty();
        if (m_connected && !wasConnected) message(tr("已连接代理内核"));
        if (!m_connected) {
            m_mode->setEnabled(false);
            serviceRequest({{"action", "status"}}, this, [this](const QJsonValue &state, const QString &serviceError) {
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
        labelText(m_overview, tr("内核已连接    混合端口 %1    HTTP %2    SOCKS %3    局域网 %4")
            .arg(config["mixed-port"].toInt()).arg(config["port"].toInt()).arg(config["socks-port"].toInt()).arg(config["allow-lan"].toBool() ? tr("允许") : tr("禁止")));
        { QSignalBlocker block(m_mode); m_mode->setCurrentIndex(m_mode->findData(config["mode"].toString())); }
        m_mode->setEnabled(true); renderNodes(); syncStreams();
    });
    }
    const int page = m_tabs->currentIndex();
    if (page == 1) refreshProxies();
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
        m_providers->setRowCount(0);
        for (const auto &kind : {"proxies", "rules"}) read(QString("/providers/%1").arg(kind), [this, kind](const QJsonValue &value, const QString &error) {
            if (!error.isEmpty()) return;
            const auto providers = value.toObject()["providers"].toObject();
            for (auto it = providers.begin(); it != providers.end(); ++it) {
                const auto p = it.value().toObject(); const int row = m_providers->rowCount(); m_providers->insertRow(row);
                cell(m_providers, row, 0, QString(kind) == "proxies" ? tr("节点") : tr("规则")); m_providers->item(row, 0)->setData(Qt::UserRole, QJsonObject{{"name", it.key()}, {"kind", kind}});
                cell(m_providers, row, 1, it.key()); cell(m_providers, row, 2, p["vehicleType"].toString()); cell(m_providers, row, 3, p["updatedAt"].toString());
                cell(m_providers, row, 4, QString::number(p.contains("proxies") ? p["proxies"].toArray().size() : p["ruleCount"].toInt()));
            }
        });
    }
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
void Panel::hideEvent(QHideEvent *event) { m_refreshTimer.stop(); m_api.stopStreams(); QWidget::hideEvent(event); }
void Panel::openPage(const QString &page)
{
    const QStringList names{"Overview", "Proxies", "Profiles", "Connections", "Rules", "Providers", "Logs", "Overrides", "Tools", "Settings", "Tun"};
    const int index = names.indexOf(page); if (index >= 0) m_tabs->setCurrentIndex(index);
}
}
