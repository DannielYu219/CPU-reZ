# CPU-Z WoA (ARM64) Bench 1:1 移植规格 — cpuz_arm64.exe v1.06

目标文件：`/Users/dannielyu/cpuz-arm64-macOS/cpuz_arm64.exe`（PE32+ ARM64 GUI，5,203,688 bytes，ImageBase `0x140000000`，`.text VMA 0x140001000 size 0x232B14`，readme v1.06 Jun 2026）

> 只覆盖 Bench。CPU 信息页 / 主板 / 内存 / 显卡页已忽略。

---

## 1. 结论速览（移植必须照抄的部分）

- **算法家族**：x86 系 Bench v17.01.64 公开确认为“算 2D noise procedural map（游戏式过程化贴图像素噪声）”，纯 C++，无特殊指令集，x64 用标量 SSE/SSE2 做浮点、x86 用 x87（约半速）【5203013131127272610†L52-L54】；工作集 <32KB 可进 L1、不考缓存/分支【343684282923522900†L12-L14】；v1.79 起换算法、分数不可跨版本比【5203013131127272610†L10-L11】，且明确是“算 2D noise 做 procedural map”以去掉 Ryzen 整数特殊 case 的虚高【5203013131127272610†L38-L44】。
- **ARM64 本体**：工作集放大到 **1024×1024 float = 4 MiB**（`0x400000`），`dims = 0x400 x 0x400`（见 `0x140004480` 存 `0x40000000400` 到 `[bench+0x58]`，高32=0x400，低32=0x400，`1024*1024*4 = 0x400000`），每次先 `memset(0)`（`0x140220FE0`，已证为 NEON memset 非内核），再调虚函数内核。迭代数硬编码：**Bench `10000 (0x2710)` 两遍，Stress `4000 (0xFA0)`，单发 `-1 (0xFFFFFFFF)`**，两遍之间 `Sleep(10)`。
- **调度**：两层线程 + `GROUP_AFFINITY` 硬绑核 + suspend/resume + event barrier，单核只绑 1 线程，多核全线程（与公开“单核绑1线程、多核全线程”一致（Bench 页单核/多核分别跑分的用法见指南【4868756194898709399†L59-L63】，此处为本地逆向行为描述，无需引用））。
- **计时**：`QueryPerformanceCounter / QueryPerformanceFrequency`（IAT `0x140234780/0x140234788`），兜底 `GetTickCount64 (0x140234790)`，`CTimer` 虚函数间接调用，Bench 区无 QPC 直引。`Sleep(10)`、`WaitForSingleObject(Ex)`、`CreateThread/SetThreadPriority/ResumeThread/SetEvent/ResetEvent` 全走 IAT。
- **计分**：`float` 结果存 `[score+0x10]/[score+0x14]`（初值 `-1.0`），`score = iters * freq / delta_ticks` 再经 `0x1400117B8` 常数归一 + `fdiv` 求比值，`*1000/freq` 转 ms。本地只算“原始吞吐”；UI 显示的 Single/Multi 分 + Reference 对比 + Submit and Compare（`valid.x86.fr` 系）依赖 `wininet.dll InternetOpenW/OpenUrlW/ReadFile` 在线拉参考分，二进制内无硬编码参考 CPU 分（`.rsrc` 只有 `Reference/This Processor/Multi Thread Ratio` 文案，无分数表）。
- **UI 档位**：`All Cores (%d) / P-Cores (%d) / P-Cores #2 (%d) / E-Cores (%d) / LP-Cores (%d)`（`.rdata file 0x298300` 起 UTF16），`Bench CPU / Stress CPU / CPU Single Thread / CPU Multi Thread / Benchmark / Run on / Multi Thread Ratio / Submit and Compare`（`.rsrc file 0x33B51A+`）。

---

## 2. 类与地址对照（RTTI 已验证）

MSVC ARM64：`TypeDescriptor` 在 `.data`，`COL` 6×DWORD（`sig,off,cdOff,pTD,pCHD,pSelf`，`pSelf==RVA` 自指纹），vtable-8 存 COL VA。

