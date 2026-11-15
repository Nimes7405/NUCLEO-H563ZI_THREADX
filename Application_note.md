# Application Note — 从 ThreadX Secure 模板到实际产品

> 面向对象：准备把本工程（NUCLEO-H563ZI + ThreadX 6.4.0 + 单镜像 Secure 模板）
> 当作**产品原型底座**继续往下做的人。
> 本文总结"现在能做什么、还差什么、可以往哪些方向做、每条路的工作量与风险"。
>
> 涉及 IP 网络的方向（DHCP/TCP/UDP、TLS、HTTP、网络 OTA）请看
> [NetXDuo Application Note](../NUCLEO-H563ZI_NetXDuo/Application_note.md)。
> 本文只讨论 ThreadX 这一层，以及**不依赖 IP 网络**的应用方向。

---

## 1. 工程现状基线（已复核，不是"应该能跑"）

本 workspace 的基线是
[`NUCLEO-H563ZI_THREADX-SECURE_TEMPLATE`](./NUCLEO-H563ZI_THREADX-SECURE_TEMPLATE/README.md)：
单镜像、Secure 状态运行、ThreadX + HAL、SysTick 100 Hz、HAL 1 ms 走 TIM6、
静态线程 + LED3(PG4) 心跳。

| 能力 | 状态 | 证据 |
| --- | --- | --- |
| 构建（`-O0`） | ✅ 本会话复核 | `make -j8` 成功；ELF 228,316 B，`.bin` 15,052 B |
| 资源占用 | ✅ 本会话复核 | `text 15,036 B / data 16 B / bss 4,360 B`（含 1,540 B heap+stack 预留） |
| 250 MHz 时钟树 | ✅ 代码/构建 | `main.c`：VOS0、HSE bypass digital、PLL1 250 MHz、`FLASH_LATENCY_5`、`PROGRAM_DELAY_2` |
| ThreadX 6.4.0 + Cortex-M33 GNU port | ✅ | Cube H5 V1.7.0；`TX_SINGLE_MODE_SECURE=1` 同时用于 C 内核与汇编端口 |
| SysTick 100 Hz 归 ThreadX | ✅ 配置 | `tx_initialize_low_level.S`：`SYSTICK_CYCLES=(250M/100)-1`；`tx_user.h` 中 `TX_TIMER_TICKS_PER_SECOND=100` |
| HAL 1 ms 时基 TIM6 | ✅ 配置 | `stm32h5xx_hal_timebase_tim.c` + `TIM6_IRQHandler`，与 SysTick 分离 |
| 静态线程 + LED3(PG4) 心跳 | ⚠️ 代码就绪，本会话未烧写 | `app_threadx.c`：512 B 栈、优先级 10、时间片 5 tick；同板 USBX HID 工程已实测 ThreadX tick 增长 |
| 中断/异常入口 | ✅ 链接 | ThreadX 端口提供 SVC/PendSV/SysTick；HAL 只保留 `TIM6_IRQHandler` |
| 串口日志 | ❌ | `syscalls.c` 的 `_write()` 直接返回 -1（无输出）；同族 Thread-Sync 已实现 USART3 重定向 |
| 看门狗 | ❌ | 未启用 IWDG |
| 故障记录/恢复 | ❌ | `HardFault_Handler()` 与 `Error_Handler()` 都是死循环（后者还关中断） |
| MPU 隔离 | ❌ 模板未配置 | 模板只有默认 map；派生项目 [`Tx-Control-Node`](./NUCLEO-H563ZI_THREADX_Tx-Control-Node/README.md) 的 P3-lite 已加静态 MPU + 5 个栈 guard |
| 动态内存 | ❌ | 无 ThreadX 字节池，`first_unused_memory` 未使用 |
| 双镜像 / NonSecure | ❌ 不在本模板 | 见 [`SecureLED-TrustZone`](../NUCLEO-H563ZI_THREADX-SECURE_Tx-SecureLED-TrustZone/report.md) |

**资源占用（本会话 `-O0` 构建）**

| 项 | 实测 |
| --- | --- |
| Flash（`.bin`） | 15,052 B ≈ 14.7 KB |
| RAM（`.data` + `.bss`） | 4,376 B ≈ 4.3 KB（其中 1,540 B 是 heap+stack 预留） |
| 可用分区 | Flash 2 MB / RAM 640 KB（SRAM1 256K + SRAM2 64K + SRAM3 320K） |

余量极大，后续加 FileX/USBX/日志缓冲不需要先做裁剪；走双镜像时按 NetXDuo 的
1 MB Flash / 320 KB SRAM 分法预留。

**同板已验证的 ThreadX 能力（可作为"内核确实在跑"的证据）**

