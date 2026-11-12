// Компілятор: g++ 15.2.0 (MSYS2 UCRT64), C++20
// Варіант 16: сопрограма, що читає дійсні числа зі стандартного входу
// і повертає середнє арифметичне трьох останніх (див. README.md).
#include "console_report.h"

#include "mean_coroutine.h"

#include <charconv>
#include <chrono>
#include <exception>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

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
           "  інтервал_мс — як часто демо пробує відновити сопрограму після нуля,\n"
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
