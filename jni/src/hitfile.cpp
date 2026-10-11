#include "hitfile.h"

#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <ostream>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace hitfile {
namespace {

// ── 小端读写（arm64 / x86_64 都是小端） ──────────────────────────────────
inline void put_u32(uint8_t* p, uint32_t v) noexcept { std::memcpy(p, &v, 4); }
inline void put_u64(uint8_t* p, uint64_t v) noexcept { std::memcpy(p, &v, 8); }
inline uint32_t get_u32(const uint8_t* p) noexcept { uint32_t v; std::memcpy(&v, p, 4); return v; }
inline uint64_t get_u64(const uint8_t* p) noexcept { uint64_t v; std::memcpy(&v, p, 8); return v; }

// ── LEB128 ──────────────────────────────────────────────────────────────
inline bool read_varint(const uint8_t*& p, const uint8_t* end, uint64_t& v) noexcept {
    uint64_t r = 0;
    int      s = 0;
    while (p < end) {
        const uint8_t b = *p++;
        r |= uint64_t(b & 0x7fu) << s;
        if (!(b & 0x80u)) { v = r; return true; }
        s += 7;
        if (s > 63) return false;
    }
    return false;
}
inline bool write_varint(int fd, uint64_t v) {
    uint8_t buf[10];
    std::size_t i = 0;
    do { uint8_t b = uint8_t(v & 0x7fu); v >>= 7; if (v) b |= 0x80u; buf[i++] = b; } while (v);
    return ::write(fd, buf, i) == static_cast<ssize_t>(i);
}

// ── 文件头 ──────────────────────────────────────────────────────────────
constexpr char kBlockMagic[4]  = {'S', 'C', 'B', '2'};
constexpr char kMergedMagic[4] = {'M', 'S', 'C', '2'};
constexpr std::size_t kBlockHdr  = 32;   // magic4 + value4 + base8 + count8 + kind1 + pad7
constexpr std::size_t kMergedHdr = 24;   // magic4 + value4 + count8 + kind1 + pad7
constexpr std::size_t kBlockKindOff  = 24;
constexpr std::size_t kMergedKindOff = 16;

bool write_all(int fd, const void* p, std::size_t n) noexcept {
    const uint8_t* s = static_cast<const uint8_t*>(p);
    while (n) {
        const ssize_t w = ::write(fd, s, n);
        if (w <= 0) return false;
        s += w;
        n -= static_cast<std::size_t>(w);
    }
    return true;
}

} // namespace

// ════════════════════════════════════════════════════════════════════════
//  写
// ════════════════════════════════════════════════════════════════════════
void put_varint(std::vector<uint8_t>& out, uint64_t v) {
    while (v >= 0x80u) { out.push_back(uint8_t(v) | 0x80u); v >>= 7; }
    out.push_back(uint8_t(v));
}

bool write_block(const std::string& path, uintptr_t base, uint32_t value, Kind kind,
                 const uint8_t* payload, std::size_t payload_bytes, uint64_t count) {
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return false;

    uint8_t hdr[kBlockHdr];
    std::memset(hdr, 0, sizeof(hdr));
    std::memcpy(hdr, kBlockMagic, 4);
    put_u32(hdr + 4, value);
    put_u64(hdr + 8, static_cast<uint64_t>(base));
    put_u64(hdr + 16, count);
    hdr[kBlockKindOff] = static_cast<uint8_t>(kind);

    const bool ok = write_all(fd, hdr, sizeof(hdr)) &&
                    write_all(fd, payload, payload_bytes);
    ::close(fd);
    return ok;
}

bool merge(const std::vector<std::string>& block_files, const std::string& out,
           uint32_t value, Kind kind, uint64_t total_count) {
    const int ofd = ::open(out.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (ofd < 0) return false;

    uint8_t hdr[kMergedHdr];
    std::memset(hdr, 0, sizeof(hdr));
    std::memcpy(hdr, kMergedMagic, 4);
    put_u32(hdr + 4, value);
    put_u64(hdr + 8, total_count);
    hdr[kMergedKindOff] = static_cast<uint8_t>(kind);
    if (!write_all(ofd, hdr, sizeof(hdr))) { ::close(ofd); return false; }

    bool ok = true;
    for (const std::string& p : block_files) {
        const int ifd = ::open(p.c_str(), O_RDONLY);
        if (ifd < 0) continue;                     // 空块没生成文件，正常
        struct stat st{};
        if (::fstat(ifd, &st) != 0 || st.st_size < static_cast<off_t>(kBlockHdr)) {
            ::close(ifd);
            continue;
        }
        const std::size_t n = static_cast<std::size_t>(st.st_size);
        void* m = ::mmap(nullptr, n, PROT_READ, MAP_PRIVATE, ifd, 0);
        if (m == MAP_FAILED) { ::close(ifd); ok = false; continue; }

        const uint8_t* p8 = static_cast<const uint8_t*>(m);
        if (std::memcmp(p8, kBlockMagic, 4) == 0) {
            const uint64_t base  = get_u64(p8 + 8);
            const uint64_t count = get_u64(p8 + 16);
            ok = ok && write_varint(ofd, base) && write_varint(ofd, count) &&
                 write_all(ofd, p8 + kBlockHdr, n - kBlockHdr);
        }
        ::munmap(m, n);
        ::close(ifd);
    }
    ::close(ofd);
    return ok;
}

uint64_t file_size(const std::string& path) {
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0) return 0;
    return static_cast<uint64_t>(st.st_size);
}