| 项目 | 已验证程度 | 关键证据 |
| --- | --- | --- |
| [USBX HID](../NUCLEO-H563ZI_USBX_Ux-Device-Human-Interface/report.md) | 板上验证 | ThreadX tick 持续增长、USB HID 枚举与按键上报成功；记录了 `INVPC` 模式不匹配的完整排障过程 |
| [Tx-Thread-Sync](../NUCLEO-H563ZI_THREADX_Tx-Thread-Sync/report.md) | 板上验证 | 双线程 + mutex/semaphore 轮转，USART3 输出序列符合预期 |
| [Tx-FreeRTOS-Wrapper](../NUCLEO-H563ZI_THREADX_Tx-FreeRTOS-Wrapper/report.md) | 烧录/校验/复位 | 运行行为未用调试器确认（报告已明确边界） |
| [SecureLED-TrustZone](../NUCLEO-H563ZI_THREADX-SECURE_Tx-SecureLED-TrustZone/report.md) | 构建/链接/烧写流程 | 双镜像、CMSE 导入库、安全栈；板上验证待执行 |

> 结论：ThreadX 内核与 TrustZone 启动链在同板上是被证明过的；本模板缺的是
> "产品化外壳"（日志、看门狗、故障记录、隔离、自检），不是内核可用性。

### 1.1 已落地的派生项目

| 项目 | 覆盖 | 状态 |
| --- | --- | --- |
| [`Tx-Reliability-Monitor`](./NUCLEO-H563ZI_THREADX_Tx-Reliability-Monitor/README.md) | 方向 G（P1）+ 缺口 #1–#4、#10、#12 | 板上验证：`selfcheck` basic/watchdog/fault 全通过；205 s 稳定运行；故障记录与 IWDG 复位均实测 |
| [`Tx-Control-Node`](./NUCLEO-H563ZI_THREADX_Tx-Control-Node/README.md) | 路线 1 P2（方向 A）+ P3-lite（方向 F）+ P4 release/校准 + 方向 G | 板上验证：TIM1 20 kHz / 控制 10 kHz、抖动 spread 0.1–3.2 µs；P3-lite 静态 MPU、guard 归因、安全模式、分级 WWDG；P4 `cal` 参数校准 + **片内 Flash 非易失保存**（写入后复位/重刷保持；写入中掉电未验收，见 §7）、`APP_PROFILE=release` 裁掉破坏性命令；`selfcheck` 七组全通过；1 h O0 soak PASS；O2 `selfcheck all`、O2 15 min soak、`release_check.sh` PASS |
| [`Tx-FileX-SimDisk`](./NUCLEO-H563ZI_THREADX_Tx-FileX-SimDisk/README.md) | 路线 2 软件栈（方向 B 的 FileX + LevelX + 采集落盘）+ 方向 G | 板上验证：RAM 内模拟 NOR（8 块 × 16 扇区 × 512 B）承载 **FileX + LevelX**，FAT 卷 120 扇区；`fs fill 32`、`fs stress 60` 触发 123 次块分配 / 93 个作废扇区 / 2 次块擦除，1→0 违规恒为 0；**采集链路**：TIM3 10 kHz 触发 ADC1 → 环形缓冲 → 50 ms 窗口 → CSV 落盘 → 2 KB 轮转 → `acq check` 回读校验，实测 30,500 样本/3 s、`drop=0`；`selfcheck` 五组（basic/fs/acq/watchdog/fault）全通过 |

> 后续路线可以直接从这两个项目派生：可靠性层（日志、心跳、看门狗、故障记录、
> 状态台、自检）和控制环骨架（PWM/ADC/DWT/PI）都已经就绪。

---

## 2. 从"能跑"到"能用"还差什么

按优先级排列，这些是任何实际应用都会先撞上的问题：

| # | 缺口 | 说明 / 建议做法 |
| --- | --- | --- |
| 1 | **故障可观测** | `HardFault/UsageFault/BusFault/MemManage` 记录 `CFSR/HFSR/MMFAR/BFAR/PC/LR/EXC_RETURN` + 复位原因，复位后经日志或状态台读出；不要只留 `while(1)` |
| 2 | **看门狗与恢复** | IWDG 用 LSI 独立时钟，由最高优先级监护线程按"心跳位图"喂狗（不是每个线程各自喂）；故障后"记录 → 复位"，不要死等 |
| 3 | **日志系统** | 现在是空 `_write`。改成环形缓冲 + 独立输出线程 + USART3（PD8/PD9, AF7），Release 下分级裁剪；可选 RTT/SWO |
| 4 | **栈与堆可观测** | 线程栈填充 `0xA5` + 水位扫描；开 `TX_ENABLE_STACK_CHECKING` + `tx_thread_stack_error_notify`；`_Min_Heap_Size` 只有 `0x200` |
| 5 | **优先级与周期设计** | 现在只有心跳线程。真实项目要给出优先级表（ISR → 控制 → 采集 → 通信 → 日志 → 空闲）与栈实测；**100 Hz tick 不能当控制周期** |
| 6 | **时间基准** | ThreadX tick / HAL tick / DWT 周期计数器三者分工，统一时间戳来源；跨时基比较要用差值而不是绝对大小 |
| 7 | **低功耗** | 本包 ThreadX **没有** tickless/low-power 模块；进 STOP2 前后要自己补偿 tick，或改由 RTC/LPTIM 记时 |
| 8 | **MPU 隔离** | 本包也**没有** ThreadX MPU 扩展；需在线程切换点重配 region，或先用静态分区 + 断言过渡 |
| 9 | **构建与版本** | `-O0` → `-O2`/`-Os`；固件版本 + git hash 编入镜像并可查询；端口依赖 GNU 扩展，保持 `-std=gnu99` |
| 10 | **复位策略** | 区分上电/看门狗/软件/引脚复位；把复位原因写进日志；避免"复位后不知道为什么复位" |
| 11 | **TrustZone 服务** | 若走双镜像：NSC 入口参数校验、安全栈需要可用 malloc 堆、NS 外设要过 SECCFGR/PRIVCFGR/ITNS 三项 |
| 12 | **自检与回归** | `scripts/selfcheck.sh`：build → flash → SWD 读 tick/PC → UART 期望序列 → 结果汇总；优先于手工步骤 |