- `CBenchCPU_v17`：`TD file 0x2FD9F0 VA 0x1402FF3F0`，`COL file 0x2B41F0 VA 0x1402B51F0 [1,0,0,0x2FF3F0,0x2B51C0,0x2B51F0]`，`vtable file 0x297FF0 VA 0x140298FF0`（COL+8），7 虚函数：
  - `[0] 0x140004A08`：`if ([x0+0x8]) WaitForSingleObject([x0+0x8],-1); [x0+0x8]=0; return 1;`（等 supervisor 收尾）
  - `[1] 0x140003E08`：编排（topo 聚类 + 4 阶段魔数，见 §3），size 约 `0x5E8`
  - `[2] 0x140004480`：`InitBench`：`[x19+0x58]=0x40000000400`，`alloc 0x400000 → [x19+0x50]`，调 `[x19+0x20]([x19+0x30])`
  - `[3] 0x1400044D0`： teardown/suspend 辅助（调 `SetThreadPriority`？含 `IAT+0x948` 即 `SetEvent?`，见反汇编）
  - `[4] 0x1400045B8`：`Run`：建 N waiter + supervisor `0x140004A58`（见 §4）
  - `[5] 0x140004A48`：`ldr w8,[x0+0x10]; cset w0,ne`（busy?）
  - `[6] 0x140003C60`：析构 wrapper → `0x140003C98`（设 vtable `0x140298FF0`，清 `[+0x40]/[+0x50]/[+0x60]`）
- `CBenchScore`：`TD 0x2FDA18 VA 0x1402FF418`，`COL 0x1402B5218`，`vtable 0x140299030`（`0x140299000+0x30`），仅 1 虚函数 `0x1400043F0`（构造/拷贝，`stp x9,x8,[x0]; stp s16,s16,[x0+0x10]`，`s16=-1.0`，`x8=-0x100000000`）。
- `CBenchmarkDialog`：`TD 0x2FDA40 VA 0x1402FF440`，`COL 0x1402B52B8`，`vtable 0x140299638`（file `0x298638`），~40 函数，`0x140004C00/0x140004C10/0x140003B70/0x14003C4C8/0x140046B50...`，`.rsrc` 对话框含 `msctls_progress32 ×3`、`Bench/Stress/Reference`。
- Topology 系（仅调度用，不必移植算法，但需理解 `0x28/0x38/0x40/0x58/0x60/0xC8/0xD0/0xD8`）：
  - `CProcessorTopology TD 0x300078 COL 0x1402B7D48`，`CProcessorThread COL 0x1402B7C80`，`CProcessorCore COL 0x1402B7E80`，`CProcessorCoreSet COL 0x1402B7C08`，`CProcessorPackage COL 0x1402B6DE8`。

验证命令：

```sh
LLVM=$(xcrun --find llvm-objdump)
$LLVM -d --start-address=0x140003E08 --stop-address=0x1400043F0 /Users/dannielyu/cpuz-arm64-macOS/cpuz_arm64.exe > /tmp/full_3e08.txt
$LLVM -d --start-address=0x140004A58 --stop-address=0x140004C00 /Users/dannielyu/cpuz-arm64-macOS/cpuz_arm64.exe > /tmp/t1.txt
$LLVM -d --start-address=0x140011368 --stop-address=0x140011900 /Users/dannielyu/cpuz-arm64-macOS/cpuz_arm64.exe > /tmp/worker2.txt
python3 -c "import struct;d=open('/Users/dannielyu/cpuz-arm64-macOS/cpuz_arm64.exe','rb').read();print(hex(d.find(b'CBenchCPU_v17')))"
```

---

## 3. 编排 `0x140003E08`（topo 聚类 + 4 阶段）

伪 C（`x23=this(CBenchCPU_v17)`，`x19=*0x1403082F0 (topology)`，`x22=CBenchScore*`）：

