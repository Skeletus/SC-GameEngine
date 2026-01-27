#pragma once
#include <cstdarg>

namespace sc
{
  enum class LogLevel { Info, Warn, Error, Debug };

  void vlog(LogLevel level, const char* fmt, va_list args);
  void log(LogLevel level, const char* fmt, ...);
}