---

## 3. 可能的实际应用方向

下面每条都按 **场景 / 为什么适合本硬件 / 要补什么 / 现有基础 / 工作量·风险** 展开。
可直接取用的组件（本地 Cube 包内已核对）：

- ThreadX 6.4.0：线程/信号量/互斥量/事件标志/消息队列/字节池/定时器；`tx_trace_enable`、`tx_thread_stack_error_notify`
- FileX 6.4.0 + LevelX 6.4.0（文件系统 + Flash 磨损均衡）
- USBX 6.4.0（Device/Host 类：CDC/MSC/HID 等）
- CMSIS-RTOS2 兼容层：`cmsis_rtos_threadx` + [`Tx-FreeRTOS-Wrapper`](../NUCLEO-H563ZI_THREADX_Tx-FreeRTOS-Wrapper/report.md)
- `Middlewares/Third_Party/`：`mcuboot`、`mbed-crypto`
- 需要 IP 网络时，直接转到 [NetXDuo workspace](../NUCLEO-H563ZI_NetXDuo/README.md)，不要在本线重复造网络栈

> 本包内**没有 GUIX**；需要图形界面时需另行引入。

### 方向 A：实时控制节点（PWM + ADC + 控制环） ★★★★★

- **场景**：电机、电源、机器人关节、温控，1–20 kHz 控制环，要求低抖动。
- **为什么适合**：TIM1/TIM8 高级定时器（互补 PWM + 死区 + BKIN 刹车）、ADC1/2 12-bit、GPDMA、250 MHz + FPU；控制环可完全由硬件触发链驱动，CPU 只做计算。
- **要补什么**：中心对齐 PWM + 死区；TIM TRGO 触发 ADC 注入组；电流/电压环 PI（可用 CMSIS-DSP）；控制环放最高优先级线程或 TIM ISR；刹车与故障恢复；参数在线整定（可选 USB/UART）。
- **现有基础**：裸机 [TIM-PWMOutput](../../NUCLEO-H563ZI_TIM-PWMOutput/README.md)、[TIM-InputCapture](../../NUCLEO-H563ZI_TIM-InputCapture/README.md)、[ADC](../../NUCLEO-H563ZI_ADC/REPORT.md)、[DAC 信号发生](../../NUCLEO-H563ZI_DAC_SIGNAL-GENERATION/README.md)；ThreadX 线程/同步模型。
- **工作量/风险**：3–5 周；风险在功率级硬件、EMC 与实时性实测（需要示波器/逻辑分析仪），不在 RTOS。

### 方向 B：多通道数据记录仪（FileX + LevelX） ★★★★★

- **场景**：现场无网络，长时间记录振动/温度/电流，事后插卡或连 USB 取数。
- **为什么适合**：ADC + GPDMA 循环双缓冲；SDMMC1/2 或 SPI NOR；FileX + LevelX 已在包内。
- **要补什么**：**板载没有 SD 卡座和外部 Flash**，真实介质需外接（SD 卡座或 SPI NOR）；但 **FileX + LevelX 软件栈现在就能验证**——[`Tx-FileX-SimDisk`](./NUCLEO-H563ZI_THREADX_Tx-FileX-SimDisk/README.md) 在 RAM 里模拟 NOR 的擦除/1→0 语义，已跑通 FAT、扇区映射与磨损均衡；真实介质阶段只需替换底层驱动。之后要补：掉电一致性（定期 flush + 双备份索引 + 允许丢尾部 N 秒）、文件轮转与容量策略。
- **现有基础**：DMA 工程、ADC 工程、ThreadX 队列/事件标志。
- **工作量/风险**：4–6 周；风险在掉电一致性与介质寿命，必须做"随机断电 ×20"验收。

### 方向 C：USB 仪器 / 复合设备（USBX） ★★★★☆

- **场景**：设备用 USB-C 接 PC，同时提供 CDC 命令行 + MSC 数据盘 + HID 控制。
- **为什么适合**：板载 USB-C（`USB_DRD_FS`）；已有可运行的 [USBX HID 样例](../NUCLEO-H563ZI_USBX_Ux-Device-Human-Interface/README.md)；USBX 自带 CDC/MSC 类。
- **要补什么**：复合设备描述符与类组合；CDC 命令解析（可复用日志/状态台）；MSC 后端接 FileX；Windows/Linux/macOS 兼容性矩阵；枚举稳定性长跑。
- **现有基础**：HID 样例已包含 USB-PD 附着检测、PCD、DMA 与内存池配置，可直接作为起点。
- **工作量/风险**：3–4 周；风险主要在描述符与主机兼容性，不在内核。

