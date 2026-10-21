# vfstool 端到端测试（cmake -P 脚本，跨平台，不依赖 shell）
# 参数：VFSTOOL（可执行文件）、SOURCE_DIR（测试数据目录）、WORK_DIR（临时工作目录）
# 可选：VFSTOOL_EMULATOR（交叉编译时运行 vfstool 的程序，例如 node、wine；多个参数用 '|' 分隔）
cmake_minimum_required(VERSION 3.16)

foreach(var VFSTOOL SOURCE_DIR WORK_DIR)
    if(NOT DEFINED ${var})
        message(FATAL_ERROR "missing argument ${var}")
    endif()
endforeach()

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")
# 避免外部环境变量影响结果
unset(ENV{VFS_KEY})
if(DEFINED VFSTOOL_EMULATOR)
    string(REPLACE "|" ";" VFSTOOL_EMULATOR "${VFSTOOL_EMULATOR}")
endif()

# 运行 vfstool，检查退出码；输出写入 ${WORK_DIR}/<name>.out
function(run name expected)
    execute_process(
        COMMAND ${VFSTOOL_EMULATOR} "${VFSTOOL}" ${ARGN}
        OUTPUT_FILE "${WORK_DIR}/${name}.out"
        ERROR_FILE "${WORK_DIR}/${name}.err"
        RESULT_VARIABLE rc
    )
    file(READ "${WORK_DIR}/${name}.err" err)
    if(NOT rc EQUAL expected)
        file(READ "${WORK_DIR}/${name}.out" out)
        message(FATAL_ERROR "[${name}] exit code ${rc}, expected ${expected}\ncommand: ${ARGN}\nstdout:\n${out}\nstderr:\n${err}")
    endif()
    message(STATUS "[${name}] OK (exit ${rc})")
endfunction()

function(expect_contains name needle)
    file(READ "${WORK_DIR}/${name}.out" out)
    string(FIND "${out}" "${needle}" pos)
    if(pos EQUAL -1)
        message(FATAL_ERROR "[${name}] output does not contain \"${needle}\":\n${out}")
    endif()
endfunction()

function(expect_same_file a b)
    file(SHA256 "${a}" ha)
    file(SHA256 "${b}" hb)
    if(NOT ha STREQUAL hb)
        message(FATAL_ERROR "file contents differ:\n  ${a}\n  ${b}")
    endif()
endfunction()

# 比较两个目录树的文件列表与内容
function(expect_same_tree a b)
    file(GLOB_RECURSE fa RELATIVE "${a}" "${a}/*")
    file(GLOB_RECURSE fb RELATIVE "${b}" "${b}/*")
    list(SORT fa)
    list(SORT fb)
    if(NOT fa STREQUAL fb)
        message(FATAL_ERROR "directory contents differ:\n  ${a}: ${fa}\n  ${b}: ${fb}")
    endif()
    foreach(f IN LISTS fa)
        expect_same_file("${a}/${f}" "${b}/${f}")
    endforeach()
    list(LENGTH fa n)
    message(STATUS "directories match: ${n} files")
endfunction()

set(A1 "${WORK_DIR}/game1.vfs")
set(A2 "${WORK_DIR}/game2.vfs")
set(PACK_ARGS --encrypt "**/*.bin" --encrypt "*.json" --compress "*.lua" --compress "*.json")

# 1. pack（两次，结果必须逐字节相同）
run(pack1 0 pack "${SOURCE_DIR}" -o "${A1}" ${PACK_ARGS})
run(pack2 0 pack "${SOURCE_DIR}" -o "${A2}" ${PACK_ARGS})
file(SHA256 "${A1}" H1)
file(SHA256 "${A2}" H2)
message(STATUS "sha256(game1.vfs) = ${H1}")
message(STATUS "sha256(game2.vfs) = ${H2}")
if(NOT H1 STREQUAL H2)
    message(FATAL_ERROR "two pack runs produced different output")
endif()

# 2. info / list
run(info 0 info "${A1}")
expect_contains(info "5")
run(info_json 0 info "${A1}" --json)
expect_contains(info_json "{")
run(list 0 list "${A1}")
expect_contains(list "scripts/main.lua")
expect_contains(list "中文目录/配置.json")
run(list_long 0 list -l "${A1}")
run(list_glob 0 list "${A1}" "*.lua")
expect_contains(list_glob "scripts/lib/util.lua")

# 3. cat：输出与源文件逐字节相同
foreach(rel "scripts/main.lua" "images/noise.bin" "中文目录/配置.json")
    string(MAKE_C_IDENTIFIER "cat_${rel}" name)
    run(${name} 0 cat "${A1}" "${rel}")
    expect_same_file("${WORK_DIR}/${name}.out" "${SOURCE_DIR}/${rel}")
endforeach()
run(cat_missing 2 cat "${A1}" "no/such/file")

# 参数文件：UTF-8 带 BOM、含中文路径（Windows 上验证参数不经过 ANSI 代码页）
string(ASCII 239 187 191 BOM)
file(WRITE "${WORK_DIR}/cat_args.txt" "${BOM}cat \"${A1}\" \"中文目录/配置.json\"\n")
run(cat_response_file 0 "@${WORK_DIR}/cat_args.txt")
expect_same_file("${WORK_DIR}/cat_response_file.out" "${SOURCE_DIR}/中文目录/配置.json")

# 4. unpack：目录树与源目录完全一致
run(unpack 0 unpack "${A1}" -o "${WORK_DIR}/unpacked")
expect_same_tree("${SOURCE_DIR}" "${WORK_DIR}/unpacked")
run(unpack_again 2 unpack "${A1}" -o "${WORK_DIR}/unpacked")
run(unpack_force 0 unpack "${A1}" -o "${WORK_DIR}/unpacked" --force)

# 5. extract
run(extract 0 extract "${A1}" "scripts" -o "${WORK_DIR}/extracted")
expect_same_file("${WORK_DIR}/extracted/scripts/lib/util.lua" "${SOURCE_DIR}/scripts/lib/util.lua")
# 输出到中文目录（UTF-8 → 本机路径转换）
run(extract_cn 0 extract "${A1}" "中文目录/配置.json" -o "${WORK_DIR}/提取结果")
expect_same_file("${WORK_DIR}/提取结果/中文目录/配置.json" "${SOURCE_DIR}/中文目录/配置.json")

# 6. verify
run(verify 0 verify "${A1}")

# 7. 外部密钥：缺密钥 → 5；密钥文件 → 0
run(keygen 0 keygen -o "${WORK_DIR}/key.txt")
run(pack_keyed 0 pack "${SOURCE_DIR}" -o "${WORK_DIR}/keyed.vfs" --key-file "${WORK_DIR}/key.txt" --encrypt-all)
run(info_nokey 5 info "${WORK_DIR}/keyed.vfs")
run(verify_key 0 verify "${WORK_DIR}/keyed.vfs" --key-file "${WORK_DIR}/key.txt")
run(cat_key 0 cat "${WORK_DIR}/keyed.vfs" "README.txt" --key-file "${WORK_DIR}/key.txt")
expect_same_file("${WORK_DIR}/cat_key.out" "${SOURCE_DIR}/README.txt")

# 8. 不是归档 → 3；参数错误 → 1
file(WRITE "${WORK_DIR}/bogus.vfs" "this is not an archive at all, just some text padding padding padding padding padding padding padding padding padding padding padding")
run(info_bogus 3 info "${WORK_DIR}/bogus.vfs")
run(usage 1 pack)

message(STATUS "all vfstool end-to-end tests passed")
