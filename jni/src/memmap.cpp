#include "memmap.h"
#include "memmap_rules.h"
#include "maps_line.h"

#include <algorithm>
#include <cstring>
#include <iomanip>
#include <ostream>
#include <unistd.h>

namespace memmap {

// ════════════════════════════════════════════════════════════════════════
//  内部小工具
// ════════════════════════════════════════════════════════════════════════
namespace {

// ── 类型名表：打印和字符串解析共用；新增类型在这里补一行即可 ──────────
struct TypeName { Type t; const char* s; };

constexpr TypeName kTypeNames[] = {
    { Type::Unknown, "Unknown" },
    { Type::Ch,      "Ch"      },
    { Type::Jh,      "Jh"      },
    { Type::Ca,      "Ca"      },
    { Type::Cb,      "Cb"      },
    { Type::Cd,      "Cd"      },
    { Type::A,       "A"       },
    { Type::S,       "S"       },
    { Type::F,       "F"       },
    { Type::Xa,      "Xa"      },
    { Type::Xs,      "Xs"      },
    { Type::J,       "J"       },
    { Type::B,       "B"       },
    { Type::PS,      "PS"      },
    { Type::As,      "As"      },
    { Type::V,       "V"       },
    { Type::O,       "O"       },
};
// 注意：All = ~0u 是通配位集合，不是单个类型，不进这张表。

/// 忽略大小写的相等
bool ieq(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = char(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = char(y - 'A' + 'a');
        if (x != y) return false;
    }
    return true;
}

void trim(std::string_view& s) noexcept {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back()  == ' ' || s.back()  == '\t')) s.remove_suffix(1);
}

/// 一次读完全部内容（给 load_file / load_stream 共用）
void read_all(FILE* f, std::string& out) {
    char buf[8192];
    std::size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
}

} // namespace

// ════════════════════════════════════════════════════════════════════════
//  名字 / 字符串 ↔ 枚举
// ════════════════════════════════════════════════════════════════════════
const char* type_name(Type t) noexcept {
    for (const TypeName& e : kTypeNames) {
        if (e.t == t) return e.s;
    }
    return "Unknown";
}

bool type_parse(std::string_view s, Type& out) noexcept {
    for (const TypeName& e : kTypeNames) {
        if (ieq(s, e.s)) { out = e.t; return true; }
    }
    return false;
}

TypeSet types_from_string(std::string_view s) noexcept {
    TypeSet set;
    std::size_t pos = 0;
    while (pos <= s.size()) {
        const std::size_t comma = s.find(',', pos);
        std::string_view tok = s.substr(pos, (comma == std::string_view::npos)? std::string_view::npos : comma - pos);
        trim(tok);
        Type t;
        if (ieq(tok, "All") || ieq(tok, "*")) {
            set = TypeSet(Type::All);          // 所有位 = 通配
        } else if (!tok.empty() && type_parse(tok, t)) {
            set = set | t;
        }
        if (comma == std::string_view::npos) break;
        pos = comma + 1;
    }
    return set;
}

PermSet perm_of(const char chars[4]) noexcept {
    return PermSet::of((chars[0] == 'r' ? 1u : 0u) | (chars[1] == 'w' ? 2u : 0u) | (chars[2] == 'x' ? 4u : 0u) | (chars[3] == 'p' ? 8u : 0u));
}

void perm_chars(PermSet p, char out[5]) noexcept {
    out[0] = p.has(Perm::Read)  ? 'r' : '-';
    out[1] = p.has(Perm::Write) ? 'w' : '-';
    out[2] = p.has(Perm::Exec)  ? 'x' : '-';
    out[3] = p.has(Perm::Priv)  ? 'p' : 's';
    out[4] = '\0';
}

