// Компілятор: g++ 15.2.0 (MSYS2 UCRT64), C++20
// Варіант 16: сопрограма, що читає дійсні числа зі стандартного входу
// і повертає середнє арифметичне трьох останніх (див. README.md).
#include "mean_coroutine.h"

#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <exception>
#include <format>
#include <iostream>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

struct DemoStats
{
    std::size_t numbers = 0;
    std::size_t zeros = 0;
    std::size_t inputErrors = 0;
    std::size_t refusedResumes = 0;
    std::optional<double> lastMean;

    void record(const MeanEvent& event);
};

class ConsoleReport
{
public:
    explicit ConsoleReport(std::ostream& out) : out_(out) {}

    void printHeader(bool interactive, std::chrono::milliseconds retryInterval) const;
    void printEvent(const MeanEvent& event) const;
    void printRefused(int attempt, std::chrono::milliseconds sinceZero,
                      std::chrono::milliseconds remaining) const;
    void printResumed(int attempt, std::chrono::milliseconds sinceZero) const;
    void printSummary(const DemoStats& stats) const;

private:
    void print(const MeanUpdate& update) const;
    void print(const ZeroPause& pause) const;
    void print(const InputError& error) const;

    std::ostream& out_;
};

class Utf8Console
{
public:
    Utf8Console();
    ~Utf8Console();

    Utf8Console(const Utf8Console&) = delete;
    Utf8Console& operator=(const Utf8Console&) = delete;

private:
    unsigned previousCodePage_ = 0;
};

bool isStdinInteractive();

namespace
{
constexpr std::size_t kRuleWidth = 80;
constexpr std::size_t kLineWidth = 5;
constexpr std::size_t kTokenWidth = 12;
constexpr std::size_t kWindowWidth = 33;
constexpr std::size_t kTableWidthBeforeMean = (kLineWidth + 3) + (kTokenWidth + 3) + (kWindowWidth + 3);

const std::string kContinuation = std::string(kLineWidth + 2, ' ') + "│   ";

std::string repeat(std::string_view piece, std::size_t count)
{
    std::string result;
    result.reserve(piece.size() * count);
    for (std::size_t i = 0; i < count; ++i)
        result += piece;
    return result;
}

bool isUtf8Continuation(char byte)
{
    return (static_cast<unsigned char>(byte) & 0xC0) == 0x80;
}

std::size_t displayWidth(std::string_view text)
{
    std::size_t width = 0;
    for (char byte : text)
        if (!isUtf8Continuation(byte))
            ++width;
    return width;
}

std::string padRight(std::string text, std::size_t width)
{
    const std::size_t current = displayWidth(text);
    if (current < width)
        text.append(width - current, ' ');
    return text;
}

std::size_t utf8SymbolLength(std::string_view text)
{
    const auto lead = static_cast<unsigned char>(text.front());
    std::size_t length = 0;
    if (lead < 0x80)
        length = 1;
    else if (lead >= 0xC2 && lead <= 0xDF)
        length = 2;
    else if (lead >= 0xE0 && lead <= 0xEF)
        length = 3;
    else if (lead >= 0xF0 && lead <= 0xF4)
        length = 4;

    if (length == 0 || length > text.size())
        return 0;
    for (std::size_t i = 1; i < length; ++i)
        if (!isUtf8Continuation(text[i]))
            return 0;
    return length;
}

std::string printableToken(std::string_view token)
{
    std::string shown;
    std::size_t symbols = 0;
    std::size_t ellipsisAt = 0;
    while (!token.empty())
    {
        if (symbols == kTokenWidth - 1)
            ellipsisAt = shown.size();
        if (symbols == kTokenWidth)
            return shown.substr(0, ellipsisAt) + "…";

        const std::size_t length = utf8SymbolLength(token);
        const auto code = static_cast<unsigned char>(token.front());
        const bool printable = length > 0 && code >= 0x20 && code != 0x7F;
        shown += printable ? token.substr(0, length) : std::string_view("?");
        token.remove_prefix(printable ? length : 1);
        ++symbols;
    }
    return shown;
}

std::string formatNumber(double value)
{
    return std::format("{:.10g}", value);
}

std::string formatWindow(const std::vector<double>& numbers)
{
    std::string text = "[";
    for (std::size_t i = 0; i < numbers.size(); ++i)
    {
        if (i > 0)
            text += ", ";
        text += formatNumber(numbers[i]);
    }
    return text + "]";
}

std::string formatMean(const std::optional<double>& mean, std::size_t count)
{
    if (mean)
        return formatNumber(*mean);
    return std::format("- ({} з {})", count, LastThreeWindow::kCapacity);
}

std::string rowStart(std::size_t line, std::string_view token)
{
    return std::format(" {:>{}} │ {} │ ", line, kLineWidth, padRight(std::string(token), kTokenWidth));
}
}

