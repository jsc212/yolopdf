// SPDX-License-Identifier: AGPL-3.0-only
#include "engine/api/Document.h"
#include "platform/FontLocator.h"
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSet>
#include <QTextStream>
#include <QUuid>
#include <cmath>

using namespace yolo::engine;

namespace {
QJsonArray jsonRect(const Rect &rect) {
    return {rect.x0, rect.y0, rect.x1, rect.y1};
}
QJsonObject jsonInfo(const DocumentInfo &info) {
    QJsonArray pages;
    QJsonArray warnings;
    for (const auto &warning : info.warnings)
        warnings.append(QString::fromStdString(warning));
    for (const auto &page : info.pages) {
        QJsonArray fonts;
        for (const auto &font : page.fonts)
            fonts.append(QJsonObject{{"resourcePath", QString::fromStdString(font.resourcePath)},
                                     {"name", QString::fromStdString(font.name)},
                                     {"type", QString::fromStdString(font.type)},
                                     {"objectNumber", font.objectNumber},
                                     {"embedded", font.embedded},
                                     {"subset", font.subset},
                                     {"hasToUnicode", font.hasToUnicode}});
        QJsonArray images;
        for (const auto &image : page.imageResources)
            images.append(QJsonObject{{"resourcePath", QString::fromStdString(image.resourcePath)},
                                      {"objectNumber", image.objectNumber},
                                      {"width", image.width},
                                      {"height", image.height},
                                      {"hasAlpha", image.hasAlpha}});
        pages.append(QJsonObject{{"page", page.index + 1},
                                 {"mediaBox", jsonRect(page.mediaBox)},
                                 {"cropBox", jsonRect(page.cropBox)},
                                 {"rotation", page.rotation},
                                 {"userUnit", page.userUnit},
                                 {"fonts", fonts},
                                 {"imageResources", images}});
    }
    return {{"schemaVersion", 1},
            {"engine", "MuPDF"},
            {"engineVersion", QString::fromStdString(engineVersion())},
            {"pageCount", static_cast<int>(info.pages.size())},
            {"pages", pages},
            {"warnings", warnings}};
}

double numeric(const QCommandLineParser &parser, const char *option) {
    bool ok = false;
    const auto value = parser.value(option).toDouble(&ok);
    if (!ok || !std::isfinite(value))
        throw Error(ErrorCode::InvalidArgument, std::string("Invalid --") + option);
    return value;
}
int integer(const QCommandLineParser &parser, const char *option, int minimum = 0) {
    bool ok = false;
    const int value = parser.value(option).toInt(&ok);
    if (!ok || value < minimum)
        throw Error(ErrorCode::InvalidArgument, std::string("Invalid --") + option);
    return value;
}
std::vector<int> pageList(const QString &value, int count) {
    std::vector<int> result;
    for (const auto &token : value.split(',')) {
        const auto bounds = token.split('-');
        bool ok = false;
        const int first = bounds[0].toInt(&ok);
        if (!ok || first < 1 || first > count)
            throw Error(ErrorCode::InvalidArgument, "Invalid page range");
        int last = first;
        if (bounds.size() == 2) {
            last = bounds[1].toInt(&ok);
            if (!ok || last < first || last > count)
                throw Error(ErrorCode::InvalidArgument, "Invalid page range");
        } else if (bounds.size() != 1)
            throw Error(ErrorCode::InvalidArgument, "Invalid page range");
        if (result.size() + last - first + 1 > 10000)
            throw Error(ErrorCode::InvalidArgument, "Page range exceeds 10000 entries");
        for (int i = first; i <= last; ++i)
            result.push_back(i - 1);
    }
    return result;
}
Matrix transform(const QString &value) {
    const auto parts = value.split(',');
    if (parts.size() != 6)
        throw Error(ErrorCode::InvalidArgument, "Matrix requires a,b,c,d,e,f");
    Matrix result;
    for (int i = 0; i < 6; ++i) {
        bool ok;
        result[i] = parts[i].toDouble(&ok);
        if (!ok || !std::isfinite(result[i]))
            throw Error(ErrorCode::InvalidArgument, "Invalid matrix value");
    }
    return result;
}
QJsonArray jsonMatrix(const Matrix &m) {
    return {m[0], m[1], m[2], m[3], m[4], m[5]};
}
void savePng(const QString &path, const RasterImage &raster) {
    const QImage image(raster.rgba.data(), raster.width, raster.height, raster.width * 4,
                       QImage::Format_RGBA8888);
    QSaveFile output(path);
    output.setDirectWriteFallback(false);
    if (!output.open(QIODevice::WriteOnly) || !image.save(&output, "PNG") || !output.commit())
        throw std::runtime_error("Cannot write output PNG");
}
void savePdf(const QString &path, IDocument &document) {
    const auto bytes = document.pdfBytes();
    QSaveFile output(path);
    output.setDirectWriteFallback(false);
    if (!output.open(QIODevice::WriteOnly) ||
        output.write(reinterpret_cast<const char *>(bytes.data()), qint64(bytes.size())) !=
            qint64(bytes.size()) ||
        !output.commit())
        throw std::runtime_error("Cannot atomically write output PDF");
}
} // namespace