### 方向 D：低功耗电池节点 ★★★★☆

- **场景**：电池供电，周期性采集/记录，占空比 <1%。
- **为什么适合**：STOP2/STANDBY、RTC、LPTIM1–6；STOP2 下可保留 SRAM。
- **要补什么**：**本包 ThreadX 没有 tickless/low-power 模块**，需要自己做：进 STOP 前计算下一次定时器到期、停 SysTick、唤醒后补偿 tick（或 STOP 期间用 RTC/LPTIM 记时）；GPIO/外设低功耗配置；唤醒源去抖；功耗实测。
- **现有基础**：[RTC ALARM 裸机工程](../../NUCLEO-H563ZI_HAL-TimeBase-RTC-ALARM/README.md)、ThreadX 定时器。
- **工作量/风险**：3–5 周；风险在 tick 补偿正确性与功耗测量（需要电流表/功耗分析仪）。

### 方向 E：本地 / 现场总线 OTA ★★★★★

- **场景**：无 IP 网络的设备，通过 UART/CAN-FD/USB 升级，要求签名校验 + 掉电安全 + 可回滚。
- **为什么适合**：TrustZone 双镜像 + Secure 侧验签（HASH/PKA）；包内有 `mcuboot`、`mbed-crypto`；NetXDuo Telemetry-Node 已有完整 A/B + 回滚 + 断电注入实现可复用。
- **要补什么**：传输层（分包/CRC/重传/窗口）；Secure 侧验签与槽管理；镜像头（版本/长度/哈希/签名）；升级期间看门狗策略；产线密钥注入。
- **现有基础**：[SecureLED 双镜像](../NUCLEO-H563ZI_THREADX-SECURE_Tx-SecureLED-TrustZone/report.md)、`Secure_nsclib`/CMSE 流程、NetXDuo 的 OTA 经验。
- **工作量/风险**：4–6 周；风险在"变砖"边界设计，必须做掉电注入 20 次。

### 方向 F：容错与隔离（MPU + 故障恢复） ★★★★☆

- **场景**：应用线程增多，一个协议解析/插件线程跑飞不能拖垮整机。
- **为什么适合**：Cortex-M33 MPU；ThreadX 线程切换点固定，便于做 region 切换。
- **要补什么**：**本包没有 ThreadX MPU 扩展**，需在线程切换钩子/端口里重配 MPU；region 规划（代码只读、线程栈/数据区、外设区、禁执行）；故障记录 + 复位策略；分级看门狗（系统级 IWDG + 线程级心跳）。
- **现有基础**：模板线程模型；`NUCLEO-H563ZI_THREADX_TIPS.md` §5 的分层排障方法。
- **工作量/风险**：4–6 周；风险是 MPU 配置错误导致难以定位的 fault，**必须先有故障记录再做隔离**。

### 方向 G：可观测性与确定性基线 ★★★★★（建议先做）

- **场景**：任何产品上线前都要能回答"现在跑得好不好、出问题怎么查"。
- **为什么适合**：改动小、收益最大；成果可被所有派生项目（**包括 NetXDuo 项目**）继承。
- **要补什么**：环形日志 + 独立输出线程 + USART3；栈水位（填充 + 扫描 + `tx_thread_stack_error_notify`）；`TX_ENABLE_EVENT_TRACE` + `tx_trace_enable`（用 RAM 换可见性）；DWT->CYCCNT 测 ISR 延迟与线程唤醒抖动；状态命令（uptime、复位原因、各线程栈余量、队列深度）。
- **现有基础**：Thread-Sync 的 `_write` → USART3 重定向；模板的 SysTick/TIM6 分工；USBX HID 报告里的 `CFSR`/`EXC_RETURN` 排障方法。
- **工作量/风险**：1–2 周；低风险，建议作为 P1。

### 方向 H：教学 / 评估 / 回归基线 ★★★★★（现在就已具备）

- **场景**：团队培训、新板 bring-up、CI 回归。
- **价值点**：模板 + 4 个样例覆盖"调度/同步/兼容层/TrustZone/USB"，是现成的教学与回归素材。
- **要补什么**：`scripts/selfcheck.sh`（build → flash → SWD 读 tick/PC → UART 期望序列 → 汇总）；统一日志格式；每个样例一条"预期输出"。
- **工作量**：0.5–1 周；几乎零风险。

---

## 4. 推荐路线（二选一，别并行铺开）

### 路线 1：**实时控制节点**（方向 A + G + F 组合）

```
P0 基线（已完成）
  → P1 可观测性（G：日志 / 栈水位 / 故障记录 / 看门狗）
  → P2 控制环（A：PWM + ADC 同步 + PI + 抖动实测）
  → P3 容错（F：MPU + 分级看门狗 + 故障恢复）
  → P4 量产化（-O2、参数校准、烧写与自检）
```