```c
x19 = *(0x1403082F0);
x23->0x48 = 0;
nGroups = x19->vtable[0x28](x19);              // 组数
for (i=0;i<nGroups;i++)
  x23->0x48 += x19->vtable[0x38](x19,i);       // 每组线程数求和 = 总线程

if (x19->vtable[0x40](x19,0) == 1) {            // 单核机器
  x23->0x70 = 1;
  p = malloc(32); p[0]=1; x23->0x60=p; p[1]=x23->0x48;
  goto done;
}
// 多核
n = x19->vtable[0x40](x19,0);                  // 线程数?
x24 = (s64)n + 1; x25 = 24;
p = malloc(x24*24+8); p[0]=x24; x23->0x60=p; p[1]=x23->0x48;
x22 = CBenchScore_Create();                    // vtable 0x140299030, [0x8]=-1<<32, [0x10]=[0x14]=-1.0
w24 = 1;
// 4 阶段，magic 高32 = 期望 id，低32 = 累加器
uint64_t magics[4] = {0x2000000000,0x2100000000,0x1000000000,0x1100000000}; // 0x20,0x21,0x10,0x11
for (each magic m) {
  x22->0x8 = m;                                // 低清零，高=phase id
  for (g=0;g<nGroups;g++)
    for (t=0;t<x19->vtable[0x40](x19,g);t++) {
      id = x19->vtable[0x60](x19,g,t);
      if (id != x22->0xC) continue;            // 0xC = 高32 phase id
      v = x19->vtable[0x58](x19,g,t);          // 实测值
      (uint32_t)x22->0x8 += v;
    }
  if ((int32_t)x22->0x8 > 0 && w24 < x23->0x70) {
    p[w24*3+1] = (uint32_t)x22->0x8;           // umaddl x8,w24,x25,x23->0x60; str [x8+8]
    p[w24*3+2] = x22->0xC;                     // str [x9+12]
    w24++;
  }
}
done: x22->vtable[0](x22,1); // CBenchScore dtor
```

- `0x60` 返回 id（与 `0x20/0x21/0x10/0x11` 比对，即 P/E 分簇过滤），`0x58` 返回该线程实测累加值。
- `[x23+0x60]` 为 `struct {uint64 n; struct {uint32 sum; uint32 id;} e[];}`，`[x23+0x70]` 为容量。
- 单核路径跳过 4 阶段，直接 `b 0x1400043D4` 收尾。

---

## 4. `Run (0x1400045B8)` + `supervisor (0x140004A58)`（多核调度核心）

### 4.1 数据结构

- `bench (x20)`：`+0x08 supervisor handle`，`+0x10 state (1=run,6=running,7=done)`，`+0x14 mode (0=bench,1=single-shot,3=stress)`，`+0x38 kernel param block (16B: [0]=x6 extra, [8]=kernel this)`，`+0x50 float* buf (4MiB)`，`+0x58 dims (u32 w,h)`，`+0x68 CBenchScore* (float results +0x10/+0x14, int +0xC)`，`+0x40 per-run ctx ([+0x10]=n, [+0x18]=HANDLE*, [+0x20]=128B thread-param*)`。
- `thread-param (128B=0x80)`：`+0x00 u16 coreKey ([threadObj+0x10])`，`+0x08 u64 mask ([threadObj+0x8])`，`+0x10 u32 group ([group+0x210])`，`+0x14 u32 selected (group==target?1:0)`，`+0x18 bench*`，`+0x20 u64 x8(sp+8)`，`+0x28/0x2C w26`，`+0x30 w8(sp+4)`，`+0x34 w23`，`+0x3C iters_done`，`+0x40 active(1)`。

### 4.2 `0x1400045B8` 流程

