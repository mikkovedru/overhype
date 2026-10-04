#include "deck.h"
#include "filedialog.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <algorithm>

namespace {
QSettings preferences() { return QSettings(QSettings::IniFormat, QSettings::UserScope, "hype", "hype"); }
QVariantList readRecents(const QString &directory) {
    QFile file(directory + "/recents.json");
    if (!file.open(QIODevice::ReadOnly)) return {};
    return QJsonDocument::fromJson(file.readAll()).array().toVariantList();
}
void writeRecents(const QString &directory, const QVariantList &entries) {
    if (!QDir().mkpath(directory)) return;
    QSaveFile file(directory + "/recents.json");
    const auto bytes = QJsonDocument(QJsonArray::fromVariantList(entries)).toJson();
    if (file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size()) file.commit();
}
}
QString Deck::stateDirectory() const { return QStandardPaths::writableLocation(QStandardPaths::StateLocation); }
QString Deck::editorMode() const {
    if (!m_guiContext || m_home) return "visual";
    const QString key = QString::fromLatin1(QCryptographicHash::hash((m_path.isEmpty() ? m_draftId : m_path).toUtf8(), QCryptographicHash::Sha256).toHex());
    return preferences().value("editorModes/" + key, "visual").toString();
}
void Deck::rememberEditorMode(const QString &mode) {
    if (!m_guiSession || m_home || !QStringList{"visual", "markdown", "overview"}.contains(mode)) return;
    const QString key = QString::fromLatin1(QCryptographicHash::hash((m_path.isEmpty() ? m_draftId : m_path).toUtf8(), QCryptographicHash::Sha256).toHex());
    preferences().setValue("editorModes/" + key, mode);
}
bool Deck::continueOnStartup() const { return preferences().value("startup/continue", false).toBool(); }
void Deck::setContinueOnStartup(bool value) { preferences().setValue("startup/continue", value); emit homeChanged(); }
bool Deck::rememberRecents() const { return preferences().value("files/rememberRecents", true).toBool(); }
void Deck::setRememberRecents(bool value) { preferences().setValue("files/rememberRecents", value); emit homeChanged(); }
QVariantList Deck::recentPresentations() const {
    if (!rememberRecents()) return {};
    auto entries = readRecents(stateDirectory());
    // Import the existing Continue target once, without changing its settings.
    if (entries.isEmpty() && !QFile::exists(stateDirectory() + "/recents.json")) {
        const auto settings = preferences();
        const QString path = settings.value("files/lastRecoveryDocument", settings.value("files/lastPresentation")).toString();
        if (!path.isEmpty()) entries.append(QVariantMap{{"path", path}, {"title", QFileInfo(path).completeBaseName()}, {"slide", 1}, {"opened", QString()}});
    }
    return entries.mid(0, 20);
}
void Deck::recordRecent(bool opened) {
    if (!m_guiSession || !rememberRecents() || m_path.isEmpty()) return;
    auto entries = readRecents(stateDirectory());
    if (!opened) {
        for (int i = 0; i < entries.size(); ++i) if (entries[i].toMap()["path"].toString() == m_path) {
            auto entry = entries[i].toMap(); entry["slide"] = m_selected + 1; entry["count"] = count();
            entries[i] = entry; writeRecents(stateDirectory(), entries); return;
        }
        return;
    }
    for (int i = entries.size() - 1; i >= 0; --i)
        if (entries[i].toMap()["path"].toString() == m_path) entries.removeAt(i);
    entries.prepend(QVariantMap{{"path", m_path}, {"title", title()}, {"slide", m_selected + 1}, {"count", count()},
                               {"opened", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)}});
    writeRecents(stateDirectory(), entries.mid(0, 20));
    emit homeChanged();
}
void Deck::removeRecent(const QString &path) {
    auto entries = recentPresentations();
    for (int i = entries.size() - 1; i >= 0; --i) if (entries[i].toMap()["path"].toString() == path) entries.removeAt(i);
    writeRecents(stateDirectory(), entries); emit homeChanged();
}
void Deck::clearRecents() { writeRecents(stateDirectory(), {}); emit homeChanged(); }
QVariantList Deck::unfinishedDrafts() const {
    if (m_homeDraftsValid) return m_homeDrafts;
    QVariantList entries;
    const QDir root(m_recoveryDirectory.isEmpty() ? stateDirectory() + "/recovery" : m_recoveryDirectory);
    for (const auto &folder : root.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        const QDir directory(root.filePath(folder));
        QFile latest(directory.filePath("latest.json"));
        QJsonObject snapshot;
        if (latest.open(QIODevice::ReadOnly)) snapshot = QJsonDocument::fromJson(latest.readAll()).object();
        if (snapshot["retired"].toBool()) continue;
        auto valid = [](const QJsonObject &entry) {
            return entry["version"].toInt() == 1 && entry["source"].isString() && entry["saved"].isString() &&
                QString::fromLatin1(QCryptographicHash::hash(entry["source"].toString().toUtf8(), QCryptographicHash::Sha256).toHex()) == entry["sha256"].toString();
        };
        if (!valid(snapshot)) {
            QDir versions(directory.filePath("versions"));
            for (const auto &name : versions.entryList({"*.json"}, QDir::Files, QDir::Name | QDir::Reversed)) {
                QFile file(versions.filePath(name));
                if (file.open(QIODevice::ReadOnly)) snapshot = QJsonDocument::fromJson(file.readAll()).object();
                if (valid(snapshot)) break;
            }
        }
        if (!valid(snapshot)) continue;
        const QString source = snapshot["source"].toString(), path = snapshot["path"].toString();
        if (QString::fromLatin1(QCryptographicHash::hash(source.toUtf8(), QCryptographicHash::Sha256).toHex()) != snapshot["sha256"].toString()) continue;
        if (!path.isEmpty() && source == snapshot["saved"].toString()) continue;
        if (path.isEmpty() && source == "---\ntitle: Untitled\ntheme: tokyo-night\n---\n\n# Your next idea\n") continue;
        const auto parsed = parseDeck(source);
        QString title = scalar(parsed.header, "title", path.isEmpty() ? "Untitled draft" : QFileInfo(path).completeBaseName());
        if (path.isEmpty() && (title.isEmpty() || title == "Untitled")) {
            for (const auto &line : source.split('\n')) if (line.startsWith("# ")) { title = line.mid(2); break; }
        }
        entries.append(QVariantMap{{"identity", snapshot["draftId"].toString("legacy-untitled")}, {"path", path}, {"title", title},
                                  {"opened", snapshot["timestamp"].toString()}, {"slide", snapshot["selected"].toInt() + 1}});
    }
    std::sort(entries.begin(), entries.end(), [](const QVariant &a, const QVariant &b) { return QDateTime::fromString(a.toMap()["opened"].toString(), Qt::ISODate) > QDateTime::fromString(b.toMap()["opened"].toString(), Qt::ISODate); });
    m_homeDrafts = entries; m_homeDraftsValid = true;
    return entries;
}
QVariantMap Deck::continuation() const {
    const auto drafts = unfinishedDrafts(), recents = recentPresentations();
    if (!drafts.isEmpty() && (recents.isEmpty() || QDateTime::fromString(drafts.first().toMap()["opened"].toString(), Qt::ISODate) >= QDateTime::fromString(recents.first().toMap()["opened"].toString(), Qt::ISODate))) return drafts.first().toMap();
    return recents.isEmpty() ? QVariantMap{} : recents.first().toMap();
}
bool Deck::showHome() {
    if (m_exporting || m_compressingImage) { setStatus("Finish the current operation before returning Home."); return false; }
    if (!m_home && !confirmDiscard()) return false;
    if (!m_home) { m_hasActive = true; recordRecent(false); }
    m_homeDraftsValid = false;
    m_reloadTimer.stop();
    m_home = true; setStatus(QString()); m_autosaveTimer.stop(); m_autosaveDeadline.stop(); emit homeChanged(); return true;
}
void Deck::resumeActive() { if (m_hasActive) { m_home = false; watch(); m_reloadTimer.start(); emit homeChanged(); emit opened(!m_path.isEmpty()); } }
bool Deck::openPresentation(const QString &path) {
    const QString absolute = QFileInfo(path).absoluteFilePath();
    const QString canonical = QFileInfo(path).canonicalFilePath();
    if (!m_path.isEmpty() && (absolute == m_path || (!canonical.isEmpty() && canonical == QFileInfo(m_path).canonicalFilePath()))) {
        if (m_home) resumeActive();
        return true;
    }
    if (m_exporting || m_compressingImage || !confirmDiscard()) return false;
    if (!loadPath(path)) { const auto error = status(); showHome(); setStatus("Could not open " + path + ": " + error); return false; }
    emit opened(true); return true;
}
bool Deck::openDroppedPresentation(const QUrl &url) {
    if (!url.isLocalFile() || !url.toLocalFile().endsWith(".md", Qt::CaseInsensitive)) { setStatus("Drop a local Markdown (.md) presentation."); return false; }
    return openPresentation(url.toLocalFile());
}
bool Deck::resumeLastGui() {
    if (!confirmDiscard()) return false;
    const bool opened = reopenLastPresentation();
    m_home = false;
    recoverDraft();
    if (opened || !m_checkpointSource.isEmpty()) { emit homeChanged(); emit this->opened(true); return true; }
    const auto error = status(); m_home = true; emit homeChanged(); setStatus(error); return false;
}
bool Deck::continuePresentation() {
    const auto entry = continuation();
    if (entry.isEmpty()) return false;
    return entry.contains("identity") ? openDraft(entry["identity"].toString(), entry["path"].toString()) : openPresentation(entry["path"].toString());
}
bool Deck::openDraft(const QString &identity, const QString &path) {
    // Accept only a draft that was actually enumerated, never an arbitrary recovery path.
    bool found = false;
    for (const auto &entry : unfinishedDrafts()) if (entry.toMap()["identity"] == identity && entry.toMap()["path"] == path) found = true;
    if (!found || !confirmDiscard()) return false;
    if (!path.isEmpty() && QFileInfo(path).isFile()) return openPresentation(path);
    m_home = false; m_hasActive = false; m_draftId = identity; m_path = path; m_saved.clear(); m_externalChange = !path.isEmpty();
    apply(QString(), 0, false); m_undo.clear(); m_redo.clear(); recoverDraft(); watch(); emit homeChanged(); emit opened(true); return true;
}
void Deck::locateRecent(const QString &path) {
    QString error;
    const QString replacement = FileDialog::choose(false, QFileInfo(path).absolutePath(), "Markdown", {"*.md"}, &error);
    if (!error.isEmpty()) setStatus(error);
    if (!replacement.isEmpty() && openPresentation(replacement)) removeRecent(path);
}