当前进度：P0、P1、P2、P3-lite、P4 均已完成（见
[`Tx-Control-Node`](./NUCLEO-H563ZI_THREADX_Tx-Control-Node/README.md)）。
P4 = `-O2` 七组自检 + `cal` 参数校准（**片内 Flash 非易失保存**，无需外部 Flash/SD）
+ `release` profile 裁剪 + `release_check.sh`。20 kHz ISR 周期抖动 spread
0.1–3.2 µs、`ovr=0`；静态 MPU + 栈 guard + 安全模式 + 分级 WWDG 均上板验证。
剩余：长跑从 1 h 逐步加长到 8/24 h。

验收标准（先测基线，再定死目标值）：
- 目标控制环 10 kHz（100 µs）周期抖动 p99 < 5 µs（DWT 或 GPIO 翻转 + 示波器实测）
- ADC 与 PWM 同步触发，单周期采样不丢
- 连续 1 h 运行无复位（先 1 h，再逐步加长到 24/72 h），关键线程栈余量 > 30%
- 注入 HardFault / 看门狗 / 欠压复位各 10 次，100% 恢复且故障记录可读
- 切到 `-O2` 后复测，指标不退化

### 路线 2：**本地数据记录 / 仪器**（方向 B + C + G 组合）

```
P0 基线（已完成）
  → P1 可观测性（G，已完成）
  → P2 采集 + FileX/LevelX 记录（模拟盘已验证 FileX+LevelX 与"采集→落盘→回读"；
    ADC 采样暂用 EOC 中断，DMA 路径待定位）
  → P3 USB 导出（MSC）+ 命令通道（CDC）
  → P4 掉电一致性与长跑
```

验收标准：
- 设计采样率下连续记录 24 h 无丢样（用序号/时间戳校验）
- 随机掉电 20 次，介质每次都能挂载，已写入文件可读（允许丢最后 N 秒）
- USB MSC 读速率与枚举成功率达标（先测再定）
- 连续 1 h 无内存/句柄泄漏（先 1 h，再逐步加长）

---

## 5. 工程与硬件的注意点

### 5.1 ThreadX 配置（本工程已踩过的，务必保留）

- **模式宏必须同时作用于 C 内核与汇编端口**。`TX_SINGLE_MODE_SECURE` 与 CPU 实际安全状态不一致时，首次上下文切换会 `INVPC` HardFault（见 USBX HID 报告）。
- **端口不只是几个 C 文件**：上下文保存/恢复、调度、栈构造、系统返回、tick 中断、低层初始化缺一不可；更换 Cube 包时保持内核/端口/低层初始化版本一致。
- **SysTick 归 ThreadX，HAL 用 TIM6**。不要给 HAL 也配 SysTick，也不要在 `stm32h5xx_it.c` 里重复定义 ThreadX 的异常处理。
- **端口源码依赖 GNU 扩展**，用 `-std=gnu99`；`-std=c99` 会导致安全栈相关文件编译失败（SecureLED 经验）。
- **线程栈 8 字节对齐**；模板 512 B 仅够心跳，真实线程按"调用深度 + 浮点上下文 + 余量"实测。
- **ISR 里只能用 `_isr` 系列 API**，不能阻塞，不能调用可能挂起的服务。
- **tick 100 Hz 只有 10 ms 粒度**：周期任务不要用 `tx_thread_sleep(1)` 当 10 ms 定时（会有相位抖动），用 ThreadX 定时器或硬件触发。
- **32-bit tick 在 100 Hz 下约 497 天回绕**；长期运行用 `ULONG` 差值比较，不要直接比大小。
- `tx_user.h` 目前只有两个宏。需要栈检测/事件 trace 时在这里补 `TX_ENABLE_STACK_CHECKING`、`TX_ENABLE_EVENT_TRACE`。
- **要写内部 Flash（参数/校准）时先过三条**：① 确认工程有 `-mcmse`
  （当前模板已启用；否则 HAL 走 `NSCR`，TZEN 下报 `PGSERR`）；② 别用
  `0x081FE000` 起的 8 KB EDATA 区；
  ③ 若开了"Flash 只读"MPU 分区，编程那一刻要临时关 MPU。详见
  [模板 TIPS §7](./NUCLEO-H563ZI_THREADX-SECURE_TEMPLATE/NUCLEO-H563ZI_THREADX_TIPS.md)。
- **GPDMA 四条（"计数但不落内存"已定位）**：① TZEN 下通道默认非安全/非特权，
  先补 `DMA_CHANNEL_PRIV + DMA_CHANNEL_SEC`，否则报 USEF；② **光有 ① 仍不落数据**：
  每个数据拍的 `SSEC`/`DSEC`（链表模式下是节点里的 `SrcSecure`/`DestSecure`）也必须
  是安全，`DMA_CHANNEL_SRC_SEC`/`DEST_SEC` 少一个都会被 GTZC 静默过滤——通道照样计数、
  照样进 TC 中断、**错误标志不置位**，只有数据消失；③ 本板 SRAM 在 MPCBB 里是
  "安全 + 特权"，所以通道特权位也不能少；④ 端口分配（PORT0/PORT1）、SRAM 体、
  0x2xxx/0x3xxx 别名都不影响结论。定位工具：`dma env` / `dma test`。详见
  [SimDisk 复盘](./NUCLEO-H563ZI_THREADX_Tx-FileX-SimDisk/report.md)。
