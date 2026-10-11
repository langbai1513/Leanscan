// ════════════════════════════════════════════════════════════════════════
//  main.cpp —— 使用示例
//
//  aaa [包名] [要搜的 Dword 值] [输出目录]
//    包名        默认 com.example.app
//    值          缺省 → 只列 maps 不搜索；支持 10 / 0x 十六进制
//    输出目录    默认 /data/local/tmp/scan（结果文件固定为 <输出目录>/hits.bin）
//
//  例：
//    aaa                                              // 看 maps 分布，不搜
//    aaa com.example.app 100                          // 搜 dword 100
//    aaa com.example.app 0x3f800000 /sdcard/scan      // 搜 float 1.0 的位模式
//
//  读取别的进程内存需要 root 或同 uid；否则搜索那步会失败，前面照常。
// ════════════════════════════════════════════════════════════════════════

#include "memmap.h"
#include "maps_line.h"
#include "pid.h"
#include "scan.h"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>

using namespace memmap;

int main(int argc, char** argv) {
    const char*  pkg      = (argc > 1) ? argv[1] : "com.example.app";
    const bool   have_val = (argc > 2);
    const uint32_t val    = have_val
                          ? static_cast<uint32_t>(std::strtoul(argv[2], nullptr, 0)) : 0u;
    const std::string out_dir = (argc > 3) ? argv[3] : "/data/local/tmp/scan";

    // ── 1. 解析 maps ────────────────────────────────────────────────
    pid = getPid(pkg);
    MemMap m(pid);
    if (!m.ok()) {
        std::cout << "读不到 maps（进程不存在或没权限）\n";
        return 1;
    }
    std::cout << "进程 " << pkg << " pid=" << pid << "，读到 " << m.size() << " 个区域\n";

    std::cout << "\n各类型分布：\n";
    m.dump_counts(std::cout);

    // ── 2. 搜索范围来自当前库；段大小总和可直接显示 ──────────────────
    auto segs = m.list(Type::All, Perm::Read);        // 只搜可读段
    const SegStat stat = stat_of(segs);
    std::cout << "\n搜索范围：" << stat << "\n";

    if (!have_val) {                                  // 没给值 → 只看 maps
        std::cout << "（未给搜索值，只列 maps）\n";
        return 0;
    }

    // ── 3. 多线程无锁缓冲搜索 ───────────────────────────────────────
    scan::Options opt;
    opt.out_dir = out_dir;                          // 每块临时文件（文件名=范围地址）
    // opt.block_bytes = 1u << 20;                  // 默认 1MB
    // opt.threads     = 8;                          // 默认硬件并发
    // opt.skip_fault_pages = true;                  // 默认开：查 pagemap，跳过不在内存/被换出的页

    scan::Scanner sc(pid);
    if (!sc.ok()) {
        std::cout << "内存读取不可用（需要 root / 同 uid）\n";
        return 1;
    }

    scan::Report rep;
    if (!sc.run(segs, scan::Value::dword(val), opt, rep)) {
        std::cout << "搜索失败\n";
        return 1;
    }

// ── 4. 报告 ─────────────────────────────────────────────────────
    std::cout << "\n命中 " << rep.hits << " 条\n"
              << "块 " << rep.blocks << " 个，线程 " << rep.threads << " 个\n"
              << "扫描 " << (rep.scan_bytes / 4) << " 个地址\n"
              << "总耗时 " << (rep.total_ms / 1000.0) << "s\n";

    // ── 5. 解析并显示搜索结果（地址 + 搜索值）────────────────────────
    std::cout << "\n";
    if (!hitfile::print_results(rep.out_file, std::cout, 20))
        std::cout << "结果文件解析失败\n";

    return 0;
}