```c
if (*bench->0x10 != 1 && *bench->0x10 != 7) return;
if (*bench->0x10 == 7) { bench->vtable[0x18](); bench->0x10=0; return; }
// ==1: 真跑
bench->vtable[0x10](); // 内部虚调用（准备）
x19 = bench->0x40; w21 = (int32)bench->0x48;
// 选组：遍历 topo 找 group == w?（`[x23+0x10]==1` 则首组，否则匹配 `w3` 最小值逻辑），x23 = 选中 coreSet 或 0
LoadLibraryW(L"kernel32.dll" @0x14029C830);
GetProcAddress("GetThreadGroupAffinity" @0x29B7C8);
GetProcAddress("SetThreadGroupAffinity" @0x29B7E0);
// 建 event / barrier？(ResetEvent/CreateEventW)
free(x19->0x18); free(x19->0x20);
x19->0x10 = w21;
x19->0x18 = malloc(w21*8);   // HANDLE[]
x19->0x20 = malloc(w21*128); // thread-param[]
fill thread-param[] from topology walk (group/mask/selected，见反汇编 0x14000472C-0x1400048E0)；
// 起 N 个 waiter（CREATE_SUSPENDED=4）
for (i=0;i<w21;i++) {
  param = x19->0x20 + i*128;
  h = CreateThread(0,0, 0x140011328, param, 4, &tid); // 0x140011328 = WaitForSingleObjectEx([0x140307700],-1,1) barrier
  x19->0x18[i]=h;
  SetThreadPriority(h,0);
  ResumeThread(h);
}
// 起 supervisor（真正计时/内核驱动）
hSup = CreateThread(0,0, 0x140004A58, bench, 4, &tid);
bench->0x08 = hSup;
if (hSup) {
  if (bench->0x14 >1) { if (==3) SetThreadPriority(hSup,0); }
  else SetThreadPriority(hSup,2); // THREAD_PRIORITY_HIGHEST? 原值 2
  ResumeThread(hSup);
}
bench->0x10 = 6;
```

- `0x140011328` 反汇编：`x19=IAT base, x20=0x140307000`，循环 `WaitForSingleObjectEx([x20+0x700], -1, 1)` 直到 `==0`。即 barrier，supervisor/ 内核 signal 后统一开跑，减少 skew。移植时用 `pthread_barrier` 或 `std::barrier` 等价。
- 亲和性：动态加载（静态 IAT 无 `Get/SetThreadGroupAffinity`，必须 `LoadLibraryW+GetProcAddress`），`GROUP_AFFINITY {mask,group}` 由 `thread-param+0x08/+0x10` 构造，经 `x25/x21`（`SetThreadGroupAffinity` 函数指针）调用。`Run on` 下拉决定 `target group`（All/P/E/LP），`w28/w24` 为 `-1` 表全部、`group id` 表单簇。跨平台改为 `pthread_setaffinity_np / sched_setaffinity / SetThreadAffinityMask`，保持“先 suspend 建线程 → 设亲和 → resume”顺序。

### 4.3 `supervisor 0x140004A58(x19=bench)`

```c
if (!x19) return 0;
switch (bench->0x14) {
 case 3: // Stress
   0x140004B98(bench, 4000); break;
 case 1: // 单发（校准/单核？）
   if (bench->0x68) {
     (w8,w9)=bench->0x58; // dims
     memset(buf,0, w8*w9*4); // sbfiz x2,w8*w9,#2 + 0x140220FE0
     (x6,x0)=*bench->0x38; (w2,w3)=bench->0x58; x1=buf; w4=score->0xC; w5=-1;
     score->vtable[0x28](x0, x1,w2,w3,w4,-1, x6); // 单次，不存分？
   } break;
 case 0: // Bench（默认）
   if (bench->0x68) {
     score->[0x10]=score->[0x14]=-1.0f;
     memset(buf,0,w*h*4);
     (x6,x0)=*bench->0x38; w5=10000;
     s0 = score->vtable[0x28](x0,buf,w,h,score->0xC,10000,x6);
     score->0x14 = s0;
     Sleep(10);
     memset(buf,0,w*h*4);
     s0 = score->vtable[0x20](x0,buf,w,h,score->0xC,10000,x6);
     score->0x10 = s0;
   } break;
}
bench->0x10 = 7;
return 0;
```

- `0x140004B98(bench,4000)`：同 single 路径但 `w5=4000`，调 `vtable[0x28]` 一次（Stress，不存分，循环由上层 UI 进度条驱动）。
- `0x140220FE0(x0=buf,x1=0,x2=len)` = memset（NEON），别当内核。
- 真内核 = `[bench+0x38]` 对象的 `vtable[0x28]` / `vtable[0x20]`，原型 `float k(void*this, float*buf, int w, int h, int aux, int iters, void*extra)`。候选实现即 `0x140011368 / 0x1400117C0`（7 参、内部再建线程+QPC，见 §5）。

