#include "Log.h"
#include <mutex>

NS_VFS_BEGIN

namespace
{
std::mutex s_logMutex;
std::shared_ptr<LogFunc> s_logFunc;
}

void setLogger(LogFunc func)
{
	std::shared_ptr<LogFunc> ptr = func ? std::make_shared<LogFunc>(std::move(func)) : nullptr;
	std::lock_guard<std::mutex> lock(s_logMutex);
	s_logFunc = std::move(ptr);
}

void log(LogLevel level, std::string_view message)
{
	// Copy the pointer only while holding the lock; run the callback outside the lock so logging from inside the callback cannot deadlock
	std::shared_ptr<LogFunc> func;
	{
		std::lock_guard<std::mutex> lock(s_logMutex);
		func = s_logFunc;
	}
	if (func)
		(*func)(level, message);
}

NS_VFS_END