- **FileX/LevelX 三条**：① FileX 没有 cortex_m33 port，用 `ports/generic/inc`；
  ② `fx_system_initialize()` 会建定时器，必须在 `tx_application_define()` 里调用；
  ③ FileX 扇区缓存会吞掉重复写，验证磨损均衡前先 `fx_media_flush()` 并核对
  驱动层写计数，否则测的是缓存。详见
  [SimDisk 复盘](./NUCLEO-H563ZI_THREADX_Tx-FileX-SimDisk/report.md)。
- **Flash 掉电边界**：Control-Node 的追加记录 CRC 能识别半写记录，但物理断电
  未验收；当前单扇区满页擦除也没有双页事务保护。手头没有可编程电源，写入窗口内
  精确断电无法可靠复现，这一步**暂不纳入验收**（见 §7 风险表）；若将来产品要求
  覆盖擦除边界，先实现双页轮换再测。

### 5.2 内存与性能预算（上线前必做）

- 单镜像：2 MB Flash / 640 KB RAM 全可用；双镜像按 1 MB / 320 KB 预留。
- 当前 `.data + .bss` 只有 4.3 KB；FileX/USBX 需要额外缓冲（USBX 类 + PCD 内存池、FileX 扇区缓存），先算后开。
- `_Min_Heap_Size = 0x200` 只够 newlib 基本使用；双镜像的安全栈和 FileX/USBX 动态分配都要放大。
- 上线前把 `-O0` 换 `-O2`/`-Os`，并复测中断延迟与栈水位。

### 5.3 硬件层面（本板事实）

- 日志默认走 ST-LINK 虚拟串口：**USART3，PD8/PD9，AF7**（Thread-Sync 已验证）。
- USB 数据口是板载 USB-C（`USB_DRD_FS`）；ST-LINK 的 USB 只用于调试，不传应用数据。
- 板载**没有** SD 卡座和外部 Flash：方向 B 的真实介质必须外接（SD 卡座或 SPI NOR）。
  但**参数/校准/配置这类小数据不必外接**——片内 2 MB Flash 尾部留一个扇区就够
  （Control-Node 已实现，见 §6 资产表）；**文件系统软件栈也能先验证**——
  `Tx-FileX-SimDisk` 用 RAM 模拟 NOR 跑通 FileX + LevelX，接真实介质时只换底层驱动。
- 片内 Flash 扇区 **8 KB**、2 MB 双 bank（`FLASH_SECTOR_NB=128`/bank）；
  **最后 8 KB（`0x081FE000`）是 EDATA 高擦写区**，只认 EDATA 专用命令，
  不能当普通用户 Flash 用。
- FDCAN1/2 外设存在，但板上没有收发器；方向 E 走 CAN 需外接收发器（如 SN65HVD230）。
- ADC1/2 为 12-bit；DAC1 有两个通道；注意输入阻抗、基准与地。
- **H563 没有 AES/SAES**（`stm32h563xx.h` 无 `AES_BASE`/`SAES_BASE`），只有 HASH/PKA/RNG；加密/验签按"控制/遥测报文"量级设计，高吞吐加密直接选 H573。

### 5.4 TrustZone（走双镜像时）

- 复用 NetXDuo Application Note §5.1 的两级检查：GTZC `SECCFGR`（安全属性）+ `PRIVCFGR`（DMA 是非特权事务）+ `NVIC_ITNS`（中断投递）。
- 安全栈扩展需要 Secure 侧可用的 newlib 堆（`_sbrk` + 足够的 `_Min_Heap_Size`），否则 `tx_thread_secure_stack_allocate` 会静默失败（SecureLED 经验）。
- 新增 NSC 入口必须做参数校验（NS 传入的指针必须在 NS 可访问范围）。
- 单镜像模板的 `THREADX_MODE=NON_SECURE` **只切编译宏**，不会生成 NS 启动链、SAU/IDAU 配置、向量表和分区。

---

## 6. 可复用的工程资产（本 workspace 及同板已沉淀）

