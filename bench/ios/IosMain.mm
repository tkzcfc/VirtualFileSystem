// iOS 宿主入口：iOS 会在约 20 秒内杀掉没有完成启动流程的进程（看门狗），
// 而完整基准要跑几分钟。这里走正常的 UIApplicationMain，启动完成后在
// 后台线程里调用真正的 vfs_bench_main，结束后直接退出进程。
#import <UIKit/UIKit.h>

#include <cstdio>
#include <cstdlib>

int vfs_bench_main(int argc, char** argv);

static int g_argc = 0;
static char** g_argv = nullptr;

@interface VfsBenchAppDelegate : UIResponder <UIApplicationDelegate>
@end

@implementation VfsBenchAppDelegate
- (BOOL)application:(UIApplication*)application didFinishLaunchingWithOptions:(NSDictionary*)launchOptions
{
	// 跑基准期间保持屏幕常亮，避免锁屏后被挂起
	application.idleTimerDisabled = YES;
	[NSThread detachNewThreadWithBlock:^{
		int rc = vfs_bench_main(g_argc, g_argv);
		fflush(stdout);
		fflush(stderr);
		std::exit(rc);
	}];
	return YES;
}
@end

int main(int argc, char** argv)
{
	g_argc = argc;
	g_argv = argv;
	@autoreleasepool
	{
		return UIApplicationMain(argc, argv, nil, NSStringFromClass([VfsBenchAppDelegate class]));
	}
}
