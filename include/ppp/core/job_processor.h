#pragma once

#include "ppp/core/job.h"
#include "ppp/core/job_service.h"

#include <functional>
#include <optional>
#include <string>
#include <utility>

namespace ppp::core {

/// Represents the outcome of running a job through a processing handler.
struct JobExecutionResult {
    enum class Outcome {
        Completed,
        Failed,
        Cancelled,
    } outcome{Outcome::Completed};

    std::optional<std::string> message{};

    [[nodiscard]] static JobExecutionResult completed() noexcept { return {Outcome::Completed, std::nullopt}; }
    [[nodiscard]] static JobExecutionResult failed(std::string message) {
        return {Outcome::Failed, std::move(message)};
    }
    [[nodiscard]] static JobExecutionResult cancelled(std::optional<std::string> reason = std::nullopt) {
        return {Outcome::Cancelled, std::move(reason)};
    }
};

/// Simple coordinator that lifts a JobService into a pull-based processing loop.
class JobProcessor {
public:
    using Handler = std::function<JobExecutionResult(const JobRecord&)>;

    JobProcessor(JobService& service, Handler handler);

    struct ProcessedJob {
        std::string id;
        JobExecutionResult result;
    };

    /// Attempts to process the next submitted job. Returns information about
    /// the processed job, or std::nullopt when no submitted jobs are available.
    [[nodiscard]] std::optional<ProcessedJob> process_next();

private:
    JobService* service_{};
    Handler handler_{};
};

} // namespace ppp::core
