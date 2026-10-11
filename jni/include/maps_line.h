#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace memmap {

/// 一行 maps 的原始字段
struct MapsLine {
    uintptr_t   start  = 0;         // 起始地址
    uintptr_t   end    = 0;         // 结束地址（不含）
    char        perm[5] = "----";   // 权限 4 字符，如 "r-xp"（第 4 位 p=私有 s=共享）
    uint64_t    offset = 0;         // 文件偏移
    uint64_t    inode  = 0;         // inode
    std::string path;               // 映射路径；无名映射为空串
};

/// 解析一行；格式不符返回 false（调用方直接跳过该行即可）
bool parse_line(std::string_view line, MapsLine& out);

/// 取路径的文件名部分："…/libx.so" → "libx.so"
/// 方括号名（[stack] / [anon:…]）不是真路径：里面有 '/' 时取最后一个 '/' 之后、
/// ']' 之前的片段（[anon:dalvik-/system/framework/boot.art] → "boot.art"）；
/// 没有 '/' 就整体返回
std::string_view base_name(std::string_view path) noexcept;

} // namespace memmap