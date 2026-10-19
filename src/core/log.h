#pragma once

#include <format>
#include <string_view>

namespace logger {

void Init();
void Write(std::string_view level, std::string_view message);

template <class... Args>
void Info(std::format_string<Args...> fmt, Args&&... args)
{
    Write("INFO ", std::format(fmt, std::forward<Args>(args)...));
}

template <class... Args>
void Warn(std::format_string<Args...> fmt, Args&&... args)
{
    Write("WARN ", std::format(fmt, std::forward<Args>(args)...));
}

template <class... Args>
void Error(std::format_string<Args...> fmt, Args&&... args)
{
    Write("ERROR", std::format(fmt, std::forward<Args>(args)...));
}

} // namespace logger
