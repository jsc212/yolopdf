// SPDX-License-Identifier: AGPL-3.0-only
#include "DocumentWorker.h"
#include "platform/FileAccess.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QUuid>
#include <algorithm>
#include <cmath>
#include <set>

namespace yolo::core {
namespace {
void atomicWrite(const QString &path, const QByteArray &bytes) {
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
        throw std::runtime_error("Cannot write verified PDF or recovery data");
}
QByteArray byteArray(const std::vector<std::uint8_t> &v) {
    return {reinterpret_cast<const char *>(v.data()), qsizetype(v.size())};
}
std::vector<std::uint8_t> vector(const QByteArray &v) {
    return {v.begin(), v.end()};
}
QString status(engine::TextStatus s) {
    return s == engine::TextStatus::OriginalFont ? "Original"
           : s == engine::TextStatus::NeedsFont  ? "Needs font"
                                                 : "Unsupported";
}
bool dirty(Session &s) {
    auto h = s.document->history();
    return h.position >= int(s.states.size()) || s.states[h.position] != s.savedState;
}
QString annotationType(engine::AnnotationType t) {
    switch (t) {
    case engine::AnnotationType::Stamp:
        return "Stamp";
    case engine::AnnotationType::Highlight:
        return "Highlight";
    case engine::AnnotationType::Strikeout:
        return "Strikeout";
    case engine::AnnotationType::Marker:
        return "Marker";
    case engine::AnnotationType::Note:
        return "Note";
    case engine::AnnotationType::Arrow:
        return "Arrow";
    case engine::AnnotationType::Rectangle:
        return "Rectangle";
    default:
        return "Annotation";
    }
}
} // namespace
QByteArray fingerprint(const QUrl &url) {
    QFile file(url.isLocalFile() ? url.toLocalFile() : url.toString(QUrl::FullyEncoded));
    if (!file.open(QIODevice::ReadOnly))
        return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file))
        return {};
    return hash.result().toHex();
}
QImage image(const engine::RasterImage &r) {
    return QImage(r.rgba.data(), r.width, r.height, r.width * 4, QImage::Format_RGBA8888).copy();
}
QVariantMap bounds(const engine::Rect &r) {
    return {{"x", r.x0}, {"y", r.y0}, {"width", r.x1 - r.x0}, {"height", r.y1 - r.y0}};
}
std::vector<int> pageRange(const QString &text, int count) {
    std::vector<int> result;
    for (auto part : text.split(',')) {
        const auto pair = part.trimmed().split('-');
        bool ok = false;
        int first = pair.value(0).toInt(&ok);
        if (!ok || first < 1 || first > count || pair.size() > 2)
            throw std::runtime_error("Invalid page range");
        int last = first;
        if (pair.size() == 2) {
            last = pair[1].toInt(&ok);
            if (!ok || last < first || last > count)
                throw std::runtime_error("Invalid page range");
        }
        for (int i = first; i <= last; ++i)
            result.push_back(i - 1);
    }
    if (result.empty())
        throw std::runtime_error("Select at least one page");
    return result;
}
DocumentWorker::DocumentWorker(QString root)
    : recoveryRoot_(root.isEmpty()
                        ? QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
                              "/recovery"
                        : root) {
    if (!QDir().mkpath(recoveryRoot_))
        throw std::runtime_error("Cannot create recovery directory");
}
DocumentWorker::~DocumentWorker() {
    shutdown();
}
Session &DocumentWorker::session(const QString &id) {
    for (auto &s : sessions_)
        if (s->id == id)
            return *s;
    throw std::runtime_error("The document is no longer open");
}
std::unique_ptr<engine::IRenderJob> DocumentWorker::renderJob(const QString &id, int page,
                                                              int revision) {
    auto &s = session(id);
    if (s.revision != revision)
        throw std::runtime_error("Render was superseded");
    const auto key = id + "/" + QString::number(revision) + "/" + QString::number(page);
    auto it = scenes_.find(key);
    if (it == scenes_.end()) {
        if (scenes_.size() >= 16)
            scenes_.erase(scenes_.begin());
        it = scenes_.emplace(key, s.document->renderSource(page)).first;
    }
    return it->second->job();
}
WorkerResult DocumentWorker::snapshot() {
    WorkerResult result;
    for (auto &s : sessions_)
        result.tabs.append(QVariantMap{{"id", s->id}, {"title", s->title}, {"dirty", dirty(*s)}});
    if (activeId.isEmpty())
        return result;
    auto &s = session(activeId);
    const auto info = s.document->inspect();
    const auto h = s.document->history();
    if (s.selection.contains("activePage"))
        for (const auto &page : info.pages)
            if (page.id == s.selection.value("activePage").toInt())
                s.page = page.index;
    s.page = std::clamp(s.page, 0, std::max(0, int(info.pages.size()) - 1));
    QVariantList pages;
    for (auto &p : info.pages) {
        double w = p.visibleBounds.x1 - p.visibleBounds.x0,
               h = p.visibleBounds.y1 - p.visibleBounds.y0;
        if (!std::isfinite(w) || !std::isfinite(h) || w <= 0 || h <= 0)
            throw std::runtime_error("PDF page geometry is invalid");
        pages.append(QVariantMap{{"index", p.index},
                                 {"id", p.id},
                                 {"width", w},
                                 {"height", h},
                                 {"rotation", p.rotation}});
    }
    QVariantList runs, images, annotations, warnings;
    for (auto &w : info.warnings)
        warnings.append(QString::fromStdString(w));
    const auto text = s.document->textRuns(s.page);
    const auto lines = s.document->textLines(s.page);
    for (auto &line : lines) {
        const auto &style = line.segments.front().style;
        const auto &tm = style.textMatrix, &ctm = style.ctm;
        const double verticalX = tm[2] * ctm[0] + tm[3] * ctm[2],
                     verticalY = tm[2] * ctm[1] + tm[3] * ctm[3];
        QSet<QString> names;
        QVariantList segments;
        for (auto &part : line.segments) {
            auto font = QString::fromStdString(part.style.fontName);
            names.insert(font);
            segments.append(QVariantMap{{"runId", part.runId},
                                        {"start", part.start},
                                        {"length", part.length},
                                        {"font", font},
                                        {"size", part.style.fontSize}});
        }
        auto b = bounds(line.bounds);
        b.insert("id", line.id);
        b.insert("text", QString::fromStdString(line.text));
        b.insert("font", names.size() > 1 ? QString("Mixed fonts")
                                          : QString::fromStdString(style.fontName));
        b.insert("size", style.fontSize * std::hypot(verticalX, verticalY));
        b.insert("status", status(line.status));
        b.insert("reason", QString::fromStdString(line.reason));
        b.insert("segments", segments);
        runs.append(b);
    }
    for (auto &i : s.document->images(s.page)) {
        auto b = bounds(i.bounds);
        QVariantList matrix;
        for (auto v : i.transform)
            matrix.append(v);
        b.insert("id", i.id);
        b.insert("matrix", matrix);
        b.insert("pixelWidth", i.width);
        b.insert("pixelHeight", i.height);
        images.append(b);
    }
    for (auto &a : s.document->annotations(s.page)) {
        auto b = bounds(a.bounds);
        b.insert("id", a.id);
        b.insert("type", annotationType(a.type));
        b.insert("contents", QString::fromStdString(a.contents));
        b.insert("opacity", a.opacity);
        annotations.append(b);
    }
    result.active = {{"id", s.id},
                     {"title", s.title},
                     {"source", s.source},
                     {"dirty", dirty(s)},
                     {"revision", s.revision},
                     {"page", s.page},
                     {"pages", pages},
                     {"selection", s.selection},
                     {"runs", runs},
                     {"images", images},
                     {"annotations", annotations},
                     {"warnings", warnings},
                     {"noTextReason", QString::fromStdString(text.noTextReason)},
                     {"signed", info.signedDocument},
                     {"encrypted", info.encrypted},
                     {"mayEdit", info.mayEdit},
                     {"mayCopy", info.mayCopy},
                     {"mayPrint", info.mayPrint},
                     {"canUndo", h.position > 0},
                     {"canRedo", h.position < h.steps},
                     {"undoLabel", h.position > 0 ? QString::fromStdString(h.labels[h.position - 1])
                                                  : QString()},
                     {"redoLabel", h.position < h.steps
                                       ? QString::fromStdString(h.labels[h.position])
                                       : QString()}};
    return result;
}
WorkerResult DocumentWorker::open(const QUrl &url, const QString &password, bool spike) {
    auto s = std::make_unique<Session>();
    s->id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    s->source = url;
    s->title = spike ? "Sample PDF" : url.fileName();
    s->inputCache = platform::cachePdf(url);
    try {
        s->document = engine::openDocument(s->inputCache.toUtf8().toStdString(),
                                           password.toUtf8().toStdString());
        s->fingerprint = fingerprint(url);
        if (spike) {
            s->document->replaceText(0, {0, "éè", {}});
            markChanged(*s);
        }
    } catch (...) {
        QFile::remove(s->inputCache);
        throw;
    }
    const auto previous = activeId;
    activeId = s->id;
    sessions_.push_back(std::move(s));
    try {
        auto r = snapshot();
        r.event = "loaded";
        const auto geometry = r.active.value("pages").toList().front().toMap();
        const double area =
            geometry.value("width").toDouble() * geometry.value("height").toDouble();
        const int dpi = std::clamp(int(std::sqrt(8.0 * 1024 * 1024 / area) * 72), 1, 96);
        r.imageKey = activeId + "/" + QString::number(session(activeId).revision) + "/0/" +
                     QString::number(dpi);
        r.image = image(session(activeId).document->render(0, dpi));
        return r;
    } catch (...) {
        QFile::remove(sessions_.back()->inputCache);
        sessions_.pop_back();
        activeId = previous;
        throw;
    }
}
WorkerResult DocumentWorker::activate(const QString &id, int page) {
    activeId = id;
    if (page >= 0) {
        auto &s = session(id);
        auto pages = s.document->inspect().pages;
        if (page >= int(pages.size()))
            throw std::runtime_error("Invalid page selection");
        s.page = page;
        s.selection["activePage"] = pages[page].id;
        s.selection["kind"] = "";
        s.selection["objectId"] = -1;
        setSelection(id, s.selection);
    }
    return snapshot();
}
void DocumentWorker::setSelection(const QString &id, const QVariantMap &selection) {
    auto &s = session(id);
    s.selection = selection;
    const int position = s.document->history().position;
    if (position < int(s.states.size()))
        s.selections[s.states[position]] = selection;
}
void DocumentWorker::restoreSelection(Session &s) {
    const int position = s.document->history().position;
    if (position < int(s.states.size())) {
        auto it = s.selections.find(s.states[position]);
        s.selection = it == s.selections.end() ? QVariantMap() : it->second;
    }
}
void DocumentWorker::markChanged(Session &s) {
    const auto h = s.document->history();
    s.states.resize(h.position + 1, 0);
    s.states[h.position] = nextState_++;
    s.selections[s.states[h.position]] = s.selection;
    ++s.revision;
    for (auto it = scenes_.begin(); it != scenes_.end();)
        if (it->first.startsWith(s.id + "/"))
            it = scenes_.erase(it);
        else
            ++it;
}
WorkerResult DocumentWorker::change(const QString &id,
                                    const std::function<void(engine::IDocument &)> &action) {
    auto &s = session(id);
    const auto position = s.document->history().position;
    if (position < int(s.states.size()))
        s.selections[s.states[position]] = s.selection;
    action(*s.document);
    markChanged(s);
    try {
        persist(s);
    } catch (const std::exception &e) {
        auto r = snapshot();
        r.error = QString("Edit applied; recovery failed: ") + e.what();
        return r;
    }
    return snapshot();
}
WorkerResult DocumentWorker::undo(const QString &id, bool redo) {
    auto &s = session(id);
    const auto h = s.document->history();
    if ((redo && h.position >= h.steps) || (!redo && h.position == 0))
        return snapshot();
    int state = s.states[redo ? h.position + 1 : h.position];
    auto paired = composites_.find(state);
    Session *other = nullptr;
    if (paired != composites_.end()) {
        const auto otherId = paired->second.a == id ? paired->second.b : paired->second.a;
        other = &session(otherId);
        const int pos = other->document->history().position;
        const int at = redo ? pos + 1 : pos;
        if (at < 0 || at >= int(other->states.size()) || other->states[at] != state)
            throw std::runtime_error("Undo later edits in the other document first");
    }
    if (redo)
        s.document->redo();
    else
        s.document->undo();
    ++s.revision;
    restoreSelection(s);
    if (other) {
        if (redo)
            other->document->redo();
        else
            other->document->undo();
        ++other->revision;
        restoreSelection(*other);
        persist(*other, true);
    }
    persist(s, true);
    return snapshot();
}
WorkerResult DocumentWorker::save(const QString &id, const QUrl &destination) {
    auto &s = session(id);
    if (destination == s.source && !s.fingerprint.isEmpty() &&
        fingerprint(s.source) != s.fingerprint)
        throw std::runtime_error(
            "The original changed outside yolopdf. Save As to preserve both versions.");
    const auto bytes = s.document->pdfBytes();
    const auto cache =
        QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/saved-" + s.id + ".pdf";
    atomicWrite(cache, byteArray(bytes));
    try {
        platform::saveCachedPdf(cache, destination);
    } catch (...) {
        throw;
    } // Keep verified cache on failure.
    QFile::remove(cache);
    s.source = destination;
    s.title = destination.fileName();
    s.fingerprint = fingerprint(destination);
    s.savedState = s.states.at(s.document->history().position);
    discardRecovery(s.id);
    auto r = snapshot();
    r.event = "saved";
    return r;
}
void DocumentWorker::persist(Session &s, bool force) {
    if (!dirty(s)) {
        discardRecovery(s.id);
        return;
    }
    const auto now = QDateTime::currentMSecsSinceEpoch();
    if (!force && now - s.recoveryAt < 10000)
        return;
    const auto data = s.document->recoveryData();
    const auto folder = recoveryRoot_ + "/" + s.id;
    if (!QDir().mkpath(folder))
        throw std::runtime_error("Cannot create recovery folder");
    // Immutable generation files; publish the metadata pointer last. A crash
    // cannot pair an old journal with a newly written snapshot.
    const auto generation = QUuid::createUuid().toString(QUuid::WithoutBraces);
    atomicWrite(folder + "/" + generation + ".pdf", byteArray(data.pdf));
    atomicWrite(folder + "/" + generation + ".journal", byteArray(data.journal));
    QJsonObject metadata{
        {"id", s.id},
        {"title", s.title},
        {"source", s.source.toString()},
        {"fingerprint", QString::fromLatin1(s.fingerprint)},
        {"generation", generation},
        {"page", s.page},
        {"selection", QJsonObject::fromVariantMap(s.selection)},
        {"pdfHash",
         QString::fromLatin1(
             QCryptographicHash::hash(byteArray(data.pdf), QCryptographicHash::Sha256).toHex())},
        {"journalHash", QString::fromLatin1(QCryptographicHash::hash(byteArray(data.journal),
                                                                     QCryptographicHash::Sha256)
                                                .toHex())}};
    atomicWrite(folder + "/session.json", QJsonDocument(metadata).toJson());
    s.recoveryAt = now;
    for (auto file : QDir(folder).entryList({"*.pdf", "*.journal"}, QDir::Files))
        if (!file.startsWith(generation))
            QFile::remove(folder + "/" + file);
}
QVariantList DocumentWorker::recoveries() const {
    QVariantList result;
    for (auto id : QDir(recoveryRoot_).entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (QUuid(id).isNull())
            continue;
        QFile file(recoveryRoot_ + "/" + id + "/session.json");
        if (!file.open(QIODevice::ReadOnly))
            continue;
        auto obj = QJsonDocument::fromJson(file.readAll()).object();
        if (obj["id"].toString() != id)
            continue;
        result.append(obj.toVariantMap());
    }
    return result;
}
void DocumentWorker::discardRecovery(const QString &id) {
    if (QUuid(id).isNull())
        throw std::runtime_error("Invalid recovery ID");
    QDir(recoveryRoot_ + "/" + id).removeRecursively();
}
WorkerResult DocumentWorker::restore(const QString &id, const QString &password) {
    if (QUuid(id).isNull())
        throw std::runtime_error("Invalid recovery ID");
    const auto folder = recoveryRoot_ + "/" + id;
    QFile meta(folder + "/session.json");
    if (!meta.open(QIODevice::ReadOnly))
        throw std::runtime_error("Recovery metadata unavailable");
    auto obj = QJsonDocument::fromJson(meta.readAll()).object();
    const auto gen = obj["generation"].toString();
    if (QUuid(gen).isNull())
        throw std::runtime_error("Invalid recovery generation");
    QFile pdf(folder + "/" + gen + ".pdf"), journal(folder + "/" + gen + ".journal");
    if (!pdf.open(QIODevice::ReadOnly) || !journal.open(QIODevice::ReadOnly))
        throw std::runtime_error("Incomplete recovery snapshot");
    const auto p = pdf.readAll(), j = journal.readAll();
    if (QCryptographicHash::hash(p, QCryptographicHash::Sha256).toHex() !=
            obj["pdfHash"].toString().toLatin1() ||
        QCryptographicHash::hash(j, QCryptographicHash::Sha256).toHex() !=
            obj["journalHash"].toString().toLatin1())
        throw std::runtime_error("Recovery snapshot is damaged");
    auto r = open(QUrl::fromLocalFile(pdf.fileName()), password);
    auto &s = session(activeId);
    s.document->loadRecoveryJournal(vector(j));
    s.page = obj["page"].toInt();
    s.selection = obj["selection"].toObject().toVariantMap();
    const int steps = s.document->history().steps;
    s.states.resize(steps + 1);
    for (auto &state : s.states)
        state = nextState_++;
    s.savedState = -1;
    s.title = obj["title"].toString() + " (recovered)";
    QUrl original(obj["source"].toString());
    const auto expected = obj["fingerprint"].toString().toLatin1();
    s.source = !expected.isEmpty() && fingerprint(original) == expected ? original : QUrl();
    s.fingerprint = expected;
    persist(s, true);
    discardRecovery(id);
    r = snapshot();
    r.event = "loaded";
    return r;
}
WorkerResult DocumentWorker::close(const QString &id) {
    auto it =
        std::find_if(sessions_.begin(), sessions_.end(), [&](auto &s) { return s->id == id; });
    if (it == sessions_.end())
        return snapshot();
    discardRecovery(id);
    QFile::remove((*it)->inputCache);
    sessions_.erase(it);
    if (activeId == id)
        activeId = sessions_.empty() ? QString() : sessions_.back()->id;
    return snapshot();
}
WorkerResult DocumentWorker::transfer(const QString &from, const QString &to,
                                      const std::vector<int> &pages, int at, bool move) {
    if (from == to)
        throw std::runtime_error("Choose another document tab");
    auto &a = session(from);
    auto &b = session(to);
    const auto count = a.document->inspect().pages.size();
    std::set<int> selected(pages.begin(), pages.end());
    if (selected.empty() || selected.size() != pages.size() || *selected.begin() < 0 ||
        *selected.rbegin() >= int(count))
        throw std::runtime_error("Invalid page selection");
    std::vector<int> keep;
    for (int i = 0; i < int(count); ++i)
        if (!selected.count(i))
            keep.push_back(i);
    if (move && keep.empty())
        throw std::runtime_error("Keep at least one page in the source tab");
    const auto tmp = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) +
                     "/transfer-" + a.id + ".pdf";
    atomicWrite(tmp, byteArray(a.document->pdfBytes()));
    try {
        b.document->insertPages(at, {{tmp.toUtf8().toStdString(), pages}});
        if (move) {
            try {
                a.document->rearrangePages(keep);
            } catch (...) {
                b.document->undo();
                throw;
            }
        }
    } catch (...) {
        QFile::remove(tmp);
        throw;
    }
    QFile::remove(tmp);
    markChanged(b);
    if (move) {
        markChanged(a);
        const int state = nextState_++;
        a.states[a.document->history().position] = state;
        b.states[b.document->history().position] = state;
        a.selections[state] = a.selection;
        b.selections[state] = b.selection;
        composites_[state] = {from, to, state};
        persist(a, true);
    }
    persist(b, true);
    return snapshot();
}
void DocumentWorker::shutdown() {
    for (auto &s : sessions_) {
        try {
            persist(*s, true);
        } catch (...) {
        }
        if (!dirty(*s))
            QFile::remove(s->inputCache);
    }
    sessions_.clear();
    scenes_.clear();
    imageClipboard.reset();
    activeId.clear();
}
} // namespace yolo::core
