#include "mean_coroutine.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

namespace
{
int g_checks = 0;
int g_failures = 0;

void check(bool condition, const char* expression, int line)
{
    ++g_checks;
    if (!condition)
    {
        ++g_failures;
        std::cerr << "FAIL tests.cpp:" << line << ": " << expression << '\n';
    }
}
}

#define CHECK(expression) check((expression), #expression, __LINE__)

namespace
{
using namespace std::chrono_literals;

bool parsesTo(std::string_view text, double expected)
{
    const ParseResult result = parseNumber(text);
    const auto* value = std::get_if<double>(&result);
    return value != nullptr && *value == expected;
}

bool failsWith(std::string_view text, ParseError expected)
{
    const ParseResult result = parseNumber(text);
    const auto* error = std::get_if<ParseError>(&result);
    return error != nullptr && *error == expected;
}

bool nearlyEqual(double a, double b)
{
    return std::fabs(a - b) <= 1e-12 * std::max(1.0, std::fabs(b));
}

void testParser()
{
    CHECK(parsesTo("1.5", 1.5));
    CHECK(parsesTo("+2", 2.0));
    CHECK(parsesTo("-3e2", -300.0));
    CHECK(parsesTo(".5", 0.5));
    CHECK(parsesTo("7.", 7.0));
    CHECK(parsesTo("1E-3", 0.001));
    CHECK(parsesTo("0", 0.0));
    CHECK(parsesTo("-0.0", 0.0));
    CHECK(parsesTo("0e10", 0.0));

    for (std::string_view bad : {"abc", "12kg", "1.2.3", "--5", "+-5", "++5", "+", "-", ".", "1e",
                                 "0x1A", "п'ять", ""})
        CHECK(failsWith(bad, ParseError::NotANumber));

    CHECK(failsWith("3,5", ParseError::DecimalComma));
    CHECK(failsWith("1e999", ParseError::OutOfRange));
    CHECK(failsWith("-1e999", ParseError::OutOfRange));
    CHECK(failsWith("1e-400", ParseError::OutOfRange));
    for (std::string_view bad : {"inf", "+inf", "-infinity", "nan", "NAN"})
        CHECK(failsWith(bad, ParseError::NotFinite));
}

void testWindow()
{
    LastThreeWindow window;
    CHECK(!window.mean());
    window.push(1.0);
    window.push(2.0);
    CHECK(!window.mean());
    CHECK((window.contents() == std::vector<double>{1.0, 2.0}));

    window.push(3.0);
    CHECK(window.mean() == 2.0);
    window.push(10.0);
    CHECK(window.mean() == 5.0);
    CHECK((window.contents() == std::vector<double>{2.0, 3.0, 10.0}));

    LastThreeWindow huge;
    for (int i = 0; i < 3; ++i)
        huge.push(1.7e308);
    CHECK(huge.mean() && nearlyEqual(*huge.mean(), 1.7e308));

    const double largest = std::numeric_limits<double>::max();
    LastThreeWindow extreme;
    for (int i = 0; i < 3; ++i)
        extreme.push(largest);
    CHECK(extreme.mean() == largest);
    extreme.push(-largest);
    CHECK(extreme.mean() && nearlyEqual(*extreme.mean(), largest / 3.0));
    extreme.push(-largest);
    extreme.push(-largest);
    CHECK(extreme.mean() == -largest);
}

void testTokenReader()
{
    std::istringstream input("\xEF\xBB\xBF" "1 2 # коментар 3\r\n\n  4\t5 #\n");
    TokenReader reader(input);
    std::vector<std::string> texts;
    std::vector<std::size_t> lines;
    while (auto token = reader.next())
    {
        texts.push_back(token->text);
        lines.push_back(token->line);
    }
    CHECK((texts == std::vector<std::string>{"1", "2", "4", "5"}));
    CHECK((lines == std::vector<std::size_t>{1, 1, 3, 3}));

    std::istringstream broken;
    broken.setstate(std::ios::badbit);
    TokenReader brokenReader(broken);
    bool thrown = false;
    try
    {
        brokenReader.next();
    }
    catch (const std::runtime_error&)
    {
        thrown = true;
    }
    CHECK(thrown);
}

const MeanUpdate* resumeToUpdate(MeanCoroutine& coroutine)
{
    if (coroutine.resume().status != ResumeStatus::Produced)
        return nullptr;
    return std::get_if<MeanUpdate>(&coroutine.current());
}

ResumeResult resumeWhenAllowed(MeanCoroutine& coroutine)
{
    for (;;)
    {
        const ResumeResult result = coroutine.resume();
        if (result.status != ResumeStatus::Refused)
            return result;
        std::this_thread::sleep_for(result.remaining);
    }
}

bool cooldownPassedSince(Clock::time_point zeroReceivedAt)
{
    return Clock::now() - zeroReceivedAt >= kResumeCooldown;
}

void testCoroutine()
{
    std::istringstream input("2 4 abc\n0 6 0");
    MeanCoroutine coroutine = meanOfLastThree(input);

    const MeanUpdate* update = resumeToUpdate(coroutine);
    CHECK(update && update->number == 2.0 && !update->mean);
    update = resumeToUpdate(coroutine);
    CHECK(update && update->number == 4.0 && !update->mean);

    CHECK(coroutine.resume().status == ResumeStatus::Produced);
    const auto* error = std::get_if<InputError>(&coroutine.current());
    CHECK(error && error->token == "abc" && error->error == ParseError::NotANumber);

    CHECK(coroutine.resume().status == ResumeStatus::Produced);
    const auto* pause = std::get_if<ZeroPause>(&coroutine.current());
    CHECK(pause && pause->line == 2 && !pause->mean);
    CHECK(pause && pause->resumeNotBefore - pause->receivedAt == kResumeCooldown);
    const Clock::time_point firstZeroAt = pause ? pause->receivedAt : Clock::now();

    const ResumeResult refused = coroutine.resume();
    CHECK(refused.status == ResumeStatus::Refused);
    CHECK(refused.remaining > 0ms && refused.remaining <= kResumeCooldown);
    CHECK(std::holds_alternative<ZeroPause>(coroutine.current()));
    CHECK(!coroutine.done());

    CHECK(resumeWhenAllowed(coroutine).status == ResumeStatus::Produced);
    CHECK(cooldownPassedSince(firstZeroAt));
    update = std::get_if<MeanUpdate>(&coroutine.current());
    CHECK(update && update->number == 6.0 && update->mean == 4.0);

    CHECK(coroutine.resume().status == ResumeStatus::Produced);
    pause = std::get_if<ZeroPause>(&coroutine.current());
    CHECK(pause && pause->mean == 4.0);
    const Clock::time_point secondZeroAt = pause ? pause->receivedAt : Clock::now();
    CHECK(coroutine.resume().status == ResumeStatus::Refused);
    CHECK(resumeWhenAllowed(coroutine).status == ResumeStatus::Finished);
    CHECK(cooldownPassedSince(secondZeroAt));
    CHECK(coroutine.done());
    CHECK(coroutine.resume().status == ResumeStatus::Finished);
}

void testCoroutineMoveAndErrors()
{
    std::istringstream input("1 2 3");
    MeanCoroutine original = meanOfLastThree(input);
    MeanCoroutine moved = std::move(original);
    CHECK(original.done());
    CHECK(original.resume().status == ResumeStatus::Finished);
    CHECK(resumeToUpdate(moved) != nullptr);

    std::istringstream broken;
    broken.setstate(std::ios::badbit);
    MeanCoroutine failing = meanOfLastThree(broken);
    bool thrown = false;
    try
    {
        failing.resume();
    }
    catch (const std::runtime_error&)
    {
        thrown = true;
    }
    CHECK(thrown);
    CHECK(failing.done());
}
}

int main()
{
    testParser();
    testWindow();
    testTokenReader();
    testCoroutine();
    testCoroutineMoveAndErrors();

    std::cout << "Checks: " << g_checks << ", failures: " << g_failures << '\n';
    return g_failures == 0 ? 0 : 1;
}
