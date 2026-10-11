#pragma once
// ════════════════════════════════════════════════════════════════════════
//  hitfile.h —— 搜索结果文件的格式（只管格式）
//
//  认识：magic / 头 / varint / 写块 / 合并 / 读回 / 显示
//  不认识：内存、线程、maps、搜索 —— 谁产出这种文件都能用，谁想读也能用
//
//  ── 格式（二进制，紧凑）────────────────────────────────────────────
//    整轮搜索只查一个值，所以「值 + 类型」只存一次；命中只存「相对块首的
//    偏移」，用 LEB128 变长 + 差分：第 1 条存偏移，之后存与上一条之差。
//
//    块文件
//       "SCB2"(4) | value u32(4) | base u64(8) | count u64(8) | kind u8(1) | pad(7)
//       | payload            (头 32 字节)
//       payload = count 个 varint：第 1 个 = off0，其余 = off_i - off_{i-1}
//       off = addr - base，升序
//    合并文件（各块按地址序拼接的 section 流）
//       "MSC2"(4) | value u32(4) | total_count u64(8) | kind u8(1) | pad(7)
//       每块：varint(base) | varint(count) | <该块 payload 原样>
//    kind: 0=Dword 1=Float（只影响显示，比较始终按 4 字节位）
// ════════════════════════════════════════════════════════════════════════

#include <cstddef>
#include <cstdint>
#include <functional>
#include <iosfwd>
#include <string>
#include <vector>

namespace hitfile {

// ────────────────────────────────────────────────────────────────────────
//  搜索类型（只影响显示）
// ────────────────────────────────────────────────────────────────────────
enum class Kind { Dword, Float };

/// 把 4 字节值按类型格式化成字符串："100 (Dword) 0x00000064"
std::string format_value(uint32_t bits, Kind kind);

// ────────────────────────────────────────────────────────────────────────
//  写
// ────────────────────────────────────────────────────────────────────────
/// LEB128 变长（构建 payload 用）
void put_varint(std::vector<uint8_t>& out, uint64_t v);

/// 写一个块文件。要不要写空块由调用方决定。
///   payload = count 个 varint 差分，相对 base
bool write_block(const std::string& path, uintptr_t base, uint32_t value, Kind kind,
                 const uint8_t* payload, std::size_t payload_bytes, uint64_t count);

/// 按给定顺序，把若干块文件合并成 section 流
bool merge(const std::vector<std::string>& block_files, const std::string& out,
           uint32_t value, Kind kind, uint64_t total_count);

/// 文件大小（报告用）
uint64_t file_size(const std::string& path);

// ────────────────────────────────────────────────────────────────────────
//  读
// ────────────────────────────────────────────────────────────────────────
struct ResultFile {
    uint32_t value = 0;              // 搜索值（4 字节位）
    uint64_t count = 0;              // 命中条数
    Kind     kind  = Kind::Dword;    // 搜索类型（文件头里存的）
};

using HitFn = std::function<bool(uintptr_t addr, uint32_t value)>;

/// 只读文件头：拿到「搜索值 + 搜索类型 + 命中数」（SCB2 / MSC2 都行）
bool result_info(const std::string& file, ResultFile& out);

/// 逐条读命中（流式回调，不一次性装进内存）；回调返回 false 表示提前停止
bool for_each_hit(const std::string& file, const HitFn& fn);

/// 直接打印：第一行「搜索值 (类型)，命中 N 条」，随后一行一个地址
///   limit = 0 表示不限条数
bool print_results(const std::string& file, std::ostream& os, std::size_t limit = 0);

} // namespace hitfile