| 资产 | 位置 | 复用价值 |
| --- | --- | --- |
| ThreadX 模板（Makefile + 端口 + 时基） | `./NUCLEO-H563ZI_THREADX-SECURE_TEMPLATE/` | 所有派生项目的起手式 |
| ThreadX 配置与踩坑规则 | [NUCLEO-H563ZI_THREADX_TIPS.md](./NUCLEO-H563ZI_THREADX-SECURE_TEMPLATE/NUCLEO-H563ZI_THREADX_TIPS.md) | 模式/端口/时基/内存/分层验证 |
| 新项目复制与命名流程 | [New_project_naming.md](./NUCLEO-H563ZI_THREADX-SECURE_TEMPLATE/New_project_naming.md) | 目录名、`TARGET`、VS Code ELF 路径同步 |
| 同步对象抽象 + printf 重定向 | [Tx-Thread-Sync](../NUCLEO-H563ZI_THREADX_Tx-Thread-Sync/report.md) | 互斥量/信号量切换、USART3 日志、优先级设计 |
| CMSIS-RTOS2 兼容层 | [Tx-FreeRTOS-Wrapper](../NUCLEO-H563ZI_THREADX_Tx-FreeRTOS-Wrapper/report.md) | 迁移既有 FreeRTOS 代码 |
| 双镜像 + CMSE 导入库 + 安全栈 | [SecureLED-TrustZone](../NUCLEO-H563ZI_THREADX-SECURE_Tx-SecureLED-TrustZone/report.md) | OTA/安全服务/NS 应用 |
| 可靠性层（IWDG/心跳/故障记录/日志/状态台/自检） | [Tx-Reliability-Monitor](./NUCLEO-H563ZI_THREADX_Tx-Reliability-Monitor/README.md) | 所有派生项目的 P1 基线，可直接复制 |
| 控制环 + P3-lite 容错（TIM1 PWM + ADC 注入 + 前馈/PI + DWT 抖动 + 静态 MPU + 栈 guard + 安全模式 + 分级 WWDG） | [Tx-Control-Node](./NUCLEO-H563ZI_THREADX_Tx-Control-Node/README.md) | 路线 1 的 P2/P3-lite 基线，可直接替换被控对象 |
| 片内 Flash 参数页（16 B 记录 + CRC32 + 追加日志 + 扇区轮擦） | [Tx-Control-Node `Core/Src/app_param.c`](./NUCLEO-H563ZI_THREADX_Tx-Control-Node/Core/Src/app_param.c) | **任何需要掉电保存小数据**的项目：校准值、阈值、设备 ID、标定表；路线 2 的配置存储可直接复用 |
| FileX + LevelX 模拟盘（RAM 模拟 NOR：整块擦除 + 只允许 1→0 + 扇区映射统计） | [Tx-FileX-SimDisk `Core/Src/app_fxdisk.c`](./NUCLEO-H563ZI_THREADX_Tx-FileX-SimDisk/Core/Src/app_fxdisk.c) | **没有外部介质也想验证文件系统**时直接用；接真实 SPI NOR/SD 只换底层驱动，FileX/LevelX 层不动 |
| release 构建档位 + 命令裁剪（`APP_PROFILE` + `release_check.sh`） | [Tx-Control-Node](./NUCLEO-H563ZI_THREADX_Tx-Control-Node/README.md) | 量产固件裁掉 `guard/stall/crash`，并断言命令确实不存在 |
| USB 设备栈 + USB-PD 附着 | [USBX HID](../NUCLEO-H563ZI_USBX_Ux-Device-Human-Interface/report.md) | 方向 C 的直接起点 |
| 故障排障方法（INVPC → CFSR/EXC_RETURN） | [USBX HID 报告](../NUCLEO-H563ZI_USBX_Ux-Device-Human-Interface/report.md) | 下次 10 分钟定位同类问题 |
| 网络/OTA/自检/长跑工具 | [NetXDuo workspace](../NUCLEO-H563ZI_NetXDuo/README.md) | 跨线复用，避免重复实现 |
| 裸机外设与板级经验 | [NUCLEO-H563ZI_TIPS.md](../../NUCLEO-H563ZI_TEMPLATE/NUCLEO-H563ZI_TIPS.md) | 时钟树、GPIO/MSP、OpenOCD 等 |

---

## 7. 风险与限制（写清了好过事后解释）

| 项 | 说明 |
| --- | --- |
| **中间件许可（重要）** | 本 Cube 包内 `Middlewares/ST/threadx/LICENSE.txt` 是 **Microsoft Azure RTOS** 许可条款（内部开发/测试/评估；分发与量产需另行确认）；其 README 说明这是 AzureRTOS ThreadX 的 ST fork。上游 Eclipse ThreadX 是 MIT，但**不要假定本包已是 MIT**，量产前逐项核对 |
| 无 AES/SAES | 该 IP 仅 H573 有（`stm32h563xx.h` 无 `AES_BASE`）；加密/验签按"控制/遥测"量级设计 |
| 模板无日志输出 | `_write()` 返回 -1，`printf` 不产生任何输出；P1 必须补 |
| MPU / tickless | 本包 ThreadX 不含 MPU 扩展和低功耗模块；P3-lite 用 CMSIS 静态 MPU + 栈 guard，动态 per-thread 隔离和 tickless 仍需自建 |
| tick 粒度 | 100 Hz = 10 ms；控制环/高频采集不能依赖 tick |
| `-O0` 构建 | 开发默认 `-O0`；`make release` 使用 `-O2` + `APP_RELEASE=1`，P4 已复测 |
| Flash 参数区 | Control-Node 把 bank 1 最后 16 KB 划给 `PARAM`（`0x080FC000–0x080FFFFF`），记录页在 `0x080FE000`；`0x081FE000` 起 8 KB 是 EDATA，不能当用户区用。模板已加 `-mcmse`；从旧 Makefile 派生的项目仍需确认 |
| 参数页擦除停顿/掉电 | 8 KB 扇区擦除实测让 20 kHz 控制中断停顿约 2 ms；当前单页擦除无掉电事务保护。**写入中掉电暂不验收**（无可用可编程电源），量产前如有要求应改双页轮换，并在擦前 `ctrl off` |
| 单镜像模板 | 不含 NS 启动链；`THREADX_MODE=NON_SECURE` 不能替代 TrustZone 配置 |
| 板载无外部存储 | FileX/LevelX 必须外接介质；但**参数/配置小数据可用片内 Flash 参数页**（P4 已实现） |
| FDCAN 无收发器 | 走 CAN 需外部硬件 |
| 无 GUIX | 本 Cube 包不含，图形界面需另行引入 |
| 故障处理是死循环 | `HardFault_Handler` / `Error_Handler` 都不会复位，现场表现为"死机" |
| 堆预留小 | `_Min_Heap_Size=0x200`，FileX/USBX/安全栈都需要放大 |
| 静态线程栈仅 512 B | 示例级配置，真实线程必须实测水位 |

