#pragma once
// ════════════════════════════════════════════════════════════════════════
//  pagemap.hpp —— 目标进程的 /proc/<pid>/pagemap（只读）
//
//  只干一件事：把「某一页在不在内存 / 是不是被换出」查出来。
//  不认识 maps / 搜索 / 线程 —— 谁想按页判断可用性都能用。
//
//  pagemap 每页一项 8 字节，常用位：
//    bit63 present  ——1=页在内存（不在就会触发缺页）
//    bit62 swapped  ——1=页被换出（读它要先换入，同样会触发缺页）
//    bit61 file     ——文件页 / 共享匿名
// ════════════════════════════════════════════════════════════════════════

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>

namespace memrw {

class PageMap {
public:
    /// 本机页大小（通常 4KB，部分设备 16KB）
    static std::size_t page_size() noexcept {
        static const std::size_t p = [] {
            const long v = ::sysconf(_SC_PAGESIZE);
            return (v > 0) ? static_cast<std::size_t>(v) : std::size_t(4096);
        }();
        return p;
    }

    static bool present(uint64_t e) noexcept { return ((e >> 63) & 1u) != 0; }  // 在内存
    static bool swapped(uint64_t e) noexcept { return ((e >> 62) & 1u) != 0; }  // 被换出
    /// 能不能直接读：在内存且没被换出（否则会触发缺页）
    static bool usable (uint64_t e) noexcept { return present(e) && !swapped(e); }

    PageMap() = default;
    explicit PageMap(pid_t pid) noexcept { open(pid); }
    ~PageMap() { close(); }

    PageMap(const PageMap&)            = delete;
    PageMap& operator=(const PageMap&) = delete;

    bool open(pid_t pid) noexcept {
        close();
        if (pid <= 0) return false;
        char fname[64];
        std::snprintf(fname, sizeof(fname), "/proc/%d/pagemap", pid);
        fd_ = ::open(fname, O_RDONLY);
        if (fd_ < 0) { fd_ = -1; return false; }
        pid_ = pid;
        return true;
    }

    void close() noexcept {
        if (fd_ >= 0) ::close(fd_);
        fd_  = -1;
        pid_ = 0;
    }

    bool  ok()  const noexcept { return fd_ >= 0; }
    pid_t pid() const noexcept { return pid_; }

    /// 读 [addr, addr + count*页) 的 pagemap 项到 out；
    /// 返回实际读到的项数（< 0 失败）。addr 建议页对齐。
    ssize_t read(uintptr_t addr, uint64_t* out, std::size_t count) const noexcept {
        if (fd_ < 0 || out == nullptr || count == 0) return -1;
        const uint64_t idx = static_cast<uint64_t>(addr / page_size());
        const std::size_t bytes = count * sizeof(uint64_t);
        return ::pread(fd_, out, bytes, static_cast<off_t>(idx * sizeof(uint64_t)));
    }

private:
    int   fd_  = -1;
    pid_t pid_ = 0;
};

} // namespace memrw