#include "ppp/core/job_processor.h"
#include "ppp/core/job_repository.h"
#include "ppp/core/job_serialization.h"
#include "ppp/core/job_service.h"
#include "ppp/core/processing_config.h"
#include "ppp/core/processing_config_io.h"
#include "ppp/core/scheduling_policy.h"
#include "ppp/core/scheduling_policy_io.h"

#include <array>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>
#include <ctime>

namespace {

void print_usage() {
    std::cerr << "Usage: ppp_jobctl <command> [args]\n"
                 "Commands:\n"
                 "  submit <source_path> [profile] [--priority N] [--due ISO8601]\n"
                 "                                     [--correlation id] [--tag label]\n"
                 "                                     Submit a new job\n"
                "  list [state] [--tag label] [--correlation id] [--json]\n"
                "                                     List jobs optionally filtered by state, tags, or correlation id\n"
                "  export [state] [--tag label] [--correlation id] [--output path]\n"
                "                                     Emit filtered jobs as JSON (stdout or file)\n"
                "  import <path|-> [--json]           Create jobs from an exported JSON payload\n"
                 "  fail <id> <message>               Mark a job as failed\n"
                 "  complete <id>                     Mark a job as completed\n"
                 "  retry <id> [--keep-error] [--note text]\n"
                 "                                     Resubmit a job for processing\n"
                 "  prioritize <id> <priority>        Update the priority of a job\n"
                 "  reschedule <id> <timestamp|clear> Update or clear a job due timestamp\n"
                 "  correlate <id> <value|clear>      Update or clear a job correlation id\n"
                 "  attach <id> <path> [more...]      Append attachment(s) to a job\n"
                 "  detach <id> <path> [more...]      Remove attachment(s) from a job\n"
                 "  clear-attachments <id>            Remove all attachments from a job\n"
                 "  tag <id> <label> [more...]        Add tag(s) to a job\n"
                 "  untag <id> <label> [more...]      Remove tag(s) from a job\n"
                 "  clear-tags <id>                   Remove all tags from a job\n"
                 "  report [--json]                   Summarize job counts and scheduling signals\n"
                 "  purge --state <state> --before <timestamp> [--dry-run]\n"
                 "                                     Remove stale jobs in a state\n"
                 "  rebalance [--policy file|--policy-dir dir] [--within-minutes <min> <priority>]\n"
                 "                                     [--overdue <priority>] Escalate priorities based on due windows,\n"
                 "                                     policy files, or layered policy directories\n"
                 "  profile-init <path> [name]        Create a default processing profile as JSON\n"
                 "  profile-show <path>               Display a processing profile in human-readable form\n"
                 "  profile-validate <path>           Validate a processing profile JSON file\n"
                 "  migrate [database|--sqlserver <connection>] Apply pending migrations for the configured backing store\n"
                 "  run-next [--fail msg|--cancel msg] [--resume] [--continue] [--json]\n"
                 "                                     Process the next submitted job, optionally\n"
                 "                                     resuming in-flight work first and continuing\n"
                 "                                     until the queue is empty\n\n"
                 "Set PPP_JOBCTL_SQLSERVER to an ODBC connection string (if built with SQL Server support),\n"
                 "PPP_JOBCTL_SQLITE to a database file (if built with SQLite), or PPP_JOBCTL_STORE to a\n"
                 "directory to persist jobs between runs.\n";
}

std::string escape_json(std::string_view value) {
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

std::string render_time(const ppp::core::TimePoint& tp) {
    const auto tt = ppp::core::Clock::to_time_t(tp);
    std::ostringstream oss;
    oss << std::put_time(std::gmtime(&tt), "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}

std::string render_optional_time(const std::optional<ppp::core::TimePoint>& tp) {
    if (!tp) {
        return "-";
    }
    return render_time(*tp);
}

std::optional<std::int32_t> parse_priority(std::string_view value) {
    std::int64_t parsed{};
    const auto* begin = value.data();
    const auto* end = begin + value.size();
    const auto [ptr, ec] = std::from_chars(begin, end, parsed);
    if (ec != std::errc{} || ptr != end || parsed < std::numeric_limits<std::int32_t>::min() ||
        parsed > std::numeric_limits<std::int32_t>::max()) {
        return std::nullopt;
    }
    return static_cast<std::int32_t>(parsed);
}

std::optional<std::chrono::minutes> parse_minutes(std::string_view value) {
    std::int64_t parsed{};
    const auto* begin = value.data();
    const auto* end = begin + value.size();
    const auto [ptr, ec] = std::from_chars(begin, end, parsed);
    if (ec != std::errc{} || ptr != end || parsed < 0) {
        return std::nullopt;
    }
    return std::chrono::minutes{parsed};
}

std::unique_ptr<ppp::core::JobRepository> create_repository() {
#if PPP_CORE_HAVE_SQLSERVER
    if (const char* conn = std::getenv("PPP_JOBCTL_SQLSERVER"); conn && *conn) {
        try {
            return std::make_unique<ppp::core::SqlServerJobRepository>(std::string{conn});
        } catch (const std::exception& ex) {
            throw std::runtime_error(std::string{"failed to initialize sql server job store: "} + ex.what());
        }
    }
#else
    if (const char* conn = std::getenv("PPP_JOBCTL_SQLSERVER"); conn && *conn) {
        throw std::runtime_error("PPP_JOBCTL_SQLSERVER set but SQL Server support is not available");
    }
#endif
#if PPP_CORE_HAVE_SQLITE
    if (const char* db = std::getenv("PPP_JOBCTL_SQLITE"); db && *db) {
        try {
            return std::make_unique<ppp::core::SqliteJobRepository>(std::filesystem::path{db});
        } catch (const std::exception& ex) {
            throw std::runtime_error(std::string{"failed to initialize sqlite job store: "} + ex.what());
        }
    }
#else
    if (const char* db = std::getenv("PPP_JOBCTL_SQLITE"); db && *db) {
        throw std::runtime_error("PPP_JOBCTL_SQLITE set but SQLite support is not available");
    }
#endif
    if (const char* env = std::getenv("PPP_JOBCTL_STORE"); env && *env) {
        try {
            return std::make_unique<ppp::core::FileJobRepository>(std::filesystem::path{env});
        } catch (const std::exception& ex) {
            throw std::runtime_error(std::string{"failed to initialize job store: "} + ex.what());
        }
    }
    return std::make_unique<ppp::core::InMemoryJobRepository>();
}

std::time_t to_time_t_utc(std::tm tm) {
#if defined(_WIN32)
    return _mkgmtime(&tm);
#else
    return timegm(&tm);
#endif
}

std::optional<ppp::core::TimePoint> parse_iso8601_timestamp(std::string_view value) {
    if (value.empty()) {
        return std::nullopt;
    }

    std::string text{value};
    if (!text.empty() && (text.back() == 'Z' || text.back() == 'z')) {
        text.pop_back();
    }

    std::tm tm{};
    tm.tm_isdst = -1;
    std::istringstream iss{text};
    iss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
    if (!iss) {
        return std::nullopt;
    }

    std::int64_t micros = 0;
    if (iss.peek() == '.') {
        iss.get();
        std::string fractional;
        while (true) {
            const int next = iss.peek();
            if (next == std::char_traits<char>::eof() || !std::isdigit(static_cast<unsigned char>(next))) {
                break;
            }
            fractional.push_back(static_cast<char>(iss.get()));
        }
        if (!fractional.empty()) {
            if (fractional.size() > 6) {
                fractional = fractional.substr(0, 6);
            }
            while (fractional.size() < 6) {
                fractional.push_back('0');
            }
            const auto* begin = fractional.data();
            const auto* end = begin + fractional.size();
            const auto [ptr, ec] = std::from_chars(begin, end, micros);
            if (ec != std::errc{} || ptr != end) {
                return std::nullopt;
            }
        }
    }

    iss >> std::ws;
    if (iss.peek() != std::char_traits<char>::eof()) {
        return std::nullopt;
    }

    const auto seconds = to_time_t_utc(tm);
    if (seconds == static_cast<std::time_t>(-1)) {
        return std::nullopt;
    }

    auto tp = ppp::core::Clock::from_time_t(seconds);
    tp += std::chrono::microseconds{micros};
    return tp;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        print_usage();
        return 1;
    }

    std::string command{argv[1]};

    if (command == "migrate") {
        bool explicit_sqlserver = false;
        bool explicit_sqlite = false;
        int next_index = 2;
        if (next_index < argc) {
            std::string flag{argv[next_index]};
            if (flag == "--sqlserver") {
                explicit_sqlserver = true;
                ++next_index;
            } else if (flag == "--sqlite") {
                explicit_sqlite = true;
                ++next_index;
            }
        }

        const char* sqlserver_env = std::getenv("PPP_JOBCTL_SQLSERVER");
        const bool prefer_sqlserver =
            explicit_sqlserver || (!explicit_sqlite && !explicit_sqlserver && sqlserver_env && *sqlserver_env);

        if (prefer_sqlserver) {
#if PPP_CORE_HAVE_SQLSERVER
            std::string connection_string;
            if (next_index < argc) {
                connection_string = argv[next_index];
            } else if (const char* env = std::getenv("PPP_JOBCTL_SQLSERVER"); env && *env) {
                connection_string = env;
            } else {
                std::cerr << "migrate --sqlserver requires a connection string argument or PPP_JOBCTL_SQLSERVER to be set"
                          << std::endl;
                return 1;
            }

            try {
                ppp::core::SqlServerJobRepository::initialize_schema(connection_string);
                std::cout << "SQL Server schema ready (version "
                          << ppp::core::SqlServerJobRepository::latest_schema_version() << ")" << std::endl;
                return 0;
            } catch (const std::exception& ex) {
                std::cerr << "failed to migrate SQL Server schema: " << ex.what() << std::endl;
                return 1;
            }
#else
            std::cerr << "migrate command requires PPP to be built with SQL Server support" << std::endl;
            return 1;
#endif
        }

#if PPP_CORE_HAVE_SQLITE
        std::filesystem::path database_path;
        if (next_index < argc) {
            database_path = argv[next_index];
        } else if (const char* env = std::getenv("PPP_JOBCTL_SQLITE"); env && *env) {
            database_path = env;
        } else {
            std::cerr << "migrate requires a database path argument or PPP_JOBCTL_SQLITE to be set" << std::endl;
            return 1;
        }

        try {
            ppp::core::SqliteJobRepository::initialize_schema(database_path);
            std::cout << "SQLite schema ready at " << database_path.string() << " (version "
                      << ppp::core::SqliteJobRepository::latest_schema_version() << ")" << std::endl;
            return 0;
        } catch (const std::exception& ex) {
            std::cerr << "failed to migrate SQLite schema: " << ex.what() << std::endl;
            return 1;
        }
#else
        std::cerr << "migrate command requires PPP to be built with SQLite support" << std::endl;
        return 1;
#endif
    }

    std::unique_ptr<ppp::core::JobRepository> repository;
    try {
        repository = create_repository();
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << std::endl;
        return 1;
    }

ppp::core::JobService service{*repository};

try {
    if (command == "submit") {
        if (argc < 3) {
            std::cerr << "submit requires <source_path>" << std::endl;
            return 1;
        }
        ppp::core::JobPayload payload{.source_path = argv[2]};
        int arg_index = 3;
        if (arg_index < argc && std::string_view{argv[arg_index]} != "--priority") {
            payload.profile_name = argv[arg_index++];
        }
        std::optional<std::int32_t> priority;
        std::optional<ppp::core::TimePoint> due_at;
        std::optional<std::string> correlation_id;
        for (; arg_index < argc; ++arg_index) {
            std::string arg{argv[arg_index]};
            if (arg == "--priority") {
                if (arg_index + 1 >= argc) {
                    std::cerr << "--priority requires a value" << std::endl;
                    return 1;
                }
                const auto parsed = parse_priority(argv[++arg_index]);
                if (!parsed) {
                    std::cerr << "invalid priority value" << std::endl;
                    return 1;
                }
                priority = *parsed;
                continue;
            }
            if (arg == "--due") {
                if (arg_index + 1 >= argc) {
                    std::cerr << "--due requires a timestamp" << std::endl;
                    return 1;
                }
                auto parsed_due = parse_iso8601_timestamp(argv[++arg_index]);
                if (!parsed_due) {
                    std::cerr << "invalid due timestamp; expected YYYY-MM-DDTHH:MM:SS[.ffffff][Z]" << std::endl;
                    return 1;
                }
                due_at = *parsed_due;
                continue;
            }
            if (arg == "--tag") {
                if (arg_index + 1 >= argc) {
                    std::cerr << "--tag requires a value" << std::endl;
                    return 1;
                }
                payload.tags.emplace_back(argv[++arg_index]);
                continue;
            }
            if (arg == "--correlation") {
                if (arg_index + 1 >= argc) {
                    std::cerr << "--correlation requires a value" << std::endl;
                    return 1;
                }
                correlation_id = std::string{argv[++arg_index]};
                continue;
            }
            std::cerr << "unknown submit option: " << arg << std::endl;
            return 1;
        }
        auto id = service.create_job(std::move(payload), correlation_id, priority.value_or(0), due_at);
        std::cout << "submitted job " << id << " (priority " << priority.value_or(0) << ")";
        if (due_at) {
            std::cout << " due " << render_time(*due_at);
        }
        if (correlation_id) {
            std::cout << " correlation " << *correlation_id;
        }
        std::cout << std::endl;
        return 0;
    }

    if (command == "export") {
        std::optional<ppp::core::JobState> state;
        std::vector<std::string> required_tags;
        std::optional<std::string> correlation_filter;
        std::optional<std::filesystem::path> output_path;
        for (int i = 2; i < argc; ++i) {
            std::string arg{argv[i]};
            if (arg == "--tag") {
                if (i + 1 >= argc) {
                    std::cerr << "--tag requires a value" << std::endl;
                    return 1;
                }
                required_tags.emplace_back(argv[++i]);
                continue;
            }
            if (arg == "--correlation") {
                if (i + 1 >= argc) {
                    std::cerr << "--correlation requires a value" << std::endl;
                    return 1;
                }
                correlation_filter = std::string{argv[++i]};
                continue;
            }
            if (arg == "--output") {
                if (i + 1 >= argc) {
                    std::cerr << "--output requires a path" << std::endl;
                    return 1;
                }
                output_path = std::filesystem::path{argv[++i]};
                continue;
            }
            if (!state) {
                state = ppp::core::job_state_from_string(arg);
                if (!state) {
                    std::cerr << "unknown state filter" << std::endl;
                    return 1;
                }
                continue;
            }

            std::cerr << "unknown export option: " << arg << std::endl;
            return 1;
        }

        const auto jobs = service.list_with_tags(state, std::move(required_tags), correlation_filter);
        if (output_path) {
            if (!ppp::core::write_job_records_to_json_file(jobs, *output_path)) {
                std::cerr << "failed to write export to " << output_path->string() << std::endl;
                return 1;
            }
            std::cout << "exported " << jobs.size() << " job(s) to " << output_path->string() << std::endl;
            return 0;
        }

        std::cout << ppp::core::job_records_to_json_array(jobs) << std::endl;
        return 0;
    }

    if (command == "import") {
        if (argc < 3) {
            std::cerr << "import requires a file path or '-' for stdin" << std::endl;
            return 1;
        }

        std::string source_spec{argv[2]};
        bool json_output = false;
        for (int i = 3; i < argc; ++i) {
            std::string arg{argv[i]};
            if (arg == "--json") {
                json_output = true;
                continue;
            }
            std::cerr << "unknown import option: " << arg << std::endl;
            return 1;
        }

        std::optional<std::vector<ppp::core::JobRecord>> imported_records;
        std::string buffer;
        if (source_spec == "-") {
            std::ostringstream stream;
            stream << std::cin.rdbuf();
            buffer = stream.str();
            imported_records = ppp::core::job_records_from_json_array(buffer);
        } else {
            imported_records =
                ppp::core::read_job_records_from_json_file(std::filesystem::path{source_spec});
        }

        if (!imported_records) {
            std::cerr << "failed to parse jobs from JSON input" << std::endl;
            return 1;
        }

        std::vector<std::pair<std::string, std::string>> mappings;
        mappings.reserve(imported_records->size());
        for (const auto& record : *imported_records) {
            ppp::core::JobPayload payload = record.payload;
            auto new_id = service.create_job(std::move(payload), record.correlation_id, record.priority,
                                             record.due_at);
            mappings.emplace_back(record.id, std::move(new_id));
        }

        if (json_output) {
            std::cout << '[';
            for (std::size_t i = 0; i < mappings.size(); ++i) {
                if (i != 0) {
                    std::cout << ',';
                }
                std::cout << "{\"source_id\":\"" << escape_json(mappings[i].first) << "\",";
                std::cout << "\"job_id\":\"" << escape_json(mappings[i].second) << "\"}";
            }
            std::cout << ']' << std::endl;
        } else {
            for (const auto& [source_id, new_id] : mappings) {
                std::cout << "imported " << source_id << " as " << new_id << std::endl;
            }
            std::cout << "imported " << mappings.size() << " job(s)" << std::endl;
        }

        return 0;
    }

    if (command == "list") {
        std::optional<ppp::core::JobState> state;
        bool json_output = false;
        std::vector<std::string> required_tags;
        std::optional<std::string> correlation_filter;
        for (int i = 2; i < argc; ++i) {
            std::string arg{argv[i]};
            if (arg == "--json") {
                json_output = true;
                continue;
            }
            if (arg == "--tag") {
                if (i + 1 >= argc) {
                    std::cerr << "--tag requires a value" << std::endl;
                    return 1;
                }
                required_tags.emplace_back(argv[++i]);
                continue;
            }
            if (arg == "--correlation") {
                if (i + 1 >= argc) {
                    std::cerr << "--correlation requires a value" << std::endl;
                    return 1;
                }
                correlation_filter = std::string{argv[++i]};
                continue;
            }
            if (!state) {
                state = ppp::core::job_state_from_string(arg);
                if (!state) {
                    std::cerr << "unknown state filter" << std::endl;
                    return 1;
                }
                continue;
            }

            std::cerr << "unknown list option: " << arg << std::endl;
            return 1;
        }

        const auto jobs = service.list_with_tags(state, std::move(required_tags), correlation_filter);
        if (json_output) {
            std::cout << ppp::core::job_records_to_json_array(jobs) << std::endl;
            return 0;
        }

        for (const auto& job : jobs) {
            std::cout << job.id << "\t" << ppp::core::to_string(job.state) << "\t"
                      << render_time(job.created_at) << "\t" << job.priority << "\t" << job.attempt_count
                      << "\t" << render_optional_time(job.last_attempt_at) << "\t"
                      << render_optional_time(job.due_at);
            if (job.error_message) {
                std::cout << "\t" << *job.error_message;
            }
            std::cout << std::endl;
        }
        return 0;
    }

    if (command == "fail") {
        if (argc < 4) {
            std::cerr << "fail requires <id> <message>" << std::endl;
            return 1;
        }
        service.mark_failed(argv[2], argv[3]);
        std::cout << "marked job failed" << std::endl;
        return 0;
    }

    if (command == "complete") {
        if (argc < 3) {
            std::cerr << "complete requires <id>" << std::endl;
            return 1;
        }
        service.mark_completed(argv[2]);
        std::cout << "marked job completed" << std::endl;
        return 0;
    }

    if (command == "retry") {
        if (argc < 3) {
            std::cerr << "retry requires <id>" << std::endl;
            return 1;
        }

        bool clear_error = true;
        std::optional<std::string> note;
        for (int i = 3; i < argc; ++i) {
            std::string arg{argv[i]};
            if (arg == "--keep-error") {
                clear_error = false;
                continue;
            }
            if (arg == "--note") {
                if (i + 1 >= argc) {
                    std::cerr << "--note requires text" << std::endl;
                    return 1;
                }
                note = argv[++i];
                continue;
            }
            std::cerr << "unknown retry option: " << arg << std::endl;
            return 1;
        }

        const std::string id{argv[2]};
        if (!service.retry(id, clear_error, note)) {
            std::cerr << "job not found: " << id << std::endl;
            return 1;
        }
        std::cout << "resubmitted job " << id;
        if (note) {
            std::cout << " (" << *note << ")";
        }
        std::cout << std::endl;
        return 0;
    }

    if (command == "prioritize") {
        if (argc < 4) {
            std::cerr << "prioritize requires <id> <priority>" << std::endl;
            return 1;
        }
        const std::string id{argv[2]};
        const auto parsed = parse_priority(argv[3]);
        if (!parsed) {
            std::cerr << "invalid priority value" << std::endl;
            return 1;
        }
        auto record = service.get(id);
        if (!record) {
            std::cerr << "job not found" << std::endl;
            return 1;
        }
        (void)service.update_priority(id, *parsed);
        std::cout << "updated priority of job " << id << " to " << *parsed << std::endl;
        return 0;
    }

    if (command == "reschedule") {
        if (argc < 4) {
            std::cerr << "reschedule requires <id> <timestamp|clear>" << std::endl;
            return 1;
        }
        const std::string id{argv[2]};
        std::optional<ppp::core::TimePoint> due_at;
        if (std::string_view{argv[3]} != "clear") {
            auto parsed = parse_iso8601_timestamp(argv[3]);
            if (!parsed) {
                std::cerr << "invalid due timestamp; expected YYYY-MM-DDTHH:MM:SS[.ffffff][Z] or 'clear'" << std::endl;
                return 1;
            }
            due_at = *parsed;
        }
        auto record = service.get(id);
        if (!record) {
            std::cerr << "job not found" << std::endl;
            return 1;
        }
        (void)service.update_due_at(id, due_at);
        std::cout << "updated due timestamp for job " << id;
        if (due_at) {
            std::cout << " to " << render_time(*due_at);
        } else {
            std::cout << " (cleared)";
        }
        std::cout << std::endl;
        return 0;
    }

    if (command == "correlate") {
        if (argc < 4) {
            std::cerr << "correlate requires <id> <value|clear>" << std::endl;
            return 1;
        }
        const std::string id{argv[2]};
        std::optional<std::string> correlation;
        if (std::string_view{argv[3]} != "clear") {
            correlation = std::string{argv[3]};
        }

        auto before = service.get(id);
        if (!before) {
            std::cerr << "job not found" << std::endl;
            return 1;
        }

        (void)service.update_correlation(id, correlation);

        std::cout << "updated correlation for job " << id;
        if (before->correlation_id == correlation) {
            std::cout << " (unchanged)";
        } else if (correlation) {
            std::cout << " to " << *correlation;
        } else {
            std::cout << " (cleared)";
        }
        std::cout << std::endl;
        return 0;
    }

    if (command == "attach") {
        if (argc < 4) {
            std::cerr << "attach requires <id> <path> [more...]" << std::endl;
            return 1;
        }
        const std::string id{argv[2]};
        auto before = service.get(id);
        if (!before) {
            std::cerr << "job not found" << std::endl;
            return 1;
        }
        for (int i = 3; i < argc; ++i) {
            (void)service.add_attachment(id, argv[i]);
        }
        auto after = service.get(id);
        if (!after) {
            std::cerr << "job not found after attachment update" << std::endl;
            return 1;
        }
        const auto before_size = before->payload.attachments.size();
        const auto after_size = after->payload.attachments.size();
        if (after_size == before_size) {
            std::cout << "no new attachments added to job " << id << std::endl;
        } else {
            std::cout << "added " << (after_size - before_size) << " attachment(s) to job " << id
                      << " (total: " << after_size << ")" << std::endl;
        }
        return 0;
    }

    if (command == "detach") {
        if (argc < 4) {
            std::cerr << "detach requires <id> <path> [more...]" << std::endl;
            return 1;
        }
        const std::string id{argv[2]};
        auto before = service.get(id);
        if (!before) {
            std::cerr << "job not found" << std::endl;
            return 1;
        }
        for (int i = 3; i < argc; ++i) {
            (void)service.remove_attachment(id, argv[i]);
        }
        auto after = service.get(id);
        if (!after) {
            std::cerr << "job not found after attachment update" << std::endl;
            return 1;
        }
        const auto before_size = before->payload.attachments.size();
        const auto after_size = after->payload.attachments.size();
        if (after_size == before_size) {
            std::cout << "no attachments removed from job " << id << std::endl;
        } else {
            std::cout << "removed " << (before_size - after_size) << " attachment(s) from job " << id
                      << " (total: " << after_size << ")" << std::endl;
        }
        return 0;
    }

    if (command == "clear-attachments") {
        if (argc < 3) {
            std::cerr << "clear-attachments requires <id>" << std::endl;
            return 1;
        }
        const std::string id{argv[2]};
        auto before = service.get(id);
        if (!before) {
            std::cerr << "job not found" << std::endl;
            return 1;
        }
        const auto cleared = before->payload.attachments.size();
        (void)service.clear_attachments(id);
        if (cleared == 0) {
            std::cout << "job " << id << " already has no attachments" << std::endl;
        } else {
            std::cout << "cleared " << cleared << " attachment(s) from job " << id << std::endl;
        }
        return 0;
    }

    if (command == "tag") {
        if (argc < 4) {
            std::cerr << "tag requires <id> <label> [more...]" << std::endl;
            return 1;
        }
        const std::string id{argv[2]};
        auto before = service.get(id);
        if (!before) {
            std::cerr << "job not found" << std::endl;
            return 1;
        }
        for (int i = 3; i < argc; ++i) {
            (void)service.add_tag(id, argv[i]);
        }
        auto after = service.get(id);
        if (!after) {
            std::cerr << "job not found after tag update" << std::endl;
            return 1;
        }
        const auto before_size = before->payload.tags.size();
        const auto after_size = after->payload.tags.size();
        if (after_size == before_size) {
            std::cout << "no new tags added to job " << id << std::endl;
        } else {
            std::cout << "added " << (after_size - before_size) << " tag(s) to job " << id
                      << " (total: " << after_size << ")" << std::endl;
        }
        return 0;
    }

    if (command == "untag") {
        if (argc < 4) {
            std::cerr << "untag requires <id> <label> [more...]" << std::endl;
            return 1;
        }
        const std::string id{argv[2]};
        auto before = service.get(id);
        if (!before) {
            std::cerr << "job not found" << std::endl;
            return 1;
        }
        for (int i = 3; i < argc; ++i) {
            (void)service.remove_tag(id, argv[i]);
        }
        auto after = service.get(id);
        if (!after) {
            std::cerr << "job not found after tag update" << std::endl;
            return 1;
        }
        const auto before_size = before->payload.tags.size();
        const auto after_size = after->payload.tags.size();
        if (after_size == before_size) {
            std::cout << "no tags removed from job " << id << std::endl;
        } else {
            std::cout << "removed " << (before_size - after_size) << " tag(s) from job " << id
                      << " (total: " << after_size << ")" << std::endl;
        }
        return 0;
    }

    if (command == "clear-tags") {
        if (argc < 3) {
            std::cerr << "clear-tags requires <id>" << std::endl;
            return 1;
        }
        const std::string id{argv[2]};
        auto before = service.get(id);
        if (!before) {
            std::cerr << "job not found" << std::endl;
            return 1;
        }
        const auto cleared = before->payload.tags.size();
        (void)service.clear_tags(id);
        if (cleared == 0) {
            std::cout << "job " << id << " already has no tags" << std::endl;
        } else {
            std::cout << "cleared " << cleared << " tag(s) from job " << id << std::endl;
        }
        return 0;
    }

    if (command == "rebalance") {
        std::vector<std::filesystem::path> policy_sources;
        std::vector<ppp::core::SchedulingEscalation> manual_escalations;
        std::optional<std::int32_t> manual_overdue_priority;
        std::optional<bool> manual_escalate_overdue;

        for (int i = 2; i < argc; ++i) {
            std::string arg{argv[i]};
            if (arg == "--policy") {
                if (i + 1 >= argc) {
                    std::cerr << "--policy requires a file path" << std::endl;
                    return 1;
                }
                policy_sources.push_back(std::filesystem::path{argv[++i]});
                continue;
            }
            if (arg == "--policy-dir") {
                if (i + 1 >= argc) {
                    std::cerr << "--policy-dir requires a directory path" << std::endl;
                    return 1;
                }
                policy_sources.push_back(std::filesystem::path{argv[++i]});
                continue;
            }
            if (arg == "--within-minutes") {
                if (i + 2 >= argc) {
                    std::cerr << "--within-minutes requires <minutes> <priority>" << std::endl;
                    return 1;
                }
                auto minutes = parse_minutes(argv[++i]);
                if (!minutes) {
                    std::cerr << "invalid minutes value; expected non-negative integer" << std::endl;
                    return 1;
                }
                auto priority = parse_priority(argv[++i]);
                if (!priority) {
                    std::cerr << "invalid priority value for --within-minutes" << std::endl;
                    return 1;
                }
                manual_escalations.push_back(ppp::core::SchedulingEscalation{
                    .within = std::chrono::duration_cast<std::chrono::seconds>(*minutes),
                    .priority = *priority,
                });
                continue;
            }
            if (arg == "--overdue") {
                if (i + 1 >= argc) {
                    std::cerr << "--overdue requires a priority" << std::endl;
                    return 1;
                }
                auto priority = parse_priority(argv[++i]);
                if (!priority) {
                    std::cerr << "invalid priority value for --overdue" << std::endl;
                    return 1;
                }
                manual_overdue_priority = *priority;
                continue;
            }
            if (arg == "--no-overdue-escalation") {
                manual_escalate_overdue = false;
                continue;
            }
            std::cerr << "unknown rebalance option: " << arg << std::endl;
            return 1;
        }

        ppp::core::SchedulingPolicy policy;
        bool policy_has_escalations = false;
        bool policy_overdue_specified = false;
        bool policy_escalate_specified = false;
        if (!policy_sources.empty()) {
            try {
                auto loaded = ppp::core::load_scheduling_policy_stack(policy_sources);
                policy = std::move(loaded.policy);
                policy_has_escalations = loaded.has_escalations;
                policy_overdue_specified = loaded.overdue_priority_specified;
                policy_escalate_specified = loaded.escalate_overdue_specified;
            } catch (const std::exception& ex) {
                std::cerr << ex.what() << std::endl;
                return 1;
            }
        }

        if (!manual_escalations.empty()) {
            policy.escalations.insert(policy.escalations.end(), manual_escalations.begin(), manual_escalations.end());
            policy_has_escalations = true;
        }
        if (manual_overdue_priority) {
            policy.overdue_priority = *manual_overdue_priority;
            policy_overdue_specified = true;
        }
        if (manual_escalate_overdue) {
            policy.escalate_overdue = *manual_escalate_overdue;
            policy_escalate_specified = true;
        }

        if (!policy_has_escalations && !policy_overdue_specified && !policy_escalate_specified) {
            std::cerr << "rebalance requires a policy file or at least one --within-minutes/--overdue rule" << std::endl;
            return 1;
        }

        const auto adjusted = service.apply_scheduling_policy(policy);
        std::cout << "rebalance applied; " << adjusted << " job(s) escalated" << std::endl;
        return 0;
    }

    if (command == "report") {
        bool json_output = false;
        for (int i = 2; i < argc; ++i) {
            std::string arg{argv[i]};
            if (arg == "--json") {
                json_output = true;
                continue;
            }
            std::cerr << "unknown report option: " << arg << std::endl;
            return 1;
        }

        const auto summary = service.summarize();
        if (json_output) {
            std::cout << ppp::core::job_summary_to_json(summary) << std::endl;
            return 0;
        }

        std::cout << "total jobs: " << summary.total << std::endl;
        std::cout << "  submitted: " << summary.submitted << std::endl;
        std::cout << "  validating: " << summary.validating << std::endl;
        std::cout << "  rendering: " << summary.rendering << std::endl;
        std::cout << "  exception: " << summary.exception << std::endl;
        std::cout << "  completed: " << summary.completed << std::endl;
        std::cout << "  cancelled: " << summary.cancelled << std::endl;
        std::cout << "outstanding jobs: " << summary.outstanding << std::endl;
        std::cout << "oldest created: " << render_optional_time(summary.oldest_created) << std::endl;
        std::cout << "oldest outstanding: " << render_optional_time(summary.oldest_outstanding) << std::endl;
        std::cout << "latest activity: " << render_optional_time(summary.latest_update) << std::endl;
        std::cout << "next due: " << render_optional_time(summary.next_due) << std::endl;
        return 0;
    }

    if (command == "purge") {
        std::optional<ppp::core::JobState> state;
        std::optional<ppp::core::TimePoint> before;
        bool dry_run = false;
        for (int i = 2; i < argc; ++i) {
            std::string arg{argv[i]};
            if (arg == "--state") {
                if (i + 1 >= argc) {
                    std::cerr << "--state requires a value" << std::endl;
                    return 1;
                }
                auto parsed_state = ppp::core::job_state_from_string(argv[++i]);
                if (!parsed_state) {
                    std::cerr << "unknown job state" << std::endl;
                    return 1;
                }
                state = *parsed_state;
                continue;
            }
            if (arg == "--before") {
                if (i + 1 >= argc) {
                    std::cerr << "--before requires a timestamp" << std::endl;
                    return 1;
                }
                auto parsed_time = parse_iso8601_timestamp(argv[++i]);
                if (!parsed_time) {
                    std::cerr << "invalid timestamp; expected YYYY-MM-DDTHH:MM:SS[.ffffff][Z]" << std::endl;
                    return 1;
                }
                before = *parsed_time;
                continue;
            }
            if (arg == "--dry-run") {
                dry_run = true;
                continue;
            }
            std::cerr << "unknown purge option: " << arg << std::endl;
            return 1;
        }

        if (!state) {
            std::cerr << "purge requires --state" << std::endl;
            return 1;
        }
        if (!before) {
            std::cerr << "purge requires --before" << std::endl;
            return 1;
        }

        const auto records = service.list(*state);
        std::size_t eligible = 0;
        for (const auto& job : records) {
            const auto reference = job.updated_at.value_or(job.created_at);
            if (reference < *before) {
                ++eligible;
            }
        }

        if (dry_run) {
            std::cout << eligible << " job(s) would be purged in state " << ppp::core::to_string(*state)
                      << " before " << render_time(*before) << std::endl;
            return 0;
        }

        const auto removed = service.purge(*state, *before);
        std::cout << "purged " << removed << " job(s) in state " << ppp::core::to_string(*state)
                  << " before " << render_time(*before) << std::endl;
        return 0;
    }

    if (command == "run-next") {
        std::optional<std::string> fail_message;
        std::optional<std::string> cancel_message;
        bool resume_active = false;
        bool auto_continue = false;
        bool json_output = false;
        for (int i = 2; i < argc; ++i) {
            std::string arg{argv[i]};
            if (arg == "--fail") {
                if (fail_message || cancel_message) {
                    std::cerr << "run-next accepts only one of --fail or --cancel" << std::endl;
                    return 1;
                }
                if (i + 1 >= argc) {
                    std::cerr << "--fail requires a message" << std::endl;
                    return 1;
                }
                fail_message = argv[++i];
                continue;
            }
            if (arg == "--cancel") {
                if (fail_message || cancel_message) {
                    std::cerr << "run-next accepts only one of --fail or --cancel" << std::endl;
                    return 1;
                }
                if (i + 1 >= argc) {
                    std::cerr << "--cancel requires a message" << std::endl;
                    return 1;
                }
                cancel_message = argv[++i];
                continue;
            }
            if (arg == "--resume") {
                resume_active = true;
                continue;
            }
            if (arg == "--continue") {
                auto_continue = true;
                continue;
            }
            if (arg == "--json") {
                json_output = true;
                continue;
            }
            std::cerr << "unknown run-next option: " << arg << std::endl;
            return 1;
        }

        std::size_t resumed_count = 0;
        if (resume_active) {
            resumed_count = service.resume_active_jobs();
            if (!json_output && resumed_count > 0) {
                std::cout << "resumed " << resumed_count << " active job(s)" << std::endl;
            }
        }

        ppp::core::JobProcessor processor{
            service, [fail_message, cancel_message, json_output](const ppp::core::JobRecord& job) {
                if (!json_output) {
                    std::cout << "processing job " << job.id << " from " << job.payload.source_path << std::endl;
                    std::cout << "  attempt: " << job.attempt_count << std::endl;
                    std::cout << "  priority: " << job.priority << std::endl;
                    std::cout << "  due: " << render_optional_time(job.due_at) << std::endl;
                    std::cout << "  last attempt: " << render_optional_time(job.last_attempt_at) << std::endl;
                    if (!job.payload.attachments.empty()) {
                        std::cout << "  attachments:" << std::endl;
                        for (const auto& attachment : job.payload.attachments) {
                            std::cout << "    - " << attachment << std::endl;
                        }
                    }
                    if (!job.payload.tags.empty()) {
                        std::cout << "  tags:" << std::endl;
                        for (const auto& tag : job.payload.tags) {
                            std::cout << "    - " << tag << std::endl;
                        }
                    }
                }
                if (fail_message) {
                    if (!json_output) {
                        std::cout << "  result: failed (" << *fail_message << ")" << std::endl;
                    }
                    return ppp::core::JobExecutionResult::failed(*fail_message);
                }
                if (cancel_message) {
                    if (!json_output) {
                        std::cout << "  result: cancelled (" << *cancel_message << ")" << std::endl;
                    }
                    return ppp::core::JobExecutionResult::cancelled(*cancel_message);
                }
                if (!json_output) {
                    std::cout << "  result: completed" << std::endl;
                }
                return ppp::core::JobExecutionResult::completed();
            }};

        struct ProcessedDetails {
            ppp::core::JobProcessor::ProcessedJob processed;
            std::optional<ppp::core::JobRecord> record;
        };
        std::vector<ProcessedDetails> processed_jobs;

        auto describe = [](ppp::core::JobExecutionResult::Outcome outcome) {
            switch (outcome) {
            case ppp::core::JobExecutionResult::Outcome::Completed:
                return "completed";
            case ppp::core::JobExecutionResult::Outcome::Failed:
                return "failed";
            case ppp::core::JobExecutionResult::Outcome::Cancelled:
                return "cancelled";
            }
            return "unknown";
        };

        while (true) {
            auto processed = processor.process_next();
            if (!processed) {
                break;
            }

            auto record = service.get(processed->id);
            processed_jobs.push_back(ProcessedDetails{*processed, record});

            if (!json_output) {
                const auto outcome = describe(processed->result.outcome);
                std::cout << "job " << processed->id << " transitioned to " << outcome;
                if (processed->result.message && !processed->result.message->empty()) {
                    std::cout << " (" << *processed->result.message << ")";
                }
                if (record) {
                    std::cout << " after " << record->attempt_count << " attempt(s)";
                    if (record->last_attempt_at) {
                        std::cout << " at " << render_time(*record->last_attempt_at);
                    }
                }
                std::cout << std::endl;
            }

            if (!auto_continue) {
                break;
            }
        }

        if (processed_jobs.empty()) {
            if (json_output) {
                std::cout << "{\"processed\":false";
                if (resume_active) {
                    std::cout << ",\"resumed\":" << resumed_count;
                }
                std::cout << ",\"job\":null}" << std::endl;
            } else {
                if (resume_active && resumed_count > 0) {
                    std::cout << "resumed " << resumed_count
                              << " active job(s) but none were available for processing" << std::endl;
                } else {
                    std::cout << "no submitted jobs available" << std::endl;
                }
            }
            return 0;
        }

        if (json_output) {
            if (!auto_continue) {
                const auto& entry = processed_jobs.front();
                const auto outcome = describe(entry.processed.result.outcome);
                std::cout << "{\"processed\":true,\"outcome\":\"" << outcome << "\"";
                if (resume_active) {
                    std::cout << ",\"resumed\":" << resumed_count;
                }
                if (entry.record) {
                    std::cout << ",\"job\":" << ppp::core::job_record_to_json(*entry.record);
                } else {
                    std::cout << ",\"job\":null";
                }
                std::cout << "}" << std::endl;
            } else {
                std::cout << "{\"processed\":true";
                if (resume_active) {
                    std::cout << ",\"resumed\":" << resumed_count;
                }
                std::cout << ",\"jobs\":[";
                for (std::size_t index = 0; index < processed_jobs.size(); ++index) {
                    const auto& entry = processed_jobs[index];
                    const auto outcome = describe(entry.processed.result.outcome);
                    std::cout << "{\"id\":\"" << entry.processed.id << "\",\"outcome\":\"" << outcome << "\"";
                    if (entry.record) {
                        std::cout << ",\"job\":" << ppp::core::job_record_to_json(*entry.record);
                    } else {
                        std::cout << ",\"job\":null";
                    }
                    std::cout << "}";
                    if (index + 1 < processed_jobs.size()) {
                        std::cout << ",";
                    }
                }
                std::cout << "]}" << std::endl;
            }
            return 0;
        }

        return 0;
    }

    if (command == "profile-init") {
        if (argc < 3) {
            std::cerr << "Usage: ppp_jobctl profile-init <path> [name]" << std::endl;
            return 1;
        }
        const auto path = std::filesystem::path{argv[2]};
        ppp::core::ProcessingProfile profile;
        if (argc > 3) {
            profile.name = argv[3];
        }
        if (!ppp::core::write_processing_profile(profile, path)) {
            std::cerr << "failed to write profile to " << path << std::endl;
            return 1;
        }
        std::cout << "profile written to " << path << std::endl;
        return 0;
    }

    if (command == "profile-show") {
        if (argc < 3) {
            std::cerr << "Usage: ppp_jobctl profile-show <path>" << std::endl;
            return 1;
        }
        auto profile = ppp::core::read_processing_profile(std::filesystem::path{argv[2]});
        if (!profile) {
            std::cerr << "failed to read or parse profile: " << argv[2] << std::endl;
            return 1;
        }
        const auto& p = *profile;
        std::cout << "name:              " << (p.name.empty() ? "(unnamed)" : p.name) << "\n";
        std::cout << "working_unit:      " << ppp::core::to_string(p.working_unit) << "\n";
        std::cout << "rotation:          " << ppp::core::to_string(p.rotation) << "\n";
        std::cout << "canvas:            " << ppp::core::to_string(p.canvas.preset)
                  << " " << ppp::core::to_string(p.canvas.orientation) << "\n";
        std::cout << "odd_even_mode:     " << (p.odd_even_mode ? "yes" : "no") << "\n";
        std::cout << "deskew:            " << (p.deskew.enabled ? "yes" : "no");
        if (p.deskew.enabled)
            std::cout << " (" << p.deskew.min_angle << "-" << p.deskew.max_angle << " deg)";
        std::cout << "\n";
        std::cout << "despeckle:         " << ppp::core::to_string(p.despeckle.mode) << "\n";
        std::cout << "edge_cleanup:      " << (p.edge_cleanup.enabled ? "yes" : "no") << "\n";
        std::cout << "hole_cleanup:      " << (p.hole_cleanup.enabled ? "yes" : "no") << "\n";
        std::cout << "resize:            " << (p.resize.enabled ? "yes" : "no") << "\n";
        std::cout << "output:            raster=" << ppp::core::to_string(p.output.raster_format)
                  << " tiff=" << (p.output.tiff_output ? "yes" : "no")
                  << " pdf=" << (p.output.pdf_output ? "yes" : "no") << "\n";
        std::cout << "jpeg_quality:      " << p.output.jpeg_quality << "\n";
        std::cout << "page_detection:    " << p.page_detection_style_sheet << "\n";
        return 0;
    }

    if (command == "profile-validate") {
        if (argc < 3) {
            std::cerr << "Usage: ppp_jobctl profile-validate <path>" << std::endl;
            return 1;
        }
        auto profile = ppp::core::read_processing_profile(std::filesystem::path{argv[2]});
        if (!profile) {
            std::cerr << "invalid profile: " << argv[2] << std::endl;
            return 1;
        }
        std::cout << "valid" << std::endl;
        return 0;
    }

    std::cerr << "Unknown command: " << command << std::endl;
    print_usage();
    return 1;
} catch (const std::exception& ex) {
    std::cerr << ex.what() << std::endl;
    return 1;
}
}
