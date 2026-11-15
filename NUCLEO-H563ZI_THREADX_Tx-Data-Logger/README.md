# NUCLEO-H563ZI_THREADX_Tx-Data-Logger

**多通道数据记录仪：ADC 序列扫描 → GPDMA 交织缓冲 → 每通道窗口统计 → CSV 落盘。**

从 [`Tx-FileX-SimDisk`](../NUCLEO-H563ZI_THREADX_Tx-FileX-SimDisk/README.md)
（标签 `v1.0.0-single-channel`）派生，保留它的 RAM 模拟 NOR 盘、FileX/LevelX
栈、可靠性层与 GPDMA 诊断，把**单通道**采集升级为 **1–4 通道同步采集**。

对应 [Application Note](../Application_note.md) 路线 2 的多通道阶段：
真实介质仍未接，先用模拟盘把"多通道 → 落盘 → 回读校验"整条链路跑通。

## 与单通道基线的关系

| 方面 | SimDisk（单通道，已归档） | 本工程 |
| --- | --- | --- |
| 通道 | ADC1_INP10 / PC0 | ADC1_INP10..13 / PC0..PC3（1–4 路可选） |
| 转换方式 | 每次触发 1 次转换 | 每次触发转换整个 rank 序列 |
| 中断路径 | 可用 | **仅单通道**（多通道必须用 DMA，原因见下） |
| 环形缓冲 | 2048 样本 | 8192 样本（交织存放） |
| CSV | `idx,min,max,avg,count` | `idx,count,nch,min0,max0,avg0[,min1,...]` |
| 文件大小 | 2 KB | 4 KB（多通道行长约为 4 倍） |
| 自检 | 六组 | 七组（新增 `mch`） |

## 通道表

| 序号 | ADC 通道 | 引脚 | 备注 |
| --- | --- | --- | --- |
| 1 | `ADC_CHANNEL_10` | PC0 | Arduino A1 / CN9，**唯一接过信号验证的口** |
| 2 | `ADC_CHANNEL_11` | PC1 | 与 PC0 相邻的排针脚位 |
| 3 | `ADC_CHANNEL_12` | PC2 | |
| 4 | `ADC_CHANNEL_13` | PC3 | |

四路都在 GPIOC 上，`HAL_ADC_MspInit()` 一次性把 `PIN_0..PIN_3` 配成模拟输入。
用 `acq ch <1..4>` 选择通道数，选中的是**表里的前 N 个**，不是任意组合。
这些脚位目前都悬空，读数是引脚噪声（实测 min/max 在 300–860 之间跳动）；
接真实传感器请从排针引线，并确认所选脚位与通道表一致。

## 数据格式

每次触发转换 N 个通道，一个"帧"= 每通道一个样本。窗口固定 **500 帧**，
一行 CSV 记录一个窗口的全部通道统计：

```
idx,count,nch,min0,max0,avg0[,min1,max1,avg1...]
```

4 通道示例：

```
281,500,4,422,582,501,425,615,519,438,591,516,452,612,525
```

- `idx` 窗口序号，`count` 帧数（恒为 500），`nch` 通道数（1–4）
- 之后每通道 3 个字段：`min`、`max`、`avg`（12 bit，0–4095）
- `acq check` 逐行校验：字段数必须等于 `3 + 3*nch`、`nch` 与本次采集一致、
  `min ≤ avg ≤ max`、`max ≤ 4095`、`count == 500`

单文件 4 KB 轮转，`LOG0000.CSV`…`LOG0003.CSV` 循环覆盖。

## 为什么多通道必须用 DMA

一次触发会把整个 rank 序列连续转换完，每通道约 0.6 µs。EOC 中断从进中断到读 DR
远慢于这个间隔，于是在扫描序列下 EOC 只来得及反映**最后一次转换**，`OVR` 被置位，
其余通道的数据在寄存器里被覆盖。

实测（10 kHz、3 s、中断路径）：1 通道 32,498 次回调，2 通道 32,499 次，4 通道 32,499 次
——无论几路，每次触发只拿到 1 个样本。同一时刻的 DMA 路径是 33,000 / 66,000 / 132,000
样本，严格按 1×/2×/4× 缩放。

所以 `acq start irq` 在通道数 > 1 时直接拒绝：

```
ACQ start=fail mode=irq rc=-8
ACQ ERR multichannel-requires-dma
```

中断路径保留给单通道低速场景（也是单通道基线的回归项）。

## 软件结构

```
ADC 序列扫描（TIM3 每触发一次扫完 N 路）
        │
        ├─ GPDMA1 通道1：单节点循环链表，双半缓冲（半个 = 约 10 ms 的帧）
        │
        └─→ sampler 线程：把就绪半缓冲搬进交织环形缓冲(8192)
                          → 每 500 帧算一次每通道 min/max/avg
                          → 记录入 TX_QUEUE(32 深)，不碰文件系统
                                    │
                             writer 线程：每 50 ms 批量写出 + 一批一次 flush
                                    │
                             FileX → LevelX → RAM 模拟 NOR
```

