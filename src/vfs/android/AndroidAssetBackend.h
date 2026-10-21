// 本文件：Android APK 资源后端（只读，基于 AAssetManager）。只在 Android 上编译。
//
// 用法：
//   // Java 层把 AssetManager 传下来（axmol 的 FileUtilsAndroid::setAssetManagerFromJava 做的是同一件事）
//   vfs::AndroidAssetBackend::setAssetManagerFromJava(env, assetManager);
//   fs.mount("/res", vfs::AndroidAssetBackend::create());          // 挂载整个 assets/
//   fs.mount("/res", vfs::AndroidAssetBackend::create({"res"}));   // 只挂载 assets/res/
//
// 已经接入 axmol 时可以直接用引擎的 AAssetManager：
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
	 * 设置全局 AAssetManager（不持有所有权；调用方保证它在使用期间有效，例如 Java 层持有 AssetManager）。
	 */
	static void setAssetManager(AAssetManager* manager);

	/**
	 * 从 Java 的 android.content.res.AssetManager 设置：内部创建全局引用，保证 Java 对象不被回收。
	 * 重复调用时替换旧的引用。
	 */
	static void setAssetManagerFromJava(JNIEnv* env, jobject assetManager);

	static AAssetManager* getAssetManager();

	/**
	 * 基于 AAssetManager 的资源来源（线程安全）。
	 */
	static std::shared_ptr<IAssetProvider> createProvider(AAssetManager* manager);

	/**
	 * 创建资源后端。
	 * @param options 选项（子目录、清单）
	 * @param manager 为 nullptr 时使用 setAssetManager 设置的全局对象
	 * @param err     可选的错误输出：没有可用的 AAssetManager 返回 UNSUPPORTED
	 */
	static std::shared_ptr<AssetBackend> create(const AssetOptions& options = {}, AAssetManager* manager = nullptr,
												Error* err = nullptr);
};

NS_VFS_END

#endif