int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName("yolopdf-cli");
    QCoreApplication::setApplicationVersion("0.1.0");
    QCommandLineParser parser;
    parser.setApplicationDescription(
        "Offline PDF engine driver. Inspect, trace, render, replace text, edit images, place "
        "stamps, and organize pages. See docs/STAGE1.md for examples and limits.");
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument("command",
                                 "Operation (info, text-runs, images, render, replace-text, etc.)");
    parser.addPositionalArgument("pdf", "Input PDF (never modified)");
    parser.addPositionalArgument("output", "Output PDF or PNG", "[output]");
    parser.addPositionalArgument("sources", "Source PDFs or artwork for insertion/replacement",
                                 "[sources...]");
    parser.addOption({"page", "Page to render (one-based)", "number", "1"});
    parser.addOption({"dpi", "Render resolution (1–1200 DPI)", "dpi", "72"});
    parser.addOption({"run", "Zero-based text run ID from text-runs", "id", "0"});
    parser.addOption({"text", "Replacement UTF-8 text", "text"});
    parser.addOption({"font", "Explicitly imported font for the whole run", "file"});
    parser.addOption({"font-index", "Font collection face index", "index", "0"});
    parser.addOption({"original-jpeg", "Copy the original JPEG stream instead of encoding a PNG"});
    parser.addOption({"occurrence", "Zero-based image occurrence ID", "id", "0"});
    parser.addOption({"matrix", "Image transform in displayed page points: a,b,c,d,e,f", "matrix"});
    parser.addOption({"pages", "One-based page range, e.g. 1-3,7", "range"});
    parser.addOption({"at", "One-based insertion position (count+1 appends)", "position", "1"});
    parser.addOption({"degrees", "Rotation in degrees", "angle", "90"});
    parser.addOption({"x", "Stamp top-left offset in mm", "mm", "0"});
    parser.addOption({"y", "Stamp top-left offset in mm", "mm", "0"});
    parser.addOption({"width", "Stamp or blank page width in mm", "mm", "40"});
    parser.addOption({"height", "Stamp or blank page height in mm", "mm", "20"});
    parser.addOption({"rotation", "Stamp clockwise rotation", "angle", "0"});
    parser.addOption({"opacity", "Stamp opacity from 0 to 1", "fraction", "1"});
    parser.addOption({"frame", "GIF frame (zero-based)", "index", "0"});
    parser.addOption({"size", "Replacement font size (0 keeps original)", "points", "0"});
    parser.addOption({"widen", "Explicitly allow wider text; neighbours may overlap"});
    parser.addOption({"wrap-width", "Wrap width in displayed points", "points", "0"});
    parser.process(app);
    const auto args = parser.positionalArguments();
    const QSet<QString> inspection = {"info", "text-runs", "text-lines", "font-match", "images"};
    const QSet<QString> simple = {
        "render",       "copy-image",      "copy-area",     "replace-text", "replace-line",
        "delete-image", "transform-image", "reorder-pages", "delete-pages", "rotate-page",
        "blank-page",   "extract-pages",   "flatten-stamps"};
    const QSet<QString> sourceCommand = {"insert-pages", "insert-image", "replace-image",
                                         "place-stamp"};
    if (args.size() < 2 ||
        (!inspection.contains(args[0]) && !simple.contains(args[0]) &&
         !sourceCommand.contains(args[0])) ||
        (inspection.contains(args[0]) && args.size() != 2) ||
        (simple.contains(args[0]) && args.size() != 3) ||
        (sourceCommand.contains(args[0]) &&
         (args[0] == "insert-pages" ? args.size() < 4 : args.size() != 4))) {
        QTextStream(stderr)
            << "Usage: yolopdf-cli <command> <input.pdf> [output] [sources...] [options]\n";
        return 2;
    }
    try {
        auto document = openDocument(args[1].toUtf8().toStdString());
        const auto command = args[0];
        const int page = integer(parser, "page", 1) - 1;
        if (command == "info") {
            QTextStream(stdout)
                << QJsonDocument(jsonInfo(document->inspect())).toJson(QJsonDocument::Indented);
        } else if (command == "text-lines") {
            QJsonArray lines;
            for (auto &line : document->textLines(page)) {
                QJsonArray segments;
                for (auto &s : line.segments)
                    segments.append(QJsonObject{{"runId", s.runId},
                                                {"start", s.start},
                                                {"length", s.length},
                                                {"font", QString::fromStdString(s.style.fontName)},
                                                {"size", s.style.fontSize}});
                lines.append(
                    QJsonObject{{"id", line.id},
                                {"text", QString::fromStdString(line.text)},
                                {"bounds", jsonRect(line.bounds)},
                                {"segments", segments},
                                {"status", line.status == TextStatus::OriginalFont ? "original-font"
                                           : line.status == TextStatus::NeedsFont  ? "needs-font"
                                                                                   : "unsupported"},
                                {"reason", QString::fromStdString(line.reason)}});
            }
            QTextStream(stdout) << QJsonDocument(lines).toJson();
        } else if (command == "text-runs") {
            const auto traced = document->textRuns(page);
            QJsonArray runs;
            for (const auto &run : traced.runs)
                runs.append(
                    QJsonObject{{"id", run.id},
                                {"text", QString::fromStdString(run.text)},
                                {"font", QString::fromStdString(run.fontName)},
                                {"size", run.fontSize},
                                {"bounds", jsonRect(run.bounds)},
                                {"textMatrix", jsonMatrix(run.textMatrix)},
                                {"ctm", jsonMatrix(run.ctm)},
                                {"status", run.status == TextStatus::OriginalFont ? "original-font"
                                           : run.status == TextStatus::NeedsFont  ? "needs-font"
                                                                                  : "unsupported"},
                                {"reason", QString::fromStdString(run.reason)}});
            QTextStream(stdout) << QJsonDocument(
                                       QJsonObject{{"runs", runs},
                                                   {"noTextReason",
                                                    QString::fromStdString(traced.noTextReason)}})
                                       .toJson();
        } else if (command == "font-match") {
            const auto runs = document->textRuns(page).runs;
            const int id = integer(parser, "run");
            if (id >= int(runs.size()))
                throw Error(ErrorCode::InvalidArgument, "Unknown text run");
            const auto match =
                yolo::platform::findExactFont(QString::fromStdString(runs[id].fontName));
            QTextStream(stdout) << QJsonDocument(QJsonObject{{"matched", !match.file.isEmpty()},
                                                             {"file", match.file},
                                                             {"faceIndex", match.faceIndex}})
                                       .toJson();
        } else if (command == "images") {
            QJsonArray images;
            for (const auto &image : document->images(page))
                images.append(QJsonObject{{"id", image.id},
                                          {"transform", jsonMatrix(image.transform)},
                                          {"bounds", jsonRect(image.bounds)},
                                          {"width", image.width},
                                          {"height", image.height},
                                          {"inline", image.inlineImage}});
            QTextStream(stdout) << QJsonDocument(images).toJson();
        } else {
            if (QFileInfo(args[1]).absoluteFilePath() == QFileInfo(args[2]).absoluteFilePath() ||
                (QFileInfo(args[2]).exists() && QFileInfo(args[1]).canonicalFilePath() ==
                                                    QFileInfo(args[2]).canonicalFilePath())) {
                QTextStream(stderr) << "Output must differ from the input PDF\n";
                return 2;
            }
            if (command == "render") {
                savePng(args[2], document->render(page, numeric(parser, "dpi")));
                return 0;
            }
            if (command == "copy-image") {
                if (parser.isSet("original-jpeg")) {
                    const auto bytes = document->originalJpeg(page, integer(parser, "occurrence"));
                    QSaveFile output(args[2]);
                    output.setDirectWriteFallback(false);
                    if (!output.open(QIODevice::WriteOnly) ||
                        output.write(reinterpret_cast<const char *>(bytes.data()), bytes.size()) !=
                            qint64(bytes.size()) ||
                        !output.commit())
                        throw std::runtime_error("Cannot write original JPEG");
                } else
                    savePng(args[2], document->copyImage(page, integer(parser, "occurrence")));
                return 0;
            }
            if (command == "copy-area") {
                const double x = numeric(parser, "x") * 72 / 25.4,
                             y = numeric(parser, "y") * 72 / 25.4;
                savePng(args[2],
                        document->copyArea(page,
                                           {x, y, x + numeric(parser, "width") * 72 / 25.4,
                                            y + numeric(parser, "height") * 72 / 25.4},
                                           parser.isSet("dpi") ? numeric(parser, "dpi") : 300));
                return 0;
            }
            for (const auto &warning : document->inspect().warnings)
                QTextStream(stderr) << "Warning: " << QString::fromStdString(warning) << '\n';
            if (command == "replace-text" || command == "replace-line") {
                if (!parser.isSet("text"))
                    throw Error(ErrorCode::InvalidArgument,
                                "--text is required (an empty value deletes the run)");
                TextReplacement edit{integer(parser, "run"),
                                     parser.value("text").toUtf8().toStdString(),
                                     parser.value("font").toUtf8().toStdString(),
                                     integer(parser, "font-index"),
                                     numeric(parser, "size"),
                                     parser.isSet("widen")};
                edit.wrapWidth = numeric(parser, "wrap-width");
                if (command == "replace-line")
                    document->replaceLine(page, edit);
                else
                    document->replaceText(page, edit);
            } else if (command == "delete-image")
                document->editImage(page,
                                    {integer(parser, "occurrence"), ImageAction::Delete, {}, {}});
            else if (command == "transform-image")
                document->editImage(page, {integer(parser, "occurrence"),
                                           ImageAction::Transform,
                                           transform(parser.value("matrix")),
                                           {}});
            else if (command == "replace-image")
                document->editImage(page, {integer(parser, "occurrence"),
                                           ImageAction::Replace,
                                           {},
                                           args[3].toUtf8().toStdString()});
            else if (command == "insert-image")
                document->insertImage(page, args[3].toUtf8().toStdString(),
                                      transform(parser.value("matrix")));
            else if (command == "place-stamp") {
                StampSpec stamp;
                stamp.artworkFile = args[3].toUtf8().toStdString();
                stamp.id = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
                stamp.xMm = numeric(parser, "x");
                stamp.yMm = numeric(parser, "y");
                stamp.widthMm = numeric(parser, "width");
                stamp.heightMm = numeric(parser, "height");
                stamp.rotation = numeric(parser, "rotation");
                stamp.opacity = numeric(parser, "opacity");
                stamp.gifFrame = integer(parser, "frame");
                document->placeStamp(page, stamp);
            } else if (command == "insert-pages") {
                std::vector<PageSource> sources;
                for (int i = 3; i < args.size(); ++i) {
                    if (QFileInfo(args[i]).absoluteFilePath() ==
                            QFileInfo(args[2]).absoluteFilePath() ||
                        (QFileInfo(args[2]).exists() && QFileInfo(args[i]).canonicalFilePath() ==
                                                            QFileInfo(args[2]).canonicalFilePath()))
                        throw Error(ErrorCode::InvalidArgument,
                                    "Output must differ from all insertion sources");
                    PageSource source{args[i].toUtf8().toStdString(), {}};
                    if (parser.isSet("pages")) {
                        auto input = openDocument(source.path);
                        source.pages =
                            pageList(parser.value("pages"), int(input->inspect().pages.size()));
                    }
                    sources.push_back(std::move(source));
                }
                document->insertPages(integer(parser, "at", 1) - 1, sources);
            } else if (command == "reorder-pages" || command == "delete-pages" ||
                       command == "extract-pages") {
                const int count = int(document->inspect().pages.size());
                if (!parser.isSet("pages"))
                    throw Error(ErrorCode::InvalidArgument, "--pages is required");
                auto order = pageList(parser.value("pages"), count);
                if (command == "delete-pages") {
                    std::vector<int> kept;
                    for (int i = 0; i < count; ++i)
                        if (std::find(order.begin(), order.end(), i) == order.end())
                            kept.push_back(i);
                    order = std::move(kept);
                }
                if (command == "extract-pages") {
                    auto extracted = createDocument();
                    extracted->insertPages(0, {{args[1].toUtf8().toStdString(), order}});
                    document = std::move(extracted);
                } else
                    document->rearrangePages(order);
            } else if (command == "rotate-page") {
                const auto degrees = numeric(parser, "degrees");
                if (std::abs(degrees) > 360000 || std::floor(degrees) != degrees)
                    throw Error(ErrorCode::InvalidArgument, "Rotation requires integer degrees");
                document->rotatePage(page, int(degrees));
            } else if (command == "blank-page")
                document->insertBlankPage(integer(parser, "at", 1) - 1, numeric(parser, "width"),
                                          numeric(parser, "height"));
            else if (command == "flatten-stamps")
                document->flattenStamps();
            savePdf(args[2], *document);
        }
    } catch (const Error &error) {
        QTextStream(stderr) << "PDF error: " << error.what() << '\n';
        return error.code() == ErrorCode::PasswordRequired  ? 3
               : error.code() == ErrorCode::InvalidArgument ? 2
                                                            : 1;
    } catch (const std::exception &error) {
        QTextStream(stderr) << "Error: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