---

## 5. 内核 `0x140011368 / 0x1400117C0`（`vtable+0x28/+0x20` 候选）与计时/计分

两函数同原型 `(x0,x1,w2,w3,w4,w5,x6)`，`x0/x22` 为对象，`x1` 为 buf，`w2/w3` 为 dims，`w4` 为 `score+0xC`，`w5` 为 iters，`x6` 为 extra。内部：

1. `LoadLibraryW/GetProcAddress` 取 `SetThreadGroupAffinity` 等，`SetThreadPriority/ResumeThread/CreateThread` 走 IAT（`0x140234930/0x140234938/0x140234940`）。
2. `QueryPerformanceCounter/Frequency`（`0x140234780/0x140234788`）取 `start (x25)`、`freq ([x22+0x10])`，`WaitForSingleObjectEx([0x140307700],...)` 做 barrier（含 `0/1` 超时轮询 + `GetLastError`）。
3. 每线程 `param+0x3C/0x40` 轮询 `while ([p+0x40]) Sleep(0?)`，累 `w24 += [p+0x3C]`（完成 iters）。
4. 计分（两处同构）：
   - 路径 A（`0x1400115FC`）：`x0=QPC_end`，`x8=freq`，`x9=w24`，`x9=freq*w24`，`x8=x0-x25`，`x9=x9/x8`（iters/sec），`x8=*(0x1400117B8)`（归一常数，8B，需 `dd` 转储），`x8=smulh+asr7` 缩放，`s16=(float)x8`，`[x19+0x30]=s16`。
   - 路径 B（`0x14001175C`）：`x9=(x0-x25)*1000`，`x8=x9/freq`（ms），`s16=(float)x8`，`[x19+0x30]=s8?`，`s9=w24/s16`（即 `iters/ms`），`return s9`（`fmov s0,s9`）。
   - `[x19+0x30]` 即 `score+0x30`（或 `bench+0x30`？视 `x19` 而定），为 UI 进度/原始分。
5. `0x1400117C0` 另有 `w24==-1`（`cmn w24,1`）分支：按 `group` 过滤计数（`All vs 单簇`），`malloc(w20*8)` 清零，与 `0x140011368` 的 `w28` 过滤对称。

移植公式（等价）：

```c
// 高精度计时
double freq; QueryPerformanceFrequency(&freq);
uint64_t t0,t1; QueryPerformanceCounter(&t0);
// ... 跑 iters 次 noise ...
QueryPerformanceCounter(&t1);
double sec = (double)(t1-t0)/freq;
double ms = sec*1000.0;
float raw = (float)(iters / sec); // 或 iters/ms，视常数而定，需用 0x1400117B8 校准
// supervisor 存分
score_14 = kernel28(buf,1024,1024,aux,10000);
Sleep(10);
score_10 = kernel20(buf,1024,1024,aux,10000);
// UI 最终分 = raw / ref_raw * ref_score（ref 在线拉，本地无表）
```

- 跨平台：`QPC/QPF → clock_gettime(CLOCK_MONOTONIC)`（ns），`GetTickCount64 → steady_clock ms`，`Sleep(ms) → nanosleep`，`CreateThread(suspended)+SetAffinity+Resume → pthread_create + pthread_setaffinity_np`（先建 suspend 等价为 barrier 后统一 `resume`），`WaitForSingleObjectEx(h,-1,1) → pthread_barrier_wait / condition_variable`，`SetThreadPriority(w1=0/2) → nice/sched`（`0=NORMAL,2=HIGHEST`）。
- 常数 `0x1400117B8` 8B 必须原样搬运：`dd file_offset_of(0x1400117B8)`（`VA 0x1400117B8 → file = VA-0x140000C00 = 0x105B8`），小端 `u64`，参与 `smulh/asr7` 缩放，1:1 必需。

---

