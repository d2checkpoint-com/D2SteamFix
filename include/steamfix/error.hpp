#pragma once

#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace steamfix {

enum class ExitCode : int {
    invalid_invocation = 10,
    log_initialization = 11,
    validation = 12,
    existing_session = 13,
    steam_discovery = 14,
    overlay_loaded = 15,
    overlay_protection = 16,
    process_creation = 17,
    internal = 19,
};

class AppError final : public std::runtime_error {
public:
    AppError(ExitCode code, std::string stage, std::string detail)
        : std::runtime_error(stage + ": " + detail), code_(code),
          context_(std::make_shared<const Context>(Context{std::move(stage), std::move(detail)})) {}
    ~AppError() noexcept override = default;
    AppError(const AppError&) noexcept = default;
    AppError& operator=(const AppError&) noexcept = default;
    AppError(AppError&& other) noexcept : AppError(static_cast<const AppError&>(other)) {}
    AppError& operator=(AppError&& other) noexcept {
        return *this = static_cast<const AppError&>(other);
    }

    [[nodiscard]] ExitCode code() const noexcept { return code_; }
    [[nodiscard]] const std::string& stage() const noexcept { return context_->stage; }
    [[nodiscard]] const std::string& detail() const noexcept { return context_->detail; }
    [[nodiscard]] bool launcher_started() const noexcept { return launcher_started_; }
    AppError& after_launch() noexcept {
        launcher_started_ = true;
        return *this;
    }

private:
    // Exception copies must not allocate, including copies made by exception_ptr.
    struct Context {
        std::string stage;
        std::string detail;
    };
    ExitCode code_;
    std::shared_ptr<const Context> context_;
    bool launcher_started_ = false;
};

} // namespace steamfix
