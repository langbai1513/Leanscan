#pragma once

#include "memmap.h"

#include <cstddef>
#include <string>
#include <string_view>

namespace memmap {
namespace rules {

/// 分类时可以看到的全部上下文
struct Ctx {
    std::string_view path;      // 原始路径（无名映射为空）
    std::string_view base;      // 路径的文件名部分
    std::string_view prev;      // 紧邻上一个 C_data 段的短名（.bss 归属用；不是 C_data 则为空）
    PermSet          perm;      // 权限

    bool has(Perm p) const noexcept { return perm.has(p); }
    bool anon()      const noexcept { return path.empty(); }

    bool any(std::string_view a) const noexcept {
        return path.find(a) != std::string_view::npos;
    }
    template <class... Rest>
    bool any(std::string_view a, Rest... rest) const noexcept {
        return any(a) || any(rest...);
    }
};

using Pred = bool (*)(const Ctx&);

struct Rule {
    Type type;
    Pred pred;
};

// ────────────────────────────────────────────────────────────────────────
//  规则表（从上到下第一条命中生效）
// ────────────────────────────────────────────────────────────────────────
inline const Rule kRules[] = {
    { Type::Xs, [](const Ctx& c) { return c.any("/vendor/","/system/","/system_ext","jit-cache","/apex/","vdso") && c.has(Perm::Exec); } },
    // ① 无名映射：maps 里连路径列都没有
    { Type::Xa, [](const Ctx& c) { return c.anon() && c.has(Perm::Exec) && !c.has(Perm::Write); } },
    { Type::A,  [](const Ctx& c) { return c.anon() && c.has(Perm::Read); } },
    { Type::O,  [](const Ctx& c) { return c.anon(); } },

    // ② 应用代码段：/data/app 下的可执行映射
    { Type::Xa, [](const Ctx& c) { return c.has(Perm::Exec) && c.any("/data") && !c.has(Perm::Write); } },

    // ②b 【新增】/dev/xLog（memtype.c → B）
    { Type::B, [](const Ctx& c) { return c.any("/dev/xLog"); } },

    // ④ .bss：可写，且紧邻上一个段是 C_data（此时 prev 才非空）。
    //    必须排在 C_data 前面，否则 "…/libX.so:bss" 会被 C_data 的 ".so" 条件抢走
    { Type::Cb, [](const Ctx& c) {
         return c.has(Perm::Write) && !c.prev.empty() && c.any("bss");
      } },

    // ⑤ 应用数据映射（.so 的非执行段、base.apk 分页）
    { Type::Cd, [](const Ctx& c) {
          return c.any("/data") && c.any(".so");
      } },

    // ⑤b 【新增】/mnt/ 下的 .so（memtype.c 的 Cd 分支里 /mnt/ 那条）
    { Type::Cd, [](const Ctx& c) { return c.any("/mnt/") && c.any(".so"); } },

    // ⑥ 匿名分配
    { Type::Ca, [](const Ctx& c) {
          return c.any("anon:libc_malloc","/dev/zero","anon:scudo:");
      } },

    // ⑥b 【新增】裸 "malloc"（memtype.c 用的是 contains("malloc")，比 ⑥ 更宽）
    { Type::Ca, [](const Ctx& c) { return c.any("malloc"); } },

    // ⑦ 栈 / 堆
    { Type::S,  [](const Ctx& c) { return c.any("[stack]","thread signal stack", "stack_and_tls"); } },
    { Type::Ch, [](const Ctx& c) { return c.any("[heap]");  } },

    // ⑧ 字体
    { Type::F, [](const Ctx& c) { return c.any("fonts/", "fontdata", ".ttf", ".otf","hyph-", ".hyb"); } },

    // ⑨ GPU / 显存：共享可读写、不可执行、非私有。
    //    注意写 "/dev/ion" 而不是裸 "ion" —— 裸写会把 anon region 也算进来
    { Type::V, [](const Ctx& c) {
          return c.any("mali", "gpu", "vram", "kgsl", "/dev/ion", "ion-heap", "ion-alloc","dmabuf:")
                 && !c.has(Perm::Exec) && !c.has(Perm::Priv)
                 && (c.has(Perm::Read) || c.has(Perm::Write));
      } },

    // ⑨b 【新增】/dev/ 图形 / 厂商驱动节点（memtype.c 的 k_v_list；
    //     去掉原规则已覆盖的 mali/kgsl/ion/dmabuf）
    { Type::V, [](const Ctx& c) {
          return !c.has(Perm::Exec) &&
                 c.any("/dev/nv", "/dev/tegra", "/dev/pvr", "/dev/render", "/dev/galcore",
                       "/dev/fimg2d", "/dev/quadd", "/dev/graphics", "/dev/mm_", "/dev/dri/");
      } },

    // ⑨ 共享内存 / 模拟器
    { Type::As, [](const Ctx& c) { return c.any("/dev/ashmem"); } },
    { Type::PS, [](const Ctx& c) { return c.any("PPSSPP_RAM"); }  },

    // ⑩ Dalvik 托管堆（先判具体堆，再兜底其它 dalvik-）
    { Type::Jh, [](const Ctx& c) {
          return c.any("dalvik-allocation", "dalvik-large", "dalvik-main", "dalvik-free","dalvik-zygote", "dalvik-bitmap", "dalvik-gc", "dalvik-jit-code-cache");
      } },

    { Type::J, [](const Ctx& c) { return c.any("dalvik-",".oat", ".vdex", ".odex", ".art","dalvik-Boot image reservation"); } },

    // 【新增】兜底：可执行且不可写 → 代码段
    //         （memtype.c 里 (flags & 6) == 4 的 Xa/Xs 分支；本项目不分包名，统一归 Xs）
    { Type::Xs, [](const Ctx& c) { return c.has(Perm::Exec) && !c.has(Perm::Write); } },
};

inline constexpr std::size_t kRuleCount = sizeof(kRules) / sizeof(kRules[0]);

/// 按规则表判定类型；都没命中就是 Other
inline Type classify(const Ctx& c) noexcept {
    for (std::size_t i = 0; i < kRuleCount; ++i) {
        if (kRules[i].pred(c)) return kRules[i].type;
    }
    return Type::O;
}

/// 区域短名：无名 → [anon]；.bss → "<紧邻 C_data 段>:bss"；其余取文件名
inline std::string name_of(const Ctx& c, Type t) {
    if (c.anon()) return "[anon]";
    if (t == Type::Cb && !c.prev.empty()) return std::string(c.prev) + ":bss";
    return std::string(c.base);
}
} // namespace rules
} // namespace memmap