// ════════════════════════════════════════════════════════════════════════
//  一行文本的解析（细节在 maps_line.h 的注释里）
// ════════════════════════════════════════════════════════════════════════
namespace {

/// 取下一个空格分隔的 token，并把 pos 推到 token 之后
std::string_view next_token(std::string_view s, std::size_t& pos) noexcept {
    while (pos < s.size() && s[pos] == ' ') ++pos;
    const std::size_t b = pos;
    while (pos < s.size() && s[pos] != ' ') ++pos;
    return s.substr(b, pos - b);
}

bool hex_val(std::string_view s, uint64_t& v) noexcept {
    if (s.empty()) return false;
    v = 0;
    for (char c : s) {
        unsigned d;
        if      (c >= '0' && c <= '9') d = unsigned(c - '0');
        else if (c >= 'a' && c <= 'f') d = unsigned(c - 'a') + 10u;
        else if (c >= 'A' && c <= 'F') d = unsigned(c - 'A') + 10u;
        else return false;
        v = (v << 4) | d;
    }
    return true;
}

bool dec_val(std::string_view s, uint64_t& v) noexcept {
    if (s.empty()) return false;
    v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        v = v * 10u + unsigned(c - '0');
    }
    return true;
}

} // namespace

std::string_view base_name(std::string_view path) noexcept {
    if (path.empty()) return path;

    // 方括号名（[stack] / [anon:…]）不是真路径，里面的 '/' 不是路径分隔符。
    if (path.front() == '[') {
        // "[anon:dalvik-/system/framework/boot.art]"
        //   → 取最后一个 '/' 之后、']' 之前的片段 → "boot.art"
        // 如果括号里没有 '/'（[stack]、[anon:libc_malloc]、[anon:.bss]），整体当一个名字。
        const std::size_t slash = path.rfind('/');
        if (slash == std::string_view::npos) return path;

        std::string_view tail = path.substr(slash + 1);
        if (!tail.empty() && tail.back() == ']') tail.remove_suffix(1);
        return tail;
    }

    const std::size_t slash = path.rfind('/');
    return (slash == std::string_view::npos) ? path : path.substr(slash + 1);
}

bool parse_line(std::string_view line, MapsLine& out) {
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.remove_suffix(1);
    if (line.empty() || line[0] == '#') return false;

    std::size_t pos = 0;

    // ① 起始-结束  "7000000000-7000010000"
    const std::string_view range = next_token(line, pos);
    const std::size_t dash = range.find('-');
    if (dash == std::string_view::npos) return false;

    uint64_t start = 0, end = 0;
    if (!hex_val(range.substr(0, dash), start)) return false;
    if (!hex_val(range.substr(dash + 1), end))  return false;

    // ② 权限  "r-xp"
    const std::string_view perm = next_token(line, pos);
    if (perm.size() < 4) return false;

    // ③ 偏移  ④ 设备号  ⑤ inode
    uint64_t offset = 0, inode = 0;
    if (!hex_val(next_token(line, pos), offset)) return false;   // 文件偏移（十六进制）
    if (next_token(line, pos).empty())           return false;   // 设备号（跳过即可）
    if (!dec_val(next_token(line, pos), inode))  return false;   // inode（十进制）

    // ⑥ 剩下整段 = 路径，允许内部含空格
    while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t')) ++pos;
    std::size_t tail = line.size();
    while (tail > pos && (line[tail - 1] == ' '  || line[tail - 1] == '\t' || line[tail - 1] == '\r' || line[tail - 1] == '\n')) --tail;

    out.start  = uintptr_t(start);
    out.end    = uintptr_t(end);
    std::memcpy(out.perm, perm.data(), 4);
    out.perm[4] = '\0';
    out.offset = offset;
    out.inode  = inode;
    out.path.assign(line.substr(pos, tail - pos));
    return true;
}

// ════════════════════════════════════════════════════════════════════════
//  加载
// ════════════════════════════════════════════════════════════════════════
void MemMap::clear() noexcept {
    regions_.clear();
    by_name_.clear();
}

bool MemMap::load_text(std::string_view text) {
    clear();
    regions_.reserve(std::size_t(std::count(text.begin(), text.end(), '\n')) + 1);

    std::string prev_data;   // 紧邻上一个 C_data 段的短名（.bss 归属用；不是 C_data 则为空）
    MapsLine    ln;
    std::size_t pos = 0;

    while (pos < text.size()) {
        const std::size_t nl  = text.find('\n', pos);
        const std::size_t end = (nl == std::string_view::npos) ? text.size() : nl;
        const std::string_view raw = text.substr(pos, end - pos);
        pos = (nl == std::string_view::npos) ? text.size() : nl + 1;

        if (!parse_line(raw, ln)) continue;

        rules::Ctx ctx;
        ctx.path = ln.path;
        ctx.base = base_name(ln.path);
        ctx.prev = prev_data;
        ctx.perm = perm_of(ln.perm);

        Region r;
        r.index  = uint32_t(regions_.size());
        r.start  = ln.start;
        r.end    = ln.end;
        r.offset = ln.offset;
        r.perm   = ctx.perm;
        r.type   = rules::classify(ctx);
        r.name   = rules::name_of(ctx, r.type);
        r.path   = ln.path;                      // 拷贝：ctx 还指着 ln，随后就被复用

        // .bss 归属：只有紧邻的上一个段是 C_data 才留名，否则清空
        prev_data = (r.type == Type::Cd) ? r.name : std::string();

        by_name_[r.name].push_back(r.index);
        regions_.push_back(std::move(r));
    }
    return !regions_.empty();
}

