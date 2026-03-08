#include "ppp/core/job_serialization.h"

#include "ppp/core/job.h"
#include "ppp/core/job_service.h"

#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace ppp::core {

namespace {

[[nodiscard]] std::string escape_json(std::string_view value) {
    static constexpr std::array<char, 16> hex_digits{
        '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'};
    std::string escaped;
    escaped.reserve(value.size() + 16);
    for (unsigned char ch : value) {
        switch (ch) {
        case '"':
            escaped += "\\\"";
            break;
        case '\\':
            escaped += "\\\\";
            break;
        case '\b':
            escaped += "\\b";
            break;
        case '\f':
            escaped += "\\f";
            break;
        case '\n':
            escaped += "\\n";
            break;
        case '\r':
            escaped += "\\r";
            break;
        case '\t':
            escaped += "\\t";
            break;
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

[[nodiscard]] std::tm to_utc_tm(std::time_t time) {
    std::tm result{};
#if defined(_WIN32)
    gmtime_s(&result, &time);
#else
    gmtime_r(&time, &result);
#endif
    return result;
}

[[nodiscard]] std::string format_time(TimePoint tp) {
    const auto seconds = std::chrono::time_point_cast<std::chrono::seconds>(tp);
    const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(tp - seconds).count();
    const auto time = Clock::to_time_t(seconds);
    auto tm = to_utc_tm(time);

    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S");
    oss << '.' << std::setw(6) << std::setfill('0') << micros << 'Z';
    return oss.str();
}

[[nodiscard]] std::string optional_string_to_json(const std::optional<std::string>& value) {
    if (!value) {
        return "null";
    }
    std::string wrapped = "\"";
    wrapped += escape_json(*value);
    wrapped += "\"";
    return wrapped;
}

[[nodiscard]] std::string optional_time_to_json(const std::optional<TimePoint>& value) {
    if (!value) {
        return "null";
    }
    std::string wrapped = "\"";
    wrapped += format_time(*value);
    wrapped += "\"";
    return wrapped;
}

std::time_t to_time_t_utc(std::tm tm) {
#if defined(_WIN32)
    return _mkgmtime(&tm);
#else
    return timegm(&tm);
#endif
}

[[nodiscard]] std::optional<TimePoint> parse_time(std::string_view text) {
    if (text.empty()) {
        return std::nullopt;
    }

    if (text.back() != 'Z' && text.back() != 'z') {
        return std::nullopt;
    }

    const auto core = text.substr(0, text.size() - 1);
    if (core.size() < 19) {
        return std::nullopt;
    }

    std::tm tm{};
    tm.tm_isdst = -1;
    std::istringstream iss{std::string{core.substr(0, 19)}};
    iss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
    if (!iss) {
        return std::nullopt;
    }

    std::int64_t micros = 0;
    if (core.size() > 19) {
        if (core[19] != '.') {
            return std::nullopt;
        }
        const auto fractional = core.substr(20);
        if (fractional.empty()) {
            return std::nullopt;
        }
        std::string fraction{fractional};
        if (fraction.size() > 6) {
            fraction.resize(6);
        }
        while (fraction.size() < 6) {
            fraction.push_back('0');
        }
        const auto* begin = fraction.data();
        const auto* end = begin + fraction.size();
        const auto [ptr, ec] = std::from_chars(begin, end, micros);
        if (ec != std::errc{} || ptr != end) {
            return std::nullopt;
        }
    }

    const auto seconds = to_time_t_utc(tm);
    if (seconds == static_cast<std::time_t>(-1)) {
        return std::nullopt;
    }

    auto tp = Clock::from_time_t(seconds);
    tp += std::chrono::microseconds{micros};
    return tp;
}

struct JsonParser {
    std::string_view input;
    std::size_t pos{0};

    void skip_ws() {
        while (pos < input.size() && std::isspace(static_cast<unsigned char>(input[pos]))) {
            ++pos;
        }
    }

    bool consume(char ch) {
        skip_ws();
        if (pos < input.size() && input[pos] == ch) {
            ++pos;
            return true;
        }
        return false;
    }

    bool consume_literal(std::string_view literal) {
        skip_ws();
        if (input.substr(pos, literal.size()) == literal) {
            pos += literal.size();
            return true;
        }
        return false;
    }

    char peek() {
        skip_ws();
        if (pos >= input.size()) {
            return '\0';
        }
        return input[pos];
    }

    bool eof() {
        skip_ws();
        return pos >= input.size();
    }
};

[[nodiscard]] std::optional<char> decode_hex_digit(char ch) {
    if (ch >= '0' && ch <= '9') {
        return static_cast<char>(ch - '0');
    }
    if (ch >= 'A' && ch <= 'F') {
        return static_cast<char>(10 + (ch - 'A'));
    }
    if (ch >= 'a' && ch <= 'f') {
        return static_cast<char>(10 + (ch - 'a'));
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<std::string> parse_string(JsonParser& parser) {
    parser.skip_ws();
    if (parser.pos >= parser.input.size() || parser.input[parser.pos] != '"') {
        return std::nullopt;
    }
    ++parser.pos;
    std::string value;
    while (parser.pos < parser.input.size()) {
        const char ch = parser.input[parser.pos++];
        if (ch == '"') {
            return value;
        }
        if (static_cast<unsigned char>(ch) < 0x20) {
            return std::nullopt;
        }
        if (ch == '\\') {
            if (parser.pos >= parser.input.size()) {
                return std::nullopt;
            }
            const char esc = parser.input[parser.pos++];
            switch (esc) {
            case '"':
                value.push_back('"');
                break;
            case '\\':
                value.push_back('\\');
                break;
            case '/':
                value.push_back('/');
                break;
            case 'b':
                value.push_back('\b');
                break;
            case 'f':
                value.push_back('\f');
                break;
            case 'n':
                value.push_back('\n');
                break;
            case 'r':
                value.push_back('\r');
                break;
            case 't':
                value.push_back('\t');
                break;
            case 'u': {
                if (parser.pos + 4 > parser.input.size()) {
                    return std::nullopt;
                }
                int code_point = 0;
                for (int i = 0; i < 4; ++i) {
                    const auto digit = decode_hex_digit(parser.input[parser.pos++]);
                    if (!digit) {
                        return std::nullopt;
                    }
                    code_point = (code_point << 4) | *digit;
                }
                if (code_point <= 0x7F) {
                    value.push_back(static_cast<char>(code_point));
                } else if (code_point <= 0x7FF) {
                    value.push_back(static_cast<char>(0xC0 | ((code_point >> 6) & 0x1F)));
                    value.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
                } else {
                    value.push_back(static_cast<char>(0xE0 | ((code_point >> 12) & 0x0F)));
                    value.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
                    value.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
                }
                break;
            }
            default:
                return std::nullopt;
            }
        } else {
            value.push_back(ch);
        }
    }
    return std::nullopt;
}

[[nodiscard]] bool skip_value(JsonParser& parser);

[[nodiscard]] std::optional<std::int64_t> parse_integer(JsonParser& parser) {
    parser.skip_ws();
    const std::size_t begin = parser.pos;
    if (begin >= parser.input.size()) {
        return std::nullopt;
    }
    if (parser.input[parser.pos] == '-') {
        ++parser.pos;
    }
    bool has_digits = false;
    while (parser.pos < parser.input.size() && std::isdigit(static_cast<unsigned char>(parser.input[parser.pos]))) {
        ++parser.pos;
        has_digits = true;
    }
    if (!has_digits) {
        return std::nullopt;
    }
    const auto number = parser.input.substr(begin, parser.pos - begin);
    std::int64_t value{};
    const auto [ptr, ec] = std::from_chars(number.data(), number.data() + number.size(), value);
    if (ec != std::errc{} || ptr != number.data() + number.size()) {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] bool skip_array(JsonParser& parser) {
    if (!parser.consume('[')) {
        return false;
    }
    if (parser.consume(']')) {
        return true;
    }
    while (true) {
        if (!skip_value(parser)) {
            return false;
        }
        if (parser.consume(']')) {
            return true;
        }
        if (!parser.consume(',')) {
            return false;
        }
    }
}

[[nodiscard]] bool skip_object(JsonParser& parser) {
    if (!parser.consume('{')) {
        return false;
    }
    if (parser.consume('}')) {
        return true;
    }
    while (true) {
        if (!parse_string(parser)) {
            return false;
        }
        if (!parser.consume(':')) {
            return false;
        }
        if (!skip_value(parser)) {
            return false;
        }
        if (parser.consume('}')) {
            return true;
        }
        if (!parser.consume(',')) {
            return false;
        }
    }
}

[[nodiscard]] bool skip_value(JsonParser& parser) {
    parser.skip_ws();
    if (parser.pos >= parser.input.size()) {
        return false;
    }
    const char ch = parser.input[parser.pos];
    if (ch == '"') {
        return parse_string(parser).has_value();
    }
    if (ch == '{') {
        return skip_object(parser);
    }
    if (ch == '[') {
        return skip_array(parser);
    }
    if (ch == 't') {
        return parser.consume_literal("true");
    }
    if (ch == 'f') {
        return parser.consume_literal("false");
    }
    if (ch == 'n') {
        return parser.consume_literal("null");
    }
    if (ch == '-' || std::isdigit(static_cast<unsigned char>(ch))) {
        return parse_integer(parser).has_value();
    }
    return false;
}

[[nodiscard]] std::optional<std::vector<std::string>> parse_string_array(JsonParser& parser) {
    if (!parser.consume('[')) {
        return std::nullopt;
    }
    std::vector<std::string> values;
    if (parser.consume(']')) {
        return values;
    }
    while (true) {
        auto value = parse_string(parser);
        if (!value) {
            return std::nullopt;
        }
        values.push_back(std::move(*value));
        if (parser.consume(']')) {
            return values;
        }
        if (!parser.consume(',')) {
            return std::nullopt;
        }
    }
}

[[nodiscard]] std::optional<std::optional<std::string>> parse_optional_string(JsonParser& parser) {
    if (parser.consume_literal("null")) {
        return std::optional<std::string>{std::nullopt};
    }
    auto value = parse_string(parser);
    if (!value) {
        return std::nullopt;
    }
    return std::optional<std::string>{std::move(*value)};
}

[[nodiscard]] std::optional<std::optional<TimePoint>> parse_optional_time(JsonParser& parser) {
    if (parser.consume_literal("null")) {
        return std::optional<TimePoint>{std::nullopt};
    }
    auto value = parse_string(parser);
    if (!value) {
        return std::nullopt;
    }
    auto tp = parse_time(*value);
    if (!tp) {
        return std::nullopt;
    }
    return std::optional<TimePoint>{*tp};
}

[[nodiscard]] std::optional<JobPayload> parse_payload(JsonParser& parser) {
    if (!parser.consume('{')) {
        return std::nullopt;
    }
    JobPayload payload;
    bool expect_member = false;
    while (true) {
        if (parser.consume('}')) {
            break;
        }
        if (expect_member) {
            if (!parser.consume(',')) {
                return std::nullopt;
            }
        }
        expect_member = true;
        auto key = parse_string(parser);
        if (!key) {
            return std::nullopt;
        }
        if (!parser.consume(':')) {
            return std::nullopt;
        }
        if (*key == "source_path") {
            auto value = parse_string(parser);
            if (!value) {
                return std::nullopt;
            }
            payload.source_path = std::move(*value);
        } else if (*key == "profile_name") {
            auto value = parse_optional_string(parser);
            if (!value) {
                return std::nullopt;
            }
            payload.profile_name = std::move(*value);
        } else if (*key == "attachments") {
            auto array = parse_string_array(parser);
            if (!array) {
                return std::nullopt;
            }
            payload.attachments = std::move(*array);
        } else if (*key == "tags") {
            auto array = parse_string_array(parser);
            if (!array) {
                return std::nullopt;
            }
            payload.tags = std::move(*array);
        } else {
            if (!skip_value(parser)) {
                return std::nullopt;
            }
        }
    }
    if (payload.source_path.empty()) {
        return std::nullopt;
    }
    return payload;
}

[[nodiscard]] std::optional<JobRecord> parse_job_record_internal(JsonParser& parser) {
    if (!parser.consume('{')) {
        return std::nullopt;
    }
    JobRecord record;
    bool expect_member = false;
    while (true) {
        if (parser.consume('}')) {
            break;
        }
        if (expect_member) {
            if (!parser.consume(',')) {
                return std::nullopt;
            }
        }
        expect_member = true;
        auto key = parse_string(parser);
        if (!key) {
            return std::nullopt;
        }
        if (!parser.consume(':')) {
            return std::nullopt;
        }
        if (*key == "id") {
            auto value = parse_string(parser);
            if (!value) {
                return std::nullopt;
            }
            record.id = std::move(*value);
        } else if (*key == "state") {
            auto value = parse_string(parser);
            if (!value) {
                return std::nullopt;
            }
            auto state = job_state_from_string(*value);
            if (!state) {
                return std::nullopt;
            }
            record.state = *state;
        } else if (*key == "created_at") {
            auto value = parse_string(parser);
            if (!value) {
                return std::nullopt;
            }
            auto tp = parse_time(*value);
            if (!tp) {
                return std::nullopt;
            }
            record.created_at = *tp;
        } else if (*key == "updated_at") {
            auto parsed = parse_optional_time(parser);
            if (!parsed) {
                return std::nullopt;
            }
            record.updated_at = std::move(*parsed);
        } else if (*key == "correlation_id") {
            auto parsed = parse_optional_string(parser);
            if (!parsed) {
                return std::nullopt;
            }
            record.correlation_id = std::move(*parsed);
        } else if (*key == "error_message") {
            auto parsed = parse_optional_string(parser);
            if (!parsed) {
                return std::nullopt;
            }
            record.error_message = std::move(*parsed);
        } else if (*key == "priority") {
            auto parsed = parse_integer(parser);
            if (!parsed) {
                return std::nullopt;
            }
            if (*parsed < std::numeric_limits<std::int32_t>::min() ||
                *parsed > std::numeric_limits<std::int32_t>::max()) {
                return std::nullopt;
            }
            record.priority = static_cast<std::int32_t>(*parsed);
        } else if (*key == "attempt_count") {
            auto parsed = parse_integer(parser);
            if (!parsed || *parsed < 0) {
                return std::nullopt;
            }
            if (*parsed > std::numeric_limits<std::uint32_t>::max()) {
                return std::nullopt;
            }
            record.attempt_count = static_cast<std::uint32_t>(*parsed);
        } else if (*key == "last_attempt_at") {
            auto parsed = parse_optional_time(parser);
            if (!parsed) {
                return std::nullopt;
            }
            record.last_attempt_at = std::move(*parsed);
        } else if (*key == "due_at") {
            auto parsed = parse_optional_time(parser);
            if (!parsed) {
                return std::nullopt;
            }
            record.due_at = std::move(*parsed);
        } else if (*key == "payload") {
            auto payload = parse_payload(parser);
            if (!payload) {
                return std::nullopt;
            }
            record.payload = std::move(*payload);
        } else {
            if (!skip_value(parser)) {
                return std::nullopt;
            }
        }
    }
    if (record.payload.source_path.empty()) {
        return std::nullopt;
    }
    return record;
}

} // namespace

std::string job_record_to_json(const JobRecord& record) {
    std::ostringstream oss;
    oss << '{';
    oss << "\"id\":\"" << escape_json(record.id) << "\",";
    oss << "\"state\":\"" << to_string(record.state) << "\",";
    oss << "\"created_at\":\"" << format_time(record.created_at) << "\",";
    oss << "\"updated_at\":" << optional_time_to_json(record.updated_at) << ',';
    oss << "\"correlation_id\":" << optional_string_to_json(record.correlation_id) << ',';
    oss << "\"error_message\":" << optional_string_to_json(record.error_message) << ',';
    oss << "\"priority\":" << record.priority << ',';
    oss << "\"attempt_count\":" << record.attempt_count << ',';
    oss << "\"last_attempt_at\":" << optional_time_to_json(record.last_attempt_at) << ',';
    oss << "\"due_at\":" << optional_time_to_json(record.due_at) << ',';
    oss << "\"payload\":{";
    oss << "\"source_path\":\"" << escape_json(record.payload.source_path) << "\",";
    oss << "\"profile_name\":" << optional_string_to_json(record.payload.profile_name) << ',';
    oss << "\"attachments\":[";
    for (std::size_t i = 0; i < record.payload.attachments.size(); ++i) {
        if (i != 0) {
            oss << ',';
        }
        oss << "\"" << escape_json(record.payload.attachments[i]) << "\"";
    }
    oss << "],";
    oss << "\"tags\":[";
    for (std::size_t i = 0; i < record.payload.tags.size(); ++i) {
        if (i != 0) {
            oss << ',';
        }
        oss << "\"" << escape_json(record.payload.tags[i]) << "\"";
    }
    oss << "]}";
    oss << '}';
    return oss.str();
}

std::string job_records_to_json_array(const std::vector<JobRecord>& records) {
    std::ostringstream oss;
    oss << '[';
    for (std::size_t i = 0; i < records.size(); ++i) {
        if (i != 0) {
            oss << ',';
        }
        oss << job_record_to_json(records[i]);
    }
    oss << ']';
    return oss.str();
}

std::string job_summary_to_json(const JobSummary& summary) {
    std::ostringstream oss;
    oss << '{';
    oss << "\"total\":" << summary.total << ',';
    oss << "\"submitted\":" << summary.submitted << ',';
    oss << "\"validating\":" << summary.validating << ',';
    oss << "\"rendering\":" << summary.rendering << ',';
    oss << "\"exception\":" << summary.exception << ',';
    oss << "\"completed\":" << summary.completed << ',';
    oss << "\"cancelled\":" << summary.cancelled << ',';
    oss << "\"outstanding\":" << summary.outstanding << ',';
    oss << "\"oldest_created\":" << optional_time_to_json(summary.oldest_created) << ',';
    oss << "\"oldest_outstanding\":" << optional_time_to_json(summary.oldest_outstanding) << ',';
    oss << "\"latest_update\":" << optional_time_to_json(summary.latest_update) << ',';
    oss << "\"next_due\":" << optional_time_to_json(summary.next_due);
    oss << '}';
    return oss.str();
}

bool write_job_records_to_json_file(const std::vector<JobRecord>& records, const std::filesystem::path& path) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        return false;
    }

    file << job_records_to_json_array(records) << '\n';
    file.flush();
    return file.good();
}

std::optional<JobRecord> job_record_from_json(std::string_view json) {
    JsonParser parser{json};
    auto record = parse_job_record_internal(parser);
    if (!record) {
        return std::nullopt;
    }
    if (!parser.eof()) {
        return std::nullopt;
    }
    return record;
}

std::optional<std::vector<JobRecord>> job_records_from_json_array(std::string_view json) {
    JsonParser parser{json};
    if (!parser.consume('[')) {
        return std::nullopt;
    }
    std::vector<JobRecord> records;
    if (parser.consume(']')) {
        return records;
    }
    while (true) {
        auto record = parse_job_record_internal(parser);
        if (!record) {
            return std::nullopt;
        }
        records.push_back(std::move(*record));
        if (parser.consume(']')) {
            break;
        }
        if (!parser.consume(',')) {
            return std::nullopt;
        }
    }
    if (!parser.eof()) {
        return std::nullopt;
    }
    return records;
}

std::optional<std::vector<JobRecord>> read_job_records_from_json_file(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return std::nullopt;
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    if (!file.good() && !file.eof()) {
        return std::nullopt;
    }
    return job_records_from_json_array(buffer.str());
}

} // namespace ppp::core

