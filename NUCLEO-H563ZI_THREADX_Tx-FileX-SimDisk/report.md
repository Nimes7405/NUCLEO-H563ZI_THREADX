# NUCLEO-H563ZI ThreadX Tx-FileX-SimDisk 复盘

## 目标

板载没有 SD 卡座和外部 Flash，但 [Application Note](../Application_note.md) 路线 2
（数据记录仪）需要 FileX + LevelX。做法是在片内 RAM 里模拟一块 NOR 闪存，
让完整的文件系统栈先跑起来：

- 不依赖任何外部介质即可验证 FileX API、FAT 布局、目录操作；
- 模拟真实 NOR 的物理约束，验证 LevelX 的扇区映射与磨损均衡确实在工作；
- 将来接真实 SPI NOR / SD 时只替换底层驱动。

底座从 `Tx-Reliability-Monitor` 复制，保留日志、看门狗、故障记录、状态台和自检。

## 结果摘要

- 构建：`text 146,844 / data 2,364 / bss 99,408`，`.bin` 149,216 B；
  其中 `app_fxdisk.o` 的 bss 为 77,528 B（模拟 NOR 64 KiB + 缓冲）。
- 启动：`FSD mount=formatted sectors=120 reason=1`（`reason=1` 即
  `FX_BOOT_ERROR`：空白 RAM 盘首次挂载失败后自动格式化）。
- 卷：`media=SIMDISK sectors=120 sector_bytes=512 free_bytes=58368`。
- `fs test`：写→读→比对→删除→确认读失败，全部 PASS。
- `fs fill 32`：32 KiB 写入 + 逐字节校验 PASS；`free_bytes` 58368 → 25600；
  `erases=5`，擦除计数分散到 e1/e2/e3/e4/e5。
- `fs stress 60`：127 次真实扇区写、123 次块分配、93 个作废扇区、2 次块擦除，
  每一步都读回校验，`violations=0`。
- `fs umount` → `fs mount` 后数据仍在；`fs format` 后目录清空。
- `./scripts/selfcheck.sh all`：`basic` 9、`fs` 19、`watchdog` 4、`fault` 5
  全部 PASS。

## 实现

### 组件与几何

- FileX：`Middlewares/ST/filex/common/src/*.c` 全量编译，port 用
  `ports/generic/inc`（该包没有 cortex_m33 port）。
- LevelX：只编译 `lx_nor_flash_*.c`，并排除 `lx_nor_flash_simulator.c`，
  改用本工程自己的模拟后端，便于控制容量和统计。
- 模拟 NOR：8 块 × 16 物理扇区 × 512 B。LevelX 每块占 1 个物理扇区做元数据
  （擦除计数、空闲位图、扇区映射表），因此可用 120 个逻辑扇区。
- 卷参数：1 个 FAT、64 个根目录项、1 扇区/簇、扇区 512 B。

### FileX 驱动映射

| FileX 请求 | 实现 |
| --- | --- |
| `FX_DRIVER_INIT` | `lx_nor_flash_open()`，并置 `free_sector_update = FX_TRUE` |
| `FX_DRIVER_UNINIT` | `lx_nor_flash_close()` |
| `FX_DRIVER_READ` / `WRITE` | 逐扇区 `lx_nor_flash_sector_read/write` |
| `FX_DRIVER_RELEASE_SECTORS` | `lx_nor_flash_sector_release`，让 LevelX 标记作废 |
| `FX_DRIVER_BOOT_READ` | 读 0 号扇区并校验 `EB 34 90` |
| `FX_DRIVER_FLUSH` / `ABORT` / `BOOT_WRITE` | 直接成功 / 写 0 号扇区 |

### 模拟 NOR 的写入语义

真实 NOR 只能把 1 写成 0，必须整块擦除才能恢复 1。模拟后端写入时执行：

```c
if ((value & ~current) != 0U) { sim_bit_violations++; value &= current; }
```

`violations` 不作为错误返回，只做观测——如果 LevelX 逻辑正常，它应该恒为 0。
实测 `fs fill 32` 与 `fs stress 60` 之后都是 0，说明确实每次先擦后写。

