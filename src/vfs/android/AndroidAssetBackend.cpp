#include "AndroidAssetBackend.h"
#include <memory>

#if VFS_PLATFORM_ANDROID

#    include <android/asset_manager.h>
#    include <android/asset_manager_jni.h>
#    include <atomic>
#    include <mutex>

NS_VFS_BEGIN

namespace
{

std::atomic<AAssetManager*> g_manager{nullptr};
std::mutex g_javaRefMutex;
jobject g_javaRef = nullptr;   // Global reference held by setAssetManagerFromJava

class AndroidAssetFile : public IAssetFile
{
public:
	explicit AndroidAssetFile(AAsset* asset) : m_asset(asset) {}
	~AndroidAssetFile() override { AAsset_close(m_asset); }

	int read(void* buf, size_t size) override { return AAsset_read(m_asset, buf, size); }

	int64_t seek(int64_t offset, int whence) override { return AAsset_seek64(m_asset, offset, whence); }

	int64_t length() const override { return AAsset_getLength64(m_asset); }

	int openFileDescriptor(int64_t& start, int64_t& length) override
	{
		// Succeeds only when the asset is stored uncompressed (aapt's noCompress list, or an already-compressed format such as png/ogg).
		off64_t s = 0, l = 0;
		int fd = AAsset_openFileDescriptor64(m_asset, &s, &l);
		if (fd < 0)
			return -1;
		start = s;
		length = l;
		return fd;
	}

private:
	AAsset* m_asset;
};

class AndroidAssetProvider : public IAssetProvider
{
public:
	explicit AndroidAssetProvider(AAssetManager* manager) : m_manager(manager) {}

	std::unique_ptr<IAssetFile> open(std::string_view path) override
	{
		std::string p(path);
		// RANDOM: hint that the system should prefetch for random access, which suits offset reads.
		AAsset* a = AAssetManager_open(m_manager, p.c_str(), AASSET_MODE_RANDOM);
		return a ? std::make_unique<AndroidAssetFile>(a) : nullptr;
	}

	bool listFiles(std::string_view dir, std::vector<std::string>& names) override
	{
		std::string d(dir);
		std::unique_ptr<AAssetDir, void (*)(AAssetDir*)> ad(AAssetManager_openDir(m_manager, d.c_str()), AAssetDir_close);
		if (!ad)
			return false;
		while (const char* name = AAssetDir_getNextFileName(ad.get()))
			names.emplace_back(name);
		return !names.empty();
	}

private:
	AAssetManager* m_manager;
};

}   // namespace

void AndroidAssetBackend::setAssetManager(AAssetManager* manager)
{
	// A Java ref from setAssetManagerFromJava is kept on purpose: mounts created earlier still hold its
	// AAssetManager*. Call setAssetManagerFromJava(env, nullptr) to release it.
	g_manager.store(manager, std::memory_order_release);
}

void AndroidAssetBackend::setAssetManagerFromJava(JNIEnv* env, jobject assetManager)
{
	std::lock_guard<std::mutex> lock(g_javaRefMutex);
	jobject ref = assetManager ? env->NewGlobalRef(assetManager) : nullptr;
	if (assetManager && !ref)
		return;   // Out of JNI references: keep the current manager
	g_manager.store(ref ? AAssetManager_fromJava(env, ref) : nullptr, std::memory_order_release);
	if (g_javaRef)
		env->DeleteGlobalRef(g_javaRef);
	g_javaRef = ref;
}

AAssetManager* AndroidAssetBackend::getAssetManager()
{
	return g_manager.load(std::memory_order_acquire);
}

std::shared_ptr<IAssetProvider> AndroidAssetBackend::createProvider(AAssetManager* manager)
{
	return manager ? std::make_shared<AndroidAssetProvider>(manager) : nullptr;
}

std::shared_ptr<AssetBackend> AndroidAssetBackend::create(const AssetOptions& options, AAssetManager* manager, Error* err)
{
	if (!manager)
		manager = getAssetManager();
	if (!manager)
	{
		if (err)
			*err = Error::UNSUPPORTED;
		return nullptr;
	}
	return AssetBackend::create(createProvider(manager), options, err);
}

NS_VFS_END

#endif
