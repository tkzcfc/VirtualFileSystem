// 本文件：项目内自带的 ChaCha20 流密码（RFC 8439），只用于加密 .vfs 索引区。
// 不引入第三方密码库，正确性由 tests/crypto 中的 RFC 8439 测试向量保证。
#pragma once

#include "../Common.h"

NS_VFS_BEGIN

/**
 * ChaCha20（RFC 8439：32 字节 key、12 字节 nonce、32 位块计数器）。
 * 对象有状态：连续调用 process 等价于对拼接后的数据一次性处理。
 * 线程安全：单个对象不能跨线程使用。析构时清零内部状态。
 */
class VFS_DLL ChaCha20
{
public:
	static constexpr size_t KEY_SIZE = 32;
	static constexpr size_t NONCE_SIZE = 12;
	static constexpr size_t BLOCK_SIZE = 64;

	/**
	 * @param key     32 字节密钥
	 * @param nonce   12 字节 nonce
	 * @param counter 初始块计数器（RFC 8439 加密时通常为 1）
	 */
	ChaCha20(const uint8_t* key, const uint8_t* nonce, uint32_t counter);

	~ChaCha20();

	ChaCha20(const ChaCha20&) = delete;

	ChaCha20& operator=(const ChaCha20&) = delete;

	/**
	 * 原地加密/解密（流密码，两者相同）。
	 */
	void process(uint8_t* data, size_t len);

	/**
	 * 计算单个密钥流块（RFC 8439 §2.3 的 ChaCha20 block function），用于测试向量。
	 * @param out 输出 64 字节
	 */
	static void block(const uint8_t* key, uint32_t counter, const uint8_t* nonce, uint8_t* out);

private:
	// 由当前状态生成下一块密钥流，并把计数器加一
	void nextBlock();

private:
	uint32_t m_state[16];       // 初始状态：常量 | key | counter | nonce
	uint8_t m_keystream[64];    // 当前块密钥流
	size_t m_used;              // 当前块已经用掉的字节数
};

NS_VFS_END
