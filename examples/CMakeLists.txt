# 示例（默认不构建：-DVFS_BUILD_EXAMPLES=ON 打开）
add_subdirectory(basic)            # 最小示例：归档 + 内存层叠加
add_subdirectory(layers)           # 分层挂载 + 别名
add_subdirectory(archive)          # 归档打包与读取：加密 + LZ4 压缩
add_subdirectory(custom_backend)   # 自定义后端
add_subdirectory(multithread)      # 多线程读取 + 热更新
add_subdirectory(stream)           # 流式 API
