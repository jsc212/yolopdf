// SPDX-License-Identifier: AGPL-3.0-only
#include "Print.h"
#include "core/DocumentWorker.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPageLayout>
#include <QPageSize>
#include <QPainter>
#include <QPdfWriter>
#include <QStandardPaths>
#include <QUuid>
#include <algorithm>
#include <cmath>
#ifdef Q_OS_ANDROID
#include <QJniObject>
#include <QtCore/qcoreapplication_platform.h>
#endif
#ifdef YOLOPDF_DESKTOP_PRINT
#include <QPrintPreviewDialog>
#include <QPrinter>
#endif
namespace yolo::platform {
PrintBundle preparePrint(engine::IDocument &doc, const QString &title) {
    const auto info = doc.inspect();
    if (!info.mayPrint)
        throw std::runtime_error("PDF permissions disallow printing");
    PrintBundle bundle;
    bundle.folder = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/print-" +
                    QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (!QDir().mkpath(bundle.folder))
        throw std::runtime_error("Cannot create print cache");
    bundle.pdf = bundle.folder + "/document.pdf";
    try {
        QPdfWriter writer(bundle.pdf);
        writer.setTitle(title);
        writer.setCreator("yolopdf");
        writer.setResolution(300);
        QPainter painter;
        qint64 cacheBytes = 0;
        for (std::size_t index = 0; index < info.pages.size(); ++index) {
            auto bounds = info.pages[index].visibleBounds;
            double width = bounds.x1 - bounds.x0, height = bounds.y1 - bounds.y0;
            const int dpi =
                std::clamp(int(std::sqrt(16.0 * 1024 * 1024 / (width * height)) * 72), 1, 300);
            auto image = core::image(doc.renderSource(index)->job()->render(dpi));
            const auto path = bundle.folder + "/" + QString::number(index) + ".png";
            if (image.isNull() || !image.save(path, "PNG"))
                throw std::runtime_error("Cannot prepare print page");
            cacheBytes += QFileInfo(path).size();
            if (cacheBytes > 1024ll * 1024 * 1024)
                throw std::runtime_error("Print preview exceeds the 1 GiB cache limit");
            writer.setPageLayout(
                QPageLayout(QPageSize(QSizeF(std::min(width, height), std::max(width, height)),
                                      QPageSize::Point),
                            width > height ? QPageLayout::Landscape : QPageLayout::Portrait,
                            QMarginsF(0, 0, 0, 0), QPageLayout::Point));
            if (index == 0) {
                if (!painter.begin(&writer))
                    throw std::runtime_error("Cannot create print PDF");
            } else if (!writer.newPage())
                throw std::runtime_error("Cannot append print page");
            painter.drawImage(QRectF(0, 0, width * 300 / 72, height * 300 / 72), image);
            bundle.pages.append(QVariantMap{{"file", path}, {"width", width}, {"height", height}});
        }
        painter.end();
    } catch (...) {
        QDir(bundle.folder).removeRecursively();
        throw;
    }
    return bundle;
}
void previewPrint(const PrintBundle &bundle, bool fit) {
#ifdef YOLOPDF_DESKTOP_PRINT
    if (bundle.pages.isEmpty())
        return;
    QPrinter printer(QPrinter::HighResolution);
    printer.setResolution(300);
    printer.setFullPage(true);
    const auto first = bundle.pages.front().toMap();
    const double w = first.value("width").toDouble(), h = first.value("height").toDouble();
    printer.setPageLayout(
        QPageLayout(QPageSize(QSizeF(std::min(w, h), std::max(w, h)), QPageSize::Point),
                    w > h ? QPageLayout::Landscape : QPageLayout::Portrait, QMarginsF(0, 0, 0, 0),
                    QPageLayout::Point));
    QPrintPreviewDialog preview(&printer);
    QObject::connect(
        &preview, &QPrintPreviewDialog::paintRequested, &preview, [&](QPrinter *device) {
            QPainter painter(device);
            if (!painter.isActive())
                return;
            for (int i = 0; i < bundle.pages.size(); ++i) {
                if (i > 0 && !device->newPage())
                    break;
                const auto page = bundle.pages[i].toMap();
                QImage image(page.value("file").toString());
                QSizeF size(page.value("width").toDouble() * device->resolution() / 72,
                            page.value("height").toDouble() * device->resolution() / 72);
                QRectF target(QPointF(0, 0), size);
                if (fit) {
                    const auto available =
                        device->pageLayout().paintRectPixels(device->resolution());
                    const double scale = std::min(available.width() / size.width(),
                                                  available.height() / size.height());
                    size *= scale;
                    target = QRectF(QPointF(available.center().x() - size.width() / 2,
                                            available.center().y() - size.height() / 2),
                                    size);
                }
                painter.drawImage(target, image);
            }
        });
    preview.resize(900, 700);
    preview.exec();
#elif defined(Q_OS_ANDROID)
    const auto folder = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) +
                        "/android-print-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (!QDir().mkpath(folder) || !QFile::copy(bundle.pdf, folder + "/document.pdf"))
        throw std::runtime_error("Cannot cache Android print document");
    const auto activity = QNativeInterface::QAndroidApplication::context();
    const auto path = QJniObject::fromString(folder + "/document.pdf"),
               title = QJniObject::fromString("yolopdf document");
    QJniObject::callStaticMethod<void>(
        "org/yolopdf/MobileIntegration", "print",
        "(Landroid/app/Activity;Ljava/lang/String;Ljava/lang/String;Z)V", activity.object(),
        path.object<jstring>(), title.object<jstring>(), jboolean(fit));
#else
    (void)bundle;
    (void)fit;
#endif
}
} // namespace yolo::platform
