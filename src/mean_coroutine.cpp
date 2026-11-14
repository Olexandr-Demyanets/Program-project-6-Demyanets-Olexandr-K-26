#include "mean_coroutine.h"

#include <charconv>
#include <cmath>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace
{
bool hasLeadingPlus(std::string_view token)
{
    return token.size() > 1 && token[0] == '+' && token[1] != '+' && token[1] != '-';
}

ParseError classifyMalformed(std::string_view token)
{
    return token.find(',') != std::string_view::npos ? ParseError::DecimalComma
                                                     : ParseError::NotANumber;
}
}

ParseResult parseNumber(std::string_view token)
{
    if (hasLeadingPlus(token))
        token.remove_prefix(1);

    double value = 0.0;
    const char* const end = token.data() + token.size();
    const auto [stop, error] = std::from_chars(token.data(), end, value);

    if (error == std::errc::invalid_argument || stop != end)
        return classifyMalformed(token);
    if (error == std::errc::result_out_of_range)
        return ParseError::OutOfRange;
    if (!std::isfinite(value))
        return ParseError::NotFinite;
    return value;
}

std::string_view describe(ParseError error)
{
    switch (error)
    {
    case ParseError::NotANumber:
        return "не є дійсним числом";
    case ParseError::DecimalComma:
        return "кома замість крапки (пишіть 3.5)";
    case ParseError::OutOfRange:
        return "значення поза діапазоном double";
    case ParseError::NotFinite:
        return "нескінченність/NaN не допускаються";
    }
    return "невідома помилка";
}

void LastThreeWindow::push(double value)
{
    values_[next_] = value;
    next_ = (next_ + 1) % kCapacity;
    if (size_ < kCapacity)
        ++size_;
}

std::optional<double> LastThreeWindow::mean() const
{
    if (!full())
        return std::nullopt;

    constexpr double divisor = static_cast<double>(kCapacity);

    double sum = 0.0;
    for (double value : values_)
        sum += value;
    if (std::isfinite(sum))
        return sum / divisor;

    constexpr double overflowSafeScale = 4.0;

    double scaledSum = 0.0;
    for (double value : values_)
        scaledSum += value / overflowSafeScale;
    return scaledSum / divisor * overflowSafeScale;
}

std::vector<double> LastThreeWindow::contents() const
{
    std::vector<double> result;
    result.reserve(size_);
    const std::size_t oldest = (next_ + kCapacity - size_) % kCapacity;
    for (std::size_t i = 0; i < size_; ++i)
        result.push_back(values_[(oldest + i) % kCapacity]);
    return result;
}

namespace
{
constexpr char kCommentMark = '#';
constexpr std::string_view kUtf8Bom = "\xEF\xBB\xBF";

void stripComment(std::string& line)
{
    if (const std::size_t pos = line.find(kCommentMark); pos != std::string::npos)
        line.erase(pos);
}
}

TokenReader::TokenReader(std::istream& input) : input_(input) {}

std::optional<Token> TokenReader::next()
{
    std::string word;
    while (!(line_ >> word))
    {
        if (!loadNextLine())
            return std::nullopt;
    }
    return Token{std::move(word), lineNumber_};
}

bool TokenReader::loadNextLine()
{
    std::string line;
    if (!std::getline(input_, line))
    {
        if (input_.bad())
            throw std::runtime_error("не вдалося прочитати стандартний вхід");
        return false;
    }

    ++lineNumber_;
    if (lineNumber_ == 1 && line.starts_with(kUtf8Bom))
        line.erase(0, kUtf8Bom.size());
    stripComment(line);

    line_.clear();
    line_.str(std::move(line));
    return true;
}

MeanCoroutine MeanCoroutine::promise_type::get_return_object()
{
    return MeanCoroutine{Handle::from_promise(*this)};
}

std::suspend_always MeanCoroutine::promise_type::yield_value(MeanEvent event)
{
    if (const auto* pause = std::get_if<ZeroPause>(&event))
        resumeNotBefore = pause->resumeNotBefore;
    current = std::move(event);
    return {};
}

MeanCoroutine::MeanCoroutine(MeanCoroutine&& other) noexcept
    : handle_(std::exchange(other.handle_, nullptr))
{
}

MeanCoroutine& MeanCoroutine::operator=(MeanCoroutine&& other) noexcept
{
    if (this != &other)
    {
        if (handle_)
            handle_.destroy();
        handle_ = std::exchange(other.handle_, nullptr);
    }
    return *this;
}

MeanCoroutine::~MeanCoroutine()
{
    if (handle_)
        handle_.destroy();
}

ResumeResult MeanCoroutine::resume()
{
    if (done())
        return {ResumeStatus::Finished};

    promise_type& promise = handle_.promise();
    const Clock::time_point now = Clock::now();
    if (promise.resumeNotBefore && now < *promise.resumeNotBefore)
    {
        const auto remaining =
            std::chrono::ceil<std::chrono::milliseconds>(*promise.resumeNotBefore - now);
        return {ResumeStatus::Refused, remaining};
    }

    handle_.resume();
    if (promise.exception)
        std::rethrow_exception(std::exchange(promise.exception, nullptr));
    return {done() ? ResumeStatus::Finished : ResumeStatus::Produced};
}

const MeanEvent& MeanCoroutine::current() const
{
    return handle_.promise().current;
}

bool MeanCoroutine::done() const noexcept
{
    return !handle_ || handle_.done();
}

MeanCoroutine meanOfLastThree(std::istream& input)
{
    TokenReader reader(input);
    LastThreeWindow window;

    while (std::optional<Token> token = reader.next())
    {
        const ParseResult parsed = parseNumber(token->text);
        if (const auto* error = std::get_if<ParseError>(&parsed))
        {
            co_yield InputError{std::move(token->text), *error, token->line};
            continue;
        }

        const double number = std::get<double>(parsed);
        if (number == 0.0)
        {
            const Clock::time_point receivedAt = Clock::now();
            co_yield ZeroPause{window.contents(), window.mean(), token->line,
                               receivedAt, receivedAt + kResumeCooldown};
            continue;
        }

        window.push(number);
        co_yield MeanUpdate{number, window.contents(), window.mean(), token->line};
    }
}
