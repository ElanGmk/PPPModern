#include "ppp/core/job_processor.h"

#include <exception>
#include <string_view>
#include <utility>

namespace ppp::core {

JobProcessor::JobProcessor(JobService& service, Handler handler)
    : service_(&service), handler_(std::move(handler)) {}

std::optional<JobProcessor::ProcessedJob> JobProcessor::process_next() {
    if (!service_ || !handler_) {
        return std::nullopt;
    }

    auto claimed = service_->claim_next_submitted();
    if (!claimed) {
        return std::nullopt;
    }

    const auto job_id = claimed->id;
    service_->mark_rendering(job_id);
    auto current = service_->get(job_id);
    if (!current) {
        return std::nullopt;
    }

    JobExecutionResult result{};
    try {
        result = handler_(*current);
    } catch (const std::exception& ex) {
        service_->mark_failed(job_id, ex.what());
        return ProcessedJob{job_id, JobExecutionResult::failed(ex.what())};
    } catch (...) {
        constexpr std::string_view message = "unhandled job handler exception";
        service_->mark_failed(job_id, std::string{message});
        return ProcessedJob{job_id, JobExecutionResult::failed(std::string{message})};
    }

    switch (result.outcome) {
    case JobExecutionResult::Outcome::Completed:
        service_->mark_completed(job_id);
        break;
    case JobExecutionResult::Outcome::Failed:
        if (!result.message) {
            result.message = "job failed";
        }
        service_->mark_failed(job_id, *result.message);
        break;
    case JobExecutionResult::Outcome::Cancelled:
        service_->mark_cancelled(job_id, result.message);
        break;
    }

    return ProcessedJob{job_id, std::move(result)};
}

} // namespace ppp::core