bool MemMap::load_file(const char* file) {
    FILE* f = fopen(file, "rb");
    if (!f) { clear(); return false; }

    std::string buf;
    if (fseek(f, 0, SEEK_END) == 0) {          // 普通文件：一次分配到位
        const long n = ftell(f);
        if (n > 0) buf.reserve(std::size_t(n));
        rewind(f);
    } else {
        clearerr(f);                            // 不可 seek（管道等）：直接顺序读
    }
    read_all(f, buf);
    fclose(f);
    return load_text(buf);
}

bool MemMap::load_stream(FILE* f) {
    if (!f) { clear(); return false; }
    std::string buf;
    read_all(f, buf);
    return load_text(buf);
}

bool MemMap::load(int pid) {
    char path[64];
    std::snprintf(path, sizeof(path), "/proc/%d/maps", pid);
    return load_file(path);
}

MemMap MemMap::self() {
    return MemMap(::getpid());   // pid_t 在 Linux/Android 上就是 int
}

// ════════════════════════════════════════════════════════════════════════
//  查询
// ════════════════════════════════════════════════════════════════════════
const Region* MemMap::at(std::size_t i) const noexcept {
    return (i < regions_.size()) ? &regions_[i] : nullptr;
}

const Region* MemMap::find(std::string_view name, unsigned nth) const {
    if (nth == 0) return nullptr;
    const auto it = by_name_.find(std::string(name));
    if (it == by_name_.end() || nth > it->second.size()) return nullptr;
    return &regions_[it->second[nth - 1]];
}

const Region* MemMap::find(std::string_view name, Type type, unsigned nth) const {
    if (nth == 0) return nullptr;
    const auto it = by_name_.find(std::string(name));
    if (it == by_name_.end()) return nullptr;
    unsigned seen = 0;
    for (uint32_t idx : it->second) {
        const Region& r = regions_[idx];
        if (r.type == type && ++seen == nth) return &r;
    }
    return nullptr;
}

const Region* MemMap::find_code(std::string_view name, unsigned nth) const {
    if (nth == 0) return nullptr;
    const auto it = by_name_.find(std::string(name));
    if (it == by_name_.end()) return nullptr;
    unsigned seen = 0;
    for (uint32_t idx : it->second) {
        const Region& r = regions_[idx];
        if (r.executable() && ++seen == nth) return &r;
    }
    return nullptr;
}

const Region* MemMap::which(uintptr_t a) const {
    // maps 本身按地址升序排列，二分定位第一个 start > a 的区域，前一条就是候选
    const auto it = std::upper_bound(
        regions_.begin(), regions_.end(), a,
        [](uintptr_t value, const Region& r) { return value < r.start; });
    if (it == regions_.begin()) return nullptr;
    const Region& cand = *(it - 1);
    return cand.contains(a) ? &cand : nullptr;
}

std::vector<const Region*> MemMap::list(TypeSet types, PermSet perms,
                                       std::string_view substr, std::size_t limit) const {
    std::vector<const Region*> out;
    if (limit) out.reserve(limit);

    const bool   check_perm = !perms.empty();
    const bool   check_sub  = !substr.empty();
    const uint32_t need     = perms.bits();
    const char*  sub        = substr.data();
    const std::size_t sub_n = substr.size();

    for (const Region& r : regions_) {
        // 类型这一维永远过滤：默认给的是 Type::All，空集合则一个都不匹配
        if (!types.has(r.type)) continue;
        if (check_perm && (r.perm.bits() & need) != need) continue;   // 要求的权限必须全含
        if (check_sub  && r.name.find(sub, 0, sub_n) == std::string::npos &&
                         r.path.find(sub, 0, sub_n) == std::string::npos) continue;
        out.push_back(&r);
        if (limit && out.size() >= limit) break;
    }
    return out;
}

