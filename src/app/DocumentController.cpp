// SPDX-License-Identifier: AGPL-3.0-only
#include "DocumentController.h"
#include "core/DropRouter.h"
#include "platform/FileAccess.h"
#include "platform/FontLocator.h"
#include <QColor>
#include <QDir>
#include <QFileInfo>
#include <QMutexLocker>
#include <QPointer>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTemporaryFile>
#include <QUuid>
#include <QtConcurrent>
#include <algorithm>
#include <cmath>
#include <set>
using namespace yolo;
namespace {
std::vector<int> indices(const QVariantList &v) {
    std::vector<int> r;
    for (auto &i : v)
        r.push_back(i.toInt());
    if (r.empty())
        throw std::runtime_error("Select pages first");
    return r;
}
QString asset(const QUrl &url, const QString &suffix = {}) {
    if (url.isLocalFile())
        return url.toLocalFile();
    auto path =
        platform::cacheAsset(url, suffix.isEmpty() ? "." + QFileInfo(url.path()).suffix() : suffix);
    if (suffix.isEmpty()) {
        QFile file(path);
        if (file.open(QIODevice::ReadOnly)) {
            auto head = file.peek(4096);
            file.close();
            QString ext;
            if (head.startsWith("%PDF-"))
                ext = ".pdf";
            else if (head.contains("<svg"))
                ext = ".svg";
            else if (head.startsWith("GIF8"))
                ext = ".gif";
            else if (head.startsWith("\x89PNG"))
                ext = ".png";
            if (!ext.isEmpty() && !path.endsWith(ext, Qt::CaseInsensitive)) {
                if (!QFile::rename(path, path + ext)) {
                    QFile::remove(path);
                    throw std::runtime_error("Cannot prepare artwork file");
                }
                path += ext;
            }
        }
    }
    return path;
}
struct Asset {
    QString path;
    bool temporary;
    explicit Asset(const QUrl &u, const QString &suffix = {})
        : path(asset(u, suffix)), temporary(!u.isLocalFile()) {}
    ~Asset() {
        if (temporary)
            QFile::remove(path);
    }
};
engine::Matrix matrix(const QVariantList &v) {
    if (v.size() != 6)
        throw std::runtime_error("A transform has six coordinates");
    engine::Matrix m;
    for (int i = 0; i < 6; ++i)
        m[i] = v[i].toDouble();
    return m;
}
engine::TextReplacement replacement(int run, const QString &text, const QString &font, double size,
                                    bool overflow, int face, const QString &color, double wrap) {
    engine::TextReplacement result{
        run, text.toUtf8().toStdString(), font.toUtf8().toStdString(), face, size, overflow};
    if (!color.isEmpty()) {
        QColor c(color);
        if (!c.isValid())
            throw std::runtime_error("Choose a valid text colour");
        result.changeColor = true;
        result.color = {c.redF(), c.greenF(), c.blueF()};
    }
    result.wrapWidth = wrap;
    return result;
}
} // namespace
QImage PageImageProvider::requestImage(const QString &id, QSize *size, const QSize &requested) {
    QMutexLocker lock(&mutex_);
    auto *value = cache_.object(id);
    if (!value)
        return {};
    if (size)
        *size = value->size();
    return requested.isValid()
               ? value->scaled(requested, Qt::KeepAspectRatio, Qt::SmoothTransformation)
               : *value;
}
void PageImageProvider::put(const QString &id, QImage image) {
    QMutexLocker lock(&mutex_);
    int cost = std::max<qsizetype>(1, image.sizeInBytes() / 1024);
    cache_.insert(id, new QImage(std::move(image)), cost);
}
bool PageImageProvider::contains(const QString &id) {
    QMutexLocker lock(&mutex_);
    return cache_.contains(id);
}
DocumentController::DocumentController(PageImageProvider *provider, QObject *parent)
    : QObject(parent), provider_(provider), worker_(new core::DocumentWorker) {
    worker_->moveToThread(&thread_);
    connect(&thread_, &QThread::finished, worker_, &QObject::deleteLater);
    thread_.start();
    recoveryTimer_.setInterval(10000);
    connect(&recoveryTimer_, &QTimer::timeout, this, &DocumentController::flushRecovery);
    recoveryTimer_.start();
    recoveries_ = worker_->recoveries();
}
DocumentController::~DocumentController() {
    recoveryTimer_.stop();
    QMetaObject::invokeMethod(
        worker_, [w = worker_] { w->shutdown(); }, Qt::BlockingQueuedConnection);
    thread_.quit();
    thread_.wait();
}
QString DocumentController::imageSource() const {
    if (!hasDocument())
        return {};
    if (!previewKey_.isEmpty())
        return "image://pages/" + previewKey_;
    return "image://pages/" + id() + "/" + QString::number(active_.value("revision").toInt()) +
           "/" + QString::number(page()) + "/96";
}
void DocumentController::submit(Job job, QString event) {
    if (busy_)
        return;
    busy_ = true;
    error_.clear();
    emit changed();
    QPointer<DocumentController> guard(this);
    QMetaObject::invokeMethod(
        worker_,
        [w = worker_, job = std::move(job), guard, event] {
            core::WorkerResult result;
            try {
                result = job(*w);
            } catch (const engine::Error &e) {
                result = w->snapshot();
                result.error = QString::fromUtf8(e.what());
                if (e.code() == engine::ErrorCode::PasswordRequired && event == "loaded")
                    result.event = "password";
            } catch (const std::exception &e) {
                result = w->snapshot();
                result.error = QString::fromUtf8(e.what());
            }
            if (!guard)
                return;
            QMetaObject::invokeMethod(
                guard,
                [guard, result = std::move(result), event] {
                    if (!guard)
                        return;
                    auto *s = guard.data();
                    s->busy_ = false;
                    s->error_ = result.error;
                    s->tabs_ = result.tabs;
                    s->active_ = result.active;
                    if (result.active.contains("recoveries"))
                        s->recoveries_ = result.active.value("recoveries").toList();
                    s->active_.remove("recoveries");
                    if (!result.imageKey.isEmpty()) {
                        s->provider_->put(result.imageKey, result.image);
                        ++s->renderEpoch_;
                    }
                    emit s->changed();
                    emit s->imagesChanged();
                    if (result.event == "print-ready") {
                        try {
                            platform::previewPrint(result.print, event == "fit");
                        } catch (const std::exception &error) {
                            s->error_ = QString::fromUtf8(error.what());
                            emit s->changed();
                        }
                        QDir(result.print.folder).removeRecursively();
                    }
                    auto actual = result.event.isEmpty() ? event : result.event;
                    if (actual == "password")
                        emit s->passwordRequired(s->pendingSource_);
                    else if (actual == "loaded") {
                        if (!result.error.isEmpty())
                            s->openQueue_.clear();
                        emit s->loaded(result.error.isEmpty());
                        if (result.error.isEmpty())
                            QMetaObject::invokeMethod(
                                s,
                                [guard] {
                                    if (guard)
                                        guard->openNext();
                                },
                                Qt::QueuedConnection);
                    } else if (actual == "saved")
                        emit s->saved(result.error.isEmpty());
                },
                Qt::QueuedConnection);
        },
        Qt::QueuedConnection);
}
void DocumentController::openPdf(const QUrl &url, bool spike, const QString &password) {
    openQueue_.clear();
    pendingRecovery_.clear();
    pendingSource_ = url;
    cancelPreview();
    submit([url, password, spike](auto &w) { return w.open(url, password, spike); }, "loaded");
}
void DocumentController::activateTab(const QString &tab) {
    cancelPreview();
    searchHits_.clear();
    submit([tab](auto &w) { return w.activate(tab); });
}
void DocumentController::selectPage(int index) {
    if (index == page() || !hasDocument())
        return;
    cancelPreview();
    submit([tab = id(), index](auto &w) { return w.activate(tab, index); });
}
QString DocumentController::pageSource(int index, int dpi) {
    if (!hasDocument() || index < 0 || index >= active_.value("pages").toList().size())
        return {};
    if (index == page() && !previewKey_.isEmpty())
        return "image://pages/" + previewKey_;
    auto geometry = active_.value("pages").toList()[index].toMap();
    const double area = geometry.value("width").toDouble() * geometry.value("height").toDouble();
    if (area > 0)
        dpi = std::min(dpi, int(std::sqrt(8.0 * 1024 * 1024 / area) * 72));
    return tileSource(index, std::clamp(dpi, 1, 600), 0, 0, 0, 0);
}
QString DocumentController::tileSource(int index, int dpi, double x, double y, double width,
                                       double height) {
    if (!hasDocument() || index < 0 || index >= active_.value("pages").toList().size())
        return {};
    if (index == page() && !previewKey_.isEmpty() && width == 0)
        return "image://pages/" + previewKey_;
    dpi = std::clamp(dpi, 1, 1200);
    const auto tab = id();
    const int revision = active_.value("revision").toInt();
    auto key = tab + "/" + QString::number(revision) + "/" + QString::number(index) + "/" +
               QString::number(dpi);
    const engine::Rect clip{x, y, x + width, y + height};
    if (width > 0 && height > 0)
        for (double v : {x, y, width, height})
            key += "/" + QString::number(v, 'g', 12);
    if (provider_->contains(key))
        return "image://pages/" + key;
    if (!pendingImages_.contains(key)) {
        pendingImages_.insert(key);
        QPointer<DocumentController> guard(this);
        QMetaObject::invokeMethod(
            worker_,
            [w = worker_, guard, tab, index, dpi, key, revision, clip] {
                std::unique_ptr<engine::IRenderJob> job;
                try {
                    job = w->renderJob(tab, index, revision);
                } catch (...) {
                    if (guard)
                        QMetaObject::invokeMethod(guard, [guard, key] {
                            if (guard)
                                guard->pendingImages_.remove(key);
                        });
                    return;
                }
                QThreadPool::globalInstance()->start(
                    [guard, key, dpi, clip, job = std::move(job)]() mutable {
                        QImage value;
                        try {
                            value = core::image(job->render(dpi, clip));
                        } catch (...) {
                        }
                        if (!guard)
                            return;
                        QMetaObject::invokeMethod(
                            guard,
                            [guard, key, value] {
                                if (!guard)
                                    return;
                                guard->pendingImages_.remove(key);
                                if (!value.isNull()) {
                                    guard->provider_->put(key, value);
                                    ++guard->renderEpoch_;
                                    emit guard->imagesChanged();
                                }
                            },
                            Qt::QueuedConnection);
                    });
            },
            Qt::QueuedConnection);
    }
    return {};
}

