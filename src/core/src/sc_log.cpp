#include "sc_log.h"
#include <cstdio>

namespace sc
{
  static const char* level_to_str(LogLevel lvl)
  {
    switch (lvl)
    {
      case LogLevel::Info:  return "INFO";
      case LogLevel::Warn:  return "WARN";
      case LogLevel::Error: return "ERROR";
      case LogLevel::Debug: return "DEBUG";
      default:              return "LOG";
    }
  }

  void vlog(LogLevel level, const char* fmt, va_list args)
  {
    std::fprintf(stdout, "[%s] ", level_to_str(level));
    std::vfprintf(stdout, fmt, args);
    std::fprintf(stdout, "\n");
    std::fflush(stdout);
  }

  void log(LogLevel level, const char* fmt, ...)
  {
    va_list args;
    va_start(args, fmt);
    vlog(level, fmt, args);
    va_end(args);
  }
}
