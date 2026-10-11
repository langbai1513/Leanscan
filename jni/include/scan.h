#pragma once
// ════════════════════════════════════════════════════════════════════════
//  scan.h —— 多线程无锁缓冲搜索（只管搜）
//
//  依赖方向（单向，谁都不反向依赖）：
//      mem_reader    只读内存
//      thread_pool   线程池 + 无锁缓冲
//      hitfile       结果文件格式（写/读/显示）
//      scan          ← 本文件：分块 → 多线程搜索 → 交给 hitfile 落盘
//
//  想只用「结果文件解析」→ 只 include hitfile.h，不用碰这里。
//
//  约定：
//    · 只搜 4 字节数值，地址一律 4 字节对齐（addr & 3 == 0），步进 4
//    · 块长度 ≤ block_bytes，块的 start/end 都 4 字节对齐
//    · 每个任务把命中写进「以范围地址命名」的文件，任务之间零共享 → 无锁
//    · 合并按地址顺序 mmap 拼接；合并耗时记进 Report::merge_ms
// ════════════════════════════════════════════════════════════════════════

#include "hitfile.h"
#include "memmap.h"
#include "mem_reader.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace scan {

// ────────────────────────────────────────────────────────────────────────
//  搜索值：4 字节，按原始位比较（Dword / Float 都用它）
//  kind 只影响显示，不影响比较
// ────────────────────────────────────────────────────────────────────────
struct Value {
    uint32_t      bits = 0;
    hitfile::Kind kind = hitfile::Kind::Dword;

    static Value dword(uint32_t v) noexcept { return Value{v, hitfile::Kind::Dword}; }
    static Value f32(float f) noexcept {
        Value v;
        v.kind = hitfile::Kind::Float;
        __builtin_memcpy(&v.bits, &f, sizeof(f));
        return v;
    }
};

// ────────────────────────────────────────────────────────────────────────
//  待搜块：start/end 都 4 字节对齐，长度 ≤ block_bytes
// ────────────────────────────────────────────────────────────────────────
struct Block {
    uintptr_t start = 0;   // 含（4 对齐）
    uintptr_t end   = 0;   // 不含（4 对齐）
    uint32_t  seg   = 0;   // 来自第几段（调试 / 归属用）

    std::size_t size() const noexcept { return std::size_t(end - start); }
};

// ────────────────────────────────────────────────────────────────────────
//  一个块的命中缓冲：紧凑的 varint 偏移流（不是逐条结构体）
// ────────────────────────────────────────────────────────────────────────
struct Hits {
    std::vector<uint8_t> payload;   // count 个 varint(差分偏移)
    uint64_t             count = 0; // 命中条数
    std::size_t bytes() const noexcept { return payload.size(); }
};

struct Options {
    std::size_t block_bytes        = 1u << 20; // 1MB
    std::size_t threads            = 0;        // 0 = 硬件并发数
    std::size_t reserve_per_block  = 4096;     // 每块命中缓冲预分配字节上限
    std::string out_dir            = ".";      // 每块临时文件的目录

    /// 缺页陷阱开关（用 /proc/<pid>/pagemap）：
    ///   开：先查每一页的 pagemap，只读「在内存且未被换出」的页（成段读），
    ///       不在内存 / 被换出的页整页跳过，绝不碰它们 —— 避免触发缺页陷阱。
    ///   关：老样子，整块一次 process_vm_readv。
    /// 开了但打不开 pagemap（没权限）时 run() 直接返回 false，不做静默回退。
    bool skip_fault_pages = true;
};

struct Report {
    std::size_t segs      = 0;    // 源段数
    std::size_t blocks    = 0;    // 切出的块数
    std::size_t threads   = 0;    // 实际线程数
    uint64_t    seg_bytes = 0;    // 段原始总大小（memmap 给的）
    uint64_t    scan_bytes = 0;   // 实际扫过的字节数
    uint64_t    hits      = 0;    // 命中数

    uint64_t out_bytes = 0;       // 合并后文件字节数
    uint64_t per_hit   = 0;       // = out_bytes / hits（看压缩效果用）

    double split_ms = 0;          // 分块
    double scan_ms  = 0;          // 多线程搜索（含落盘）
    double merge_ms = 0;          // 合并
    double total_ms = 0;

    std::string out_file;         // 合并后的文件
};

// ────────────────────────────────────────────────────────────────────────
//  纯函数：把一批段切成 ≤block_bytes、4 对齐的块（可单独测）
//    out_bytes 返回这些段的原始字节总和
// ────────────────────────────────────────────────────────────────────────
std::vector<Block> split_blocks(const std::vector<const memmap::Region*>& segs,
                                std::size_t block_bytes,
                                uint64_t* out_bytes = nullptr);

// ────────────────────────────────────────────────────────────────────────
//  二分查找：地址落在哪个块（blocks 需按地址升序；split_blocks 天然有序）
// ────────────────────────────────────────────────────────────────────────
const Block* locate(const std::vector<Block>& blocks, uintptr_t addr) noexcept;

// ────────────────────────────────────────────────────────────────────────
//  输出
// ────────────────────────────────────────────────────────────────────────
/// 结果文件名（固定放在 out_dir 里，合并时创建 / 覆盖）
inline constexpr char kResultName[] = "hits.bin";

/// 结果文件路径 = out_dir + "/hits.bin"
std::string result_path(const std::string& out_dir);

// ────────────────────────────────────────────────────────────────────────
//  搜索器
// ────────────────────────────────────────────────────────────────────────
class Scanner {
public:
    explicit Scanner(pid_t pid) noexcept : rd_(pid) {}

    bool ok() const noexcept { return rd_.valid(); }

    /// 在 segs（memmap 给的范围）里搜 v：
    ///   1. 用段大小总和预分配
    ///   2. 分块
    ///   3. 线程池里跑，每块结果写 out_dir/<范围地址>.bin（格式见 hitfile.h）
    ///   4. 按地址合并到 out_dir/hits.bin（没有就创建），删临时文件
    /// out_dir 不存在会自动创建；结果路径在 rep.out_file
    bool run(const std::vector<const memmap::Region*>& segs,
             const Value& v,
             const Options& opt,
             Report& rep);

private:
    memrw::Reader rd_;
};

} // namespace scan