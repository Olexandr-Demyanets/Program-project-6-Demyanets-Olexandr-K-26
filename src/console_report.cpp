#include "console_report.h"

#include <cstdio>
#include <format>
#include <iostream>
#include <string>
#include <string_view>
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

std::string printableToken(std::string_view token)
{
    const bool tooLong = displayWidth(token) > kTokenWidth;
    std::string shown;
    std::size_t symbols = 0;
    for (char byte : token)
    {
        if (!isUtf8Continuation(byte))
        {
            if (tooLong && symbols == kTokenWidth - 1)
                return shown + "…";
            ++symbols;
        }
        const auto code = static_cast<unsigned char>(byte);
        shown += (code < 0x20 || code == 0x7F) ? '?' : byte;
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
    return std::format("— ({} з {})", count, LastThreeWindow::kCapacity);
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
             << " 0 — зупинка; кінець вводу: Ctrl+Z, Enter (Windows) / Ctrl+D (Linux).\n";
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
         << " — пропущено\n";
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
