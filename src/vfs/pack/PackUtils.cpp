#include "PackUtils.h"
#include <assert.h>

#ifdef VFS_HAS_ZLIB
#include "zlib.h"
#endif

NS_VFS_BEGIN

#define SET_ERR_CODE(code) if(errCode) { *errCode = code; }
#define CHUNK 16384

namespace pack
{
	char* decompressData(const char* inData, uint64_t inLen, uint64_t& outLen, int* errCode)
	{
		SET_ERR_CODE(0);
		outLen = 0;

#ifdef VFS_HAS_ZLIB
		if (!inData)
		{
			SET_ERR_CODE(Z_DATA_ERROR);
			return nullptr;
		}

		/* allocate inflate state */
		z_stream strm;
		strm.zalloc = Z_NULL;
		strm.zfree = Z_NULL;
		strm.opaque = Z_NULL;
		strm.avail_in = 0;
		strm.next_in = Z_NULL;
		int ret = inflateInit2(&strm, MAX_WBITS + 16);
		if (ret != Z_OK)
		{
			SET_ERR_CODE(ret);
			return nullptr;
		}

		std::shared_ptr<z_stream> sp_strm(&strm, [](z_stream* strm) { (void)inflateEnd(strm); });

		std::unique_ptr<unsigned char[]> outBuf(new unsigned char[CHUNK]);
		if (!outBuf)
		{
			SET_ERR_CODE(Z_MEM_ERROR);
			return nullptr;
		}

		size_t decompressedDataLen = CHUNK;
		unsigned char* decompressedData = (unsigned char*)malloc(decompressedDataLen);
		if (decompressedData == nullptr)
		{
			SET_ERR_CODE(Z_MEM_ERROR);
			return nullptr;
		}

		const char* end = inData + inLen;
		int flush = 0;
		do 
		{
			ptrdiff_t distance = end - inData;
			strm.avail_in = (distance >= (ptrdiff_t)CHUNK) ? CHUNK : distance;
			strm.next_in = (Bytef*)inData;

			// next pos
			inData += strm.avail_in;
			flush = (inData == end) ? Z_FINISH : Z_NO_FLUSH;

			/* run inflate() on input until output buffer not full */
			do 
			{
				strm.avail_out = CHUNK;
				strm.next_out = &outBuf[0];
				ret = inflate(&strm, Z_NO_FLUSH);
				if (ret == Z_STREAM_ERROR) /* state not clobbered */
					break;

				switch (ret) 
				{
				case Z_NEED_DICT:
				case Z_DATA_ERROR:
				case Z_MEM_ERROR:
				{
					SET_ERR_CODE(ret);
					free(decompressedData);
					return nullptr;
				}
				}
				auto dataLen = CHUNK - strm.avail_out;

				if (outLen + dataLen > decompressedDataLen)
				{
					decompressedDataLen *= 2;
					auto tmp = (unsigned char*)realloc(decompressedData, decompressedDataLen);
					if (tmp == nullptr)
					{
						SET_ERR_CODE(Z_MEM_ERROR);
						free(decompressedData);
						return nullptr;
					}
					decompressedData = tmp;
				}
				memcpy(decompressedData + outLen, &outBuf[0], dataLen);
				outLen += dataLen;
			} while (strm.avail_out == 0);
		} while (flush != Z_FINISH);

		if (ret == Z_STREAM_END)
		{
			return (char*)decompressedData;
		}
		else
		{
			SET_ERR_CODE(Z_DATA_ERROR);
			free(decompressedData);
			return nullptr;
		}
#else
		// Not implemented
		assert(false);
		return nullptr;
#endif
	}
}

NS_VFS_END
