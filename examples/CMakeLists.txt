# Examples (off by default; enable with -DVFS_BUILD_EXAMPLES=ON).
add_subdirectory(basic)            # Minimal example: archive plus a memory layer
add_subdirectory(layers)           # Layered mounts and aliases
add_subdirectory(archive)          # Pack and read an archive: encryption plus LZ4 compression
add_subdirectory(custom_backend)   # Custom backend
add_subdirectory(multithread)      # Multithreaded reads and hot reload
add_subdirectory(stream)           # Streaming API
