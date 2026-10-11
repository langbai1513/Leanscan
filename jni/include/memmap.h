#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <iosfwd>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace memmap {

// ════════════════════════════════════════════════════════════════════════
//  1. 区域类型（位标志：一个枚举值 = 一个位，多个相或 = 一个「类型集合」）
//     All = ~0u 当通配；Unknown = 0 表示空集合 / 未分类。
// ════════════════════════════════════════════════════════════════════════
enum class Type : uint32_t {
    Unknown     = 0u,          // 空集合 / 未分类
    Ch          = 1u << 0,     // 1      C 堆
    Jh          = 1u << 1,     // 2      Java 堆
    Ca          = 1u << 2,     // 4      C 分配
    Cb          = 1u << 3,     // 8      C bss
    Cd          = 1u << 4,     // 16     C 数据
    A           = 1u << 5,     // 32     匿名
    S           = 1u << 6,     // 64     栈
    F           = 1u << 7,     // 128    字体
    // 1<<8 ~ 1<<13 空缺
    Xa          = 1u << 14,    // 16384  目标包代码
    Xs          = 1u << 15,    // 32768  非目标包代码
    J           = 1u << 16,    // 65536  Dalvik
    B           = 1u << 17,    // 131072 Bad
    PS          = 1u << 18,    // 262144 PPSSPP
    As          = 1u << 19,    // 524288 ashmem
    V           = 1u << 20,    // 1048576 显存
    O           = 1u << 21,    // 2097152 其它
    All         = ~0u,         // 0xFFFFFFFF（所有位，通配用）
};

// ════════════════════════════════════════════════════════════════════════
//  2. 权限（是索引，不是位；Mask 会做 1 << index）
// ════════════════════════════════════════════════════════════════════════
enum class Perm : uint32_t {
    Read  = 0,  // r
    Write,      // w
    Exec,       // x
    Priv,       // p（私有映射；相对的是 s=共享）
    Count
};

// ════════════════════════════════════════════════════════════════════════
//  3. 位集合：类型集合 / 权限集合（同一套代码，不重复造）
//     E → 位的换算各按各的规矩：
//       Type 的值本身就是位     → to_bits 原样返回
//       Perm 是索引，需要左移   → to_bits 做 1 << index
//     语义：集合里有哪些位 = 匹配哪些。
//       空集合（bits_ == 0）就是「一个都不匹配」；要「全部」用 Type::All。
//       （以前「空集合 = 这一维不过滤」的约定已删：Type::Unknown 也是 0，
//         两者撞车会让「统计 Unknown」变成「统计全部」。）
// ════════════════════════════════════════════════════════════════════════
constexpr uint32_t to_bits(Type e) noexcept { return static_cast<uint32_t>(e); }
constexpr uint32_t to_bits(Perm e) noexcept { return 1u << static_cast<unsigned>(e); }

template <class E>
class Mask {
public:
    constexpr Mask() noexcept = default;
    constexpr Mask(E e) noexcept : bits_(to_bits(e)) {}
    constexpr Mask(std::initializer_list<E> es) noexcept {
        for (E e : es) bits_ |= to_bits(e);
    }

    static constexpr Mask of(uint32_t bits) noexcept { Mask m; m.bits_ = bits; return m; }

    constexpr uint32_t bits()  const noexcept { return bits_; }
    constexpr bool     empty() const noexcept { return bits_ == 0; }
    constexpr bool     has(E e) const noexcept { return (bits_ & to_bits(e)) != 0; }

    constexpr Mask operator|(E e) const noexcept { return of(bits_ | to_bits(e)); }
    constexpr Mask operator|(Mask m) const noexcept { return of(bits_ | m.bits_); }

private:
    uint32_t bits_ = 0;
};

using TypeSet = Mask<Type>;
using PermSet = Mask<Perm>;

constexpr TypeSet operator|(Type a, Type b) noexcept { return TypeSet(a) | b; }
constexpr TypeSet operator|(Type a, TypeSet b) noexcept { return b | a; }
constexpr PermSet operator|(Perm a, Perm b) noexcept { return PermSet(a) | b; }
constexpr PermSet operator|(Perm a, PermSet b) noexcept { return b | a; }

// ════════════════════════════════════════════════════════════════════════
//  4. 区域
// ════════════════════════════════════════════════════════════════════════
struct Region {
    uintptr_t   start  = 0;                 // 起始地址（含）
    uintptr_t   end    = 0;                 // 结束地址（不含）
    uint64_t    offset = 0;                 // 在映射文件中的偏移（算 vaddr→fileoff 用）
    uint32_t    index  = 0;                 // 在 maps 中的 0 基序号（原始顺序）
    PermSet     perm;                       // 权限
    Type        type = Type::Unknown;       // 类型
    std::string name;                       // 短名："libGameCore.so" / "[stack]" / "libX.so:bss"
    std::string path;                       // 原始路径（无名映射为空串）

    std::size_t size()       const noexcept { return end - start; }
    bool        readable()   const noexcept { return perm.has(Perm::Read);  }
    bool        writable()   const noexcept { return perm.has(Perm::Write); }
    bool        executable() const noexcept { return perm.has(Perm::Exec);  }
    bool        contains(uintptr_t a) const noexcept { return a >= start && a < end; }
};

