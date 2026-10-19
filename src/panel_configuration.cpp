#include "panel.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QComboBox>
#include <QPushButton>
#include <QPlainTextEdit>
#include <QListWidget>
#include <QDialog>
#include <QDialogButtonBox>
#include <QInputDialog>
#include <QMessageBox>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QFileInfo>
#include <QTabWidget>
#include <QSignalBlocker>
#include <QLineEdit>
#include <QSet>
#include <stdexcept>
namespace Party {
void Panel::restoreArchive(const QByteArray &bytes)
{
    if (bytes.size() > 32 * 1024 * 1024) throw std::runtime_error("备份超过 32 MiB");
    const auto archive = QJsonDocument::fromJson(bytes).object();
    auto files = archive["files"].toObject();
    if (archive["format"].toInt() != 1 || !files.contains("settings.json") || !files.contains("profiles.json")) throw std::runtime_error("备份缺少完整的设置和订阅索引");
    for (const auto &file : managedConfigFiles(m_store.root())) if (!files.contains(file)) files[file] = QJsonValue::Null;
    saveChange({{"files", files}, {"restore", true}}, [this] { loadSettings(); reloadProfiles(); message(tr("备份已通过完整校验并恢复；后台继续运行，新配置待明确应用。")); });
}
void Panel::saveChange(const QJsonObject &change, std::function<void()> after)
{
    if (m_configBusy) { message(tr("另一项配置正在校验，请稍后保存"), true); return; }
    m_configBusy = true; message(tr("正在校验完整配置，当前代理继续运行…"));
    try {
        validateAndCommit(m_store.root(), change, this, scopedReply([this, after](const QJsonValue &, const QString &error) {
            m_configBusy = false;
            if (!error.isEmpty()) { message(error, true); return; }
            reloadProfiles(); message(tr("完整配置校验通过并已保存"));
            if (after) after();
        }));
    } catch (const std::exception &e) { m_configBusy = false; message(QString::fromUtf8(e.what()), true); }
}
void Panel::saveProfileCandidate(const QString &name, const QString &url, const QByteArray &bytes, const QString &existing, const QJsonObject &metadata)
{
    const auto id = existing.isEmpty() ? newId() : existing;
    bool changed = true;
    if (!existing.isEmpty()) changed = emitYaml(parseConfig(normalizeSubscription(bytes))) != emitYaml(parseConfig(m_store.profile(id)));
    saveChange({{"id", id}, {"name", name}, {"url", url}, {"source", QString::fromLatin1(bytes.toBase64())}, {"metadata", metadata}}, [this, id, existing, changed] {
        showProfileNodes(id);
        if (!existing.isEmpty() && changed) applySavedProfile(id);
        else if (!existing.isEmpty()) message(tr("订阅内容未变化，仅更新元数据；当前内核未重载。"));
        else message(tr("订阅已通过完整校验并导入，可在代理组查看"));
    });
}
void Panel::applySavedProfile(const QString &id)
{
    if (id != m_store.settings()["active"].toString()) { message(tr("配置已保存，使用该订阅时生效")); return; }
    if (!localController(m_store.settings()["endpoint"].toString())) { message(tr("本地配置已保存；当前远程控制器未修改")); return; }
    requestService({{"action", "status"}}, this, [this, id](const QJsonValue &value, const QString &error) {
        const auto state = value.toObject();
        if (!error.isEmpty() || !state["running"].toBool()) { message(tr("配置已保存，下次启动生效")); return; }
        if (!state["capabilities"].toArray().contains("apply")) {
            message(tr("配置已校验保存。当前后台仍为旧版本，为保持代理不中断未重启；新后台正常启动后生效。")); return;
        }
        backgroundCommand({{"action", "apply"}, {"profile", id}});
    });
}
void Panel::editCustomRules()
{
    if (m_store.profiles().isEmpty()) { message(tr("请先导入订阅"), true); return; }
    QDialog dialog(this); dialog.setWindowTitle(tr("独立自定义规则")); dialog.resize(720, 540);
    auto *layout = new QVBoxLayout(&dialog);
    auto *hint = new QLabel(tr("自定义规则按当前顺序优先匹配，独立保存，订阅更新不会覆盖。每行一条 Mihomo 规则。旧版本直接写进订阅的规则，可从下方原始规则中复制需要保留的条目。"));
    hint->setWordWrap(true); layout->addWidget(hint);
    auto *scope = new QComboBox; scope->setObjectName("custom-rules-profile");
    for (const auto &entry : m_store.profiles()) { const auto p = entry.toObject(); scope->addItem(p["name"].toString(), p["id"]); }
    scope->setCurrentIndex(qMax(0, scope->findData(websiteRuleProfile()))); layout->addWidget(scope);
    auto *edit = new QPlainTextEdit; edit->setObjectName("custom-rules-editor"); layout->addWidget(edit, 1);
    auto *source = new QPlainTextEdit; source->setReadOnly(true); source->setMaximumHeight(130); layout->addWidget(source);
    auto custom = m_store.customizations(); auto draft = custom["rules"].toObject(); QString current;
    auto flush = [&] { if (!current.isEmpty()) { QJsonArray rules; for (const auto &line : edit->toPlainText().split('\n')) if (!line.trimmed().isEmpty()) rules.append(line.trimmed()); draft[current] = rules; } };
    auto load = [&] {
        flush(); current = scope->currentData().toString(); QStringList rules, original;
        for (const auto &entry : draft[current].toArray()) rules << entry.toString();
        edit->setPlainText(rules.join('\n'));
        try { const auto config = parseConfig(m_store.profile(current)); if (config["rules"]) for (const auto &r : config["rules"]) original << QString::fromStdString(r.as<std::string>()); }
        catch (const std::exception &e) { original << QString::fromUtf8(e.what()); }
        source->setPlainText(original.join('\n'));
    };
    connect(scope, QOverload<int>::of(&QComboBox::currentIndexChanged), &dialog, [&](int) { load(); }); load();
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel); layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        flush(); custom["rules"] = draft;
        saveChange({{"files", QJsonObject{{"customizations.json", QString::fromLatin1(QJsonDocument(custom).toJson().toBase64())}}}}, [this] { applySavedProfile(m_store.settings()["active"].toString()); });
        dialog.accept();
    }); dialog.exec();
}
void Panel::editManagedOverrides()
{
    QDialog dialog(this); dialog.setWindowTitle(tr("有序覆写管理")); dialog.resize(760, 580);
    auto *layout = new QVBoxLayout(&dialog);
    auto *hint = new QLabel(tr("按列表顺序执行，后面的同名字段覆盖前面的字段。可关联全部订阅或一个订阅；自定义规则与专用端口/TUN 设置最终生效。保存前会校验受影响的完整配置。")); hint->setWordWrap(true); layout->addWidget(hint);
    auto *list = new QListWidget; list->setObjectName("ordered-overrides"); layout->addWidget(list, 1);
    auto custom = m_store.customizations(); auto draft = custom["overrides"].toArray();
    auto reload = [&](int selected) {
        const QSignalBlocker block(list); list->clear();
        for (const auto &entry : draft) {
            const auto item = entry.toObject(); QString scope = tr("全部订阅");
            if (!item["profile"].toString().isEmpty()) {
                scope = item["profile"].toString();
                for (const auto &p : m_store.profiles()) if (p.toObject()["id"] == item["profile"]) scope = p.toObject()["name"].toString();
            }
            auto *row = new QListWidgetItem(item["name"].toString() + " · " + item["kind"].toString() + " · " + scope, list);
            row->setFlags(row->flags() | Qt::ItemIsUserCheckable); row->setCheckState(item.value("enabled").toBool(true) ? Qt::Checked : Qt::Unchecked);
        }
        list->setCurrentRow(qMin(selected, list->count() - 1));
    }; reload(0);
    connect(list, &QListWidget::itemChanged, &dialog, [&](QListWidgetItem *row) { const int index = list->row(row); auto item = draft[index].toObject(); item["enabled"] = row->checkState() == Qt::Checked; draft[index] = item; });
    auto *row = new QHBoxLayout; layout->addLayout(row);
    auto button = [&](const QString &text, std::function<void()> action) { auto *b = new QPushButton(text); row->addWidget(b); connect(b, &QPushButton::clicked, &dialog, std::move(action)); return b; };
    auto editItem = [&](bool create) {
        const int index = create ? draft.size() : list->currentRow(); if (index < 0) return;
        auto item = create ? QJsonObject{{"id", newId()}, {"enabled", true}, {"kind", "yaml"}} : draft[index].toObject();
        bool ok = false;
        const auto name = QInputDialog::getText(&dialog, tr("覆写名称"), tr("名称"), QLineEdit::Normal, item["name"].toString(), &ok); if (!ok || name.trimmed().isEmpty()) return;
        const auto kind = QInputDialog::getItem(&dialog, tr("覆写类型"), tr("类型"), {"yaml", "javascript"}, item["kind"].toString() == "javascript" ? 1 : 0, false, &ok); if (!ok) return;
        QStringList labels{tr("全部订阅")}, ids{QString()};
        for (const auto &p : m_store.profiles()) { labels << p.toObject()["name"].toString() + " [" + p.toObject()["id"].toString().left(8) + ']'; ids << p.toObject()["id"].toString(); }
        const auto label = QInputDialog::getItem(&dialog, tr("关联订阅"), tr("作用范围"), labels, qMax(0, ids.indexOf(item["profile"].toString())), false, &ok); if (!ok) return;
        item["name"] = name.trimmed(); item["kind"] = kind; item["profile"] = ids.value(labels.indexOf(label));
        QByteArray content = item["content"].toString().toUtf8();
        if (content.isEmpty()) content = kind == "yaml" ? QByteArray("{}\n") : QByteArray("function main(config, profileName) { return config; }\n");
        editDocument(name, content, [&](QByteArray bytes) {
            if (kind == "yaml") parseConfig(bytes);
            item["content"] = QString::fromUtf8(bytes);
            if (create) draft.append(item); else draft[index] = item;
            reload(index);
        });
    };
    button(tr("新增"), [&] { editItem(true); }); button(tr("编辑"), [&] { editItem(false); });
    button(tr("删除"), [&] { const int i = list->currentRow(); if (i >= 0) { draft.removeAt(i); reload(i); } });
    button(tr("上移"), [&] { const int i = list->currentRow(); if (i > 0) { const auto item = draft.takeAt(i); draft.insert(i - 1, item); reload(i - 1); } });
    button(tr("下移"), [&] { const int i = list->currentRow(); if (i >= 0 && i + 1 < draft.size()) { const auto item = draft.takeAt(i); draft.insert(i + 1, item); reload(i + 1); } });
    button(tr("差异与冲突预览"), [&] {
        guarded([&] {
            auto id = websiteRuleProfile(); if (!validId(id)) throw std::runtime_error("请先导入并选择订阅");
            QTemporaryDir stage;
            for (const auto &file : managedConfigFiles(m_store.root())) if (QFileInfo::exists(m_store.root() + '/' + file)) writeFile(stage.path() + '/' + file, readFile(m_store.root() + '/' + file));
            Store candidate(stage.path()); auto changed = custom; changed["overrides"] = draft; candidate.setCustomizations(changed);
            QSet<QString> fields; QStringList conflicts;
            for (const auto &entry : draft) {
                const auto item = entry.toObject(); if (!item.value("enabled").toBool(true) || (!item["profile"].toString().isEmpty() && item["profile"].toString() != id) || item["kind"].toString() != "yaml") continue;
                const auto yaml = parseConfig(item["content"].toString().toUtf8());
                for (const auto &field : yaml) { const auto key = QString::fromStdString(field.first.as<std::string>()); if (fields.contains(key)) conflicts << key; fields.insert(key); }
            }
            QDialog preview(&dialog); preview.setWindowTitle(tr("生效配置差异")); preview.resize(900, 650); auto *box = new QVBoxLayout(&preview);
            auto *warning = new QLabel(conflicts.isEmpty() ? tr("未发现 YAML 顶层字段重复；JavaScript 的实际效果以下方预览为准。") : tr("存在重复字段，按顺序覆盖：%1").arg(conflicts.join(", "))); warning->setWordWrap(true); box->addWidget(warning);
            auto *tabs = new QTabWidget; box->addWidget(tabs);
            for (const auto &entry : {qMakePair(tr("保存前"), m_store.runtime(id)), qMakePair(tr("保存后"), candidate.runtime(id))}) { auto *text = new QPlainTextEdit(QString::fromUtf8(entry.second)); text->setReadOnly(true); tabs->addTab(text, entry.first); }
            preview.exec();
        });
    });
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel); layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        custom["overrides"] = draft;
        saveChange({{"files", QJsonObject{{"customizations.json", QString::fromLatin1(QJsonDocument(custom).toJson().toBase64())}}}}, [this] { applySavedProfile(m_store.settings()["active"].toString()); }); dialog.accept();
    }); dialog.exec();
}
}