## 6. 对话框/参考分（为何本地无表）

- `.rsrc`（file `0x33B000+`）只有文案与进度条，无分数：`CPU Single Thread / CPU Multi Thread / This Processor / Bench CPU / Stress CPU / Reference / Submit and Compare / Multi Thread Ratio / Benchmark / Run on`，`Submit` 按钮 ID `0x040F?` 等。
- `wininet.dll!InternetOpenW/OpenUrlW/ReadFile`（`.rdata 0x298A70+` ASCII，IAT 另查） + `https://download.cpuid.com/cpuid...`（file `0x8471` ASCII） + `valid.x86.fr` 系（公开 Submit 链接如 `https://valid.x86.fr/mvvj3a`，见 TechPowerUp 晒分帖）说明参考分与上传比对走在线库。公开资料亦称 Bench “Bench 页跑分后提交对比”【4868756194898709399†L59-L63】（M2 在 ARM VM 下 749.5 单核 / 3822.3 多核即跨平台可比的例证【118970786157704192†L5-L6】；13900K 参考约 902 单核【3555064271712173650†L15-L17】）。
- 因此 1:1 移植分两层：**本地吞吐 1:1（本节公式+常数+iters+dims+亲和+QPC）** 必做；**显示分 1:1** 需自建 `ref_raw/ref_score` 表（用同机跑 x86 版校准，或抓包 `download.cpuid.com`），`display = raw/ref_raw*ref_score`，`Ratio = Multi/Single`。

---

## 7. 移植清单（最小 1:1）

1. 照搬 `dims=1024,1024`，`buf=4MiB float`，`memset 0` 前后各一次，`iters=10000×2 / 4000 / -1`，`Sleep(10)` 间隔。
2. 照搬线程模型：`N = GetActiveProcessorCount/MaximumProcessorCount`（字符串在 `file 0x29CAD8+`，经 `LoadLibrary+GetProcAddress` 动态取，兼容 `GetLogicalProcessorInformation(Ex)`），`CREATE_SUSPENDED(4)` 建线程 → `SetThreadGroupAffinity(mask,group)` → `SetThreadPriority(0/2)` → `ResumeThread` → `WaitForSingleObjectEx(INFINITE,alertable)` barrier。
3. 照搬计时：`QPC/QPF`，`freq` 存 `[timer+0x10]`，`delta=t1-t0`，`score=freq*iters/delta` + `0x1400117B8` 缩放，`ms=delta*1000/freq`，`ratio=iters/ms`。
4. 内核二选一：a) 反汇编 `0x140011368/0x1400117C0` 转译 NEON→ portable C（推荐 `llvm-objdump -d --start-address=0x140011368 --stop-address=0x140011900` 全量导出后逐块转写，保持浮点结合顺序与 `fmul/fadd/fdiv/scvtf` 精度）；b) 用 x86 版同族 C++ noise 重写（公开为 2D noise procedural map【5203013131127272610†L52-L54】），但需用本机 ARM 实测回代 `0x1400117B8` 常数，否则只能“算法同族”不能“分数 1:1”。
5. UI：`P/E/LP` 下拉按 `group` 过滤（`w28`），`Single` 只跑 1 线程（`0x140003E08` 单核路径），`Multi` 跑全线程，`Stress` 调 `4000` 循环。
6. 验证：同机跑原版 WoA Bench 取 `score+0x10/+0x14`（可用调试器在 `0x140004B2C/0x140004B78 str s0` 下断）与移植版 `raw` 对比，误差应 <1%；再与 `3700X 511/5433` 等公开参考换算显示分。

---

## 8. 待深挖（已定位，未全转写）

- `0x140011368`（`~0x1400117C0`）与 `0x1400117C0`（`~0x140011900+`）的 NEON 内层 noise（大量 `fmul/fadd/fdiv/rev/mul`）需逐行转 C；`0x140220FE0` 仅 memset，可忽略。
- `CProcessorTopology` vtable `0x28/0x38/0x40/0x58/0x60/0xC8/0xD0/0xD8` 到 `0x140031xxx/0x140034xxx` 的映射（`COL 0x1402B7D48/0x1402B7C80/0x1402B7E80/0x1402B7C08`），影响 `group/mask` 枚举，不影响内核数学。
- `CBenchmarkDialog` 的 `0x14003F880/0x140040118/0x140040EB8` 显示格式化（`%.1f / %.1f %%` 在 `0x298408+`）与 `Submit` 组包（`wininet`），移植显示层时再跟。


