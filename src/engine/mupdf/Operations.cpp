// SPDX-License-Identifier: AGPL-3.0-only
#include "Operations.h"
#include "Content.h"
#include <QFileInfo>
#include <QImageReader>
#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace yolo::engine::mupdf {
namespace {
constexpr double ptPerMm = 72 / 25.4;
pdf_obj *root(fz_context *ctx, pdf_document *doc) {
    return checked(ctx, [&] { return pdf_dict_get(ctx, pdf_trailer(ctx, doc), PDF_NAME(Root)); });
}
bool signedFields(fz_context *ctx, pdf_obj *fields, int depth = 0) {
    if (depth > 64)
        throw Error(ErrorCode::InvalidDocument, "Field tree exceeds 64 levels");
    const int n = checked(ctx, [&] { return pdf_array_len(ctx, fields); });
    for (int i = 0; i < n; ++i) {
        auto *field = checked(ctx, [&] { return pdf_array_get(ctx, fields, i); });
        if (get(ctx, field, "V") && checked(ctx, [&] {
                return pdf_name_eq(ctx, pdf_dict_get_inheritable(ctx, field, PDF_NAME(FT)),
                                   PDF_NAME(Sig));
            }))
            return true;
        if (signedFields(ctx, get(ctx, field, "Kids"), depth + 1))
            return true;
    }
    return false;
}
std::string uniqueResource(fz_context *ctx, pdf_obj *resources, pdf_obj *object) {
    auto *objects = get(ctx, resources, "XObject");
    for (int i = 0;; ++i) {
        const auto name = "YoloStamp" + std::to_string(i);
        if (!get(ctx, objects, name.c_str())) {
            put(ctx, objects, name.c_str(), object);
            return name;
        }
    }
}
void checkInsertion(fz_context *ctx, pdf_document *doc, int at) {
    const int count = checked(ctx, [&] { return pdf_count_pages(ctx, doc); });
    if (at < 0 || at > count)
        throw Error(ErrorCode::InvalidPage, "Insertion index is out of range");
}
bool pruneField(fz_context *ctx, pdf_document *target, pdf_document *source, pdf_obj *original,
                pdf_obj *copied, const std::set<int> &widgets, pdf_obj *defaultAppearance,
                pdf_obj *sourceResources, const std::map<std::string, std::string> &names,
                int depth = 0) {
    if (depth > 64)
        throw Error(ErrorCode::Unsupported, "Form field tree exceeds 64 levels");
    if (get(ctx, original, "AA") || get(ctx, original, "A"))
        throw Error(ErrorCode::Unsupported, "Form actions require reference-aware rewriting");
    const bool widget = checked(ctx, [&] {
        return pdf_name_eq(ctx, pdf_dict_get(ctx, original, PDF_NAME(Subtype)), PDF_NAME(Widget));
    });
    if (widget && !widgets.count(checked(ctx, [&] { return pdf_to_num(ctx, original); })))
        return false;
    auto *oldKids = get(ctx, original, "Kids");
    auto *copiedKids = get(ctx, copied, "Kids");
    const int n = checked(ctx, [&] { return pdf_array_len(ctx, oldKids); });
    if (n) {
        Object kept(ctx, checked(ctx, [&] { return pdf_new_array(ctx, target, n); }));
        for (int i = 0; i < n; ++i) {
            auto *old = checked(ctx, [&] { return pdf_array_get(ctx, oldKids, i); });
            auto *next = checked(ctx, [&] { return pdf_array_get(ctx, copiedKids, i); });
            if (pruneField(ctx, target, source, old, next, widgets, defaultAppearance,
                           sourceResources, names, depth + 1))
                checked(ctx, [&] { pdf_array_push(ctx, kept.get(), next); });
        }
        if (!checked(ctx, [&] { return pdf_array_len(ctx, kept.get()); }))
            return false;
        put(ctx, copied, "Kids", kept.get());
    }
    auto *da = inherited(ctx, original, "DA");
    if (!da)
        da = defaultAppearance;
    if (da) {
        const auto rewritten = rewriteAppearance(ctx, source, sourceResources, da, names);
        checked(ctx, [&] {
            pdf_dict_put_string(ctx, copied, PDF_NAME(DA), rewritten.data(), rewritten.size());
        });
    }
    return true;
}
void mergeFields(fz_context *ctx, pdf_document *target, pdf_document *source, pdf_graft_map *map,
                 const std::set<int> &widgets) {
    auto *sourceForm = get(ctx, root(ctx, source), "AcroForm");
    auto *sourceFields = get(ctx, sourceForm, "Fields");
    const int n = checked(ctx, [&] { return pdf_array_len(ctx, sourceFields); });
    if (!n)
        return;
    if (get(ctx, sourceForm, "XFA") || get(ctx, sourceForm, "CO"))
        throw Error(ErrorCode::Unsupported, "XFA and calculated form merging are not supported");
    auto *targetRoot = root(ctx, target);
    auto *oldForm = get(ctx, targetRoot, "AcroForm");
    Object form(ctx, checked(ctx, [&] {
                    return oldForm ? pdf_copy_dict(ctx, oldForm) : pdf_new_dict(ctx, target, 4);
                }));
    auto *oldFields = get(ctx, form.get(), "Fields");
    Object fields(ctx, checked(ctx, [&] {
                      return oldFields ? pdf_copy_array(ctx, oldFields)
                                       : pdf_new_array(ctx, target, n);
                  }));
    auto *oldResources = get(ctx, form.get(), "DR");
    Object resources(ctx, checked(ctx, [&] {
                         return oldResources ? pdf_copy_dict(ctx, oldResources)
                                             : pdf_new_dict(ctx, target, 4);
                     }));
    auto *sourceResources = get(ctx, sourceForm, "DR");
    std::string prefix;
    for (int suffix = 0;; ++suffix) {
        prefix = "YoloMerge" + std::to_string(suffix) + "_";
        bool used = false;
        const int types = checked(ctx, [&] { return pdf_dict_len(ctx, resources.get()); });
        for (int i = 0; i < types; ++i) {
            auto *dict = checked(ctx, [&] { return pdf_dict_get_val(ctx, resources.get(), i); });
            const int count = checked(ctx, [&] { return pdf_dict_len(ctx, dict); });
            for (int j = 0; j < count; ++j) {
                const std::string name =
                    checked(ctx, [&] { return pdf_to_name(ctx, pdf_dict_get_key(ctx, dict, j)); });
                if (name.starts_with(prefix))
                    used = true;
            }
        }
        if (!used)
            break;
    }
    std::map<std::string, std::string> names;
    const int types = checked(ctx, [&] { return pdf_dict_len(ctx, sourceResources); });
    for (int i = 0; i < types; ++i) {
        auto *type = checked(ctx, [&] { return pdf_dict_get_key(ctx, sourceResources, i); });
        auto *sourceDict = checked(ctx, [&] { return pdf_dict_get_val(ctx, sourceResources, i); });
        if (!checked(ctx, [&] { return pdf_is_dict(ctx, sourceDict); }))
            continue;
        auto *existing = checked(ctx, [&] { return pdf_dict_get(ctx, resources.get(), type); });
        Object dest(ctx, checked(ctx, [&] {
                        return existing ? pdf_copy_dict(ctx, existing)
                                        : pdf_new_dict(ctx, target, 4);
                    }));
        const int count = checked(ctx, [&] { return pdf_dict_len(ctx, sourceDict); });
        for (int j = 0; j < count; ++j) {
            const std::string oldName = checked(
                ctx, [&] { return pdf_to_name(ctx, pdf_dict_get_key(ctx, sourceDict, j)); });
            auto *object = checked(ctx, [&] { return pdf_dict_get_val(ctx, sourceDict, j); });
            Object copied(ctx,
                          checked(ctx, [&] { return pdf_graft_mapped_object(ctx, map, object); }));
            const auto name = prefix + oldName;
            put(ctx, dest.get(), name.c_str(), copied.get());
            names[oldName] = name;
        }
        checked(ctx, [&] { pdf_dict_put(ctx, resources.get(), type, dest.get()); });
    }
    std::set<std::string> existingNames;
    const int existingCount = checked(ctx, [&] { return pdf_array_len(ctx, fields.get()); });
    for (int i = 0; i < existingCount; ++i) {
        auto *field = checked(ctx, [&] { return pdf_array_get(ctx, fields.get(), i); });
        auto *value = get(ctx, field, "T");
        existingNames.insert(checked(ctx, [&] { return pdf_to_text_string(ctx, value); }));
    }
    auto *defaultAppearance = get(ctx, sourceForm, "DA");
    for (int i = 0; i < n; ++i) {
        auto *original = checked(ctx, [&] { return pdf_array_get(ctx, sourceFields, i); });
        Object copied(ctx,
                      checked(ctx, [&] { return pdf_graft_mapped_object(ctx, map, original); }));
        if (!pruneField(ctx, target, source, original, copied.get(), widgets, defaultAppearance,
                        sourceResources, names))
            continue;
        checked(ctx, [&] { pdf_dict_del(ctx, copied.get(), PDF_NAME(Parent)); });
        auto *originalName = get(ctx, original, "T");
        const std::string base =
            checked(ctx, [&] { return pdf_to_text_string(ctx, originalName); });
        std::string name = base.empty() ? "YoloField" : base;
        for (int suffix = 2; existingNames.count(name); ++suffix)
            name = (base.empty() ? "YoloField" : base) + "_" + std::to_string(suffix);
        checked(ctx, [&] {
            pdf_dict_put_text_string(ctx, copied.get(), PDF_NAME(T), name.c_str());
            pdf_array_push(ctx, fields.get(), copied.get());
        });
        existingNames.insert(name);
    }
    put(ctx, form.get(), "Fields", fields.get());
    put(ctx, form.get(), "DR", resources.get());
    put(ctx, targetRoot, "AcroForm", form.get());
}
pdf_obj *resolveDestination(fz_context *ctx, pdf_document *doc, pdf_obj *value) {
    for (int i = 0; i < 10 && value; ++i) {
        if (checked(ctx, [&] { return pdf_is_array(ctx, value); }))
            return value;
        if (checked(ctx, [&] { return pdf_is_name(ctx, value) || pdf_is_string(ctx, value); }))
            value = checked(ctx, [&] { return pdf_lookup_dest(ctx, doc, value); });
        else
            value = get(ctx, value, "D");
    }
    throw Error(ErrorCode::Unsupported, "Link destination could not be resolved safely");
}
fz_image *loadRaster(fz_context *ctx, const StampSpec &stamp) {
    if (QFileInfo(QString::fromStdString(stamp.artworkFile))
            .suffix()
            .compare("gif", Qt::CaseInsensitive) != 0)
        return checked(ctx, [&] { return fz_new_image_from_file(ctx, stamp.artworkFile.c_str()); });
    QImageReader reader(QString::fromStdString(stamp.artworkFile));
    if (stamp.gifFrame < 0 || stamp.gifFrame > 10000)
        throw Error(ErrorCode::InvalidArgument, "Invalid GIF frame");
    // GIF decoding is sequential; Qt's GIF handler does not support random jumps.
    QImage image;
    for (int i = 0; i <= stamp.gifFrame; ++i) {
        image = reader.read();
        if (image.isNull())
            throw Error(ErrorCode::InvalidArgument, "GIF frame is unavailable");
    }
    image = image.convertToFormat(QImage::Format_RGBA8888);
    if (image.isNull())
        throw Error(ErrorCode::InvalidArgument, "Cannot decode GIF frame");
    Ref<fz_pixmap, fz_drop_pixmap> pix(ctx, checked(ctx, [&] {
                                           return fz_new_pixmap(ctx, fz_device_rgb(ctx),
                                                                image.width(), image.height(),
                                                                nullptr, 1);
                                       }));
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x) {
            const auto *source = image.constScanLine(y) + x * 4;
            auto *dest = pix.get()->samples + y * pix.get()->stride + x * 4;
            for (int c = 0; c < 3; ++c)
                dest[c] = int(source[c]) * source[3] / 255;
            dest[3] = source[3];
        }
    return checked(ctx, [&] { return fz_new_image_from_pixmap(ctx, pix.get(), nullptr); });
}
} // namespace
bool isSigned(fz_context *ctx, pdf_document *doc) {
    return signedFields(ctx, get(ctx, get(ctx, root(ctx, doc), "AcroForm"), "Fields")) ||
           get(ctx, get(ctx, root(ctx, doc), "Perms"), "DocMDP");
}
void ensureEditable(fz_context *ctx, pdf_document *doc, bool allowSigned) {
    if (!checked(ctx, [&] { return pdf_has_permission(ctx, doc, FZ_PERMISSION_EDIT); }))
        throw Error(ErrorCode::Unsupported, "PDF permission restrictions disallow editing");
    if (!allowSigned && isSigned(ctx, doc))
        throw Error(ErrorCode::Unsupported,
                    "Signed PDFs require the incremental save workflow planned for Stage 2");
}
void addStamp(fz_context *ctx, pdf_document *doc, int index, const StampSpec &stamp) {
    for (double value :
         {stamp.xMm, stamp.yMm, stamp.widthMm, stamp.heightMm, stamp.rotation, stamp.opacity})
        if (!std::isfinite(value))
            throw Error(ErrorCode::InvalidArgument, "Stamp dimensions and rotation must be finite");
    if (stamp.widthMm <= 0 || stamp.heightMm <= 0 || stamp.opacity < 0 || stamp.opacity > 1)
        throw Error(ErrorCode::InvalidArgument, "Stamp size must be positive and opacity in [0,1]");
    const float width = stamp.widthMm * ptPerMm, height = stamp.heightMm * ptPerMm;
    const float x = stamp.xMm * ptPerMm, y = stamp.yMm * ptPerMm;
    const auto placement =
        fz_concat(fz_concat(fz_concat(fz_matrix{1, 0, 0, -1, -width / 2, height / 2},
                                      fz_rotate(stamp.rotation)),
                            fz_translate(x + width / 2, y + height / 2)),
                  fz_invert_matrix(pageTransform(ctx, doc, index)));
    const fz_rect bbox{0, 0, width, height};
    Object resources(ctx, checked(ctx, [&] { return pdf_new_dict(ctx, doc, 2); }));
    Buffer contents(ctx, checked(ctx, [&] { return fz_new_buffer(ctx, 256); }));
    Object ext(ctx, checked(ctx, [&] { return pdf_new_dict(ctx, doc, 3); }));
    checked(ctx, [&] {
        pdf_dict_put_name(ctx, ext.get(), PDF_NAME(Type), "ExtGState");
        pdf_dict_put_real(ctx, ext.get(), PDF_NAME(ca), stamp.opacity);
        pdf_dict_put_real(ctx, ext.get(), PDF_NAME(CA), stamp.opacity);
    });
    Object gs(ctx, checked(ctx, [&] { return pdf_new_dict(ctx, doc, 1); }));
    put(ctx, gs.get(), "YoloOpacity", ext.get());
    put(ctx, resources.get(), "ExtGState", gs.get());
    checked(ctx, [&] { fz_append_string(ctx, contents.get(), "q /YoloOpacity gs\n"); });
    const auto extension = QFileInfo(QString::fromStdString(stamp.artworkFile)).suffix().toLower();
    const bool vector = extension == "svg" || extension == "pdf";
    if (vector) {
        checked(ctx, [&] { fz_register_document_handlers(ctx); });
        Ref<fz_document, fz_drop_document> svg(
            ctx, checked(ctx, [&] { return fz_open_document(ctx, stamp.artworkFile.c_str()); }));
        Page page(ctx, checked(ctx, [&] { return fz_load_page(ctx, svg.get(), 0); }));
        const auto bounds = checked(ctx, [&] { return fz_bound_page(ctx, page.get()); });
        if (bounds.x1 <= bounds.x0 || bounds.y1 <= bounds.y0)
            throw Error(ErrorCode::InvalidArgument, "SVG has no positive dimensions");
        const float sx = width / (bounds.x1 - bounds.x0), sy = height / (bounds.y1 - bounds.y0);
        const fz_matrix transform{sx, 0, 0, -sy, -bounds.x0 * sx, height + bounds.y0 * sy};
        Ref<fz_device, fz_drop_device> device(
            ctx, checked(ctx, [&] {
                return pdf_new_pdf_device(ctx, doc, transform, resources.get(), contents.get());
            }));
        checked(ctx, [&] {
            fz_run_page(ctx, page.get(), device.get(), fz_identity, nullptr);
            fz_close_device(ctx, device.get());
        });
    } else {
        Image image(ctx, loadRaster(ctx, stamp));
        Object imageObject(ctx, checked(ctx, [&] { return pdf_add_image(ctx, doc, image.get()); }));
        Object objects(ctx, checked(ctx, [&] { return pdf_new_dict(ctx, doc, 1); }));
        put(ctx, objects.get(), "Artwork", imageObject.get());
        put(ctx, resources.get(), "XObject", objects.get());
        checked(ctx, [&] {
            fz_append_printf(ctx, contents.get(), "%g 0 0 %g 0 0 cm /Artwork Do\n", width, height);
        });
    }
    checked(ctx, [&] { fz_append_string(ctx, contents.get(), "Q\n"); });
    Page page(ctx, checked(ctx, [&] {
                  return reinterpret_cast<fz_page *>(pdf_load_page(ctx, doc, index));
              }));
    auto *annotation = checked(ctx, [&] {
        return pdf_create_annot(ctx, reinterpret_cast<pdf_page *>(page.get()), PDF_ANNOT_STAMP);
    });
    auto *obj = checked(ctx, [&] { return pdf_annot_obj(ctx, annotation); });
    checked(ctx, [&] {
        pdf_dict_put_rect(ctx, obj, PDF_NAME(Rect), fz_transform_rect(bbox, placement));
        pdf_set_annot_flags(ctx, annotation, PDF_ANNOT_IS_PRINT);
        pdf_set_annot_opacity(ctx, annotation, stamp.opacity);
        pdf_set_annot_appearance(ctx, annotation, "N", nullptr, placement, bbox, resources.get(),
                                 contents.get());
    });
    Object metadata(ctx, checked(ctx, [&] { return pdf_new_dict(ctx, doc, 6); }));
    checked(ctx, [&] {
        pdf_dict_puts_drop(ctx, metadata.get(), "Id", pdf_new_text_string(ctx, stamp.id.c_str()));
        pdf_dict_puts_drop(ctx, metadata.get(), "WidthMm", pdf_new_real(ctx, stamp.widthMm));
        pdf_dict_puts_drop(ctx, metadata.get(), "HeightMm", pdf_new_real(ctx, stamp.heightMm));
        pdf_dict_puts_drop(ctx, metadata.get(), "Rot", pdf_new_real(ctx, stamp.rotation));
        pdf_dict_puts(ctx, metadata.get(), "SizeLocked", PDF_TRUE);
    });
    put(ctx, obj, "YoloStamp", metadata.get());
}
void flattenStamps(fz_context *ctx, pdf_document *doc) {
    const int count = checked(ctx, [&] { return pdf_count_pages(ctx, doc); });
    for (int i = 0; i < count; ++i) {
        auto *obj = pageObject(ctx, doc, i);
        Page page(ctx, checked(ctx, [&] {
                      return reinterpret_cast<fz_page *>(pdf_load_page(ctx, doc, i));
                  }));
        Object resources(ctx, copyPageResources(ctx, doc, i));
        Buffer contents(ctx, checked(ctx, [&] { return fz_new_buffer(ctx, 256); }));
        std::set<pdf_obj *> flattened;
        auto *first = checked(
            ctx, [&] { return pdf_first_annot(ctx, reinterpret_cast<pdf_page *>(page.get())); });
        for (auto *annot = first; annot;
             annot = checked(ctx, [&] { return pdf_next_annot(ctx, annot); })) {
            if (checked(ctx, [&] { return pdf_annot_type(ctx, annot); }) != PDF_ANNOT_STAMP)
                continue;
            checked(ctx, [&] { pdf_update_annot(ctx, annot); });
            auto *appearance = checked(ctx, [&] { return pdf_annot_ap(ctx, annot); });
            if (!appearance)
                throw Error(ErrorCode::Unsupported, "Stamp has no usable appearance");
            const auto transform = checked(ctx, [&] { return pdf_annot_transform(ctx, annot); });
            const auto name = uniqueResource(ctx, resources.get(), appearance);
            checked(ctx, [&] {
                fz_append_printf(ctx, contents.get(), "q %g %g %g %g %g %g cm /%s Do Q\n",
                                 transform.a, transform.b, transform.c, transform.d, transform.e,
                                 transform.f, name.c_str());
            });
            flattened.insert(checked(ctx, [&] { return pdf_annot_obj(ctx, annot); }));
        }
        if (flattened.empty())
            continue;
        Object kept(ctx, checked(ctx, [&] { return pdf_new_array(ctx, doc, 4); }));
        auto *old = get(ctx, obj, "Annots");
        const int n = checked(ctx, [&] { return pdf_array_len(ctx, old); });
        for (int j = 0; j < n; ++j) {
            auto *annot = checked(ctx, [&] { return pdf_array_get(ctx, old, j); });
            if (!flattened.count(annot))
                checked(ctx, [&] { pdf_array_push(ctx, kept.get(), annot); });
        }
        appendContent(ctx, doc, i, resources.get(), contents.get());
        put(ctx, obj, "Annots", kept.get());
    }
}
void insertPages(fz_context *ctx, pdf_document *doc, int at,
                 const std::vector<PageSource> &sources) {
    checkInsertion(ctx, doc, at);
    if (sources.empty())
        throw Error(ErrorCode::InvalidArgument, "No source files provided");
    for (const auto &source : sources) {
        Ref<pdf_document, pdf_drop_document> input(
            ctx, checked(ctx, [&] { return pdf_open_document(ctx, source.path.c_str()); }));
        if (checked(ctx, [&] { return pdf_needs_password(ctx, input.get()); }))
            throw Error(ErrorCode::PasswordRequired, "Insertion source requires a password");
        if (signedFields(ctx, get(ctx, get(ctx, root(ctx, input.get()), "AcroForm"), "Fields")))
            throw Error(ErrorCode::Unsupported,
                        "Signed source form fields cannot be grafted as valid signatures");
        const int count = checked(ctx, [&] { return pdf_count_pages(ctx, input.get()); });
        auto order = source.pages;
        if (order.empty())
            for (int i = 0; i < count; ++i)
                order.push_back(i);
        for (int page : order)
            if (page < 0 || page >= count)
                throw Error(ErrorCode::InvalidPage, "Insertion source page out of range");
        Ref<pdf_graft_map, pdf_drop_graft_map> map(
            ctx, checked(ctx, [&] { return pdf_new_graft_map(ctx, doc); }));
        std::set<int> widgets;
        std::set<int> widgetPages;
        std::map<int, pdf_obj *> destinations;
        std::vector<std::pair<pdf_obj *, pdf_obj *>> links;
        for (int page : order) {
            auto *sourcePage = pageObject(ctx, input.get(), page);
            auto *annots = get(ctx, sourcePage, "Annots");
            const int n = checked(ctx, [&] { return pdf_array_len(ctx, annots); });
            bool hasWidgets = false;
            for (int j = 0; j < n; ++j) {
                const auto type = checked(ctx, [&] {
                    return pdf_dict_get_name(ctx, pdf_array_get(ctx, annots, j), PDF_NAME(Subtype));
                });
                if (std::string(type) == "Widget") {
                    hasWidgets = true;
                    widgets.insert(checked(
                        ctx, [&] { return pdf_to_num(ctx, pdf_array_get(ctx, annots, j)); }));
                }
            }
            if (hasWidgets && !widgetPages.insert(page).second)
                throw Error(ErrorCode::Unsupported,
                            "Duplicate imported form pages require separate field subtrees");
            checked(ctx, [&] { pdf_graft_mapped_page(ctx, map.get(), at, input.get(), page); });
            auto *target = pageObject(ctx, doc, at++);
            destinations.emplace(page, target);
            if (annots) {
                Object copied(ctx, checked(ctx, [&] {
                                  return pdf_graft_mapped_object(ctx, map.get(), annots);
                              }));
                put(ctx, target, "Annots", copied.get());
                for (int j = 0; j < n; ++j) {
                    auto *annotation =
                        checked(ctx, [&] { return pdf_array_get(ctx, copied.get(), j); });
                    put(ctx, annotation, "P", target);
                    auto *original = checked(ctx, [&] { return pdf_array_get(ctx, annots, j); });
                    if (checked(ctx, [&] {
                            return pdf_name_eq(ctx, pdf_dict_get(ctx, original, PDF_NAME(Subtype)),
                                               PDF_NAME(Link));
                        }))
                        links.emplace_back(original, annotation);
                }
            }
        }
        for (auto [original, copied] : links) {
            auto *action = get(ctx, original, "A");
            auto *value = get(ctx, original, "Dest");
            const bool goTo =
                action && checked(ctx, [&] {
                    return pdf_name_eq(ctx, pdf_dict_get(ctx, action, PDF_NAME(S)), PDF_NAME(GoTo));
                });
            if (goTo)
                value = get(ctx, action, "D");
            if (!value)
                continue;
            auto *destination = resolveDestination(ctx, input.get(), value);
            auto *page = checked(ctx, [&] { return pdf_array_get(ctx, destination, 0); });
            const int index = checked(ctx, [&] {
                return pdf_is_int(ctx, page) ? pdf_to_int(ctx, page)
                                             : pdf_lookup_page_number(ctx, input.get(), page);
            });
            if (!destinations.count(index))
                throw Error(ErrorCode::Unsupported,
                            "Link targets a page outside the selected source range");
            Object next(ctx, checked(ctx, [&] {
                            return pdf_graft_mapped_object(ctx, map.get(), destination);
                        }));
            checked(ctx, [&] { pdf_array_put(ctx, next.get(), 0, destinations[index]); });
            if (goTo)
                put(ctx, get(ctx, copied, "A"), "D", next.get());
            else
                put(ctx, copied, "Dest", next.get());
        }
        mergeFields(ctx, doc, input.get(), map.get(), widgets);
    }
}
void blankPage(fz_context *ctx, pdf_document *doc, int at, double width, double height) {
    checkInsertion(ctx, doc, at);
    if (!std::isfinite(width) || !std::isfinite(height) || width <= 0 || height <= 0 ||
        width > 5000 || height > 5000)
        throw Error(ErrorCode::InvalidArgument, "Blank page dimensions must be in (0,5000] mm");
    Buffer buffer(ctx, checked(ctx, [&] { return fz_new_buffer(ctx, 0); }));
    Object resources(ctx, checked(ctx, [&] { return pdf_new_dict(ctx, doc, 0); }));
    const fz_rect bounds{0, 0, float(width * ptPerMm), float(height * ptPerMm)};
    Object page(ctx, checked(ctx, [&] {
                    return pdf_add_page(ctx, doc, bounds, 0, resources.get(), buffer.get());
                }));
    checked(ctx, [&] { pdf_insert_page(ctx, doc, at, page.get()); });
}
void rearrange(fz_context *ctx, pdf_document *doc, const std::vector<int> &order) {
    auto *oldRoot = root(ctx, doc);
    Object catalog(ctx, checked(ctx, [&] { return pdf_copy_dict(ctx, oldRoot); }));
    const int count = checked(ctx, [&] { return pdf_count_pages(ctx, doc); });
    std::map<int, int> seen;
    std::vector<int> actual;
    int extra = 0;
    for (int index : order) {
        if (!seen[index]++) {
            actual.push_back(index);
            continue;
        }
        auto *source = pageObject(ctx, doc, index);
        if (get(ctx, source, "StructParents"))
            throw Error(ErrorCode::Unsupported,
                        "Duplicating a tagged page requires a new structure subtree");
        checked(ctx, [&] { pdf_flatten_inheritable_page_items(ctx, source); });
        Object copied(ctx, checked(ctx, [&] { return pdf_copy_dict(ctx, source); }));
        Object ref(ctx, checked(ctx, [&] { return pdf_add_object(ctx, doc, copied.get()); }));
        auto *annots = get(ctx, source, "Annots");
        Object newAnnots(ctx, checked(ctx, [&] { return pdf_new_array(ctx, doc, 4); }));
        const int n = checked(ctx, [&] { return pdf_array_len(ctx, annots); });
        for (int i = 0; i < n; ++i) {
            auto *annot = checked(ctx, [&] { return pdf_array_get(ctx, annots, i); });
            if (checked(ctx, [&] {
                    return pdf_name_eq(ctx, pdf_dict_get(ctx, annot, PDF_NAME(Subtype)),
                                       PDF_NAME(Widget));
                }))
                throw Error(ErrorCode::Unsupported,
                            "Duplicating form widgets requires a new field subtree");
            Object copy(ctx, checked(ctx, [&] { return pdf_copy_dict(ctx, annot); }));
            put(ctx, copy.get(), "P", ref.get());
            Object annotationRef(
                ctx, checked(ctx, [&] { return pdf_add_object(ctx, doc, copy.get()); }));
            checked(ctx, [&] { pdf_array_push(ctx, newAnnots.get(), annotationRef.get()); });
        }
        if (annots)
            put(ctx, ref.get(), "Annots", newAnnots.get());
        checked(ctx, [&] { pdf_insert_page(ctx, doc, count + extra, ref.get()); });
        actual.push_back(count + extra++);
    }
    checked(ctx, [&] {
        pdf_rearrange_pages(ctx, doc, int(actual.size()), actual.data(), PDF_CLEAN_STRUCTURE_KEEP);
    });
    auto *target = root(ctx, doc);
    const int n = checked(ctx, [&] { return pdf_dict_len(ctx, catalog.get()); });
    for (int i = 0; i < n; ++i) {
        auto *key = checked(ctx, [&] { return pdf_dict_get_key(ctx, catalog.get(), i); });
        const std::string name = checked(ctx, [&] { return pdf_to_name(ctx, key); });
        if (name == "AcroForm" || name == "Names" || name == "Dests" || name == "StructTreeRoot")
            continue;
        if (checked(ctx, [&] { return pdf_dict_get(ctx, target, key); }))
            continue;
        auto *value = checked(ctx, [&] { return pdf_dict_get_val(ctx, catalog.get(), i); });
        checked(ctx, [&] { pdf_dict_put(ctx, target, key, value); });
    }
}
std::vector<std::uint8_t> serialize(fz_context *ctx, pdf_document *doc) {
    if (doc->crypt || isSigned(ctx, doc)) {
        if (!checked(ctx, [&] { return pdf_can_be_saved_incrementally(ctx, doc); }))
            throw Error(ErrorCode::Unsupported, "This protected PDF cannot be saved incrementally");
        Buffer bytes(ctx, checked(ctx, [&] { return fz_new_buffer(ctx, 4096); }));
        Ref<fz_output, fz_drop_output> out(
            ctx, checked(ctx, [&] { return fz_new_output_with_buffer(ctx, bytes.get()); }));
        checked(ctx, [&] {
            writeSnapshot(ctx, doc, out.get());
            fz_close_output(ctx, out.get());
        });
        unsigned char *data;
        const auto size = checked(ctx, [&] { return fz_buffer_storage(ctx, bytes.get(), &data); });
        return {data, data + size};
    }
    const int expectedPages = checked(ctx, [&] { return pdf_count_pages(ctx, doc); });
    // Save a reachable-object clone, so subsetting and garbage collection cannot
    // alter the live document or its journal. Preserve the entire catalog graph.
    Ref<pdf_document, pdf_drop_document> clone(
        ctx, checked(ctx, [&] { return pdf_create_document(ctx); }));
    Ref<pdf_graft_map, pdf_drop_graft_map> map(
        ctx, checked(ctx, [&] { return pdf_new_graft_map(ctx, clone.get()); }));
    auto *catalog = root(ctx, doc);
    Object clonedRoot(
        ctx, checked(ctx, [&] { return pdf_graft_mapped_object(ctx, map.get(), catalog); }));
    auto *trailer = checked(ctx, [&] { return pdf_trailer(ctx, clone.get()); });
    put(ctx, trailer, "Root", clonedRoot.get());
    auto *sourceTrailer = checked(ctx, [&] { return pdf_trailer(ctx, doc); });
    for (const char *key : {"Info", "ID"}) {
        auto *object = get(ctx, sourceTrailer, key);
        if (!object)
            continue;
        Object copied(
            ctx, checked(ctx, [&] { return pdf_graft_mapped_object(ctx, map.get(), object); }));
        put(ctx, trailer, key, copied.get());
    }
    if (doc->crypt)
        throw Error(ErrorCode::Unsupported,
                    "Encrypted export requires the encryption-preserving save workflow");
    clone.get()->version = doc->version;
    checked(ctx, [&] { pdf_subset_fonts(ctx, clone.get(), 0, nullptr); });
    Buffer buffer(ctx, checked(ctx, [&] { return fz_new_buffer(ctx, 4096); }));
    Ref<fz_output, fz_drop_output> output(
        ctx, checked(ctx, [&] { return fz_new_output_with_buffer(ctx, buffer.get()); }));
    auto options = pdf_default_write_options;
    options.do_encrypt = PDF_ENCRYPT_KEEP;
    options.dont_regenerate_id = 1;
    options.do_garbage = 1;
    options.do_compress_fonts = 1;
    checked(ctx, [&] {
        pdf_write_document(ctx, clone.get(), output.get(), &options);
        fz_close_output(ctx, output.get());
    });
    unsigned char *bytes;
    const auto length = checked(ctx, [&] { return fz_buffer_storage(ctx, buffer.get(), &bytes); });
    std::vector<std::uint8_t> result(bytes, bytes + length);
    Ref<fz_stream, fz_drop_stream> stream(
        ctx, checked(ctx, [&] { return fz_open_memory(ctx, result.data(), result.size()); }));
    Ref<pdf_document, pdf_drop_document> verify(
        ctx, checked(ctx, [&] { return pdf_open_document_with_stream(ctx, stream.get()); }));
    if (checked(ctx, [&] { return pdf_count_pages(ctx, verify.get()); }) != expectedPages)
        throw Error(ErrorCode::UnsafeEdit,
                    "Serialized PDF page count differs from the edited document");
    return result;
}
} // namespace yolo::engine::mupdf
