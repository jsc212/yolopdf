// SPDX-License-Identifier: AGPL-3.0-only
// Native replay driver for adb. It deliberately creates no Qt application or
// event loop: a standalone Android shell process has no Java activity/VM.
#include "engine/api/Document.h"
#include <charconv>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <unistd.h>

using namespace yolo::engine;
namespace {
std::string quote(const std::string &value) {
    std::ostringstream out;
    out << '"';
    for (unsigned char c : value) {
        if (c == '"' || c == '\\')
            out << '\\' << c;
        else if (c < 32)
            out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << int(c) << std::dec;
        else
            out << c;
    }
    out << '"';
    return out.str();
}
int number(const std::map<std::string, std::string> &options, const char *name, int fallback,
           int minimum) {
    const auto found = options.find(name);
    if (found == options.end())
        return fallback;
    int result;
    const auto &value = found->second;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || result < minimum)
        throw Error(ErrorCode::InvalidArgument, std::string("Invalid ") + name);
    return result;
}
void save(const std::string &source, const std::string &destination, IDocument &document) {
    namespace fs = std::filesystem;
    if (fs::weakly_canonical(source) == fs::weakly_canonical(destination) ||
        (fs::exists(destination) && fs::equivalent(source, destination)))
        throw Error(ErrorCode::InvalidArgument, "Output must differ from input");
    auto bytes = document.pdfBytes();
    std::string pattern = destination + ".XXXXXX";
    int fd = mkstemp(pattern.data());
    if (fd < 0)
        throw std::runtime_error("Cannot create output temporary file");
    try {
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            const auto count = ::write(fd, bytes.data() + offset, bytes.size() - offset);
            if (count <= 0)
                throw std::runtime_error("PDF write failed");
            offset += count;
        }
        if (fsync(fd) != 0)
            throw std::runtime_error("PDF flush failed");
        const int closeResult = ::close(fd);
        fd = -1;
        if (closeResult != 0)
            throw std::runtime_error("PDF close failed");
        fs::rename(pattern, destination);
    } catch (...) {
        if (fd >= 0)
            ::close(fd);
        fs::remove(pattern);
        throw;
    }
}
} // namespace
int main(int argc, char **argv) {
    if (argc < 3) {
        std::cerr << "Usage: yolopdf-cli info|text-runs <pdf> | replace-text <pdf> <output> --text "
                     "TEXT [--run N --page N --font FILE --font-index N]\n";
        return 2;
    }
    try {
        const std::string command = argv[1];
        const bool replace = command == "replace-text";
        if ((command != "info" && command != "text-runs" && !replace) || (replace && argc < 6))
            throw Error(ErrorCode::InvalidArgument, "Unknown command or missing arguments");
        std::map<std::string, std::string> options;
        for (int i = replace ? 4 : 3; i < argc; i += 2) {
            if (i + 1 >= argc)
                throw Error(ErrorCode::InvalidArgument, "Option requires a value");
            const std::string key = argv[i];
            if (key != "--page" && key != "--run" && key != "--text" && key != "--font" &&
                key != "--font-index")
                throw Error(ErrorCode::InvalidArgument, "Unknown option");
            options[key] = argv[i + 1];
        }
        auto doc = openDocument(argv[2]);
        const int page = number(options, "--page", 1, 1) - 1;
        if (replace) {
            if (!options.count("--text"))
                throw Error(ErrorCode::InvalidArgument, "--text is required");
            doc->replaceText(page, {number(options, "--run", 0, 0), options["--text"],
                                    options["--font"], number(options, "--font-index", 0, 0)});
            save(argv[2], argv[3], *doc);
            return 0;
        }
        std::cout << std::setprecision(12);
        if (command == "info") {
            const auto info = doc->inspect();
            std::cout << "{\"pageCount\":" << info.pages.size()
                      << ",\"engineVersion\":" << quote(engineVersion()) << ",\"pages\":[";
            for (std::size_t i = 0; i < info.pages.size(); ++i) {
                if (i)
                    std::cout << ',';
                const auto &page = info.pages[i];
                std::cout << "{\"page\":" << i + 1 << ",\"rotation\":" << page.rotation
                          << ",\"userUnit\":" << page.userUnit << '}';
            }
            std::cout << "]}\n";
        } else {
            const auto text = doc->textRuns(page);
            std::cout << "{\"runs\":[";
            for (std::size_t i = 0; i < text.runs.size(); ++i) {
                if (i)
                    std::cout << ',';
                const auto &run = text.runs[i];
                std::cout << "{\"id\":" << run.id << ",\"text\":" << quote(run.text)
                          << ",\"font\":" << quote(run.fontName) << ",\"status\":"
                          << quote(run.status == TextStatus::OriginalFont ? "original-font"
                                   : run.status == TextStatus::NeedsFont  ? "needs-font"
                                                                          : "unsupported")
                          << ",\"reason\":" << quote(run.reason) << '}';
            }
            std::cout << "],\"noTextReason\":" << quote(text.noTextReason) << "}\n";
        }
    } catch (const Error &error) {
        std::cerr << "PDF error: " << error.what() << '\n';
        return error.code() == ErrorCode::InvalidArgument    ? 2
               : error.code() == ErrorCode::PasswordRequired ? 3
                                                             : 1;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
