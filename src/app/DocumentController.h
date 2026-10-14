// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "core/DocumentWorker.h"
#include <QCache>
#include <QImage>
#include <QMutex>
#include <QQuickImageProvider>
#include <QSet>
#include <QThread>
#include <QTimer>

class PageImageProvider final : public QQuickImageProvider {
  public:
    PageImageProvider() : QQuickImageProvider(QQuickImageProvider::Image), cache_(128 * 1024) {}
    QImage requestImage(const QString &, QSize *, const QSize &) override;
    void put(const QString &, QImage);
    bool contains(const QString &);

  private:
    QMutex mutex_;
    QCache<QString, QImage> cache_;
};
class DocumentController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(bool hasDocument READ hasDocument NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
    Q_PROPERTY(QString title READ title NOTIFY changed)
    Q_PROPERTY(QString imageSource READ imageSource NOTIFY imagesChanged)
    Q_PROPERTY(QVariantList tabs READ tabs NOTIFY changed)
    Q_PROPERTY(QVariantMap document READ document NOTIFY changed)
    Q_PROPERTY(QVariantList searchHits READ searchHits NOTIFY changed)
    Q_PROPERTY(QVariantList recoveryCandidates READ recoveryCandidates NOTIFY changed)
    Q_PROPERTY(int renderEpoch READ renderEpoch NOTIFY imagesChanged)
    Q_PROPERTY(QString stampPreviewSource READ stampPreviewSource NOTIFY imagesChanged)
    Q_PROPERTY(QString comparisonOriginal READ comparisonOriginal NOTIFY imagesChanged)
    Q_PROPERTY(QString comparisonEdited READ comparisonEdited NOTIFY imagesChanged)
  public:
    explicit DocumentController(PageImageProvider *, QObject *parent = nullptr);
    ~DocumentController() override;
    bool busy() const { return busy_; }
    bool hasDocument() const { return !active_.isEmpty(); }
    QString error() const { return error_; }
    QString title() const { return active_.value("title").toString(); }
    QString imageSource() const;
    QString comparisonOriginal() const {
        return compareOriginalKey_.isEmpty() ? QString() : "image://pages/" + compareOriginalKey_;
    }
    QString comparisonEdited() const {
        return compareEditedKey_.isEmpty() ? QString() : "image://pages/" + compareEditedKey_;
    }
    QVariantList tabs() const { return tabs_; }
    QVariantMap document() const { return active_; }
    QVariantList searchHits() const { return searchHits_; }
    QVariantList recoveryCandidates() const { return recoveries_; }
    int renderEpoch() const { return renderEpoch_; }
    QString stampPreviewSource() const {
        return stampPreviewKey_.isEmpty() ? QString() : "image://pages/" + stampPreviewKey_;
    }
    Q_INVOKABLE void openPdf(const QUrl &, bool fixtureSpike = false, const QString &password = {});
    Q_INVOKABLE void providePassword(const QString &);
    Q_INVOKABLE void cancelPassword();
    Q_INVOKABLE void openSample() { openPdf(QUrl("qrc:/fixtures/stage1-sample.pdf"), true); }
    Q_INVOKABLE void activateTab(const QString &);
    Q_INVOKABLE void selectPage(int);
    Q_INVOKABLE void setSelection(const QVariantList &pages, const QString &kind, int objectId);
    Q_INVOKABLE QString pageSource(int page, int dpi = 96);
    Q_INVOKABLE QString tileSource(int page, int dpi, double x, double y, double width,
                                   double height);
    Q_INVOKABLE void saveAs(const QUrl &);
    Q_INVOKABLE void save();
    Q_INVOKABLE void closeTab(const QString &, bool discard = false);
    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();
    Q_INVOKABLE void search(const QString &);
    Q_INVOKABLE void copySelection(double, double, double, double);
    Q_INVOKABLE void replaceText(int, const QString &, const QUrl &font = {}, double size = 0,
                                 bool overflow = false, int face = 0, const QString &color = {},
                                 double wrap = 0);
    Q_INVOKABLE void previewText(int, const QString &, const QUrl &font = {}, double size = 0,
                                 bool overflow = false, int face = 0, const QString &color = {},
                                 double wrap = 0);
    Q_INVOKABLE void cancelPreview();
    Q_INVOKABLE void addText(const QString &, const QUrl &, double, double, double, double, double,
                             const QString &, int alignment = 0, int face = 0);
    Q_INVOKABLE void deleteImage(int);
    Q_INVOKABLE void transformImage(int, const QVariantList &);
    Q_INVOKABLE void replaceImage(int, const QUrl &);
    Q_INVOKABLE void insertImage(const QUrl &, double, double, double, double);
    Q_INVOKABLE void copyImage(int);
    Q_INVOKABLE void exportImage(int, const QUrl &);
    Q_INVOKABLE void copyArea(double, double, double, double);
    Q_INVOKABLE void pasteImage(double, double, double, double);
    Q_INVOKABLE void placeStamp(const QUrl &, double, double, double, double, double = 0,
                                double = 1, int frame = 0, const QVariantList &pages = {});
    Q_INVOKABLE void previewStamp(const QUrl &, double width, double height, int frame = 0);
    Q_INVOKABLE void deleteAnnotation(int);
    Q_INVOKABLE void moveAnnotation(int, double, double, double, double);
    Q_INVOKABLE void annotationOpacity(int, double);
    Q_INVOKABLE void rotateStamp(int, double);
    Q_INVOKABLE void duplicateStamp(int);
    Q_INVOKABLE void findInstalledFont(const QString &);
    Q_INVOKABLE void annotate(const QString &, double, double, double, double,
                              const QString &contents = {}, const QString &color = "#ffd234");
    Q_INVOKABLE void flattenStamps();
    Q_INVOKABLE void markStroke(const QVariantList &, double thickness,
                                const QString &color = "#ffd234", double opacity = .4);
    Q_INVOKABLE void markText(const QString &type, double x0, double y0, double x1, double y1);
    Q_INVOKABLE void exportFlattened(const QUrl &);
    Q_INVOKABLE void insertPages(const QVariantList &, int, const QString &range = {});
    Q_INVOKABLE void deletePages(const QVariantList &);
    Q_INVOKABLE void duplicatePages(const QVariantList &);
    Q_INVOKABLE void reorderPages(const QVariantList &);
    Q_INVOKABLE void movePages(const QVariantList &, int at);
    Q_INVOKABLE void rotatePages(const QVariantList &, int degrees);
    Q_INVOKABLE void blankPage(int, double, double);
    Q_INVOKABLE void extractPages(const QVariantList &, const QUrl &);
    Q_INVOKABLE void transferPages(const QString &, const QVariantList &, int, bool);
    Q_INVOKABLE void dropFiles(const QVariantList &, bool overview, int at = 0);
    Q_INVOKABLE void authorizeSignedEdits(bool);
    Q_INVOKABLE void restoreRecovery(const QString &, const QString &password = {});
    Q_INVOKABLE void discardRecovery(const QString &);
    Q_INVOKABLE void flushRecovery();
    Q_INVOKABLE void printDocument(bool fit = false, const QUrl &pdfOutput = {});
  signals:
    void changed();
    void imagesChanged();
    void loaded(bool);
    void saved(bool);
    void passwordRequired(QUrl source);
    void signatureRequested();
    void closeRequested(QString id);
    void installedFontFound(QUrl file, int face);
    void previewFinished(bool);
    void textApplied(bool);
    void imageCopied(QImage);
    void textCopied(QString);

  private:
    using Job = std::function<yolo::core::WorkerResult(yolo::core::DocumentWorker &)>;
    void submit(Job, QString event = {});
    void openNext();
    void edit(std::function<void(yolo::engine::IDocument &)>);
    void editJob(Job, QString completion = {});
    QString id() const { return active_.value("id").toString(); }
    int page() const { return active_.value("page").toInt(); }
    PageImageProvider *provider_;
    QThread thread_;
    yolo::core::DocumentWorker *worker_{};
    QVariantList tabs_, searchHits_, recoveries_;
    QVariantMap active_;
    bool busy_{};
    QString error_, previewKey_, stampPreviewKey_;
    int renderEpoch_{}, previewToken_{}, stampPreviewToken_{};
    QSet<QString> pendingImages_, signedApproved_;
    QString compareOriginalKey_, compareEditedKey_;
    QList<QUrl> openQueue_;
    Job pendingEdit_;
    QString pendingCompletion_;
    QUrl pendingSource_;
    QString pendingRecovery_;
    QImage clipboardImage_;
    QTimer recoveryTimer_;
};