// ════════════════════════════════════════════════════════════════════════
//  5. 名字 / 字符串 ↔ 枚举
// ════════════════════════════════════════════════════════════════════════
const char* type_name(Type t) noexcept;                                  // Type → "CodeApp"
bool        type_parse(std::string_view s, Type& out) noexcept;          // "codeapp" → Type（忽略大小写）
TypeSet     types_from_string(std::string_view s) noexcept;              // "CodeApp,Stack" / "All"
PermSet     perm_of(const char chars[4]) noexcept;                       // "r-xp" → PermSet
void        perm_chars(PermSet p, char out[5]) noexcept;                  // PermSet → "r-xp"

// ════════════════════════════════════════════════════════════════════════
//  6. 输出（标准库〈iostream〉：Region 直接 <<，dump 系列走 ostream）
// ════════════════════════════════════════════════════════════════════════
std::ostream& operator<<(std::ostream& os, const Region& r);      // 一条一行
std::ostream& operator<<(std::ostream& os, const std::vector<const Region*>& v);  // 一批（list/segments 的返回值）

// ════════════════════════════════════════════════════════════════════════
//  7. 段大小统计（外部可显示；搜索据此预分配）
//     一个信息：段数 + 原始字节总和 + 4 字节对齐后可扫字节数
// ════════════════════════════════════════════════════════════════════════
struct SegStat {
    std::size_t count = 0;   // 段数
    uint64_t    bytes = 0;   // Σ size()（原始字节）
    uint64_t    span  = 0;   // Σ 4 字节对齐后的可用字节（= 搜索实际会扫的量）

    bool empty() const noexcept { return count == 0; }
};

/// 对一批段（list / segments 的返回值）做统计
SegStat stat_of(const std::vector<const Region*>& v) noexcept;

/// 外部显示："4053 段, 2205836288 个地址"（每地址 4 字节 = span / 4）
std::ostream& operator<<(std::ostream& os, const SegStat& s);

// ════════════════════════════════════════════════════════════════════════
//  7. 门面
// ════════════════════════════════════════════════════════════════════════
class MemMap {
public:
    MemMap() = default;
    explicit MemMap(int pid) { load(pid); }

    // ── 加载 ────────────────────────────────────────────────────
    bool load(int pid);                              // 读 /proc/<pid>/maps
    bool load_file(const char* file);                // 读任意 maps 格式文件
    bool load_file(const std::string& file) { return load_file(file.c_str()); }
    bool load_stream(FILE* f);                       // 读任意流（管道 / 已打开的 fd）
    bool load_text(std::string_view text);           // 直接喂文本
    void clear() noexcept;

    static MemMap self();                            // 当前进程

    // ── 概览 ────────────────────────────────────────────────────
    std::size_t size()  const noexcept { return regions_.size(); }
    bool        empty() const noexcept { return regions_.empty(); }
    bool        ok()    const noexcept { return !regions_.empty(); }
    const std::vector<Region>& regions() const noexcept { return regions_; }
    const Region* at(std::size_t i) const noexcept;   // 越界返回 nullptr

    // ── 查询 ────────────────────────────────────────────────────
    /// 按短名取第 nth 条（nth 从 1 开始）
    const Region* find(std::string_view name, unsigned nth = 1) const;
    /// 同上，再限定类型
    const Region* find(std::string_view name, Type type, unsigned nth = 1) const;
    /// 取第 nth 个「可执行段」——hook 找代码段用这个
    const Region* find_code(std::string_view name, unsigned nth = 1) const;
    /// 地址 → 所在区域（maps 按地址升序，二分查找）
    const Region* which(uintptr_t addr) const;
    /// 同名区域全体
    std::vector<const Region*> segments(std::string_view name) const;
    /// 条件查询：类型（默认 Type::All = 全部）/ 权限（要求全部包含；空 = 这一维不过滤）
    ///           / 子串（匹配 name 或 path）/ 条数上限
    std::vector<const Region*> list(TypeSet types = Type::All, PermSet perms = {},
                                    std::string_view substr = "", std::size_t limit = 0) const;

    uintptr_t   addr(std::string_view name, unsigned nth = 1) const;      // 没找到返回 0
    uintptr_t   addr(std::string_view name, Type type, unsigned nth = 1) const;
    /// 数一数集的区域个数（默认 Type::All = 全部）
    std::size_t count(TypeSet types = Type::All) const;

    /// 某个类型（或全部）的段大小统计：一步拿到「段数 + 总大小」（外部可显示 / 搜索预分配用）
    SegStat stat_of(TypeSet types = Type::All, PermSet perms = {},
                    std::string_view substr = "") const;

    // ── 输出 ────────────────────────────────────────────────────
    void dump(std::ostream& os) const;               // 全部区域，一行一条
    void dump_counts(std::ostream& os) const;        // 各类型计数（只列非零）

private:
    std::vector<Region> regions_;
    std::unordered_map<std::string, std::vector<uint32_t>> by_name_;   // 短名 → 序号
};

} // namespace memmap