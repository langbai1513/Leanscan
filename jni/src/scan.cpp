#include "scan.h"
#include "pagemap.hpp"
#include "thread_pool.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace scan {
namespace {

using clk = std::chrono::steady_clock;

inline double since_ms(clk::time_point t0) noexcept {
    return std::chrono::duration<double, std::milli>(clk::now() - t0).count();
}
inline uintptr_t up4(uintptr_t a)   noexcept { return (a + 3u) & ~uintptr_t(3); }
inline uintptr_t down4(uintptr_t a) noexcept { return a & ~uintptr_t(3); }

// 临时文件名 = 范围地址
std::string block_file(const std::string& dir, const Block& b) {
    char name[64];
    std::snprintf(name, sizeof(name), "/0x%016llx-0x%016llx.bin",
                  static_cast<unsigned long long>(b.start),
                  static_cast<unsigned long long>(b.end));
    return dir + name;
}

} // namespace

// ════════════════════════════════════════════════════════════════════════
//  分块：>block_bytes 的段切成 block_bytes 一块；start/end 全部 4 字节对齐
// ════════════════════════════════════════════════════════════════════════
std::vector<Block> split_blocks(const std::vector<const memmap::Region*>& segs,
                                std::size_t block_bytes, uint64_t* out_bytes) {
    std::vector<Block> blocks;
    uint64_t total = 0;

    std::size_t step = block_bytes & ~std::size_t(3);   // 步长保持 4 的倍数
    if (step < 4) step = 4;

    for (std::size_t i = 0; i < segs.size(); ++i) {
        const memmap::Region* r = segs[i];
        if (!r) continue;
        total += r->size();

        uintptr_t       a = up4(r->start);              // 起：向上取整到 4
        const uintptr_t b = down4(r->end);              // 止：向下取整到 4
        while (b > a) {
            std::size_t len = static_cast<std::size_t>(b - a);
            if (len > step) len = step;
            Block blk;
            blk.start = a;
            blk.end   = a + len;                        // a、len 都是 4 的倍数 → 仍对齐
            blk.seg   = static_cast<uint32_t>(i);
            blocks.push_back(blk);
            a += len;
        }
    }
    if (out_bytes) *out_bytes = total;
    return blocks;
}

// ════════════════════════════════════════════════════════════════════════
//  二分查找：地址落在哪个块
// ════════════════════════════════════════════════════════════════════════
const Block* locate(const std::vector<Block>& blocks, uintptr_t addr) noexcept {
    const auto it = std::upper_bound(blocks.begin(), blocks.end(), addr,
                                     [](uintptr_t a, const Block& b) { return a < b.start; });
    if (it == blocks.begin()) return nullptr;
    const Block& c = *(it - 1);
    return (addr >= c.start && addr < c.end) ? &c : nullptr;
}

// ════════════════════════════════════════════════════════════════════════
//  输出路径
// ════════════════════════════════════════════════════════════════════════
std::string result_path(const std::string& out_dir) {
    return (out_dir.empty() ? std::string(".") : out_dir) + "/" + kResultName;
}