## 踩到的问题

### 1. FileX 没有 cortex_m33 port

`fx_api.h` 需要 `fx_port.h`，包内只有 cortex_m0/m3/m4/m7、generic、linux、win32。
H5 是 Cortex-M33，直接用 `ports/generic/inc` 即可（FileX 的可移植 port 不依赖
具体内核指令）。

### 2. 空白 RAM 盘首次挂载返回 `FX_BOOT_ERROR`

RAM 上电是 0x00 而不是擦除态 0xFF。模拟后端在第一次初始化时把整片填成
`0xFFFFFFFF`（模拟出厂擦除态），随后 `BOOT_READ` 读到的仍不是 FAT 引导记录，
于是 `fx_media_open` 失败，返回 `reason=1`。上层捕获后调用 `fx_media_format`
再挂载——这就是真实产品里"新盘需要格式化"的同一条路径。

### 3. FileX 扇区缓存让压力测试失真（最重要的坑）

最初 `fs stress` 的语义是"truncate → write 512 B → close"重复 N 次。
实测 60 次改写只产生了 **1 次**真实扇区写，`erases` 一直是 0，
看起来"磨损均衡没工作"。

原因是 FileX 把该文件所在的逻辑扇区一直留在 `media_memory` 缓存里：
改写只改了缓存内容，`fx_file_close()` 也不强制落盘，缓存不淘汰就永远不写介质。

修复：写入路径在 `fx_file_close()` 后显式 `fx_media_flush()`。此后：

| 指标 | 加 flush 前 | 加 flush 后 |
| --- | --- | --- |
| 60 次改写的驱动扇区写 | 1 | 127 |
| LevelX 块分配 | +1 | 123 |
| 作废扇区 | 1 | 93 |
| 块擦除 | 0 | 2 |

**教训**：验证磨损均衡/擦除行为时，必须确认写请求真的到达驱动层，
否则测的是缓存而不是闪存。为此在驱动里加了 `reads/writes/sectors` 计数，
`fs info` 直接可见。

### 4. `fx_system_initialize()` 必须在核内上下文调用

它内部会 `tx_timer_create` 创建 FileX 系统定时器，只能在
`tx_application_define()` 这类内核上下文里做。格式化/挂载则放到 console 线程
入口，避免在"还没调度"的阶段做 I/O。

### 5. `fs` 命令解析与栈深度

`fs write <name> <text>` 需要切分参数，用了 64 B 名 + 512 B 数据的静态缓冲，
避免占用 console 线程栈；同时把 console 栈从 3072 B 提到 4096 B，
给 FileX → LevelX 的调用链留余量。实测 `STACK console=` 仍有余量。

### 6. 厂商源码的一条 warning

编译 `fx_media_format.c` 会报 `-Wunterminated-string-initialization`（卷标初值
9 字节入 8 字节数组）。这是上游 6.x 源码自带的写法，本工程没有改厂商代码，
也不影响功能；如果要求零 warning，可对 FileX 源单独加
`-Wno-unterminated-string-initialization`。

## 尚未覆盖

- **掉电一致性**：RAM 盘复位即失，这一层无法验证。真实介质阶段需要单独设计
  （日志式写入 / 双索引 / 允许丢尾部 N 秒）并做断电注入。
- **擦除延时与取指停顿**：RAM 擦除是瞬时完成的，测不出真实 NOR 的擦除时间，
  也测不出擦除期间同 bank 取指的停顿。
- **容量与性能**：60 KiB 只能验证功能；真实介质要按容量重算簇大小、FAT 类型，
  并测写入吞吐。
- **并发**：目前所有文件操作都在 console 线程里串行执行，没有多线程同时读写
  同一媒体的测试（FileX 自带媒体互斥，但未实测）。

## 下一步

1. 有 SPI NOR / SD 卡后，替换 `app_fxdisk.c` 的底层读写为真实驱动，
   保持 FileX/LevelX 层不变，先跑同一套 `fs` 自检。
2. 接入 ADC/DMA 采集，把采样数据按文件轮转写入，形成路线 2 的完整数据记录仪。
3. 真实介质上做掉电注入与介质寿命评估。
