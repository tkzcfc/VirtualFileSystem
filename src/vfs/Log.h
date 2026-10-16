// 本文件：可注入的日志输出。库内不直接 printf。
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
 * 设置全局日志回调。传空函数表示关闭日志（默认关闭）。
 * 线程安全：可以在任何线程调用；回调本身可能在任意线程被调用。
 */
VFS_DLL void setLogger(LogFunc func);

/**
 * 输出一条日志。没有设置回调时什么也不做。
 */
VFS_DLL void log(LogLevel level, std::string_view message);

NS_VFS_END