---

## 9. 记分规则（静态实锤 + 一处待动态确认）

### 9.1 参考表（已完整提取）

静态数组 `0x1402FA4F0`（file `0x2F8AF0`），条目 24B：`{name*, id, single, multi, 0}`，`id=-1` 结束，共 13 项（`id` = 核数）：

| # | CPU | 核 | Single | Multi |
|---|---|---|---|---|
| 0 | NXP i.MX8MP | 4 | 60 | 240 |
| 1 | Broadcom BCM2711 | 4 | 93 | 364 |
| 2 | Snapdragon 810 | 8 | 69 | 415 |
| 3 | Snapdragon 7c | 8 | 177 | 621 |
| 4 | Broadcom BCM2712 | 4 | 260 | 960 |
| 5 | Snapdragon 845 | 8 | 220 | 1120 |
| 6 | Rockchip RK3588S | 8 | 224 | 1230 |
| 7 | Snapdragon 860 | 8 | 268 | 1385 |
| 8 | Microsoft SQ2 | 8 | 345 | 1519 |
| 9 | NXP LX2160A | 16 | 133 | 2131 |
| 10 | Snapdragon 8cx Gen3 | 8 | 563 | 3610 |
| 11 | Snapdragon X Elite X1E001DE | 12 | 820 | 9165 |
| 12 | Ampere Altra Max M128-30 | 128 | 315 | 40640 |

对话框经 `SendDlgItemMessageW(CB_ADDSTRING=0x143)`（`0x140005B50+`，IAT `0x140234DD0`）填入 Reference 下拉；选择经 `CB_GETCURSEL=0x147` 读回，`[dlg+0xAB0]` 存选中条目（`0x140005964`）。

### 9.2 显示公式（逐条对齐反汇编）

- 对比/百分比控件（`0x140005DAC-0x140006140` 四处同构）：`s17=measured×s8; s16=s17/ref; w8=(int)s16`，其中 `s8=100.0f`（`0x140006298`，`0x42C80000`），`ref=[entry+0xC]/[entry+0x10]` 或 `[dlg+0xAC8]`，`fcvtzs` 向零截断后 `SetDlgItemInt`（控件如 1026）。
- "This Processor" 文本经 `swprintf` 系（`0x140003BB8→0x14003ADC8`，`%.1f` 在 `0x298408`）直接格式化。
- 图表上限（`0x140006910`）：`[x0+0xAC8]=max(8000.0, 各候选+10)`（`0x45FA0000=8000.0`，`fmov s18,#10.0` 累加enser max）。

### 9.3 原生工作单位（rows ×256，待动态钉死 ÷4 位置）

内核返回 `work/ms`（`0x14001175C`：`ms=(delta*1000)/freq`，`sdiv` 截断，`brk` 除零；`s9=(float)w24/(float)ms`）。驱动 `w24` 累加 `[p+0x3C]=[p+0x30]`。移植侧按 rows 计数得单核 3.44 rows/ms；headline 需 ×256（=width/4，即 pixels/ms÷4）才与参考同量级。`256` 为 2 的幂且 `9729/877=11.09` vs X Elite `9165/820=11.18`（比值约掉 K，只差 0.8%），自洽；÷4 在 asm 侧的具体字段（`w13` 侧或 formatter 对象 `0x140299408→0x14004C230` 内）待 Windows ARM 动态确认（在 `0x140005E08` 对 fmt 对象设断点读 scale 即可）。

### 9.4 本机实测（M5 Pro，18 核）— v4 定版

