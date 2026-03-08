#include "ppp/core/processing_config_io.h"

#include <array>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

namespace ppp::core {

namespace {

// ---------------------------------------------------------------------------
// JSON writer helpers
// ---------------------------------------------------------------------------

[[nodiscard]] std::string escape_json(std::string_view value) {
    static constexpr std::array<char, 16> hex_digits{
        '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'};
    std::string escaped;
    escaped.reserve(value.size() + 16);
    for (unsigned char ch : value) {
        switch (ch) {
        case '"': escaped += "\\\""; break;
        case '\\': escaped += "\\\\"; break;
        case '\n': escaped += "\\n"; break;
        case '\r': escaped += "\\r"; break;
        case '\t': escaped += "\\t"; break;
        default:
            if (ch < 0x20) {
                escaped += "\\u00";
                escaped.push_back(hex_digits[(ch >> 4) & 0x0F]);
                escaped.push_back(hex_digits[ch & 0x0F]);
            } else {
                escaped.push_back(static_cast<char>(ch));
            }
            break;
        }
    }
    return escaped;
}

class JsonWriter {
public:
    void begin_object() {
        maybe_comma();
        out_ << '{';
        depth_.push_back(0);
    }
    void end_object() {
        out_ << '}';
        depth_.pop_back();
    }
    void begin_array() {
        maybe_comma();
        out_ << '[';
        depth_.push_back(0);
    }
    void end_array() {
        out_ << ']';
        depth_.pop_back();
    }

    void key(std::string_view k) {
        maybe_comma();
        out_ << '"' << escape_json(k) << "\":";
        suppress_comma_ = true;
    }

    void value_string(std::string_view v) {
        maybe_comma();
        out_ << '"' << escape_json(v) << '"';
    }
    void value_int(std::int64_t v) {
        maybe_comma();
        out_ << v;
    }
    void value_double(double v) {
        maybe_comma();
        out_ << v;
    }
    void value_bool(bool v) {
        maybe_comma();
        out_ << (v ? "true" : "false");
    }

    // Convenience: key + value in one call.
    void kv(std::string_view k, std::string_view v) { key(k); value_string(v); }
    void kv(std::string_view k, const std::string& v) { kv(k, std::string_view{v}); }
    void kv(std::string_view k, const char* v) { kv(k, std::string_view{v}); }
    void kv(std::string_view k, std::int32_t v) { key(k); value_int(v); }
    void kv(std::string_view k, double v) { key(k); value_double(v); }
    void kv(std::string_view k, bool v) { key(k); value_bool(v); }

    [[nodiscard]] std::string str() const { return out_.str(); }

private:
    void maybe_comma() {
        if (suppress_comma_) {
            suppress_comma_ = false;
            return;
        }
        if (!depth_.empty() && depth_.back()++ > 0) {
            out_ << ',';
        }
    }