void DocumentController::saveAs(const QUrl &url) {
    cancelPreview();
    submit([tab = id(), url](auto &w) { return w.save(tab, url); }, "saved");
}
void DocumentController::save() {
    auto url = active_.value("source").toUrl();
    if (url.isValid() && !url.isEmpty() && url.scheme() != "qrc")
        saveAs(url);
}
void DocumentController::closeTab(const QString &tab, bool discard) {
    for (auto &item : tabs_)
        if (item.toMap().value("id") == tab && item.toMap().value("dirty").toBool() && !discard) {
            emit closeRequested(tab);
            return;
        }
    cancelPreview();
    submit([tab](auto &w) { return w.close(tab); });
}
void DocumentController::edit(std::function<void(engine::IDocument &)> action) {
    const auto tab = id();
    editJob([tab, action = std::move(action)](auto &w) { return w.change(tab, action); });
}
void DocumentController::editJob(Job job) {
    cancelPreview();
    const auto tab = id();
    if (!hasDocument() || busy_)
        return;
    if (!active_.value("mayEdit").toBool()) {
        error_ = "PDF permission restrictions disallow editing";
        emit changed();
        return;
    }
    if (active_.value("signed").toBool() && !signedApproved_.contains(tab)) {
        pendingEdit_ = job;
        emit signatureRequested();
        return;
    }
    submit(std::move(job));
}
void DocumentController::authorizeSignedEdits(bool allow) {
    if (!allow) {
        pendingEdit_ = {};
        return;
    }
    auto job = std::move(pendingEdit_);
    pendingEdit_ = {};
    if (!job)
        return;
    const auto tab = id();
    signedApproved_.insert(tab);
    submit([tab, job](auto &w) {
        w.session(tab).document->allowSignedEdits(true);
        return job(w);
    });
}
void DocumentController::undo() {
    cancelPreview();
    submit([tab = id()](auto &w) { return w.undo(tab); });
}
void DocumentController::redo() {
    cancelPreview();
    submit([tab = id()](auto &w) { return w.undo(tab, true); });
}
void DocumentController::search(const QString &query) {
    const auto tab = id();
    QPointer<DocumentController> guard(this);
    submit([tab, query, guard](auto &w) {
        QVariantList hits;
        for (auto &h : w.session(tab).document->search(query.toUtf8().toStdString())) {
            auto b = core::bounds(h.bounds);
            b.insert("page", h.page);
            hits.append(b);
        }
        if (guard)
            QMetaObject::invokeMethod(guard, [guard, hits] {
                if (guard)
                    guard->searchHits_ = hits;
            });
        return w.snapshot();
    });
}
void DocumentController::copySelection(double x0, double y0, double x1, double y1) {
    QPointer<DocumentController> guard(this);
    submit([tab = id(), pg = page(), x0, y0, x1, y1, guard](auto &w) {
        auto text =
            QString::fromStdString(w.session(tab).document->copySelection(pg, x0, y0, x1, y1));
        if (guard)
            QMetaObject::invokeMethod(guard, [guard, text] {
                if (guard)
                    emit guard->textCopied(text);
            });
        return w.snapshot();
    });
}
void DocumentController::replaceText(int run, const QString &text, const QUrl &font, double size,
                                     bool overflow, int face, const QString &color, double wrap) {
    edit([=, pg = page()](auto &doc) {
        QString path;
        if (!font.isEmpty())
            path = asset(font, ".ttf");
        try {
            doc.replaceLine(pg, replacement(run, text, path, size, overflow, face, color, wrap));
        } catch (...) {
            if (!font.isLocalFile())
                QFile::remove(path);
            throw;
        }
        if (!font.isLocalFile())
            QFile::remove(path);
    });
}
void DocumentController::previewText(int run, const QString &text, const QUrl &font, double size,
                                     bool overflow, int face, const QString &color, double wrap) {
    if (busy_ || !hasDocument())
        return;
    if (active_.value("signed").toBool() && !signedApproved_.contains(id()))
        return;
    const int token = ++previewToken_, pg = page();
    const auto tab = id();
    QPointer<DocumentController> guard(this);
    QMetaObject::invokeMethod(worker_, [=, w = worker_] {
        QImage value, original;
        QRect comparison;
        QString error;
        try {
            if (!font.isEmpty()) {
                auto &doc = *w->session(tab).document;
                original = core::image(doc.render(pg, 96));
                for (const auto &line : doc.textLines(pg))
                    if (line.id == run) {
                        const double physicalSize =
                            size > 0 ? size : std::max(1.0, line.bounds.y1 - line.bounds.y0);
                        const double rows =
                            wrap > 0 ? std::max(1.0, std::ceil(text.toUtf8().size() * physicalSize *
                                                               1.5 / wrap))
                                     : 1;
                        comparison = QRect(int(line.bounds.x0 * 96 / 72) - 8,
                                           int(line.bounds.y0 * 96 / 72) - 8,
                                           int(std::max(line.bounds.x1 - line.bounds.x0,
                                                        text.toUtf8().size() * physicalSize * 1.5) *
                                               96 / 72) +
                                               16,
                                           int(std::max(line.bounds.y1 - line.bounds.y0,
                                                        rows * physicalSize * 1.5) *
                                               96 / 72) +
                                               16)
                                         .intersected(original.rect());
                        break;
                    }
            }
            QString path;
            if (!font.isEmpty())
                path = asset(font, ".ttf");
            try {
                value = core::image(w->session(tab).document->previewLine(
                    pg, replacement(run, text, path, size, overflow, face, color, wrap)));
            } catch (...) {
                if (!font.isLocalFile())
                    QFile::remove(path);
                throw;
            }
            if (!font.isLocalFile())
                QFile::remove(path);
        } catch (const std::exception &e) {
            error = QString::fromUtf8(e.what());
        }
        if (!guard)
            return;
        QMetaObject::invokeMethod(guard, [=] {
            if (!guard || guard->previewToken_ != token || guard->id() != tab)
                return;
            guard->error_ = error;
            if (error.isEmpty()) {
                guard->previewKey_ = tab + "/preview/" + QString::number(token);
                guard->provider_->put(guard->previewKey_, value);
                guard->compareOriginalKey_.clear();
                guard->compareEditedKey_.clear();
                if (!original.isNull() && !comparison.isEmpty()) {
                    guard->compareOriginalKey_ =
                        tab + "/comparison-original/" + QString::number(token);
                    guard->compareEditedKey_ = tab + "/comparison-edited/" + QString::number(token);
                    guard->provider_->put(guard->compareOriginalKey_, original.copy(comparison));
                    guard->provider_->put(guard->compareEditedKey_, value.copy(comparison));
                }
                ++guard->renderEpoch_;
            } else {
                guard->previewKey_.clear();
                guard->compareOriginalKey_.clear();
                guard->compareEditedKey_.clear();
            }
            emit guard->changed();
            emit guard->imagesChanged();
            emit guard->previewFinished(error.isEmpty());
        });
    });
}
void DocumentController::cancelPreview() {
    compareOriginalKey_.clear();
    compareEditedKey_.clear();
    ++previewToken_;
    previewKey_.clear();
    ++renderEpoch_;
    emit imagesChanged();
}
void DocumentController::addText(const QString &text, const QUrl &font, double x, double y,
                                 double width, double height, double size, const QString &color,
                                 int alignment, int face) {
    edit([=, pg = page()](auto &doc) {
        Asset f(font, ".ttf");
        QColor c(color);
        if (!c.isValid())
            throw std::runtime_error("Invalid colour");
        engine::AddTextSpec spec{text.toUtf8().toStdString(),
                                 f.path.toUtf8().toStdString(),
                                 face,
                                 alignment,
                                 x,
                                 y,
                                 width,
                                 height,
                                 size,
                                 {c.redF(), c.greenF(), c.blueF()}};
        doc.addText(pg, spec);
    });
}
void DocumentController::deleteImage(int occurrence) {
    edit([=, pg = page()](auto &d) {
        d.editImage(pg, {occurrence, engine::ImageAction::Delete, {}, {}});
    });
}
void DocumentController::transformImage(int occurrence, const QVariantList &v) {
    edit([=, pg = page()](auto &d) {
        d.editImage(pg, {occurrence, engine::ImageAction::Transform, matrix(v), {}});
    });
}
void DocumentController::replaceImage(int occurrence, const QUrl &url) {
    edit([=, pg = page()](auto &d) {
        Asset f(url);
        d.editImage(pg,
                    {occurrence, engine::ImageAction::Replace, {}, f.path.toUtf8().toStdString()});
    });
}
void DocumentController::insertImage(const QUrl &url, double x, double y, double width,
                                     double height) {
    edit([=, pg = page()](auto &d) {
        Asset f(url);
        d.insertImage(pg, f.path.toUtf8().toStdString(), {width, 0, 0, -height, x, y + height});
    });
}
void DocumentController::copyImage(int occurrence) {
    QPointer<DocumentController> guard(this);
    submit([=, tab = id(), pg = page()](auto &w) {
        auto img = core::image(w.session(tab).document->copyImage(pg, occurrence));
        auto frozen = engine::createDocument();
        frozen->insertBlankPage(0, 1, 1);
        w.session(tab).document->copyImageTo(pg, occurrence, *frozen, 0, {1, 0, 0, 1, 0, 0});
        w.imageClipboard = std::move(frozen);
        if (guard)
            QMetaObject::invokeMethod(guard, [guard, img] {
                if (guard) {
                    guard->clipboardImage_ = img;
                    emit guard->imageCopied(img);
                }
            });
        return w.snapshot();
    });
}
void DocumentController::exportImage(int occurrence, const QUrl &url) {
    submit([=, tab = id(), pg = page()](auto &w) {
        auto img = core::image(w.session(tab).document->copyImage(pg, occurrence));
        auto file = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) +
                    "/image-export-" + QUuid::createUuid().toString(QUuid::WithoutBraces) + ".png";
        if (!img.save(file, "PNG"))
            throw std::runtime_error("Cannot export image");
        platform::saveCachedPdf(file, url);
        QFile::remove(file);
        return w.snapshot();
    });
}
void DocumentController::copyArea(double x, double y, double width, double height) {
    QPointer<DocumentController> guard(this);
    submit([=, tab = id(), pg = page()](auto &w) {
        auto img =
            core::image(w.session(tab).document->copyArea(pg, {x, y, x + width, y + height}, 300));
        w.imageClipboard.reset();
        if (guard)
            QMetaObject::invokeMethod(guard, [guard, img] {
                if (guard) {
                    guard->clipboardImage_ = img;
                    emit guard->imageCopied(img);
                }
            });
        return w.snapshot();
    });
}
void DocumentController::pasteImage(double x, double y, double width, double height) {
    const auto img = clipboardImage_;
    if (img.isNull())
        return;
    const auto tab = id();
    const int pg = page();
    editJob([=](auto &w) {
        return w.change(tab, [&](auto &d) {
            if (w.imageClipboard) {
                w.imageClipboard->copyImageTo(0, 0, d, pg, {width, 0, 0, -height, x, y + height});
                return;
            }
            auto file = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) +
                        "/clipboard-" + QUuid::createUuid().toString(QUuid::WithoutBraces) + ".png";
            if (!img.save(file, "PNG"))
                throw std::runtime_error("Cannot cache clipboard image");
            try {
                d.insertImage(pg, file.toUtf8().toStdString(),
                              {width, 0, 0, -height, x, y + height});
            } catch (...) {
                QFile::remove(file);
                throw;
            }
            QFile::remove(file);
        });
    });
}

