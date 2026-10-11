# Leanscan

Android 进程内存分析工具：解析 `/proc/<pid>/maps`、按类型分类，并在分类结果上做
**多线程无锁缓冲的 4 字节（Dword / Float）搜索**。

**纯 C++17 标准库 + NDK，零第三方依赖。**

> Author: [@池鱼] · Feedback: QQ 群 614302184

*A lightweight Android memory scanner in C++17 — parses `/proc/<pid>/maps`,
classifies every region, then scans selected regions with a lock-free
multithreaded engine. No third-party dependencies.*

---

## 目录

- [特性](#特性)
- [目录结构](#目录结构)
- [依赖方向](#依赖方向)
- [构建](#构建)
- [快速开始](#快速开始)
- [类型（短码）](#类型短码)
- [API](#api)
- [结果文件格式](#结果文件格式)
- [设计要点](#设计要点)
- [已知限制](#已知限制)

---

## 特性

- **maps 解析 + 类型分类**：位标志类型（可组合）、规则表驱动，加规则只需插一行。
- **段大小统计**：一次查询即得「段数 + 可扫地址数」，可直接用于搜索预分配。
- **多线程无锁缓冲搜索**
  - 固定线程池，线程只建 / 销毁一次；
  - 无锁缓冲池（原子栈 CAS 借还），1MB 读缓冲**搜索前一次性分配**；
  - 一块一个任务，**任务之间零共享**（命中只写自己的缓冲和文件）→ 全程无锁；
  - > 1MB 的段切 1MB 块，`start/end` 全部 4 字节对齐。
- **缺页陷阱规避**：查 `/proc/<pid>/pagemap`，只读「在内存且未被换出」的页，
  其余整页跳过，不触发缺页。
- **紧凑结果文件**：值只存一次，命中用 LEB128 变长 + 差分，**1~3 字节/条**；
  流式读取 + 直接打印，不必把结果全读进内存。

## 目录结构

```
jni/
├── Android.mk
├── Application.mk
├── main.cpp                  使用示例：解析 maps + 搜索          → aaa
├── parse_main.cpp            只解析结果文件的示例（不搜索）       → aab
├── include/
│   ├── memmap.h              maps 门面：Type / Region / MemMap / SegStat
│   ├── memmap_rules.h        分类规则表（只改这里就能改分类）
│   ├── maps_line.h           单行解析：一行文本 → MapsLine
│   ├── pid.h                 按包名找 pid
│   ├── mem_reader.hpp        只负责读进程内存（process_vm_readv）
│   ├── pagemap.hpp           只负责查 /proc/<pid>/pagemap
│   ├── thread_pool.hpp       线程池 Pool + 无锁缓冲池 BufferPool
│   ├── hitfile.h             结果文件格式：写 / 合并 / 读 / 显示
│   └── scan.h                多线程搜索门面
└── src/
    ├── memmap.cpp            maps 实现
    ├── hitfile.cpp           结果文件格式实现
    └── scan.cpp              搜索实现（分块 / 线程 / 落盘 / 合并）
```

## 依赖方向

单向，谁也不反向依赖：

```
memmap → rules → maps_line
scan   → hitfile / mem_reader / pagemap / thread_pool / memmap
hitfile / mem_reader / pagemap / thread_pool   ← 谁也不依赖
```

- 只想改分类 → 只碰 `memmap_rules.h`；
- 只想解析结果文件 → 只 `#include "hitfile.h"`，不碰搜索 / 内存 / 线程。

---

## 构建

**前置**

- Android NDK（`ndk-build`，含 arm64-v8a 工具链；作者在 r24 上验证）
- 目标 `arm64-v8a`，`APP_STL := c++_static`

```bash
cd jni
ndk-build            # 等价于 ../编译.sh（先把里面 NDK 路径改成你自己的）
```

**产物**

| 文件 | 说明 |
|---|---|
| `libs/arm64-v8a/aaa` | 解析 maps + 搜索 |
| `libs/arm64-v8a/aab` | 只解析结果文件（不含搜索 / 内存 / 线程） |

> `libs/`、`obj/` 是编译产物，建议加进 `.gitignore`。

---

## 快速开始

### `aaa` —— 解析 maps + 搜索

```
aaa [包名] [要搜的 Dword 值] [输出目录]
```

| 参数 | 默认 | 说明 |
|---|---|---|
| 包名 | `com.example.app` | 与 `/proc/<pid>/cmdline` 完全匹配 |
| 值 | 无 | 缺省只列 maps 不搜索；支持 `100` / `0x3f800000` |
| 输出目录 | `/data/local/tmp/scan` | 每块临时文件 + 合并结果 `hits.bin` |

```bash
aaa                                              # 只看类型分布
aaa com.example.app 100                          # 搜 dword 100
aaa com.example.app 0x3f800000 /sdcard/scan      # 搜 float 1.0 的位模式
```

输出示例：

```
进程 com.example.app pid=12345，读到 4049 个区域
...
搜索范围：4049 段, 2201633792 个地址

命中 44472332 条
块 12159 个，线程 8 个
扫描 96857088 个地址
总耗时 0.354655s

搜索值 0 (Dword) 0x00000000，命中 44472332 条
0x711d4004
0x711d400c
...
```

### `aab` —— 只解析结果文件

```
aab [结果文件] [显示条数]      # 默认 hits.bin / 20（0 = 全部）
```

```bash
aab                                     # 解析 hits.bin，前 20 条
aab /data/local/tmp/scan/hits.bin 100   # 前 100 条
aab hits.bin 0                          # 全部
```

> 读取别的进程内存需要 **root 或同 uid**；没权限时搜索会失败，maps 解析部分照常。

---

## 类型（短码）

```cpp
enum class Type : uint32_t {
    Unknown = 0,  Ch = 1<<0,  Jh = 1<<1,  Ca = 1<<2,  Cb = 1<<3,
    Cd = 1<<4,    A  = 1<<5,  S  = 1<<6,  F  = 1<<7,
    Xa = 1<<14,   Xs = 1<<15, J  = 1<<16, B  = 1<<17,
    PS = 1<<18,   As = 1<<19, V  = 1<<20, O  = 1<<21,
    All = ~0u,    // 通配
};
```

| 码 | 含义 | | 码 | 含义 |
|---|---|---|---|---|
| `Ch` | C 堆 | | `Xa` | 目标包代码 |
| `Jh` | Java 堆 | | `Xs` | 非目标包代码 |
| `Ca` | C 分配 | | `J` | Dalvik |
| `Cb` | C bss | | `B` | Bad |
| `Cd` | C 数据 | | `PS` | PPSSPP |
| `A` | 匿名 | | `As` | ashmem |
| `S` | 栈 | | `V` | 显存 / 驱动节点 |
| `F` | 字体 | | `O` | 其它 |

分类规则在 `memmap_rules.h` 的 `kRules[]`，**从上到下第一条命中生效**（顺序即优先级）；
加一条规则：插一行即可。

---

## API

### maps 解析（`memmap.h`）

```cpp
#include "memmap.h"
using namespace memmap;

pid_t pid = getPid("com.xxx");
MemMap m(pid);
if (!m.ok()) return 1;

auto all  = m.list(Type::All);                             // 全部
auto cd   = m.list(Type::Cd);                              // 某一类
auto rw   = m.list(Type::All, Perm::Read | Perm::Write);   // 类型 + 权限
auto libs = m.list(Type::All, {}, "lib");                  // 子串匹配 name / path

const Region* seg  = m.find("libc.so");        // 按名取
const Region* code = m.find_code("libc.so");   // 第一个可执行段
const Region* who  = m.which(addr);            // 地址反查（二分）
uintptr_t     base = m.addr("libc.so");        // 只要地址

m.dump(std::cout);                             // 全部区域，一行一条
m.dump_counts(std::cout);                      // 各类型计数
```

段大小统计（外部可显示，也用于搜索预分配）：

```cpp
SegStat st = m.stat_of(Type::All);   // 也可 stat_of(m.list(...))
std::cout << st;                     // "4053 段, 2205836288 个地址"
// st.count / st.bytes(原始字节) / st.span(4 字节对齐后可扫字节；显示时 /4 = 地址数)
```

> **约定**：`TypeSet` 里有哪些位就匹配哪些；**空集合 = 一个都不匹配**，
> 要「全部」用 `Type::All`。

### 搜索（`scan.h`）

```cpp
#include "scan.h"

auto segs = m.list(Type::All, Perm::Read);   // 搜索范围来自 memmap

scan::Options opt;
opt.out_dir          = "/data/local/tmp/scan";
opt.block_bytes      = 1u << 20;   // 1MB
opt.threads          = 0;          // 0 = 硬件并发
opt.skip_fault_pages = true;       // 默认开：查 pagemap，跳过不在内存的页
// opt.reserve_per_block = 4096;   // 每块命中缓冲预分配上限（字节）

scan::Scanner sc(pid);
scan::Report  rep;
sc.run(segs, scan::Value::dword(100), opt, rep);   // 或 scan::Value::f32(1.0f)
// 结果固定在 <out_dir>/hits.bin（没有就创建），路径在 rep.out_file
```

搜索流程：

1. **分块** —— 段的 `start/end` 先 4 字节对齐；> 1MB 的段按 1MB 切开，切点仍 4 对齐。
2. **搜索前预分配** —— 结果缓冲（每块按自身大小给上界，受 `reserve_per_block` 封顶）
   + 读缓冲 `N × 1MB`（无锁池一次分配好）。
3. **线程池搜索** —— 一块一个任务；借缓冲 → 读内存 → 按 `off += 4` 扫 4 字节值
   （地址天然 `&3 == 0`）；命中只写本块自己的缓冲 → 零共享、全程无锁；
   落盘文件名 = 该块范围地址。
   - `skip_fault_pages`（默认开）：先查 `/proc/<pid>/pagemap`，只读「在内存且未被换出」
     的页（连续可用的页成段读），其余整页跳过，**不碰会触发缺页的页**。
4. **按地址合并** —— 文件名（定宽十六进制）排序 = 地址序 → `mmap` 各块文件顺序拼；
   合并耗时记进 `Report::merge_ms`；最后删除临时文件。

`Report` 字段：

| 字段 | 说明 |
|---|---|
| `segs` / `blocks` / `threads` | 源段数 / 切出的块数 / 实际线程数 |
| `seg_bytes` / `scan_bytes` | 段原始字节 / 实际扫过的字节 |
| `hits` | 命中数 |
| `out_bytes` / `per_hit` | 结果文件字节 / 平均每条字节（看压缩效果） |
| `split_ms` / `scan_ms` / `merge_ms` / `total_ms` | 各阶段耗时 |
| `out_file` | 结果文件路径 |

### 结果文件读取（`hitfile.h`）

```cpp
#include "hitfile.h"

hitfile::ResultFile info;
hitfile::result_info("hits.bin", info);   // info.value / info.kind / info.count

hitfile::for_each_hit("hits.bin", [](uintptr_t addr, uint32_t value) {
    return true;                          // 逐条回调，流式不占内存；返回 false 提前停
});

hitfile::print_results("hits.bin", std::cout, 20);  // 前 20 条（0 = 全部）
hitfile::print_results("hits.bin", std::cout);

std::cout << hitfile::format_value(100, hitfile::Kind::Dword);  // "100 (Dword) 0x00000064"
```

`result_info` / `for_each_hit` 对**合并文件**（`MSC2`）和**单个块文件**（`SCB2`）都能读；
搜索类型存在文件头里（`kind`），所以 `print_results` 不用再传。

> 本节只依赖 `hitfile.h`（+ `hitfile.cpp`），**不需要** `scan.h` / `mem_reader` /
> `pagemap` / `thread_pool`。

---

## 结果文件格式

**紧凑二进制**。整轮搜索只查一个值，所以「值 + 类型」只存一次；命中只存偏移，
用 **LEB128 变长 + 差分** —— 1~3 字节/条（聚集命中常 1 字节）。

### 块文件 `<out_dir>/0x<start>-0x<end>.bin`

```
"SCB2"(4) | value u32(4) | base u64(8) | count u64(8) | kind u8(1) | pad(7) | payload
payload = count 个 varint：第 1 个 = off0，其余 = off_i - off_{i-1}
          off = addr - base，升序，off ≤ 1MB
kind    = 0:Dword 1:Float（只影响显示，比较始终按 4 字节位）
```

### 合并文件（各块按地址序拼接的 section 流）

```
"MSC2"(4) | value u32(4) | total_count u64(8) | kind u8(1) | pad(7)
每块：varint(base) | varint(count) | <该块 payload 原样>
```

读取：

```cpp
uint64_t off = 0;
for (每块) {
    base = varint();               // varint(base)
    n    = varint();               // varint(count)
    off  = 0;
    for (i = 0; i < n; ++i) { off += varint(); addr = base + off; /* 命中 */ }
}
```

---

## 设计要点

- **各司其职**：读内存 / 查 pagemap / 线程池 / 无锁缓冲 / 分类 / 搜索互不认识彼此的细节。
- **单向依赖**：见上文依赖图；换平台只改 `mem_reader.hpp`。
- **线程池**：线程只建一次、只销毁一次，任务反复投递。
- **无锁**：`BufferPool` 借还用原子栈（CAS）；每个任务只写自己的缓冲和文件，
  没有共享写，也就没有锁竞争。
- **无静默回退**：读失败 / 打不开 pagemap 就是失败，由调用方决定，不做悄悄兜底。

---

## 已知限制

- **需要权限**：读别的进程内存要 root 或同 uid；`MemMap::self()` 读自己不需要。
  开着 `skip_fault_pages` 时还要能读 `/proc/<pid>/pagemap`，打不开会直接失败。
- **只搜 4 字节**值（Dword / Float），地址一律 4 字节对齐、步进 4。
- **跳过的页搜不到**：不在内存 / 被换出的页按设计整页跳过，里面的值不会出现在结果里。
- **「搜索范围」≠「扫描」**：范围是所选段的地址总数（按 maps 统计，不判断可读性）；
  扫描是真正读回来的量。选了含大量不可读段（如 `---p` 保留段）的类型时两者会差很多；
  想让它们对齐就在选范围时加 `Perm::Read`，例如 `m.list(Type::O, Perm::Read)`。
- **合并走临时文件**：每块先落盘再按地址序拼接，最后删除临时文件。
- 类型分类规则按 Android 常见布局编写；不同 ROM / 厂商命名可能有出入，
  需要时改 `memmap_rules.h`。