std::vector<const Region*> MemMap::segments(std::string_view name) const {
    std::vector<const Region*> out;
    const auto it = by_name_.find(std::string(name));
    if (it == by_name_.end()) return out;
    out.reserve(it->second.size());
    for (uint32_t idx : it->second) out.push_back(&regions_[idx]);
    return out;
}

uintptr_t MemMap::addr(std::string_view name, unsigned nth) const {
    const Region* r = find(name, nth);
    return r ? r->start : 0;
}

uintptr_t MemMap::addr(std::string_view name, Type type, unsigned nth) const {
    const Region* r = find(name, type, nth);
    return r ? r->start : 0;
}

std::size_t MemMap::count(TypeSet types) const {
    // 空集合 = 一个都不匹配（不再当「全部」）。要全部请显式传 Type::All。
    std::size_t n = 0;
    for (const Region& r : regions_) {
        if (types.has(r.type)) ++n;
    }
    return n;
}

// ════════════════════════════════════════════════════════════════════════
//  输出（标准库〈iostream〉，Region 直接 <<；不再有自定义 print）
// ════════════════════════════════════════════════════════════════════════
std::ostream& operator<<(std::ostream& os, const Region& r) {
    char p[5];
    perm_chars(r.perm, p);

    // 保存/恢复流状态：本函数会改 hex / 对齐 / 填充，不能污染调用方
    const std::ios_base::fmtflags flags = os.flags();
    const char                   fill  = os.fill();

    os << "0x" << std::hex << std::setfill('0')
       << std::setw(10) << r.start << "-0x"
       << std::setw(10) << r.end
       << std::setfill(fill) << ' '
       << p << ' '
       << std::left  << std::setw(2) << type_name(r.type)
       << std::right << std::dec << ' ' << std::setw(4) << (r.size() / 1024) << "KB "
       << r.name << '\n';

    os.flags(flags);
    os.fill(fill);
    return os;
}

std::ostream& operator<<(std::ostream& os, const std::vector<const Region*>& v) {
    for (const Region* r : v) {
        if (r) os << *r;
    }
    return os;
}

void MemMap::dump(std::ostream& os) const {
    os << "# " << regions_.size() << " regions\n";
    for (const Region& r : regions_) os << r;
}

void MemMap::dump_counts(std::ostream& os) const {
    const std::ios_base::fmtflags flags = os.flags();
    const char                   fill  = os.fill();
    const std::streamsize        width = os.width();

    // Type 是位标志，不能再靠 1..Count 数出来；类型清单以 kTypeNames 为准
    for (const TypeName& e : kTypeNames) {
        const std::size_t n = count(TypeSet(e.t));
        if (n) os << std::left << std::setw(12) << e.s
                  << std::right << std::setw(4) << n << '\n';
    }
    os << std::left << std::setw(12) << "TOTAL"
       << std::right << std::setw(4) << regions_.size() << '\n';

    os.flags(flags);
    os.fill(fill);
    os.width(width);
}

// ════════════════════════════════════════════════════════════════════════
//  段大小统计（外部可显示；搜索据此预分配）
// ════════════════════════════════════════════════════════════════════════
SegStat stat_of(const std::vector<const Region*>& v) noexcept {
    SegStat s;
    for (const Region* r : v) {
        if (!r) continue;
        ++s.count;
        s.bytes += r->size();

        // 只算 4 字节对齐后真正能扫的那一段：[align_up4(start), align_down4(end))
        const uintptr_t a = (r->start + 3) & ~uintptr_t(3);
        const uintptr_t b =  r->end         & ~uintptr_t(3);
        if (b > a) s.span += uint64_t(b - a);
    }
    return s;
}

std::ostream& operator<<(std::ostream& os, const SegStat& s) {
    std::ios_base::fmtflags flags = os.flags();
    // 地址数量 = 4 字节对齐后可扫字节数 / 4（每地址 4 字节）
    os << std::dec << s.count << " 段, " << (s.span / 4) << " 个地址";
    os.flags(flags);
    return os;
}

SegStat MemMap::stat_of(TypeSet types, PermSet perms, std::string_view substr) const {
    return memmap::stat_of(list(types, perms, substr));
}

} // namespace memmap