#pragma once
// ════════════════════════════════════════════════════════════════════════
//  mem_reader.hpp —— 只干一件事：把目标进程的一段内存读出来
//
//  完全不认识 maps / 搜索 / 线程：谁都能拿它来读内存，改平台只改这一个文件。
//  读法用 process_vm_readv（需 root 或同 uid）。
// ════════════════════════════════════════════════════════════════════════

#include <cstddef>
#include <cstdint>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace memrw {

class Reader {
public:
    Reader() = default;
    explicit Reader(pid_t pid) noexcept : pid_(pid) {}

    void  setPid(pid_t p) noexcept { pid_ = p; }
    pid_t pid()  const noexcept { return pid_; }
    bool  valid() const noexcept { return pid_ > 0; }

    /// 读目标进程 [addr, addr+size) 到 dst。
    /// 返回实际读到的字节数；< 0 表示这次读失败（调用方自己决定跳过/重试）。
    /// 注：进程中途死掉、地址不可读都会走到这里，不做任何隐藏回退。
    ssize_t read(uintptr_t addr, void* dst, std::size_t size) const noexcept {
        if (pid_ <= 0 || dst == nullptr || size == 0) return -1;

        struct iovec local;
        local.iov_base = dst;
        local.iov_len  = size;
        struct iovec remote;
        remote.iov_base = reinterpret_cast<void*>(addr);
        remote.iov_len  = size;

        return ::syscall(SYS_process_vm_readv, pid_, &local, 1, &remote, 1, 0);
    }

private:
    pid_t pid_ = 0;
};

} // namespace memrw
