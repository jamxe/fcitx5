/*
 * SPDX-FileCopyrightText: 2017-2017 CSSlayer <wengxt@gmail.com>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 */

#include "log.h"
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <ostream>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>
#include <format>
#include <syncstream>
#include "macros.h"
#include "stringutils.h"

namespace fcitx {

namespace {

FCITX_DEFINE_LOG_CATEGORY(defaultCategory, "default");

using LogRule = std::pair<std::string, LogLevel>;

struct LogConfig {
    static std::ostream *defaultLogStream;
    static thread_local std::osyncstream localLogStream;
    static bool showTimeDate;
};

std::ostream *LogConfig::defaultLogStream = &std::cerr;
thread_local std::osyncstream LogConfig::localLogStream = []() {
    std::osyncstream out(*LogConfig::defaultLogStream);
    out.rdbuf()->set_emit_on_sync(true);
    return out;
}();
bool LogConfig::showTimeDate = true;

bool validateLogLevel(std::underlying_type_t<LogLevel> l) {
    return (l >= 0 &&
            l <= std::underlying_type_t<LogLevel>(LogLevel::LastLogLevel));
}

class LogRegistry {
public:
    static LogRegistry &instance() {
        static LogRegistry instance_;
        return instance_;
    }

    void registerCategory(LogCategory &category) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!categories_.contains(&category)) {
            categories_.insert(&category);
            applyRule(&category);
        }
    }

    void unregisterCategory(LogCategory &category) {
        std::lock_guard<std::mutex> lock(mutex_);
        categories_.erase(&category);
    }

    void setLogRules(const std::vector<LogRule> &rules) {
        std::lock_guard<std::mutex> lock(mutex_);

        rules_ = rules;

        for (auto *category : categories_) {
            applyRule(category);
        }
    }

    void applyRule(LogCategory *category) {
        category->resetLogLevel();
        for (auto &rule : rules_) {
            if (rule.first == "*" || rule.first == category->name()) {
                category->setLogLevel(rule.second);
            }
        }
    }

private:
    std::unordered_set<LogCategory *> categories_;
    std::vector<LogRule> rules_;
    std::mutex mutex_;
};
} // namespace

class LogCategoryPrivate {
public:
    LogCategoryPrivate(const char *name, LogLevel level)
        : name_(name), level_(level), defaultLevel_(level) {}

    std::string name_;
    LogLevel level_;
    LogLevel defaultLevel_;
};

LogCategory::LogCategory(const char *name, LogLevel level)
    : d_ptr(std::make_unique<LogCategoryPrivate>(name, level)) {
    LogRegistry::instance().registerCategory(*this);
}

LogCategory::~LogCategory() {
    LogRegistry::instance().unregisterCategory(*this);
}

bool LogCategory::checkLogLevel(LogLevel l) const {
    FCITX_D();
    return l != LogLevel::NoLog &&
           static_cast<std::underlying_type_t<LogLevel>>(l) <=
               static_cast<std::underlying_type_t<LogLevel>>(d->level_);
}

void LogCategory::resetLogLevel() {
    FCITX_D();
    d->level_ = d->defaultLevel_;
}

void LogCategory::setLogLevel(std::underlying_type_t<LogLevel> l) {
    if (validateLogLevel(l)) {
        setLogLevel(static_cast<LogLevel>(l));
    }
}

void LogCategory::setLogLevel(LogLevel l) {
    FCITX_D();
    d->level_ = l;
}

LogLevel LogCategory::logLevel() const {
    FCITX_D();
    return d->level_;
}

const std::string &LogCategory::name() const {
    FCITX_D();
    return d->name_;
}

bool LogCategory::fatalWrapper(LogLevel level) const {
    // If level if fatal and we don't write fatal log, abort right away.
    bool needLog = checkLogLevel(level);
    if (level == LogLevel::Fatal && !needLog) {
        std::abort();
    }
    return needLog;
}

bool LogCategory::fatalWrapper2(LogLevel level) {
    if (level == LogLevel::Fatal) {
        std::abort();
    }
    return false;
}

const LogCategory &Log::defaultCategory() { return fcitx::defaultCategory(); }

void Log::setLogRule(const std::string &ruleString) {
    std::vector<LogRule> parsedRules;
    auto rules = stringutils::split(ruleString, ",");
    for (const auto &rule : rules) {
        if (rule == "notimedate") {
            LogConfig::showTimeDate = false;
            continue;
        }

        auto ruleItem = stringutils::split(rule, "=");
        if (ruleItem.size() != 2) {
            continue;
        }
        auto &name = ruleItem[0];
        int level;
        if (std::from_chars(ruleItem[1].data(),
                            ruleItem[1].data() + ruleItem[1].size(), level)
                .ec == std::errc()) {
            if (validateLogLevel(level)) {
                parsedRules.emplace_back(name, static_cast<LogLevel>(level));
            }
        }
    }
    LogRegistry::instance().setLogRules(parsedRules);
}

void Log::setLogStream(std::ostream &stream) {
    LogConfig::defaultLogStream = &stream;
}

std::ostream &Log::logStream() {
    auto *buf = LogConfig::defaultLogStream->rdbuf();
    if (LogConfig::localLogStream.get_wrapped() != buf) {
        LogConfig::localLogStream = std::osyncstream(buf);
        LogConfig::localLogStream.rdbuf()->set_emit_on_sync(true);
    }
    return LogConfig::localLogStream;
}

LogMessageBuilder::LogMessageBuilder(std::ostream &out, LogLevel l,
                                     const char *filename, int lineNumber)
    : out_(out) {
    switch (l) {
    case LogLevel::Fatal:
        out_ << "F";
        break;
    case LogLevel::Debug:
        out_ << "D";
        break;
    case LogLevel::Info:
        out_ << "I";
        break;
    case LogLevel::Warn:
        out_ << "W";
        break;
    case LogLevel::Error:
        out_ << "E";
        break;
    default:
        break;
    }

    if (LogConfig::showTimeDate) {
        try {
            auto now = std::chrono::time_point_cast<std::chrono::microseconds>(
                std::chrono::system_clock::now());
#if __cpp_lib_chrono >= 201907L
            const auto *current_zone = std::chrono::current_zone();
            std::chrono::zoned_time zoned_time{current_zone, now};

            auto timeString = std::format("{:%F %T}", zoned_time);
#else
            auto timeString = std::format("{:%F %T}", now);
#endif
            out_ << timeString << " ";
        } catch (...) {
        }
    }

    out_ << filename << ":" << lineNumber << "] ";
}

LogMessageBuilder::~LogMessageBuilder() {
    out_ << '\n';
    out_.flush();
}

} // namespace fcitx