void DemoStats::record(const MeanEvent& event)
{
    if (const auto* update = std::get_if<MeanUpdate>(&event))
    {
        ++numbers;
        if (update->mean)
            lastMean = update->mean;
    }
    else if (std::holds_alternative<ZeroPause>(event))
    {
        ++zeros;
    }
    else
    {
        ++inputErrors;
    }
}

void ConsoleReport::printHeader(bool interactive, std::chrono::milliseconds retryInterval) const
{
    const std::string doubleRule = repeat("═", kRuleWidth);
    out_ << doubleRule << '\n'
         << " Сопрограма: середнє арифметичне трьох останніх ненульових чисел\n"
         << std::format(" Пауза після нуля: {} мс · повторна спроба відновлення кожні {} мс\n",
                        kResumeCooldown.count(), retryInterval.count());
    if (interactive)
    {
        out_ << " Вхід: клавіатура. Вводьте дійсні числа через пробіл або Enter.\n"
             << " 0 - зупинка; кінець вводу: Ctrl+Z, Enter (Windows) / Ctrl+D (Linux).\n";
    }
    else
    {
        out_ << " Вхід: перенаправлений файл або конвеєр.\n";
    }
    out_ << doubleRule << '\n'
         << ' ' << padRight("рядок", kLineWidth) << " │ " << padRight("число", kTokenWidth) << " │ "
         << padRight("три останні числа", kWindowWidth) << " │ середнє\n"
         << repeat("─", kLineWidth + 2) << "┼" << repeat("─", kTokenWidth + 2) << "┼"
         << repeat("─", kWindowWidth + 2) << "┼" << repeat("─", kRuleWidth - kTableWidthBeforeMean)
         << '\n';
}

void ConsoleReport::printEvent(const MeanEvent& event) const
{
    std::visit([this](const auto& concrete) { print(concrete); }, event);
}

void ConsoleReport::print(const MeanUpdate& update) const
{
    out_ << rowStart(update.line, formatNumber(update.number))
         << padRight(formatWindow(update.lastNumbers), kWindowWidth) << " │ "
         << formatMean(update.mean, update.lastNumbers.size()) << '\n';
}

void ConsoleReport::print(const ZeroPause& pause) const
{
    out_ << rowStart(pause.line, "0") << "НУЛЬ: не враховано, сопрограму зупинено; середнє "
         << formatMean(pause.mean, pause.lastNumbers.size()) << '\n';
}

void ConsoleReport::print(const InputError& error) const
{
    out_ << rowStart(error.line, printableToken(error.token)) << "ПОМИЛКА: " << describe(error.error)
         << " - пропущено\n";
}

void ConsoleReport::printRefused(int attempt, std::chrono::milliseconds sinceZero,
                                 std::chrono::milliseconds remaining) const
{
    out_ << kContinuation
         << std::format("✗ спроба {} через {:>4} мс після нуля: ВІДМОВА, ще {:>4} мс\n", attempt,
                        sinceZero.count(), remaining.count());
}

void ConsoleReport::printResumed(int attempt, std::chrono::milliseconds sinceZero) const
{
    out_ << kContinuation
         << std::format("✓ спроба {} через {:>4} мс після нуля: сопрограму відновлено\n", attempt,
                        sinceZero.count());
}

void ConsoleReport::printSummary(const DemoStats& stats) const
{
    out_ << repeat("─", kRuleWidth) << '\n'
         << std::format(" Прийнято чисел: {} · нулів (зупинок): {} · помилок у вхідних даних: {}\n",
                        stats.numbers, stats.zeros, stats.inputErrors)
         << std::format(" Відхилено передчасних спроб відновлення: {}\n", stats.refusedResumes);
    if (stats.lastMean)
        out_ << " Середнє трьох останніх чисел наприкінці: " << formatNumber(*stats.lastMean) << '\n';
    else
        out_ << " Середнє не обчислено: прийнято менше трьох ненульових чисел.\n";
    out_ << repeat("═", kRuleWidth) << '\n';
}

Utf8Console::Utf8Console()
{
#ifdef _WIN32
    previousCodePage_ = GetConsoleOutputCP();
    SetConsoleOutputCP(CP_UTF8);
#endif
}

Utf8Console::~Utf8Console()
{
    std::cout.flush();
    std::fflush(stdout);
#ifdef _WIN32
    if (previousCodePage_ != 0)
        SetConsoleOutputCP(previousCodePage_);
#endif
}

bool isStdinInteractive()
{
#ifdef _WIN32
    return _isatty(_fileno(stdin)) != 0;
#else
    return isatty(fileno(stdin)) != 0;
#endif
}