---

## 8. 下一步建议（按投入产出排序）

1. **已完成**：P1 可观测性（方向 G）—— 见 [`Tx-Reliability-Monitor`](./NUCLEO-H563ZI_THREADX_Tx-Reliability-Monitor/README.md)。
2. **已完成**：路线 1 的 P2 控制环 —— 见 [`Tx-Control-Node`](./NUCLEO-H563ZI_THREADX_Tx-Control-Node/README.md)。TIM1 20 kHz、控制 10 kHz、抖动 spread 0.1–3.2 µs、`stall 3` 触发 IWDG。
3. **已完成**：路线 1 的 P3-lite 容错 —— 静态 MPU（12 regions / 5 guards）、guard→MemManage 归因、连续 3 次故障安全模式、`stall 4` 触发 WWDG。
4. **已完成**：P4 量产化主体 —— `-O2` 七组自检、`cal` 参数校准与片内 Flash 掉电保存、`release` profile + `release_check.sh`。
5. **已完成**：路线 2 的文件系统 + 采集链路 —— [`Tx-FileX-SimDisk`](./NUCLEO-H563ZI_THREADX_Tx-FileX-SimDisk/README.md) 用 RAM 模拟 NOR 跑通 FileX + LevelX（FAT、扇区映射、块回收、磨损均衡），并完成 TIM3→ADC1→环形缓冲→CSV 落盘→回读校验，六组自检全 PASS。
6. **已完成**：GPDMA 采样的"计数但不落内存"根因 —— 传输级安全属性（`SSEC`/`DSEC`）没设，GTZC 静默丢拍。现在 `acq start dma [hz]` 用单节点循环链表跑 ADC→GPDMA，实测 **50 kHz 零丢弃**；100 kHz 时瓶颈变成写盘（采样线程同步 `fx_media_flush`），下一步是解耦采集与落盘。
7. **下一步**：路线 1 按需要把长跑加长到 8/24 h；路线 2 先做"采集入队 / writer 落盘"解耦以突破 50 kHz，再接真实介质，并给日志加 RTC 时间戳与文件头。
8. **已完成**：P4 七组 `selfcheck all` 全 PASS；guard/BusFault/IWDG/WWDG 重复注入 35 次全部通过。**片内 Flash 写入中掉电注入已决定暂不执行**（无可编程电源），后续有设备或产品强制要求时再做，届时先实现双页轮换。
9. **贯穿始终**：每加一个外设或中断，先过 §5.1 检查清单（模式宏、端口完整性、时基分工、NVIC、栈大小），再谈功能。

---

## 9. 参考

- 本 workspace 模板：[README.md](./NUCLEO-H563ZI_THREADX-SECURE_TEMPLATE/README.md)、[NUCLEO-H563ZI_THREADX_TIPS.md](./NUCLEO-H563ZI_THREADX-SECURE_TEMPLATE/NUCLEO-H563ZI_THREADX_TIPS.md)、[New_project_naming.md](./NUCLEO-H563ZI_THREADX-SECURE_TEMPLATE/New_project_naming.md)
- 网络线：[NetXDuo Application Note](../NUCLEO-H563ZI_NetXDuo/Application_note.md)、[NetXDuo workspace](../NUCLEO-H563ZI_NetXDuo/README.md)
- 同板样例：[Tx-Thread-Sync](../NUCLEO-H563ZI_THREADX_Tx-Thread-Sync/report.md)、[Tx-FreeRTOS-Wrapper](../NUCLEO-H563ZI_THREADX_Tx-FreeRTOS-Wrapper/report.md)、[SecureLED-TrustZone](../NUCLEO-H563ZI_THREADX-SECURE_Tx-SecureLED-TrustZone/report.md)、[USBX HID](../NUCLEO-H563ZI_USBX_Ux-Device-Human-Interface/report.md)
- 可加装的中间件（本地 Cube 包内）：`Middlewares/ST/{threadx,filex,levelx,usbx}`、`Middlewares/Third_Party/{mcuboot,mbed-crypto}`
- 机器可查的事实来源：`Drivers/CMSIS/Device/ST/STM32H5xx/Include/stm32h563xx.h`（外设基址、DMA/描述符位定义）
- RM0481 检索库：`/mnt/study/docs/RAG_Index/H563ZI/v2_pdf_generated/`