// ════════════════════════════════════════════════════════════════════════
//  搜索主流程
// ════════════════════════════════════════════════════════════════════════
bool Scanner::run(const std::vector<const memmap::Region*>& segs,
                  const Value& v, const Options& opt, Report& rep) {
    const clk::time_point t_all = clk::now();
    rep = Report{};
    if (!rd_.valid()) return false;

    std::size_t nthreads = opt.threads;
    if (nthreads == 0) nthreads = std::thread::hardware_concurrency();
    if (nthreads == 0) nthreads = 4;
    rep.threads = nthreads;

    // 缺页陷阱开关：开着就用目标进程的 pagemap（打不开就直接失败，不做静默回退）
    memrw::PageMap pmap;
    if (opt.skip_fault_pages && !pmap.open(rd_.pid())) return false;

    if (!opt.out_dir.empty()) ::mkdir(opt.out_dir.c_str(), 0777);

    // ① 分块（顺带拿到段原始总大小）
    const clk::time_point t1 = clk::now();
    uint64_t seg_bytes = 0;
    std::vector<Block> blocks = split_blocks(segs, opt.block_bytes, &seg_bytes);
    rep.segs      = segs.size();
    rep.blocks    = blocks.size();
    rep.seg_bytes = seg_bytes;
    rep.split_ms  = since_ms(t1);
    if (blocks.empty()) { rep.total_ms = since_ms(t_all); return true; }

    // ② 搜索前的预分配
    //    命中缓冲：每块按自身大小给上界（全命中 = size/4 条 → 受 reserve_per_block 字节封顶）
    //    读缓冲：  N × block_bytes，一次性分配好（无锁池）
    std::vector<Hits> results(blocks.size());
    for (std::size_t i = 0; i < blocks.size(); ++i) {
        std::size_t est = blocks[i].size() / 4;            // 最坏条数
        if (est > opt.reserve_per_block) est = opt.reserve_per_block;
        results[i].payload.reserve(est);
    }
    tp::BufferPool bufs(nthreads, opt.block_bytes);

    // ③ 多线程搜索：一个块一个任务，命中写本块文件（紧凑 varint）
    const clk::time_point t2 = clk::now();
    std::atomic<uint64_t> scanned{0};
    {
        tp::Pool pool(nthreads);
        for (std::size_t i = 0; i < blocks.size(); ++i) {
            pool.submit([&, i] {
                const Block& b = blocks[i];

                tp::BufferPool::Lease lease = bufs.acquire();
                if (!lease.ok()) return;

                Hits& h = results[i];
                uintptr_t prev_off = 0;
                bool      first    = true;
                uint64_t  got      = 0;

                // 扫 buf 里从 off0 开始、长 len 的字节（4 字节步进；地址 = b.start + off）
                auto scan_at = [&](const uint8_t* buf, std::size_t off0, std::size_t len) {
                    const std::size_t n4 = len & ~std::size_t(3);
                    for (std::size_t o = 0; o < n4; o += 4) {
                        uint32_t w;
                        std::memcpy(&w, buf + o, 4);
                        if (w != v.bits) continue;
                        const std::size_t off = off0 + o;          // 相对块首
                        hitfile::put_varint(h.payload, first ? off : (off - prev_off));
                        prev_off = off;
                        first    = false;
                        ++h.count;
                    }
                };

                const std::size_t bsz = b.size();

                if (!pmap.ok()) {
                    // 没开缺页跳过：整块一次读
                    const ssize_t n = rd_.read(b.start, lease.data, bsz);
                    const std::size_t n4 =
                        (n > 0) ? (static_cast<std::size_t>(n) & ~std::size_t(3)) : 0;
                    scan_at(lease.data, 0, n4);
                    got = n4;
                } else {
                    // 查 pagemap：只读「在内存且未换出」的页，连续可用的页成段读，
                    // 其余整页跳过 —— 绝不碰会触发缺页的页
                    const std::size_t page = memrw::PageMap::page_size();
                    const uintptr_t first_page = b.start & ~uintptr_t(page - 1);
                    const uintptr_t last_page  = (b.end + (page - 1)) & ~uintptr_t(page - 1);
                    const std::size_t npages =
                        static_cast<std::size_t>((last_page - first_page) / page);

                    std::vector<uint64_t> ents(npages, 0);
                    const ssize_t ne = pmap.read(first_page, ents.data(), npages);

                    auto usable = [&](std::size_t k) -> bool {
                        if (ne < 0 || k >= static_cast<std::size_t>(ne)) return false;
                        return memrw::PageMap::usable(ents[k]);
                    };

                    std::size_t k = 0;
                    while (k < npages) {
                        if (!usable(k)) { ++k; continue; }        // 不在内存 → 整页跳过
                        std::size_t j = k + 1;
                        while (j < npages && usable(j)) ++j;      // 连续可用的页成一段

                        uintptr_t a = first_page + k * page;
                        if (a < b.start) a = b.start;             // 块首可能不在页首
                        uintptr_t e2 = first_page + j * page;
                        if (e2 > b.end) e2 = b.end;

                        const std::size_t off0 = static_cast<std::size_t>(a - b.start);
                        const ssize_t m =
                            rd_.read(a, lease.data + off0, static_cast<std::size_t>(e2 - a));
                        if (m > 0) {
                            const std::size_t m4 = static_cast<std::size_t>(m) & ~std::size_t(3);
                            scan_at(lease.data + off0, off0, m4);
                            got += m4;
                        }
                        k = j;
                    }
                }
                scanned.fetch_add(got, std::memory_order_relaxed);

                // 任务间零共享；空块不建文件
                if (h.count)
                    hitfile::write_block(block_file(opt.out_dir, b), b.start,
                                         v.bits, v.kind,
                                         h.payload.data(), h.payload.size(), h.count);
                bufs.release(lease);
            });
        }
        pool.wait();
    }
    rep.scan_ms    = since_ms(t2);
    rep.scan_bytes = scanned.load(std::memory_order_relaxed);
    for (const Hits& h : results) rep.hits += h.count;

    // ④ 按地址合并
    std::vector<std::string> files;
    files.reserve(blocks.size());
    for (const Block& b : blocks) files.push_back(block_file(opt.out_dir, b));
    std::sort(files.begin(), files.end());     // 定宽 16 进制 → 文件名序 == 地址序

    std::string out = result_path(opt.out_dir);   // out_dir 不存在会在前面 mkdir 出来
    std::remove(out.c_str());

    const clk::time_point t3 = clk::now();
    hitfile::merge(files, out, v.bits, v.kind, rep.hits);
    rep.merge_ms = since_ms(t3);

    for (const std::string& f : files) std::remove(f.c_str());   // 合并完删临时文件

    rep.out_file  = out;
    rep.out_bytes = hitfile::file_size(out);
    rep.per_hit   = rep.hits ? (rep.out_bytes + rep.hits - 1) / rep.hits : 0;
    rep.total_ms  = since_ms(t_all);
    return true;
}

} // namespace scan