// ════════════════════════════════════════════════════════════════════════
//  读
// ════════════════════════════════════════════════════════════════════════
std::string format_value(uint32_t bits, Kind kind) {
    char buf[64];
    if (kind == Kind::Float) {
        float f;
        std::memcpy(&f, &bits, sizeof(f));
        std::snprintf(buf, sizeof(buf), "%g (Float) 0x%08X", static_cast<double>(f), bits);
    } else {
        std::snprintf(buf, sizeof(buf), "%u (Dword) 0x%08X", bits, bits);
    }
    return std::string(buf);
}

bool result_info(const std::string& file, ResultFile& out) {
    out = ResultFile{};
    const int fd = ::open(file.c_str(), O_RDONLY);
    if (fd < 0) return false;

    uint8_t hdr[kBlockHdr];
    std::memset(hdr, 0, sizeof(hdr));
    const ssize_t n = ::read(fd, hdr, sizeof(hdr));
    ::close(fd);

    if (n < static_cast<ssize_t>(kMergedHdr)) return false;
    if (std::memcmp(hdr, kMergedMagic, 4) == 0) {
        out.value = get_u32(hdr + 4);
        out.count = get_u64(hdr + 8);
        out.kind  = static_cast<Kind>(hdr[kMergedKindOff]);
        return true;
    }
    if (n >= static_cast<ssize_t>(kBlockHdr) && std::memcmp(hdr, kBlockMagic, 4) == 0) {
        out.value = get_u32(hdr + 4);
        out.count = get_u64(hdr + 16);
        out.kind  = static_cast<Kind>(hdr[kBlockKindOff]);
        return true;
    }
    return false;
}

bool for_each_hit(const std::string& file, const HitFn& fn) {
    if (!fn) return false;

    const int fd = ::open(file.c_str(), O_RDONLY);
    if (fd < 0) return false;

    struct stat st{};
    if (::fstat(fd, &st) != 0 || st.st_size < static_cast<off_t>(kMergedHdr)) {
        ::close(fd);
        return false;
    }
    const std::size_t n = static_cast<std::size_t>(st.st_size);
    void* m = ::mmap(nullptr, n, PROT_READ, MAP_PRIVATE, fd, 0);
    ::close(fd);
    if (m == MAP_FAILED) return false;

    const uint8_t* p   = static_cast<const uint8_t*>(m);
    const uint8_t* end = p + n;
    bool ok = true, stop = false;

    if (std::memcmp(p, kMergedMagic, 4) == 0 && n >= kMergedHdr) {
        // 合并文件：头 + 若干 section（每段：varint(base) varint(count) payload）
        const uint32_t value = get_u32(p + 4);
        p += kMergedHdr;
        while (p < end && !stop) {
            uint64_t base = 0, count = 0;
            if (!read_varint(p, end, base) || !read_varint(p, end, count)) { ok = false; break; }
            uint64_t off = 0;
            for (uint64_t i = 0; i < count; ++i) {
                uint64_t d = 0;
                if (!read_varint(p, end, d)) { ok = false; break; }
                off += d;                                  // 块内是差分
                if (!fn(uintptr_t(base + off), value)) { stop = true; break; }
            }
        }
    } else if (std::memcmp(p, kBlockMagic, 4) == 0 && n >= kBlockHdr) {
        // 单个块文件：头 + 一段 payload
        const uint32_t value = get_u32(p + 4);
        const uint64_t base  = get_u64(p + 8);
        const uint64_t count = get_u64(p + 16);
        p += kBlockHdr;
        uint64_t off = 0;
        for (uint64_t i = 0; i < count; ++i) {
            uint64_t d = 0;
            if (!read_varint(p, end, d)) { ok = false; break; }
            off += d;
            if (!fn(uintptr_t(base + off), value)) break;
        }
    } else {
        ok = false;
    }

    ::munmap(m, n);
    return ok;
}

bool print_results(const std::string& file, std::ostream& os, std::size_t limit) {
    ResultFile info;
    if (!result_info(file, info)) return false;

    os << "搜索值 " << format_value(info.value, info.kind) << "，命中 " << info.count << " 条\n";

    const std::ios_base::fmtflags flags = os.flags();
    const bool all = (limit == 0);
    std::size_t shown = 0;

    os << std::hex;
    const bool ok = for_each_hit(file, [&](uintptr_t addr, uint32_t) {
        os << "0x" << addr << '\n';
        ++shown;
        return all || shown < limit;
    });
    os.flags(flags);

    if (!all && info.count > shown)
        os << "（已截断，还有 " << (info.count - shown) << " 条）\n";
    return ok;
}

} // namespace hitfile