void DocumentController::placeStamp(const QUrl &url, double x, double y, double width,
                                    double height, double rotation, double opacity, int frame,
                                    const QVariantList &selected) {
    edit([=, pg = page()](auto &d) {
        Asset f(url);
        engine::StampSpec s{f.path.toUtf8().toStdString(),
                            QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString(),
                            x,
                            y,
                            width,
                            height,
                            rotation,
                            opacity,
                            frame};
        auto pages = selected.isEmpty() ? std::vector<int>{pg} : indices(selected);
        d.operation("Apply stamp", [&](auto &doc) {
            for (int p : pages)
                doc.placeStamp(p, s);
        });
    });
}
void DocumentController::previewStamp(const QUrl &url, double width, double height, int frame) {
    const int token = ++stampPreviewToken_;
    QPointer<DocumentController> guard(this);
    QThreadPool::globalInstance()->start([=] {
        QImage value;
        QString error;
        try {
            Asset f(url);
            auto d = engine::createDocument();
            d->insertBlankPage(0, width, height);
            engine::StampSpec s;
            s.artworkFile = f.path.toUtf8().toStdString();
            s.widthMm = width;
            s.heightMm = height;
            s.gifFrame = frame;
            d->placeStamp(0, s);
            value = core::image(
                d->render(0, std::clamp(240.0 * 25.4 / std::max(width, height), 1.0, 600.0)));
        } catch (const std::exception &e) {
            error = QString::fromUtf8(e.what());
        }
        if (guard)
            QMetaObject::invokeMethod(guard, [=] {
                if (!guard || guard->stampPreviewToken_ != token)
                    return;
                guard->error_ = error;
                if (error.isEmpty()) {
                    guard->stampPreviewKey_ = "stamp-preview/" + QString::number(token);
                    guard->provider_->put(guard->stampPreviewKey_, value);
                } else
                    guard->stampPreviewKey_.clear();
                emit guard->changed();
                emit guard->imagesChanged();
            });
    });
}
void DocumentController::deleteAnnotation(int annotation) {
    edit([=, pg = page()](auto &d) { d.deleteAnnotation(pg, annotation); });
}
void DocumentController::moveAnnotation(int annotation, double x, double y, double width,
                                        double height) {
    edit([=, pg = page()](auto &d) {
        d.moveAnnotation(pg, annotation, {x, y, x + width, y + height});
    });
}
void DocumentController::annotationOpacity(int annotation, double opacity) {
    edit([=, pg = page()](auto &d) { d.setAnnotationOpacity(pg, annotation, opacity); });
}
void DocumentController::annotate(const QString &type, double x, double y, double width,
                                  double height, const QString &contents, const QString &color) {
    edit([=, pg = page()](auto &d) {
        QColor c(color);
        engine::AnnotationSpec s;
        s.type = type == "Highlight"   ? engine::AnnotationType::Highlight
                 : type == "Strikeout" ? engine::AnnotationType::Strikeout
                 : type == "Arrow"     ? engine::AnnotationType::Arrow
                 : type == "Rectangle" ? engine::AnnotationType::Rectangle
                                       : engine::AnnotationType::Note;
        s.bounds = {x, y, x + width, y + height};
        s.contents = contents.toStdString();
        s.color = {c.redF(), c.greenF(), c.blueF()};
        d.addAnnotation(pg, s);
    });
}
void DocumentController::flattenStamps() {
    edit([](auto &d) { d.flattenStamps(); });
}
void DocumentController::insertPages(const QVariantList &urls, int at, const QString &range) {
    edit([=](auto &d) {
        std::vector<engine::PageSource> sources;
        std::vector<std::unique_ptr<Asset>> assets;
        for (auto &u : urls) {
            auto f = std::make_unique<Asset>(u.toUrl(), ".pdf");
            std::vector<int> pages;
            if (!range.isEmpty()) {
                auto source = engine::openDocument(f->path.toUtf8().toStdString());
                pages = core::pageRange(range, source->inspect().pages.size());
            }
            sources.push_back({f->path.toUtf8().toStdString(), pages});
            assets.push_back(std::move(f));
        }
        d.insertPages(at, sources);
    });
}
void DocumentController::deletePages(const QVariantList &v) {
    edit([v](auto &d) {
        auto selected = indices(v);
        std::set<int> remove(selected.begin(), selected.end());
        std::vector<int> keep;
        auto count = d.inspect().pages.size();
        for (int i = 0; i < int(count); ++i)
            if (!remove.count(i))
                keep.push_back(i);
        d.rearrangePages(keep);
    });
}
void DocumentController::duplicatePages(const QVariantList &v) {
    edit([v](auto &d) {
        auto selected = indices(v);
        std::set<int> duplicate(selected.begin(), selected.end());
        std::vector<int> order;
        for (int i = 0; i < int(d.inspect().pages.size()); ++i) {
            order.push_back(i);
            if (duplicate.count(i))
                order.push_back(i);
        }
        d.rearrangePages(order);
    });
}
void DocumentController::reorderPages(const QVariantList &v) {
    edit([v](auto &d) { d.rearrangePages(indices(v)); });
}
void DocumentController::movePages(const QVariantList &v, int at) {
    edit([v, at](auto &d) {
        auto selected = indices(v);
        std::set<int> set(selected.begin(), selected.end());
        std::vector<int> order;
        int insertion = 0;
        const int count = d.inspect().pages.size();
        if (at < 0 || at > count)
            throw std::runtime_error("Invalid insertion index");
        for (int i = 0; i < count; ++i)
            if (!set.count(i)) {
                if (i < at)
                    ++insertion;
                order.push_back(i);
            }
        order.insert(order.begin() + insertion, selected.begin(), selected.end());
        d.rearrangePages(order);
    });
}
void DocumentController::rotatePages(const QVariantList &v, int degrees) {
    edit([v, degrees](auto &d) {
        d.operation("Rotate pages", [&](auto &doc) {
            for (int p : indices(v))
                doc.rotatePage(p, degrees);
        });
    });
}
void DocumentController::blankPage(int at, double width, double height) {
    edit([=](auto &d) { d.insertBlankPage(at, width, height); });
}
void DocumentController::extractPages(const QVariantList &v, const QUrl &url) {
    submit([=, tab = id()](auto &w) {
        auto &s = w.session(tab);
        auto cache = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/extract-" +
                     QUuid::createUuid().toString(QUuid::WithoutBraces) + ".pdf";
        auto bytes = s.document->pdfBytes();
        QSaveFile f(cache);
        if (!f.open(QIODevice::WriteOnly) ||
            f.write(reinterpret_cast<const char *>(bytes.data()), bytes.size()) !=
                qint64(bytes.size()) ||
            !f.commit())
            throw std::runtime_error("Cannot cache extraction source");
        try {
            auto out = engine::createDocument();
            out->insertPages(0, {{cache.toStdString(), indices(v)}});
            auto data = out->pdfBytes();
            QSaveFile result(cache + ".out.pdf");
            if (!result.open(QIODevice::WriteOnly) ||
                result.write(reinterpret_cast<const char *>(data.data()), data.size()) !=
                    qint64(data.size()) ||
                !result.commit())
                throw std::runtime_error("Cannot export extracted pages");
            platform::saveCachedPdf(cache + ".out.pdf", url);
            QFile::remove(cache + ".out.pdf");
        } catch (...) {
            QFile::remove(cache);
            throw;
        }
        QFile::remove(cache);
        return w.snapshot();
    });
}
void DocumentController::transferPages(const QString &to, const QVariantList &v, int at,
                                       bool move) {
    submit([=, from = id()](auto &w) { return w.transfer(from, to, indices(v), at, move); });
}
void DocumentController::dropFiles(const QVariantList &urls, bool overview, int at) {
    core::DropRouter router;
    const auto route = router.route(overview ? "overview" : "main", hasDocument(), false,
                                    active_.value("pages").toList().size(), at, false, urls);
    if (route.value("action").toString() == "ignore")
        return;
    if (route.value("action").toString() == "insert")
        insertPages(urls, route.value("at").toInt());
    else {
        if (busy_)
            return;
        openQueue_.clear();
        pendingRecovery_.clear();
        for (const auto &url : urls)
            openQueue_.append(url.toUrl());
        openNext();
    }
}
void DocumentController::restoreRecovery(const QString &recovery, const QString &password) {
    openQueue_.clear();
    pendingRecovery_ = recovery;
    submit(
        [=](auto &w) {
            auto r = w.restore(recovery, password);
            r.active.insert("recoveries", w.recoveries());
            return r;
        },
        "loaded");
}
void DocumentController::discardRecovery(const QString &recovery) {
    submit([=](auto &w) {
        w.discardRecovery(recovery);
        auto r = w.snapshot();
        r.active.insert("recoveries", w.recoveries());
        r.event = "recoveries";
        return r;
    });
}
void DocumentController::flushRecovery() {
    if (busy_ || !hasDocument())
        return;
    submit([](auto &w) {
        auto r = w.snapshot();
        for (auto &tab : r.tabs)
            w.persist(w.session(tab.toMap().value("id").toString()), false);
        return w.snapshot();
    });
}

