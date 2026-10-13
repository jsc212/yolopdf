// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "engine/api/Document.h"
#include "platform/Print.h"
#include <QImage>
#include <QObject>
#include <QUrl>
#include <QVariant>
#include <functional>
#include <map>

namespace yolo::core {
struct Session {
    QString id, title, inputCache;
    QUrl source;
    QByteArray fingerprint;
    std::unique_ptr<engine::IDocument> document;
    int page{}, revision{}, savedState{};
    std::vector<int> states{0};
    qint64 recoveryAt{};
};
struct WorkerResult {
    QVariantList tabs;
    QVariantMap active;
    QString error, event;
    QString imageKey;
    QImage image;
    platform::PrintBundle print;
};
class DocumentWorker final : public QObject {
  public:
    explicit DocumentWorker(QString recoveryRoot = {});
    ~DocumentWorker();
    Session &session(const QString &id);
    WorkerResult snapshot();
    WorkerResult open(const QUrl &, const QString &password = {}, bool spike = false);
    WorkerResult activate(const QString &, int page = -1);
    WorkerResult change(const QString &, const std::function<void(engine::IDocument &)> &);
    WorkerResult undo(const QString &, bool redo = false);
    WorkerResult save(const QString &, const QUrl &);
    WorkerResult close(const QString &);
    WorkerResult transfer(const QString &, const QString &, const std::vector<int> &, int at,
                          bool move);
    WorkerResult restore(const QString &id, const QString &password = {});
    std::unique_ptr<engine::IRenderJob> renderJob(const QString &, int page, int revision);
    QVariantList recoveries() const;
    void discardRecovery(const QString &id);
    void persist(Session &, bool force = false);
    void shutdown();
    QString activeId;
    std::unique_ptr<engine::IDocument> imageClipboard;

  private:
    void markChanged(Session &);
    std::vector<std::unique_ptr<Session>> sessions_;
    QString recoveryRoot_;
    int nextState_{1};
    struct Composite {
        QString a, b;
        int state{};
    };
    std::map<int, Composite> composites_;
    std::map<QString, std::shared_ptr<engine::IRenderSource>> scenes_;
};
QByteArray fingerprint(const QUrl &);
QImage image(const engine::RasterImage &);
QVariantMap bounds(const engine::Rect &);
std::vector<int> pageRange(const QString &, int count);
} // namespace yolo::core
