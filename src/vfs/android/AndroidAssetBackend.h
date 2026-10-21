// This file: the Android APK asset backend (read-only, based on AAssetManager). Compiled only on Android.
//
// Usage:
//   // The Java side passes AssetManager down (axmol's FileUtilsAndroid::setAssetManagerFromJava does the same thing)
//   vfs::AndroidAssetBackend::setAssetManagerFromJava(env, assetManager);
//   fs.mount("/res", vfs::AndroidAssetBackend::create());          // mount all of assets/
//   fs.mount("/res", vfs::AndroidAssetBackend::create({"res"}));   // mount only assets/res/
//
// When axmol is already integrated, use the engine's AAssetManager directly:
//   vfs::AndroidAssetBackend::setAssetManager(ax::FileUtilsAndroid::getAssetManager());
#pragma once

#include "AssetBackend.h"

#if VFS_PLATFORM_ANDROID

#    include <jni.h>

struct AAssetManager;

NS_VFS_BEGIN

class VFS_DLL AndroidAssetBackend
{
public:
	/**
	 * Set the global AAssetManager (does not take ownership; the caller keeps it alive for the whole use, for example the Java side holds the AssetManager).
	 */
	static void setAssetManager(AAssetManager* manager);

	/**
	 * Set it from Java's android.content.res.AssetManager: a global reference is created internally so the Java object is not collected.
	 * A later call replaces the previous reference.
	 */
	static void setAssetManagerFromJava(JNIEnv* env, jobject assetManager);

	static AAssetManager* getAssetManager();

	/**
	 * Asset source backed by AAssetManager (thread-safe).
	 */
	static std::shared_ptr<IAssetProvider> createProvider(AAssetManager* manager);

	/**
	 * Create the asset backend.
	 * @param options options (subdirectory, manifest)
	 * @param manager when nullptr, use the global object set by setAssetManager
	 * @param err     optional error output: UNSUPPORTED when no AAssetManager is available
	 */
	static std::shared_ptr<AssetBackend> create(const AssetOptions& options = {}, AAssetManager* manager = nullptr,
												Error* err = nullptr);
};

NS_VFS_END

#endif
