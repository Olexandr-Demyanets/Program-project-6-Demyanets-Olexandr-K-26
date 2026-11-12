#pragma once

#include <array>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <exception>
#include <istream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

enum class ParseError
{
    NotANumber,
    DecimalComma,
    OutOfRange,
    NotFinite,
};

using ParseResult = std::variant<double, ParseError>;

ParseResult parseNumber(std::string_view token);

std::string_view describe(ParseError error);

class LastThreeWindow
{
public:
    static constexpr std::size_t kCapacity = 3;

    void push(double value);

    std::size_t size() const noexcept { return size_; }
    bool full() const noexcept { return size_ == kCapacity; }

    std::optional<double> mean() const;

    std::vector<double> contents() const;

private:
    std::array<double, kCapacity> values_{};
    std::size_t next_ = 0;
    std::size_t size_ = 0;
};

struct Token
{
    std::string text;
    std::size_t line = 0;
};

class TokenReader
{
public:
    explicit TokenReader(std::istream& input);

    std::optional<Token> next();

private:
    bool loadNextLine();

    std::istream& input_;
    std::istringstream line_;
    std::size_t lineNumber_ = 0;
};

using Clock = std::chrono::steady_clock;

inline constexpr std::chrono::milliseconds kResumeCooldown{1000};

struct MeanUpdate
{
    double number = 0.0;
    std::vector<double> lastNumbers;
    std::optional<double> mean;
    std::size_t line = 0;
};

struct ZeroPause
{
    std::vector<double> lastNumbers;
    std::optional<double> mean;
    std::size_t line = 0;
    Clock::time_point receivedAt;
    Clock::time_point resumeNotBefore;
};

struct InputError
{
    std::string token;
    ParseError error = ParseError::NotANumber;
    std::size_t line = 0;
};

using MeanEvent = std::variant<MeanUpdate, ZeroPause, InputError>;

enum class ResumeStatus
{
    Produced,
    Refused,
    Finished,
};

struct ResumeResult
{
    ResumeStatus status = ResumeStatus::Finished;
    std::chrono::milliseconds remaining{0};
};

class MeanCoroutine
{
public:
    struct promise_type
    {
        MeanEvent current;
        std::optional<Clock::time_point> resumeNotBefore;
        std::exception_ptr exception;

        MeanCoroutine get_return_object();
        std::suspend_always initial_suspend() noexcept { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; }
        std::suspend_always yield_value(MeanEvent event);
        void return_void() noexcept {}
        void unhandled_exception() noexcept { exception = std::current_exception(); }
    };

    MeanCoroutine(const MeanCoroutine&) = delete;
    MeanCoroutine& operator=(const MeanCoroutine&) = delete;
    MeanCoroutine(MeanCoroutine&& other) noexcept;
    MeanCoroutine& operator=(MeanCoroutine&& other) noexcept;
    ~MeanCoroutine();

    ResumeResult resume();

    const MeanEvent& current() const;
    bool done() const noexcept;

private:
    using Handle = std::coroutine_handle<promise_type>;

    explicit MeanCoroutine(Handle handle) noexcept : handle_(handle) {}

    Handle handle_;
};

MeanCoroutine meanOfLastThree(std::istream& input);