    std::ostringstream out_;
    std::vector<int> depth_;
    bool suppress_comma_{false};
};

// ---------------------------------------------------------------------------
// Writer: sub-struct serialization
// ---------------------------------------------------------------------------

void write_measurement(JsonWriter& w, std::string_view k, const Measurement& m) {
    w.key(k);
    w.begin_object();
    w.kv("value", m.value);
    w.kv("unit", to_string(m.unit));
    w.end_object();
}

void write_edge_values(JsonWriter& w, std::string_view k, const EdgeValues& ev) {
    w.key(k);
    w.begin_object();
    write_measurement(w, "top", ev.top);
    write_measurement(w, "left", ev.left);
    write_measurement(w, "right", ev.right);
    write_measurement(w, "bottom", ev.bottom);
    w.end_object();
}

void write_margin_edge(JsonWriter& w, std::string_view k, const MarginEdge& me) {
    w.key(k);
    w.begin_object();
    write_measurement(w, "distance", me.distance);
    w.kv("mode", to_string(me.mode));
    w.end_object();
}

void write_margin_config(JsonWriter& w, const MarginConfig& mc) {
    w.begin_object();
    write_margin_edge(w, "top", mc.top);
    write_margin_edge(w, "left", mc.left);
    write_margin_edge(w, "right", mc.right);
    write_margin_edge(w, "bottom", mc.bottom);
    w.kv("center_horizontal", mc.center_horizontal);
    w.kv("center_vertical", mc.center_vertical);
    w.kv("keep_horizontal", mc.keep_horizontal);
    w.kv("keep_vertical", mc.keep_vertical);
    write_measurement(w, "keep_x", mc.keep_x);
    write_measurement(w, "keep_y", mc.keep_y);
    w.kv("keep_h_center", mc.keep_h_center);
    w.kv("keep_v_center", mc.keep_v_center);
    w.end_object();
}

void write_canvas_config(JsonWriter& w, std::string_view k, const CanvasConfig& cc) {
    w.key(k);
    w.begin_object();
    w.kv("preset", to_string(cc.preset));
    write_measurement(w, "width", cc.width);
    write_measurement(w, "height", cc.height);
    w.kv("orientation", to_string(cc.orientation));
    w.end_object();
}

void write_offset_config(JsonWriter& w, const OffsetConfig& oc) {
    w.begin_object();
    write_measurement(w, "x", oc.x);
    write_measurement(w, "y", oc.y);
    w.end_object();
}

void write_deskew_config(JsonWriter& w, std::string_view k, const DeskewConfig& dc) {
    w.key(k);
    w.begin_object();
    w.kv("enabled", dc.enabled);
    w.kv("detect_mode", dc.detect_mode);
    w.kv("min_angle", dc.min_angle);
    w.kv("max_angle", dc.max_angle);
    w.kv("fast", dc.fast);
    w.kv("border_protect", dc.border_protect);
    w.kv("interpolate", dc.interpolate);
    w.kv("character_protect", dc.character_protect);
    w.kv("char_protect_below", dc.char_protect_below);
    w.kv("algorithm", dc.algorithm);
    w.kv("report_no_skew", dc.report_no_skew);
    w.end_object();
}

void write_despeckle_config(JsonWriter& w, std::string_view k, const DespeckleConfig& dc) {
    w.key(k);
    w.begin_object();
    w.kv("mode", to_string(dc.mode));
    w.kv("object_min", dc.object_min);
    w.kv("object_max", dc.object_max);
    w.end_object();
}

void write_edge_cleanup_config(JsonWriter& w, std::string_view k, const EdgeCleanupConfig& ec) {
    w.key(k);
    w.begin_object();
    w.kv("enabled", ec.enabled);
    w.kv("order", to_string(ec.order));
    write_edge_values(w, "set1", ec.set1);
    write_edge_values(w, "set2", ec.set2);
    w.end_object();
}

void write_hole_cleanup_config(JsonWriter& w, std::string_view k, const HoleCleanupConfig& hc) {
    w.key(k);
    w.begin_object();
    w.kv("enabled", hc.enabled);
    write_edge_values(w, "set1", hc.set1);
    write_edge_values(w, "set2", hc.set2);
    w.end_object();
}

void write_subimage_config(JsonWriter& w, std::string_view k, const SubimageConfig& sc) {
    w.key(k);
    w.begin_object();
    write_measurement(w, "max_width", sc.max_width);
    write_measurement(w, "max_height", sc.max_height);
    w.kv("report_small", sc.report_small);
    w.kv("min_width_px", sc.min_width_px);
    w.kv("min_height_px", sc.min_height_px);
    w.end_object();
}

void write_blank_page_config(JsonWriter& w, std::string_view k, const BlankPageConfig& bp) {
    w.key(k);
    w.begin_object();
    w.kv("enabled", bp.enabled);
    w.kv("threshold_percent", bp.threshold_percent);
    w.kv("min_components", bp.min_components);
    write_measurement(w, "edge_margin", bp.edge_margin);
    w.end_object();
}

void write_color_dropout_config(JsonWriter& w, std::string_view k, const ColorDropoutConfig& cd) {
    w.key(k);
    w.begin_object();
    w.kv("enabled", cd.enabled);
    w.kv("color", to_string(cd.color));
    w.kv("threshold", static_cast<std::int32_t>(cd.threshold));
    w.end_object();
}

void write_movement_limit_config(JsonWriter& w, std::string_view k, const MovementLimitConfig& ml) {
    w.key(k);
    w.begin_object();
    w.kv("enabled", ml.enabled);
    write_measurement(w, "max_vertical", ml.max_vertical);
    write_measurement(w, "max_horizontal", ml.max_horizontal);
    w.end_object();
}

void write_resize_config(JsonWriter& w, std::string_view k, const ResizeConfig& rc) {
    w.key(k);
    w.begin_object();
    w.kv("enabled", rc.enabled);
    write_edge_values(w, "margins_set1", rc.margins_set1);
    write_edge_values(w, "margins_set2", rc.margins_set2);
    w.kv("source", to_string(rc.source));
    write_edge_values(w, "source_set1", rc.source_set1);
    write_edge_values(w, "source_set2", rc.source_set2);
    w.kv("anti_alias", rc.anti_alias);
    w.kv("keep_size", rc.keep_size);
    write_canvas_config(w, "canvas", rc.canvas);
    w.kv("v_alignment", to_string(rc.v_alignment));
    w.kv("h_alignment", to_string(rc.h_alignment));
    w.kv("allow_shrink", rc.allow_shrink);
    w.kv("allow_enlarge", rc.allow_enlarge);
    w.kv("output_path", rc.output_path);
    w.kv("merge_tiff", rc.merge_tiff);
    w.kv("merge_pdf", rc.merge_pdf);
    w.kv("merge_tiff_name", rc.merge_tiff_name);
    w.kv("merge_pdf_name", rc.merge_pdf_name);
    w.end_object();
}

void write_output_config(JsonWriter& w, std::string_view k, const OutputConfig& oc) {
    w.key(k);
    w.begin_object();
    w.kv("tiff_output", oc.tiff_output);
    w.kv("pdf_output", oc.pdf_output);
    w.kv("raster_format", to_string(oc.raster_format));
    w.kv("jpeg_quality", oc.jpeg_quality);
    w.kv("new_extension", oc.new_extension);
    w.kv("save_to_different_dir", oc.save_to_different_dir);
    w.kv("output_directory", oc.output_directory);
    w.kv("conflict_policy", to_string(oc.conflict_policy));
    w.kv("path_mode", to_string(oc.path_mode));
    w.end_object();
}

void write_scan_config(JsonWriter& w, std::string_view k, const ScanConfig& sc) {
    w.key(k);
    w.begin_object();
    w.kv("prefix", sc.prefix);
    w.kv("start_at", sc.start_at);
    w.kv("increment", sc.increment);
    w.kv("extension", sc.extension);
    w.kv("crop", sc.crop);
    w.kv("verso_recto", sc.verso_recto);
    w.kv("invert", sc.invert);
    w.kv("transfer_mode", sc.transfer_mode);
    w.kv("compression", sc.compression);
    w.kv("force_resolution", sc.force_resolution);
    write_edge_values(w, "crop_verso", sc.crop_verso);
    write_edge_values(w, "crop_recto", sc.crop_recto);
    w.end_object();
}

void write_print_config(JsonWriter& w, std::string_view k, const PrintConfig& pc) {
    w.key(k);
    w.begin_object();
    w.kv("page_use_mode", pc.page_use_mode);
    w.kv("scale_mode", pc.scale_mode);
    w.kv("scale_x", pc.scale_x);
    w.kv("scale_y", pc.scale_y);
    w.end_object();
}

// ---------------------------------------------------------------------------
// JSON parser (reused from job_serialization pattern)
// ---------------------------------------------------------------------------

struct JsonParser {
    std::string_view input;
    std::size_t pos{0};

