// iOS host entry point: iOS kills a process that has not finished launching
// within about 20 seconds (watchdog), while a full benchmark runs for several
// minutes. This goes through the normal UIApplicationMain, then calls the real
// vfs_bench_main on a background thread after launch and exits the process when it returns.
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
	// Keep the screen awake while the benchmark runs so the app is not suspended after lock.
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