void DocumentController::printDocument(bool fit, const QUrl &output) {
    submit(
        [tab = id(), output](auto &w) {
            auto &s = w.session(tab);
            auto bundle = platform::preparePrint(*s.document, s.title);
            auto r = w.snapshot();
            if (!output.isEmpty()) {
                platform::saveCachedPdf(bundle.pdf, output);
                QDir(bundle.folder).removeRecursively();
                r.event = "saved";
            } else {
                r.print = std::move(bundle);
                r.event = "print-ready";
            }
            return r;
        },
        fit ? "fit" : "actual");
}

void DocumentController::rotateStamp(int id, double degrees) {
    edit([=, pg = page()](auto &d) { d.rotateStamp(pg, id, degrees); });
}
void DocumentController::duplicateStamp(int id) {
    edit([=, pg = page()](auto &d) { d.duplicateStamp(pg, id, 10, 10); });
}
void DocumentController::findInstalledFont(const QString &name) {
    QPointer<DocumentController> guard(this);
    QThreadPool::globalInstance()->start([guard, name] {
        auto match = yolo::platform::findExactFont(name);
        if (guard)
            QMetaObject::invokeMethod(guard, [guard, match] {
                if (!guard)
                    return;
                if (match.file.isEmpty()) {
                    guard->error_ = "No exact installed font was found. Import a font or choose an "
                                    "explicit substitute.";
                    emit guard->changed();
                } else
                    emit guard->installedFontFound(QUrl::fromLocalFile(match.file),
                                                   match.faceIndex);
            });
    });
}