    void skip_ws() {
        while (pos < input.size() && std::isspace(static_cast<unsigned char>(input[pos])))
            ++pos;
    }
    bool consume(char ch) {
        skip_ws();
        if (pos < input.size() && input[pos] == ch) { ++pos; return true; }
        return false;
    }
    bool consume_literal(std::string_view lit) {
        skip_ws();
        if (input.substr(pos, lit.size()) == lit) { pos += lit.size(); return true; }
        return false;
    }
    char peek() { skip_ws(); return pos < input.size() ? input[pos] : '\0'; }
    bool eof() { skip_ws(); return pos >= input.size(); }
};

[[nodiscard]] std::optional<std::string> parse_string(JsonParser& p) {
    p.skip_ws();
    if (p.pos >= p.input.size() || p.input[p.pos] != '"') return std::nullopt;
    ++p.pos;
    std::string value;
    while (p.pos < p.input.size()) {
        const char ch = p.input[p.pos++];
        if (ch == '"') return value;
        if (static_cast<unsigned char>(ch) < 0x20) return std::nullopt;
        if (ch == '\\') {
            if (p.pos >= p.input.size()) return std::nullopt;
            const char esc = p.input[p.pos++];
            switch (esc) {
            case '"': value.push_back('"'); break;
            case '\\': value.push_back('\\'); break;
            case '/': value.push_back('/'); break;
            case 'n': value.push_back('\n'); break;
            case 'r': value.push_back('\r'); break;
            case 't': value.push_back('\t'); break;
            default: return std::nullopt;
            }
        } else {
            value.push_back(ch);
        }
    }
    return std::nullopt;
}

[[nodiscard]] bool skip_value(JsonParser& p);

[[nodiscard]] std::optional<std::int64_t> parse_integer(JsonParser& p) {
    p.skip_ws();
    const std::size_t begin = p.pos;
    if (begin >= p.input.size()) return std::nullopt;
    if (p.input[p.pos] == '-') ++p.pos;
    bool has_digits = false;
    while (p.pos < p.input.size() && std::isdigit(static_cast<unsigned char>(p.input[p.pos]))) {
        ++p.pos;
        has_digits = true;
    }
    if (!has_digits) return std::nullopt;
    const auto num = p.input.substr(begin, p.pos - begin);
    std::int64_t value{};
    const auto [ptr, ec] = std::from_chars(num.data(), num.data() + num.size(), value);
    if (ec != std::errc{} || ptr != num.data() + num.size()) return std::nullopt;
    return value;
}

[[nodiscard]] std::optional<double> parse_double(JsonParser& p) {
    p.skip_ws();
    const std::size_t begin = p.pos;
    if (begin >= p.input.size()) return std::nullopt;
    if (p.input[p.pos] == '-') ++p.pos;
    bool has_digits = false;
    while (p.pos < p.input.size() && std::isdigit(static_cast<unsigned char>(p.input[p.pos]))) {
        ++p.pos;
        has_digits = true;
    }
    if (p.pos < p.input.size() && p.input[p.pos] == '.') {
        ++p.pos;
        while (p.pos < p.input.size() && std::isdigit(static_cast<unsigned char>(p.input[p.pos])))
            ++p.pos;
    }
    if (p.pos < p.input.size() && (p.input[p.pos] == 'e' || p.input[p.pos] == 'E')) {
        ++p.pos;
        if (p.pos < p.input.size() && (p.input[p.pos] == '+' || p.input[p.pos] == '-'))
            ++p.pos;
        while (p.pos < p.input.size() && std::isdigit(static_cast<unsigned char>(p.input[p.pos])))
            ++p.pos;
    }
    if (!has_digits) return std::nullopt;
    const auto num = p.input.substr(begin, p.pos - begin);
#if defined(_MSC_VER) && _MSC_VER >= 1924
    double value{};
    const auto [ptr, ec] = std::from_chars(num.data(), num.data() + num.size(), value);
    if (ec != std::errc{}) return std::nullopt;
    return value;
#else
    try {
        return std::stod(std::string{num});
    } catch (...) {
        return std::nullopt;
    }
#endif
}

[[nodiscard]] std::optional<bool> parse_bool(JsonParser& p) {
    if (p.consume_literal("true")) return true;
    if (p.consume_literal("false")) return false;
    return std::nullopt;
}

[[nodiscard]] bool skip_array(JsonParser& p) {
    if (!p.consume('[')) return false;
    if (p.consume(']')) return true;
    while (true) {
        if (!skip_value(p)) return false;
        if (p.consume(']')) return true;
        if (!p.consume(',')) return false;
    }
}

[[nodiscard]] bool skip_object(JsonParser& p) {
    if (!p.consume('{')) return false;
    if (p.consume('}')) return true;
    while (true) {
        if (!parse_string(p)) return false;
        if (!p.consume(':')) return false;
        if (!skip_value(p)) return false;
        if (p.consume('}')) return true;
        if (!p.consume(',')) return false;
    }
}

[[nodiscard]] bool skip_value(JsonParser& p) {
    p.skip_ws();
    if (p.pos >= p.input.size()) return false;
    const char ch = p.input[p.pos];
    if (ch == '"') return parse_string(p).has_value();
    if (ch == '{') return skip_object(p);
    if (ch == '[') return skip_array(p);
    if (ch == 't') return p.consume_literal("true");
    if (ch == 'f') return p.consume_literal("false");
    if (ch == 'n') return p.consume_literal("null");
    if (ch == '-' || std::isdigit(static_cast<unsigned char>(ch))) return parse_double(p).has_value();
    return false;
}

// ---------------------------------------------------------------------------
// Parser: sub-struct deserialization
// ---------------------------------------------------------------------------

// Helper: iterate object members, calling fn(key) for each. fn must consume the value.
template <typename Fn>
[[nodiscard]] bool parse_object(JsonParser& p, Fn&& fn) {
    if (!p.consume('{')) return false;
    if (p.consume('}')) return true;
    bool first = true;
    while (true) {
        if (!first && !p.consume(',')) return false;
        first = false;
        auto k = parse_string(p);
        if (!k) return false;
        if (!p.consume(':')) return false;
        if (!fn(*k)) return false;
        if (p.peek() == '}') { p.consume('}'); return true; }
    }
}

[[nodiscard]] bool parse_measurement(JsonParser& p, Measurement& m) {
    return parse_object(p, [&](const std::string& k) -> bool {
        if (k == "value") {
            auto v = parse_double(p);
            if (!v) return false;
            m.value = *v;
        } else if (k == "unit") {
            auto v = parse_string(p);
            if (!v) return false;
            auto u = measurement_unit_from_string(*v);
            if (!u) return false;
            m.unit = *u;
        } else {
            return skip_value(p);
        }
        return true;
    });
}

[[nodiscard]] bool parse_edge_values(JsonParser& p, EdgeValues& ev) {
    return parse_object(p, [&](const std::string& k) -> bool {
        if (k == "top") return parse_measurement(p, ev.top);
        if (k == "left") return parse_measurement(p, ev.left);
        if (k == "right") return parse_measurement(p, ev.right);
        if (k == "bottom") return parse_measurement(p, ev.bottom);
        return skip_value(p);
    });
}

[[nodiscard]] bool parse_margin_edge(JsonParser& p, MarginEdge& me) {
    return parse_object(p, [&](const std::string& k) -> bool {
        if (k == "distance") return parse_measurement(p, me.distance);
        if (k == "mode") {
            auto v = parse_string(p);
            if (!v) return false;
            auto m = margin_mode_from_string(*v);
            if (!m) return false;
            me.mode = *m;
        } else {
            return skip_value(p);
        }
        return true;
    });
}

[[nodiscard]] bool parse_margin_config(JsonParser& p, MarginConfig& mc) {
    return parse_object(p, [&](const std::string& k) -> bool {
        if (k == "top") return parse_margin_edge(p, mc.top);
        if (k == "left") return parse_margin_edge(p, mc.left);
        if (k == "right") return parse_margin_edge(p, mc.right);
        if (k == "bottom") return parse_margin_edge(p, mc.bottom);
        if (k == "center_horizontal") { auto v = parse_bool(p); if (!v) return false; mc.center_horizontal = *v; return true; }
        if (k == "center_vertical") { auto v = parse_bool(p); if (!v) return false; mc.center_vertical = *v; return true; }
        if (k == "keep_horizontal") { auto v = parse_bool(p); if (!v) return false; mc.keep_horizontal = *v; return true; }
        if (k == "keep_vertical") { auto v = parse_bool(p); if (!v) return false; mc.keep_vertical = *v; return true; }
        if (k == "keep_x") return parse_measurement(p, mc.keep_x);
        if (k == "keep_y") return parse_measurement(p, mc.keep_y);
        if (k == "keep_h_center") { auto v = parse_bool(p); if (!v) return false; mc.keep_h_center = *v; return true; }
        if (k == "keep_v_center") { auto v = parse_bool(p); if (!v) return false; mc.keep_v_center = *v; return true; }
        return skip_value(p);
    });
}

[[nodiscard]] bool parse_canvas_config(JsonParser& p, CanvasConfig& cc) {
    return parse_object(p, [&](const std::string& k) -> bool {
        if (k == "preset") {
            auto v = parse_string(p);
            if (!v) return false;
            auto cp = canvas_preset_from_string(*v);
            if (!cp) return false;
            cc.preset = *cp;
        } else if (k == "width") {
            return parse_measurement(p, cc.width);
        } else if (k == "height") {
            return parse_measurement(p, cc.height);
        } else if (k == "orientation") {
            auto v = parse_string(p);
            if (!v) return false;
            auto o = orientation_from_string(*v);
            if (!o) return false;
            cc.orientation = *o;
        } else {
            return skip_value(p);
        }
        return true;
    });
}

[[nodiscard]] bool parse_offset_config(JsonParser& p, OffsetConfig& oc) {
    return parse_object(p, [&](const std::string& k) -> bool {
        if (k == "x") return parse_measurement(p, oc.x);
        if (k == "y") return parse_measurement(p, oc.y);
        return skip_value(p);
    });
}

[[nodiscard]] bool parse_deskew_config(JsonParser& p, DeskewConfig& dc) {
    return parse_object(p, [&](const std::string& k) -> bool {
        if (k == "enabled") { auto v = parse_bool(p); if (!v) return false; dc.enabled = *v; return true; }
        if (k == "detect_mode") { auto v = parse_integer(p); if (!v) return false; dc.detect_mode = static_cast<std::int32_t>(*v); return true; }
        if (k == "min_angle") { auto v = parse_double(p); if (!v) return false; dc.min_angle = *v; return true; }
        if (k == "max_angle") { auto v = parse_double(p); if (!v) return false; dc.max_angle = *v; return true; }
        if (k == "fast") { auto v = parse_bool(p); if (!v) return false; dc.fast = *v; return true; }
        if (k == "border_protect") { auto v = parse_bool(p); if (!v) return false; dc.border_protect = *v; return true; }
        if (k == "interpolate") { auto v = parse_bool(p); if (!v) return false; dc.interpolate = *v; return true; }
        if (k == "character_protect") { auto v = parse_bool(p); if (!v) return false; dc.character_protect = *v; return true; }
        if (k == "char_protect_below") { auto v = parse_double(p); if (!v) return false; dc.char_protect_below = *v; return true; }
        if (k == "algorithm") { auto v = parse_integer(p); if (!v) return false; dc.algorithm = static_cast<std::int32_t>(*v); return true; }
        if (k == "report_no_skew") { auto v = parse_bool(p); if (!v) return false; dc.report_no_skew = *v; return true; }
        return skip_value(p);
    });
}

[[nodiscard]] bool parse_despeckle_config(JsonParser& p, DespeckleConfig& dc) {
    return parse_object(p, [&](const std::string& k) -> bool {
        if (k == "mode") {
            auto v = parse_string(p);
            if (!v) return false;
            auto m = despeckle_mode_from_string(*v);
            if (!m) return false;
            dc.mode = *m;
        } else if (k == "object_min") { auto v = parse_integer(p); if (!v) return false; dc.object_min = static_cast<std::int32_t>(*v);
        } else if (k == "object_max") { auto v = parse_integer(p); if (!v) return false; dc.object_max = static_cast<std::int32_t>(*v);
        } else {
            return skip_value(p);
        }
        return true;
    });
}

[[nodiscard]] bool parse_edge_cleanup_config(JsonParser& p, EdgeCleanupConfig& ec) {
    return parse_object(p, [&](const std::string& k) -> bool {
        if (k == "enabled") { auto v = parse_bool(p); if (!v) return false; ec.enabled = *v; return true; }
        if (k == "order") {
            auto v = parse_string(p);
            if (!v) return false;
            auto o = edge_cleanup_order_from_string(*v);
            if (!o) return false;
            ec.order = *o;
            return true;
        }
        if (k == "set1") return parse_edge_values(p, ec.set1);
        if (k == "set2") return parse_edge_values(p, ec.set2);
        return skip_value(p);
    });
}

[[nodiscard]] bool parse_hole_cleanup_config(JsonParser& p, HoleCleanupConfig& hc) {
    return parse_object(p, [&](const std::string& k) -> bool {
        if (k == "enabled") { auto v = parse_bool(p); if (!v) return false; hc.enabled = *v; return true; }
        if (k == "set1") return parse_edge_values(p, hc.set1);
        if (k == "set2") return parse_edge_values(p, hc.set2);
        return skip_value(p);
    });
}

[[nodiscard]] bool parse_subimage_config(JsonParser& p, SubimageConfig& sc) {
    return parse_object(p, [&](const std::string& k) -> bool {
        if (k == "max_width") return parse_measurement(p, sc.max_width);
        if (k == "max_height") return parse_measurement(p, sc.max_height);
        if (k == "report_small") { auto v = parse_bool(p); if (!v) return false; sc.report_small = *v; return true; }
        if (k == "min_width_px") { auto v = parse_integer(p); if (!v) return false; sc.min_width_px = static_cast<std::int32_t>(*v); return true; }
        if (k == "min_height_px") { auto v = parse_integer(p); if (!v) return false; sc.min_height_px = static_cast<std::int32_t>(*v); return true; }
        return skip_value(p);
    });
}

[[nodiscard]] bool parse_blank_page_config(JsonParser& p, BlankPageConfig& bp) {
    return parse_object(p, [&](const std::string& k) -> bool {
        if (k == "enabled") { auto v = parse_bool(p); if (!v) return false; bp.enabled = *v; return true; }
        if (k == "threshold_percent") { auto v = parse_double(p); if (!v) return false; bp.threshold_percent = *v; return true; }
        if (k == "min_components") { auto v = parse_integer(p); if (!v) return false; bp.min_components = static_cast<std::int32_t>(*v); return true; }
        if (k == "edge_margin") return parse_measurement(p, bp.edge_margin);
        return skip_value(p);
    });
}

[[nodiscard]] bool parse_color_dropout_config(JsonParser& p, ColorDropoutConfig& cd) {
    return parse_object(p, [&](const std::string& k) -> bool {
        if (k == "enabled") { auto v = parse_bool(p); if (!v) return false; cd.enabled = *v; return true; }
        if (k == "color") {
            auto v = parse_string(p);
            if (!v) return false;
            auto c = dropout_color_from_string(*v);
            if (!c) return false;
            cd.color = *c;
            return true;
        }
        if (k == "threshold") { auto v = parse_integer(p); if (!v) return false; cd.threshold = static_cast<std::uint8_t>(*v); return true; }
        return skip_value(p);
    });
}

[[nodiscard]] bool parse_movement_limit_config(JsonParser& p, MovementLimitConfig& ml) {
    return parse_object(p, [&](const std::string& k) -> bool {
        if (k == "enabled") { auto v = parse_bool(p); if (!v) return false; ml.enabled = *v; return true; }
        if (k == "max_vertical") return parse_measurement(p, ml.max_vertical);
        if (k == "max_horizontal") return parse_measurement(p, ml.max_horizontal);
        return skip_value(p);
    });
}

[[nodiscard]] bool parse_resize_config(JsonParser& p, ResizeConfig& rc) {
    return parse_object(p, [&](const std::string& k) -> bool {
        if (k == "enabled") { auto v = parse_bool(p); if (!v) return false; rc.enabled = *v; return true; }
        if (k == "margins_set1") return parse_edge_values(p, rc.margins_set1);
        if (k == "margins_set2") return parse_edge_values(p, rc.margins_set2);
        if (k == "source") {
            auto v = parse_string(p);
            if (!v) return false;
            auto s = resize_from_from_string(*v);
            if (!s) return false;
            rc.source = *s;
            return true;
        }
        if (k == "source_set1") return parse_edge_values(p, rc.source_set1);
        if (k == "source_set2") return parse_edge_values(p, rc.source_set2);
        if (k == "anti_alias") { auto v = parse_bool(p); if (!v) return false; rc.anti_alias = *v; return true; }
        if (k == "keep_size") { auto v = parse_bool(p); if (!v) return false; rc.keep_size = *v; return true; }
        if (k == "canvas") return parse_canvas_config(p, rc.canvas);
        if (k == "v_alignment") {
            auto v = parse_string(p);
            if (!v) return false;
            auto va = v_alignment_from_string(*v);
            if (!va) return false;
            rc.v_alignment = *va;
            return true;
        }
        if (k == "h_alignment") {
            auto v = parse_string(p);
            if (!v) return false;
            auto ha = h_alignment_from_string(*v);
            if (!ha) return false;
            rc.h_alignment = *ha;
            return true;
        }
        if (k == "allow_shrink") { auto v = parse_bool(p); if (!v) return false; rc.allow_shrink = *v; return true; }
        if (k == "allow_enlarge") { auto v = parse_bool(p); if (!v) return false; rc.allow_enlarge = *v; return true; }
        if (k == "output_path") { auto v = parse_string(p); if (!v) return false; rc.output_path = std::move(*v); return true; }
        if (k == "merge_tiff") { auto v = parse_bool(p); if (!v) return false; rc.merge_tiff = *v; return true; }
        if (k == "merge_pdf") { auto v = parse_bool(p); if (!v) return false; rc.merge_pdf = *v; return true; }
        if (k == "merge_tiff_name") { auto v = parse_string(p); if (!v) return false; rc.merge_tiff_name = std::move(*v); return true; }
        if (k == "merge_pdf_name") { auto v = parse_string(p); if (!v) return false; rc.merge_pdf_name = std::move(*v); return true; }
        return skip_value(p);
    });
}

[[nodiscard]] bool parse_output_config(JsonParser& p, OutputConfig& oc) {
    return parse_object(p, [&](const std::string& k) -> bool {
        if (k == "tiff_output") { auto v = parse_bool(p); if (!v) return false; oc.tiff_output = *v; return true; }
        if (k == "pdf_output") { auto v = parse_bool(p); if (!v) return false; oc.pdf_output = *v; return true; }
        if (k == "raster_format") {
            auto v = parse_string(p);
            if (!v) return false;
            auto rf = raster_format_from_string(*v);
            if (!rf) return false;
            oc.raster_format = *rf;
            return true;
        }
        if (k == "jpeg_quality") { auto v = parse_integer(p); if (!v) return false; oc.jpeg_quality = static_cast<std::int32_t>(*v); return true; }
        if (k == "new_extension") { auto v = parse_string(p); if (!v) return false; oc.new_extension = std::move(*v); return true; }
        if (k == "save_to_different_dir") { auto v = parse_bool(p); if (!v) return false; oc.save_to_different_dir = *v; return true; }
        if (k == "output_directory") { auto v = parse_string(p); if (!v) return false; oc.output_directory = std::move(*v); return true; }
        if (k == "conflict_policy") {
            auto v = parse_string(p);
            if (!v) return false;
            auto cp = conflict_policy_from_string(*v);
            if (!cp) return false;
            oc.conflict_policy = *cp;
            return true;
        }
        if (k == "path_mode") {
            auto v = parse_string(p);
            if (!v) return false;
            auto pm = path_mode_from_string(*v);
            if (!pm) return false;
            oc.path_mode = *pm;
            return true;
        }
        return skip_value(p);
    });
}

[[nodiscard]] bool parse_scan_config(JsonParser& p, ScanConfig& sc) {
    return parse_object(p, [&](const std::string& k) -> bool {
        if (k == "prefix") { auto v = parse_string(p); if (!v) return false; sc.prefix = std::move(*v); return true; }
        if (k == "start_at") { auto v = parse_integer(p); if (!v) return false; sc.start_at = static_cast<std::int32_t>(*v); return true; }
        if (k == "increment") { auto v = parse_integer(p); if (!v) return false; sc.increment = static_cast<std::int32_t>(*v); return true; }
        if (k == "extension") { auto v = parse_string(p); if (!v) return false; sc.extension = std::move(*v); return true; }
        if (k == "crop") { auto v = parse_bool(p); if (!v) return false; sc.crop = *v; return true; }
        if (k == "verso_recto") { auto v = parse_bool(p); if (!v) return false; sc.verso_recto = *v; return true; }
        if (k == "invert") { auto v = parse_bool(p); if (!v) return false; sc.invert = *v; return true; }
        if (k == "transfer_mode") { auto v = parse_integer(p); if (!v) return false; sc.transfer_mode = static_cast<std::int32_t>(*v); return true; }
        if (k == "compression") { auto v = parse_integer(p); if (!v) return false; sc.compression = static_cast<std::int32_t>(*v); return true; }
        if (k == "force_resolution") { auto v = parse_integer(p); if (!v) return false; sc.force_resolution = static_cast<std::int32_t>(*v); return true; }
        if (k == "crop_verso") return parse_edge_values(p, sc.crop_verso);
        if (k == "crop_recto") return parse_edge_values(p, sc.crop_recto);
        return skip_value(p);
    });
}

[[nodiscard]] bool parse_print_config(JsonParser& p, PrintConfig& pc) {
    return parse_object(p, [&](const std::string& k) -> bool {
        if (k == "page_use_mode") { auto v = parse_integer(p); if (!v) return false; pc.page_use_mode = static_cast<std::int32_t>(*v); return true; }
        if (k == "scale_mode") { auto v = parse_integer(p); if (!v) return false; pc.scale_mode = static_cast<std::int32_t>(*v); return true; }
        if (k == "scale_x") { auto v = parse_double(p); if (!v) return false; pc.scale_x = *v; return true; }
        if (k == "scale_y") { auto v = parse_double(p); if (!v) return false; pc.scale_y = *v; return true; }
        return skip_value(p);
    });
}

// ---------------------------------------------------------------------------
// Top-level: parse margins array (fixed size 2)
// ---------------------------------------------------------------------------

[[nodiscard]] bool parse_margins_array(JsonParser& p, std::array<MarginConfig, 2>& margins) {
    if (!p.consume('[')) return false;
    for (std::size_t i = 0; i < 2; ++i) {
        if (i > 0 && !p.consume(',')) return false;
        if (!parse_margin_config(p, margins[i])) return false;
    }
    return p.consume(']');
}

[[nodiscard]] bool parse_offsets_array(JsonParser& p, std::array<OffsetConfig, 2>& offsets) {
    if (!p.consume('[')) return false;
    for (std::size_t i = 0; i < 2; ++i) {
        if (i > 0 && !p.consume(',')) return false;
        if (!parse_offset_config(p, offsets[i])) return false;
    }
    return p.consume(']');
}

} // namespace

// ===========================================================================
// Public API
// ===========================================================================

std::string processing_profile_to_json(const ProcessingProfile& profile) {
    JsonWriter w;
    w.begin_object();

    w.kv("name", profile.name);
    w.kv("working_unit", to_string(profile.working_unit));
    w.kv("detected_dpi", profile.detected_dpi);
    w.kv("detected_width", profile.detected_width);
    w.kv("detected_height", profile.detected_height);

    w.kv("position_image", profile.position_image);
    w.kv("keep_outside_subimage", profile.keep_outside_subimage);
    w.kv("keep_original_size", profile.keep_original_size);
    w.kv("odd_even_mode", profile.odd_even_mode);
    w.kv("rotation", to_string(profile.rotation));

    write_canvas_config(w, "canvas", profile.canvas);

    w.key("margins");
    w.begin_array();
    write_margin_config(w, profile.margins[0]);
    write_margin_config(w, profile.margins[1]);
    w.end_array();

    w.key("offsets");
    w.begin_array();
    write_offset_config(w, profile.offsets[0]);
    write_offset_config(w, profile.offsets[1]);
    w.end_array();

    write_deskew_config(w, "deskew", profile.deskew);
    write_despeckle_config(w, "despeckle", profile.despeckle);
    write_edge_cleanup_config(w, "edge_cleanup", profile.edge_cleanup);
    write_hole_cleanup_config(w, "hole_cleanup", profile.hole_cleanup);
    write_subimage_config(w, "subimage", profile.subimage);
    write_blank_page_config(w, "blank_page", profile.blank_page);
    write_color_dropout_config(w, "color_dropout", profile.color_dropout);
    write_movement_limit_config(w, "movement_limit", profile.movement_limit);
    write_resize_config(w, "resize", profile.resize);
    write_output_config(w, "output", profile.output);
    write_scan_config(w, "scan", profile.scan);
    write_print_config(w, "print", profile.print);

    w.kv("page_detection_style_sheet", profile.page_detection_style_sheet);

    w.end_object();
    return w.str();
}

std::optional<ProcessingProfile> processing_profile_from_json(std::string_view json) {
    JsonParser p{json};
    ProcessingProfile profile;

    bool ok = parse_object(p, [&](const std::string& k) -> bool {
        if (k == "name") { auto v = parse_string(p); if (!v) return false; profile.name = std::move(*v); return true; }
        if (k == "working_unit") {
            auto v = parse_string(p);
            if (!v) return false;
            auto u = measurement_unit_from_string(*v);
            if (!u) return false;
            profile.working_unit = *u;
            return true;
        }
        if (k == "detected_dpi") { auto v = parse_integer(p); if (!v) return false; profile.detected_dpi = static_cast<std::int32_t>(*v); return true; }
        if (k == "detected_width") { auto v = parse_integer(p); if (!v) return false; profile.detected_width = static_cast<std::int32_t>(*v); return true; }
        if (k == "detected_height") { auto v = parse_integer(p); if (!v) return false; profile.detected_height = static_cast<std::int32_t>(*v); return true; }
        if (k == "position_image") { auto v = parse_bool(p); if (!v) return false; profile.position_image = *v; return true; }
        if (k == "keep_outside_subimage") { auto v = parse_bool(p); if (!v) return false; profile.keep_outside_subimage = *v; return true; }
        if (k == "keep_original_size") { auto v = parse_bool(p); if (!v) return false; profile.keep_original_size = *v; return true; }
        if (k == "odd_even_mode") { auto v = parse_bool(p); if (!v) return false; profile.odd_even_mode = *v; return true; }
        if (k == "rotation") {
            auto v = parse_string(p);
            if (!v) return false;
            auto r = rotation_from_string(*v);
            if (!r) return false;
            profile.rotation = *r;
            return true;
        }
        if (k == "canvas") return parse_canvas_config(p, profile.canvas);
        if (k == "margins") return parse_margins_array(p, profile.margins);
        if (k == "offsets") return parse_offsets_array(p, profile.offsets);
        if (k == "deskew") return parse_deskew_config(p, profile.deskew);
        if (k == "despeckle") return parse_despeckle_config(p, profile.despeckle);
        if (k == "edge_cleanup") return parse_edge_cleanup_config(p, profile.edge_cleanup);
        if (k == "hole_cleanup") return parse_hole_cleanup_config(p, profile.hole_cleanup);
        if (k == "subimage") return parse_subimage_config(p, profile.subimage);
        if (k == "blank_page") return parse_blank_page_config(p, profile.blank_page);
        if (k == "color_dropout") return parse_color_dropout_config(p, profile.color_dropout);
        if (k == "movement_limit") return parse_movement_limit_config(p, profile.movement_limit);
        if (k == "resize") return parse_resize_config(p, profile.resize);
        if (k == "output") return parse_output_config(p, profile.output);
        if (k == "scan") return parse_scan_config(p, profile.scan);
        if (k == "print") return parse_print_config(p, profile.print);
        if (k == "page_detection_style_sheet") { auto v = parse_string(p); if (!v) return false; profile.page_detection_style_sheet = std::move(*v); return true; }
        return skip_value(p);
    });

    if (!ok || !p.eof()) return std::nullopt;
    return profile;
}

bool write_processing_profile(const ProcessingProfile& profile, const std::filesystem::path& path) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) return false;
    file << processing_profile_to_json(profile) << '\n';
    file.flush();
    return file.good();
}

std::optional<ProcessingProfile> read_processing_profile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return std::nullopt;
    std::ostringstream buffer;
    buffer << file.rdbuf();
    if (!file.good() && !file.eof()) return std::nullopt;
    return processing_profile_from_json(buffer.str());
}

} // namespace ppp::core