using namespace std;
using namespace std::chrono;

namespace
{
constexpr milliseconds kDefaultRetryInterval{300};
constexpr milliseconds kMinRetryInterval{10};
constexpr milliseconds kMaxRetryInterval{5000};

enum class ArgsStatus
{
    Ok,
    Help,
    Invalid,
};

struct ParsedArgs
{
    ArgsStatus status = ArgsStatus::Ok;
    milliseconds retryInterval = kDefaultRetryInterval;
    string error;
};

void printUsage(ostream& out)
{
    out << "Використання: mean3_demo [інтервал_мс]\n"
           "  інтервал_мс - як часто демо пробує відновити сопрограму після нуля,\n"
           "                ціле від "
        << kMinRetryInterval.count() << " до " << kMaxRetryInterval.count()
        << " (типово " << kDefaultRetryInterval.count()
        << ").\n"
           "Числа читаються зі стандартного входу, наприклад:\n"
           "  cmd:        mean3_demo.exe < input\\01_basic.txt\n"
           "  PowerShell: Get-Content input\\01_basic.txt | .\\mean3_demo.exe\n";
}

optional<milliseconds> parseRetryInterval(string_view text)
{
    long long value = 0;
    const char* const end = text.data() + text.size();
    const auto [stop, error] = from_chars(text.data(), end, value);
    if (error != errc{} || stop != end)
        return nullopt;
    const milliseconds interval{value};
    if (interval < kMinRetryInterval || interval > kMaxRetryInterval)
        return nullopt;
    return interval;
}

ParsedArgs parseArgs(int argc, char* argv[])
{
    if (argc > 2)
        return {ArgsStatus::Invalid, kDefaultRetryInterval, "забагато аргументів"};
    if (argc < 2)
        return {};

    const string_view argument = argv[1];
    if (argument == "-h" || argument == "--help" || argument == "/?")
        return {ArgsStatus::Help, kDefaultRetryInterval, {}};

    const optional<milliseconds> interval = parseRetryInterval(argument);
    if (!interval)
        return {ArgsStatus::Invalid, kDefaultRetryInterval,
                "інтервал має бути цілим числом від " + to_string(kMinRetryInterval.count()) +
                    " до " + to_string(kMaxRetryInterval.count()) + " мс, отримано \"" +
                    string(argument) + "\""};
    return {ArgsStatus::Ok, *interval, {}};
}

milliseconds elapsedSince(optional<Clock::time_point> start, Clock::time_point now)
{
    return start ? duration_cast<milliseconds>(now - *start) : milliseconds{0};
}

void runDemo(MeanCoroutine& coroutine, const ConsoleReport& report, DemoStats& stats,
             milliseconds retryInterval)
{
    optional<Clock::time_point> zeroReceivedAt;
    int attempt = 0;

    for (;;)
    {
        const Clock::time_point attemptAt = Clock::now();
        const ResumeResult result = coroutine.resume();
        const milliseconds sinceZero = elapsedSince(zeroReceivedAt, attemptAt);

        if (result.status == ResumeStatus::Refused)
        {
            ++attempt;
            ++stats.refusedResumes;
            report.printRefused(attempt, sinceZero, result.remaining);
            this_thread::sleep_for(retryInterval);
            continue;
        }

        if (zeroReceivedAt)
        {
            report.printResumed(attempt + 1, sinceZero);
            zeroReceivedAt.reset();
            attempt = 0;
        }

        if (result.status == ResumeStatus::Finished)
            return;

        const MeanEvent& event = coroutine.current();
        stats.record(event);
        report.printEvent(event);
        if (const auto* pause = get_if<ZeroPause>(&event))
            zeroReceivedAt = pause->receivedAt;
    }
}
}

int main(int argc, char* argv[])
{
    const Utf8Console utf8Console;

    const ParsedArgs args = parseArgs(argc, argv);
    if (args.status == ArgsStatus::Help)
    {
        printUsage(cout);
        return 0;
    }
    if (args.status == ArgsStatus::Invalid)
    {
        cerr << "Помилка: " << args.error << ".\n\n";
        printUsage(cerr);
        return 2;
    }

    const ConsoleReport report(cout);
    report.printHeader(isStdinInteractive(), args.retryInterval);

    DemoStats stats;
    try
    {
        MeanCoroutine coroutine = meanOfLastThree(cin);
        runDemo(coroutine, report, stats, args.retryInterval);
    }
    catch (const exception& e)
    {
        cout.flush();
        cerr << "\nКритична помилка: " << e.what() << '\n';
        return 1;
    }

    report.printSummary(stats);
    return 0;
}