void DocumentController::providePassword(const QString &password) {
    if (!pendingRecovery_.isEmpty())
        restoreRecovery(pendingRecovery_, password);
    else {
        const auto source = pendingSource_;
        submit([source, password](auto &w) { return w.open(source, password); }, "loaded");
    }
}
void DocumentController::cancelPassword() {
    openQueue_.clear();
    pendingRecovery_.clear();
    pendingSource_.clear();
}
void DocumentController::openNext() {
    if (busy_ || openQueue_.isEmpty())
        return;
    pendingSource_ = openQueue_.takeFirst();
    cancelPreview();
    const auto source = pendingSource_;
    submit([source](auto &w) { return w.open(source); }, "loaded");
}

void DocumentController::markStroke(const QVariantList &points, double thickness,
                                    const QString &color, double opacity) {
    edit([=, pg = page()](auto &d) {
        engine::AnnotationSpec s;
        s.type = engine::AnnotationType::Marker;
        s.thickness = thickness;
        s.opacity = opacity;
        QColor c(color);
        if (!c.isValid())
            throw std::runtime_error("Choose a marker colour");
        s.color = {c.redF(), c.greenF(), c.blueF()};
        for (auto point : points) {
            auto p = point.toList();
            if (p.size() != 2)
                throw std::runtime_error("Invalid marker point");
            s.points.push_back({p[0].toDouble(), p[1].toDouble()});
        }
        d.addAnnotation(pg, s);
    });
}
void DocumentController::exportFlattened(const QUrl &destination) {
    editJob([tab = id(), destination](auto &w) {
        auto &s = w.session(tab);
        if (destination == s.source || (destination.isLocalFile() && s.source.isLocalFile() &&
                                        QFileInfo(destination.toLocalFile()).exists() &&
                                        QFileInfo(destination.toLocalFile()).canonicalFilePath() ==
                                            QFileInfo(s.source.toLocalFile()).canonicalFilePath()))
            throw std::runtime_error(
                "Choose a different file for the flattened copy to keep your editable original");
        const auto bytes = s.document->flattenedBytes();
        QTemporaryFile cache(QStandardPaths::writableLocation(QStandardPaths::CacheLocation) +
                             "/flattened-XXXXXX.pdf");
        if (!cache.open() ||
            cache.write(reinterpret_cast<const char *>(bytes.data()), bytes.size()) !=
                qint64(bytes.size()) ||
            !cache.flush())
            throw std::runtime_error("Cannot create flattened copy");
        platform::saveCachedPdf(cache.fileName(), destination);
        auto r = w.snapshot();
        r.event = "saved";
        return r;
    });
}
void DocumentController::setSelection(const QVariantList &pages, const QString &kind,
                                      int objectId) {
    if (!hasDocument())
        return;
    const auto tab = id();
    const auto geometry = active_.value("pages").toList();
    QVariantList ids;
    for (auto item : pages) {
        const int index = item.toInt();
        if (index >= 0 && index < geometry.size())
            ids.append(geometry[index].toMap().value("id"));
    }
    QVariantMap selection{{"pages", ids},
                          {"kind", kind},
                          {"objectId", objectId},
                          {"activePage", geometry.value(page()).toMap().value("id")}};
    QMetaObject::invokeMethod(
        worker_,
        [w = worker_, tab, selection] {
            try {
                w->setSelection(tab, selection);
            } catch (...) {
            }
        },
        Qt::QueuedConnection);
}