- 半个 DMA 缓冲按**帧**定尺寸（`rate/100`，64–1024 帧），让半个缓冲正好覆盖
  一个 10 ms 线程 tick。这一步把 4 通道 100 kHz 的 `dma_ovr` 从 47 降到 0。
- 交织环形缓冲必须是 2 的幂（索引用掩码），编译期有两个 static assert 守住
  "2 的幂"和"至少放得下两个 4 通道窗口"。
- `app_fxdisk_lock()` 让 writer 线程与 `fs` 命令族串行访问 FileX。

## 板上已验证

| 项目 | 结果 |
| --- | --- |
| 构建 | `text 179,860 / data 2,396 / bss 141,320`，`.bin` 182,264 B |
| 启动 | `FSD mount=formatted sectors=120 reason=1` → `READY tx-data-logger 1.0.0` |
| 解交织自检 | `acq demux`：4 通道注入 `1000c..1000c+99` 斜坡，每通道 min/max/avg 全部命中，`ACQ DEMUX PASS` |
| 多通道拒绝 | `acq ch 2` + `acq start irq 10000` → `rc=-8` / `multichannel-requires-dma` |
| 2 通道 100 kHz | 3.3 s：`samples=668000 drop=0 dma_ovr=0 qdrop=0`，`acq check PASS channels=2` |
| 4 通道 50 kHz | 4.3 s：`windows=434 samples=868000 drop=0 dma_ovr=0 qdrop=0 flush=86` |
| 4 通道 100 kHz | 3.3 s：`windows=668 samples=1336000 drop=0 dma_ovr=0 qdrop=0`，即 **400 k 样本/秒零丢失** |
| CSV 内容 | `281,500,4,422,582,501,425,615,519,438,591,516,452,612,525` |
| 线程栈 | `sampler=400/2048`、`writer=1544/3072`、`console=1444/4096`、`monitor=1008/2048` |
| 一键自检 | `./scripts/selfcheck.sh all`：`basic` 10、`fs` 19、`acq` 42、`mch` 36、`dma` 20、`watchdog` 4、`fault` 5 全 PASS |

## 串口命令

USART3（ST-LINK 虚拟串口，PD8/PD9，115200 8N1）。文件系统命令与
[SimDisk](../NUCLEO-H563ZI_THREADX_Tx-FileX-SimDisk/README.md) 相同，采集部分为：

| 命令 | 作用 |
| --- | --- |
| `acq ch` | 打印当前通道数与上限 |
| `acq ch <1..4>` | 选择通道数（重建 ADC 序列并重新校准；采集中拒绝） |
| `acq start dma [hz]` | 多通道/高速采集，`hz` 1000–100000 |
| `acq start irq [hz]` | 单通道中断路径 |
| `acq stop` | 停止、写空队列、关闭文件 |
| `acq info` | 模式、采样率、`ch`、窗口/样本/文件数、min/max、`drop`/`dma_ovr`/`qdrop`/`flush`，以及 ADC `isr` 计数与 `SQR1/CFGR/ISR` |
| `acq check` | 回读最新日志并按 `nch` 逐通道校验 |
| `acq demux` | 确定性解交织自检（不依赖外部信号） |
| `acq selftest` | 采集 1.5 s → 停止 → 校验 |
| `dma env` / `dma test` | GPDMA 通道属性 / 受 TrustZone 影响的安全属性矩阵 |

## 一键自检

```sh
./scripts/selfcheck.sh basic     # 构建 + 符号 + 烧写 + 状态/栈/故障/日志
./scripts/selfcheck.sh fs        # 文件系统 19 项断言
./scripts/selfcheck.sh acq       # 单通道中断 10 kHz + DMA 50/100 kHz，42 项断言
./scripts/selfcheck.sh mch       # 解交织 + 中断拒绝 + 2/4 通道 DMA，36 项断言
./scripts/selfcheck.sh dma       # GPDMA 属性矩阵 + 采集自愈，20 项断言
./scripts/selfcheck.sh watchdog  # 线程卡死 → IWDG 复位
./scripts/selfcheck.sh fault     # BusFault 记录与重启报告
./scripts/selfcheck.sh all       # 七组顺序执行
```

可用 `PORT=/dev/ttyACM0` 覆盖串口。

## 已知限制

- 只有 PC0 接过真实信号；PC1–PC3 的读数是悬空噪声，通道表与排针脚位的对应关系
  需要按板子手册再核一次。
- 通道选择是"表里的前 N 个"，不支持任意组合；要任意组合需要把 `acq_chan_table`
  改成运行时可写，并处理各通道采样时间差异。
- 模拟盘掉电即失；RTC 未接，日志没有绝对时间戳。
- `fs fill` / `fs stress` 是调试命令，量产固件应裁掉。
- 4 通道 100 kHz 已零丢失，但不是硬实时保证：sampler 线程仍与 10 ms tick 对齐，
  再提高速率需要更高 tick 频率，或把采集搬出 RTOS 线程。

## 文档

- [report.md](./report.md)：多通道实现与踩坑复盘
- [Application Note](../Application_note.md)：方向、路线与验收标准
- [SimDisk README](../NUCLEO-H563ZI_THREADX_Tx-FileX-SimDisk/README.md)：单通道基线
