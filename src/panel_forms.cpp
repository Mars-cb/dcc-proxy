#include "panel.h"
#include <QDialog>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QLabel>
#include <QCheckBox>
#include <QLineEdit>
#include <QComboBox>
#include <QPlainTextEdit>
#include <QTableWidget>
#include <QHeaderView>
#include <QPushButton>
#include <QDialogButtonBox>
#include <QMessageBox>
#include <QJsonDocument>
#include <QHostAddress>
#include <stdexcept>
#include <QRegularExpression>
#include <QScrollArea>
namespace Party {
static QString lines(const YAML::Node &node)
{
    QStringList result;
    if (node && node.IsSequence()) { for (const auto &entry : node) if (entry.IsScalar()) result << QString::fromStdString(entry.as<std::string>()); }
    else if (node && node.IsScalar()) result << QString::fromStdString(node.as<std::string>());
    return result.join('\n');
}
void Panel::editSectionForm(const QString &section)
{
    guarded([&] {
        const auto id = websiteRuleProfile();
        if (!validId(id)) throw std::runtime_error("请先导入并选择一个订阅");
        auto runtime = parseConfig(m_store.runtime(id));
        auto value = runtime[section.toStdString()] ? YAML::Clone(runtime[section.toStdString()]) : YAML::Node(YAML::NodeType::Map);
        QDialog dialog(this); dialog.setWindowTitle(section.toUpper() + tr(" 常用设置")); dialog.resize(730, 650);
        auto *outer = new QVBoxLayout(&dialog);
        auto *scroll = new QScrollArea; scroll->setWidgetResizable(true); outer->addWidget(scroll, 1);
        auto *content = new QWidget; scroll->setWidget(content);
        auto *layout = new QVBoxLayout(content);
        auto *hint = new QLabel(tr("仅修改当前订阅的独立覆写，订阅更新后保留。未展示的高级字段保持原值。保存前会校验完整配置；应用 DNS、监听器等变更可能影响连接。")); hint->setWordWrap(true); layout->addWidget(hint);
        auto *form = new QFormLayout; layout->addLayout(form);
        QMap<QString, QCheckBox *> flags; QMap<QString, bool> initialFlags;
        QMap<QString, QLineEdit *> texts; QMap<QString, QPlainTextEdit *> lists;
        auto flag = [&](const QString &key, const QString &title) {
            auto *edit = new QCheckBox; const bool initial = value.IsMap() && value[key.toStdString()] && value[key.toStdString()].as<bool>();
            edit->setChecked(initial); edit->setObjectName(section + '-' + key); form->addRow(title, edit); flags[key] = edit; initialFlags[key] = initial;
        };
        auto text = [&](const QString &key, const QString &title) {
            auto *edit = new QLineEdit; if (value.IsMap() && value[key.toStdString()] && value[key.toStdString()].IsScalar()) edit->setText(QString::fromStdString(value[key.toStdString()].as<std::string>()));
            edit->setObjectName(section + '-' + key); form->addRow(title, edit); texts[key] = edit;
        };
        auto list = [&](const QString &key, const QString &title) {
            auto *edit = new QPlainTextEdit(lines(value[key.toStdString()])); edit->setMaximumHeight(75); edit->setObjectName(section + '-' + key); form->addRow(title, edit); lists[key] = edit;
        };
        QComboBox *mode = nullptr; QTableWidget *table = nullptr;
        QMap<QString, QLineEdit *> sniffPorts; QMap<QString, QCheckBox *> sniffEnabled;
        if (section == "dns") {
            flag("enable", tr("启用 DNS")); flag("ipv6", tr("返回 IPv6 结果"));
            mode = new QComboBox; mode->setObjectName("dns-enhanced-mode"); mode->addItems({"fake-ip", "redir-host", "normal"});
            const auto current = value["enhanced-mode"] ? QString::fromStdString(value["enhanced-mode"].as<std::string>()) : QString();
            mode->insertItem(0, tr("保持内核默认"), "");
            if (!current.isEmpty()) { int i = mode->findText(current); if (i < 0) { mode->addItem(current); i = mode->count() - 1; } mode->setCurrentIndex(i); }
            form->addRow(tr("解析模式"), mode);
            text("listen", tr("DNS 监听（例如 127.0.0.1:1053）")); text("fake-ip-range", tr("Fake IP 网段"));
            list("nameserver", tr("上游解析器，每行一项")); list("default-nameserver", tr("上游域名引导解析器")); list("proxy-server-nameserver", tr("节点域名专用解析器"));
            flag("respect-rules", tr("DNS 请求遵循分流规则")); flag("use-hosts", tr("使用配置 Hosts")); flag("use-system-hosts", tr("使用系统 Hosts"));
        } else if (section == "sniffer") {
            flag("enable", tr("启用域名嗅探")); flag("force-dns-mapping", tr("映射 DNS 域名")); flag("parse-pure-ip", tr("嗅探纯 IP 连接")); flag("override-destination", tr("覆盖连接目标"));
            for (const auto &protocol : {"TLS", "HTTP", "QUIC"}) {
                auto *row = new QHBoxLayout; auto *enabled = new QCheckBox(protocol); auto *ports = new QLineEdit;
                const auto item = value["sniff"][protocol]; enabled->setChecked(bool(item));
                auto portText = lines(item["ports"]).replace('\n', ','); ports->setText(portText); ports->setPlaceholderText("443,8443,8000-9000");
                ports->setObjectName(QString("sniffer-") + protocol + "-ports"); row->addWidget(enabled); row->addWidget(ports); form->addRow(tr("协议与端口"), row);
                sniffPorts[protocol] = ports; sniffEnabled[protocol] = enabled;
            }
            list("skip-domain", tr("跳过域名，每行一项")); list("force-domain", tr("强制嗅探域名，每行一项"));
        } else if (section == "hosts" || section == "listeners" || section == "tunnels") {
            table = new QTableWidget; table->setObjectName(section + "-form-table");
            const QStringList headers = section == "hosts" ? QStringList{tr("域名"), tr("IP 或别名，多个 IP 逗号分隔")} :
                (section == "listeners" ? QStringList{tr("名称"), tr("类型"), tr("监听地址"), tr("端口"), tr("策略")} :
                 QStringList{tr("网络（tcp/udp）"), tr("本地地址"), tr("目标地址"), tr("策略")});
            table->setColumnCount(headers.size()); table->setHorizontalHeaderLabels(headers); table->horizontalHeader()->setStretchLastSection(true); layout->addWidget(table, 1);
            auto add = [&](const QStringList &columns, const QJsonObject &original = {}) {
                const int row = table->rowCount(); table->insertRow(row);
                for (int i = 0; i < headers.size(); ++i) table->setItem(row, i, new QTableWidgetItem(columns.value(i)));
                table->item(row, 0)->setData(Qt::UserRole, original);
            };
            if (section == "hosts" && value.IsMap()) {
                for (const auto &entry : value) {
                    const auto text = entry.second.IsSequence() ? lines(entry.second).replace('\n', ',') : QString::fromStdString(entry.second.as<std::string>());
                    add({QString::fromStdString(entry.first.as<std::string>()), text});
                }
            } else if (value.IsSequence()) for (const auto &entry : value) {
                const auto original = yamlToJson(entry).toObject();
                if (entry.IsScalar()) { add(QString::fromStdString(entry.as<std::string>()).split(',')); continue; }
                auto str = [&](const char *key) { return entry[key] ? QString::fromStdString(entry[key].as<std::string>()) : QString(); };
                if (section == "listeners") add({str("name"), str("type"), str("listen"), str("port"), str("proxy")}, original);
                else add({entry["network"] && entry["network"].IsSequence() ? lines(entry["network"]).replace('\n', '/') : str("network"), str("address"), str("target"), str("proxy")}, original);
            }
            auto *row = new QHBoxLayout; layout->addLayout(row);
            auto *addButton = new QPushButton(tr("添加")); auto *remove = new QPushButton(tr("删除选中")); row->addWidget(addButton); row->addWidget(remove); row->addStretch();
            connect(addButton, &QPushButton::clicked, &dialog, [table, section] {
                const int row = table->rowCount(); table->insertRow(row);
                const QStringList defaults = section == "listeners" ? QStringList{"listener", "mixed", "127.0.0.1", "17890", "DIRECT"} : (section == "tunnels" ? QStringList{"tcp/udp", "127.0.0.1:1054", "1.1.1.1:53", "DIRECT"} : QStringList{"example.com", "127.0.0.1"});
                for (int i = 0; i < table->columnCount(); ++i) table->setItem(row, i, new QTableWidgetItem(defaults.value(i)));
            });
            connect(remove, &QPushButton::clicked, &dialog, [table] { if (table->currentRow() >= 0) table->removeRow(table->currentRow()); });
        } else { editSection(section); return; }
        auto *advanced = new QPushButton(tr("高级 YAML 编辑")); layout->addWidget(advanced);
        connect(advanced, &QPushButton::clicked, &dialog, [&] { dialog.reject(); editSection(section); });
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel); outer->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
            try {
                auto changed = YAML::Clone(value);
                for (auto it = flags.begin(); it != flags.end(); ++it)
                    if (value[it.key().toStdString()] || initialFlags[it.key()] != it.value()->isChecked()) changed[it.key().toStdString()] = it.value()->isChecked();
                for (auto it = texts.begin(); it != texts.end(); ++it) {
                    const auto text = it.value()->text().trimmed();
                    if (text.isEmpty()) changed.remove(it.key().toStdString()); else changed[it.key().toStdString()] = text.toStdString();
                }
                for (auto it = lists.begin(); it != lists.end(); ++it) {
                    YAML::Node entries(YAML::NodeType::Sequence);
                    for (const auto &line : it.value()->toPlainText().split('\n')) if (!line.trimmed().isEmpty()) entries.push_back(line.trimmed().toStdString());
                    if (entries.size() || value[it.key().toStdString()]) changed[it.key().toStdString()] = entries;
                }
                auto endpoint = [](const QString &text) {
                    const QUrl url("tcp://" + text);
                    if (url.host().isEmpty() || url.port() < 1 || url.port() > 65535 || !url.path().isEmpty()) throw std::runtime_error("监听或目标地址须包含有效主机和 1–65535 端口");
                };
                if (section == "dns") {
                    if (mode->currentIndex() > 0) changed["enhanced-mode"] = mode->currentText().toStdString(); else changed.remove("enhanced-mode");
                    if (!texts["listen"]->text().trimmed().isEmpty()) endpoint(texts["listen"]->text().trimmed());
                    const auto range = texts["fake-ip-range"]->text().trimmed();
                    if (!range.isEmpty() && (!range.contains('/') || QHostAddress::parseSubnet(range).first.isNull())) throw std::runtime_error("Fake IP 网段必须是有效 CIDR");
                } else if (section == "sniffer") {
                    for (auto it = sniffPorts.begin(); it != sniffPorts.end(); ++it) {
                        if (!sniffEnabled[it.key()]->isChecked()) { changed["sniff"].remove(it.key().toStdString()); continue; }
                        YAML::Node ports(YAML::NodeType::Sequence);
                        for (const auto &part : it.value()->text().split(',', QString::SkipEmptyParts)) {
                            const auto range = part.trimmed().split('-'); bool ok = false; const int first = range.value(0).toInt(&ok);
                            if (!ok || first < 1 || first > 65535 || range.size() > 2) throw std::runtime_error("嗅探端口须为 1–65535 或有效端口范围");
                            if (range.size() == 2) { const int last = range[1].toInt(&ok); if (!ok || last < first || last > 65535) throw std::runtime_error("嗅探端口范围无效"); ports.push_back(part.trimmed().toStdString()); }
                            else ports.push_back(first);
                        }
                        if (!ports.size()) throw std::runtime_error("启用嗅探协议时请填写端口");
                        changed["sniff"][it.key().toStdString()]["ports"] = ports;
                    }
                } else if (table) {
                    changed = YAML::Node(section == "hosts" ? YAML::NodeType::Map : YAML::NodeType::Sequence);
                    QSet<QString> names;
                    for (int r = 0; r < table->rowCount(); ++r) {
                        auto cell = [&](int c) { return table->item(r, c) ? table->item(r, c)->text().trimmed() : QString(); };
                        if (section == "hosts") {
                            if (cell(0).isEmpty() || cell(0).contains(QRegularExpression("[\\s,]")) || cell(1).isEmpty() || names.contains(cell(0))) throw std::runtime_error("Hosts 域名须唯一且不能为空，地址不能为空");
                            names.insert(cell(0)); const auto addresses = cell(1).split(',', QString::SkipEmptyParts);
                            if (addresses.size() == 1) changed[cell(0).toStdString()] = addresses.first().trimmed().toStdString();
                            else for (const auto &address : addresses) changed[cell(0).toStdString()].push_back(address.trimmed().toStdString());
                        } else {
                            auto item = table->item(r, 0)->data(Qt::UserRole).toJsonObject();
                            if (section == "listeners") {
                                bool ok = false; const int port = cell(3).toInt(&ok);
                                if (!ok || port < 1 || port > 65535 || cell(0).isEmpty() || names.contains(cell(0))) throw std::runtime_error("监听器名称须唯一，端口须为 1–65535");
                                const auto host = cell(2).contains(':') && !cell(2).startsWith('[') ? '[' + cell(2) + ']' : cell(2);
                                endpoint(host + ':' + cell(3)); names.insert(cell(0));
                                item["name"] = cell(0); item["type"] = cell(1); item["listen"] = cell(2); item["port"] = port;
                                if (!cell(4).isEmpty()) item["proxy"] = cell(4); else item.remove("proxy");
                            } else {
                                endpoint(cell(1)); endpoint(cell(2)); QJsonArray networks;
                                for (const auto &part : cell(0).split('/')) { if (part != "tcp" && part != "udp") throw std::runtime_error("隧道网络仅支持 tcp/udp"); networks.append(part); }
                                item["network"] = networks; item["address"] = cell(1); item["target"] = cell(2); item["proxy"] = cell(3);
                            }
                            changed.push_back(parseConfig(QJsonDocument(item).toJson()));
                        }
                    }
                }
                auto custom = m_store.customizations(); auto overrides = custom["overrides"].toArray(); int index = -1;
                for (int i = 0; i < overrides.size(); ++i) if (overrides[i].toObject()["editor"].toString() == section && overrides[i].toObject()["profile"].toString() == id) index = i;
                auto item = index < 0 ? QJsonObject{{"id", newId()}} : overrides[index].toObject();
                YAML::Node wrapper; wrapper[section.toStdString()] = changed;
                item["name"] = section.toUpper() + tr(" 常用设置"); item["kind"] = "yaml"; item["profile"] = id; item["enabled"] = true; item["editor"] = section; item["content"] = QString::fromUtf8(emitYaml(wrapper));
                if (index < 0) overrides.append(item); else overrides[index] = item;
                custom["overrides"] = overrides;
                saveChange({{"files", QJsonObject{{"customizations.json", QString::fromLatin1(QJsonDocument(custom).toJson().toBase64())}}}}, [this, id] { applySavedProfile(id); }); dialog.accept();
            } catch (const std::exception &e) { QMessageBox::warning(&dialog, tr("设置无效"), QString::fromUtf8(e.what())); }
        }); dialog.exec();
    });
}
}
