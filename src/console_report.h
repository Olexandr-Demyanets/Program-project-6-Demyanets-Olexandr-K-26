#pragma once

#include "mean_coroutine.h"

#include <chrono>
#include <cstddef>
#include <optional>
#include <ostream>

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
