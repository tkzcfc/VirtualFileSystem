// This file: injectable log output. The library does not call printf directly.
#pragma once

#include "Common.h"

NS_VFS_BEGIN

enum class LogLevel
{
	VERBOSE,
	INFO,
	WARN,
	FAIL,
};

using LogFunc = std::function<void(LogLevel level, std::string_view message)>;

/**
 * Set the global log callback. An empty function turns logging off (the default).
 * Thread safety: may be called from any thread; the callback itself may run on any thread.
 */
VFS_DLL void setLogger(LogFunc func);

/**
 * Emit one log line. Does nothing when no callback is set.
 */
VFS_DLL void log(LogLevel level, std::string_view message);

NS_VFS_END