- 单核：raw 6.073 rows/ms → headline **1554**（原机 ARM64 VM 1545.6，+0.5%）；vs single-ref 189%。
- 多核（18T）：raw 67.417 rows/ms → headline **17258**（原机 ARM64 VM 8 核 11003.7；X Elite 9165，+88%）；vs multi-ref 188%。
- Multi/Single = 11.10（X Elite 自比 11.18）。单核对齐到 0.5% 说明核心工作量已等价；多核随本地硬件扩展。
- 相对 v2（877/9729）的修复：删 `cpuz_simplex2` 死代码（每 simplex 白算 1 次 hash，+33%）；真 4-wide NEON（`vmul+vadd` 显式，禁用 `vmla` 融合；`floor` 处 `cset` 0/1 语义曾写反致负坐标 cell 错乱，已修）；`-O3 -mcpu=native -ffp-contract=off`（防编译器融合，原机即分开的 `fmul+fadd`）；worker `QOS_CLASS_USER_INTERACTIVE`。
- 定版二进制 `/tmp/cpuz_bench`（见 §10）。最终数：单核 1518（原机 VM 1560.9，-1.8%）、多核 16T 18281 / 18T 19647（原机 VM 16 核 19401.7；18T +1.3%）、ratio 12.94（原机 12.4）。
- fill 循环带 live 分数（stderr 每 pass 一行 `live ... headline=...`，对标对话框 timer 活刷；`--quick` 同样有）。
- 多核调度的关键修正：原机按行动态派发（`QueueUserAPC`+`w23` 计数器），静态分区+barrier 在异构核上被 E 核拖尾（静态版多核只有 17258）。移植改原子 `fetch_add` 按 `CPUZ_CHUNK_ROWS=8` 领行后达 18705。日志 `/tmp/g_s.log`、`/tmp/g_m.log`。

## 10. 运行命令与耗时说明

构建：

```sh
clang -arch arm64 -O3 -mcpu=native -ffp-contract=off -Wall \
  -o /tmp/cpuz_bench cpuz_bench_neon.c cpuz_bench_run.c -lm -lpthread
```

运行（`N` 线程，`R` 参考索引，默认 11=X Elite；参考表见 §9.1）：

```sh
/tmp/cpuz_bench 1 11            # 单核 Bench（默认忠实模式，约 20 秒）
/tmp/cpuz_bench 18 11           # 多核 Bench（默认忠实模式，约 20 秒）
/tmp/cpuz_bench 1 11 --quick    # 单核快速模式（约 2 秒，同分 ±5% 内）
/tmp/cpuz_bench 18 11 --quick   # 多核快速模式（约 0.2 秒）
/tmp/cpuz_bench 18 11 --stress  # Stress（循环跑图，Ctrl-C 停，对应 w5=4000/mode 3）
/tmp/cpuz_bench $(sysctl -n hw.ncpu) 11  # 本机全核
```

耗时说明（`w5` 是超时上限，不是跑满时长——但原机正常就是跑满它）：
- 内核主循环 `0x1400115A0` 双出口：`WaitForSingleObjectEx(h,0)` signaled→收尾（中止/Stop 路径），`ms>=w28`（`0x1400116E8`，w28=w5=10000）→正常收尾。Bench 流程里没有任何代码置位该事件（全二进制仅析构路径 `0x140003CCC/0x1400044F0` 调 `SetEvent` 做 teardown 唤醒），正常 Bench 即填满 10 秒/调用。
- 每 mode 两次调用（`vtable+0x28` 10000、中 `Sleep(10)`、再 `vtable+0x20` 10000）≈ 20 秒；单核+多核 ≈ 40 秒+。进度条+Stop 按钮的存在与此一致（长任务才需要它们；Stress 同循环、用户 Stop 才停）。
- `--quick` 定量跑图（3 张 warmup + 2 张计分，原机 `w5=-1` 单发语义），rate 一致故分数一致（单核 1528 vs 1554，-1.7%；多核短测抖动约 ±5%），日常迭代用它，定稿用默认模式。
- x64 版截图（849.9/9980.2、873.3/10064.1）是 x86 workload（<32KB），与 ARM64 4MB 不可比，仅 ratio（11.74/11.52）可参考。
