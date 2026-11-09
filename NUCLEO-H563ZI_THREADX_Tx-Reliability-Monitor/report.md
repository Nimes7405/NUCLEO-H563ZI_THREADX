# NUCLEO-H563ZI ThreadX Tx-Reliability-Monitor 复盘

## 目标

在 `NUCLEO-H563ZI_THREADX-SECURE_TEMPLATE` 之上落地 Application Note 的 P1
可靠性 / 可观测性层：IWDG、线程心跳、故障记录、复位原因、环形日志、USART3
状态台、栈水位和一键自检。要求全部在实板上验证，而不是只做到"能编译"。

## 结果摘要

- 从模板复制出 `NUCLEO-H563ZI_THREADX_Tx-Reliability-Monitor`，目录名、
  Makefile `TARGET`、`.vscode/launch.json` 已同步。
- `make clean && make -j8` 成功；`-O0` 产物 `text 90,716 B / data 2,252 B /
  bss 20,192 B`，`.bin` 92,976 B；`-O2` 产物 `.bin` 72,104 B。
- `make flash` 成功，OpenOCD 显示 `Programming Finished` / `Verified OK` /
  `Resetting Target`。
- 板上连续运行 205 s 无复位：`wdg_feed=410`（2 Hz），monitor / console /
  prod 心跳分别为 2051 / 20500 / 410。
- 看门狗闭环：`stall 1` → 1.5 s 检出 → 停止喂狗 → IWDG 复位，重启后
  `BOOT reset=iwdg fault=none`。
- 故障闭环：`crash` → BusFault（`cfsr=0x00008200`、`bfar=0xfffffff0`）→
  记录写入 `.noinit` → 复位，重启后自动打印 `FAULT seq=...`。
- `./scripts/selfcheck.sh all`：basic 9 项、watchdog 4 项、fault 5 项全部 PASS。

## 实现内容

### 线程与心跳

| 线程 | 优先级 | 栈 | 周期 | 心跳 |
| --- | --- | --- | --- | --- |
| monitor | 3 | 2048 | 100 ms | 自己 |
| console | 8 | 3072 | 10 ms 轮询 RX | 是 |
| prod_a | 10 | 2048 | 500 ms | 是 |
| prod_b | 11 | 2048 | 500 ms | 是 |
| logger | 20 | 1024 | ≤500 ms 唤醒 | 是 |

- monitor 每 100 ms 检查除自己外的 4 路心跳；1.5 s 计数不变即判定卡死，
  打印告警、点亮红色 LED、停止喂 IWDG。
- IWDG 4.0 s 超时，给 logger 留出约 2.5 s 把告警发完。
- monitor 本身不喂狗则系统复位，覆盖"监护线程自己挂掉"的场景。

### IWDG 与复位原因

- LSI 32 kHz，`IWDG_PRESCALER_32`，`Reload=3999` → `32 × 4000 / 32000 = 4.0 s`。
- 每 500 ms 调用一次 `HAL_IWDG_Refresh()`；`wdg_feed` 计入状态台。
- `DBGMCU->APB1FZR1 |= DBGMCU_APB1FZR1_DBG_IWDG_STOP`，调试器 halt 时冻结
  IWDG，避免单步调试被复位。
- 复位原因从 `RCC->RSR` 读取后立即 `__HAL_RCC_CLEAR_RESET_FLAGS()`，映射为
  iwdg / wwdg / software / brown-out / low-power / pin。

### 故障捕获

- `HardFault/MemManage/BusFault/UsageFault` 用 `naked` 包装，按 `EXC_RETURN`
  选择 MSP/PSP，把栈帧和 `CFSR/HFSR/MMFAR/BFAR` 写入 `.noinit` 段。
- 记录包含 `sequence`、`PC/LR/xPSR/SP/EXC_RETURN` 和 `r0–r3/r12`，共 72 B。
- 写完 `__DSB()` 后 `NVIC_SystemReset()`；下次启动由 `app_fault_init`
  读取并打印一次，`fault` 命令继续可查。
- `MMFAR/BFAR` 只在 `MMARVALID/BFARVALID` 置位时记录，避免打印无效值。

### 日志与状态台

- 4 KB 单生产者/单消费者环形缓冲；`app_log_printf` 在日志锁内完成
  `vsnprintf` 与入队，避免 newlib `_impure_ptr` 的并发问题。
- logger 线程等事件标志，按 256 B 分块调用 `HAL_UART_Transmit`。
- `_write()` 重定向到同一日志链路，因此 `printf` 也可用。
- USART3：PD8/PD9、AF7、115200 8N1，RX 中断 + 256 B 环形缓冲。
- 命令：`help/status/stack/fault/wdg/log/stall/crash/reboot`。

### 一键自检

- `scripts/selfcheck.sh`：清理构建 → 检查关键符号 → 烧写 → 调用串口断言。
- `scripts/serial_check.py`：
  - `basic`：重启后断言 BOOT/READY/STATUS/STACK/FAULT/WDG/LOG，且无卡死、无 canary；
  - `watchdog`：`stall 1` → 断言告警与 `reset=iwdg`；
  - `fault`：`crash` → 断言 `reset=software fault=yes`、`cfsr=0x00008200`、`bfar=0xfffffff0`。
- 脚本先 `drain_quiet()`，避免把烧写前固件残留在 tty 缓冲里的输出当成新结果。

## 构建与链接验证

- 关键符号均链接为真实实现：`app_fault_capture`、四个 fault handler、
  `app_watchdog_poll`、`app_log_printf`、`tx_application_define`、
  `USART3_IRQHandler`、`TIM6_IRQHandler`。
- `.noinit` 段 72 B，位于 `.bss` 之后、`._user_heap_stack` 之前；启动代码
  不清零该段。
- `-O0` 与 `-O2` 均可构建；`OPT` 变量可在命令行覆盖。

## 问题定位与修复

### 1. OpenOCD examination failed

- **现象**：`Warn : target stm32h5x.cpu0 examination failed` / `Unable to reset target`。
- **根因**：模板自带的 `openocd/st_nucleo_h5.cfg` 使用 `stlink.cfg` + `hla_swd`，
  本地 `stm32h5x.cfg` 把 Cortex-M33 建在 AP0；H563 的 CPU debug target 在 AP1。
- **修复**：改用 `interface/stlink-dap.cfg` + `transport select dapdirect_swd` +
  官方 `target/stm32h5x.cfg`，本地文件只保留一行 source。

### 2. HAL tick 不递增

- **现象**：模板没有 `HAL_TIM_PeriodElapsedCallback`，`HAL_GetTick()` 永远为 0；
  UART 等依赖 HAL 超时的调用在异常时可能死等。
- **修复**：在 `main.c` 增加 TIM6 回调调用 `HAL_IncTick()`。

### 3. IWDG HAL 编译失败

- **现象**：`LSI_STARTUP_TIME undeclared`。
- **修复**：在 `stm32h5xx_hal_conf.h` 增加 `#define LSI_STARTUP_TIME 130UL`。

### 4. 线程栈溢出（STKOF）

- **现象**：复位循环，`CFSR=0x00100000`（UFSR.STKOF），`PC` 落在
  `_svfprintf_r`，`SP` 在 prod_a 栈内。
- **根因**：初版 prod_a/prod_b 栈只有 768 B、monitor 1024 B；newlib
  `vsnprintf` 的调用深度超过小栈。
- **修复**：monitor 2048、console 3072、prod_a/prod_b 2048、logger 1024；
  同时把格式化放进日志锁，串行化 newlib 调用。实测水位约
  monitor 1000/2048、console 1040/3072、prod 920/2048。

### 5. ThreadX 会把栈填成 0xEF

- **现象**：用 0xA5 填充做水位/canary 时，所有栈都被报告"已用满"，
  并立刻触发"stack-canary-overwritten"。
- **根因**：`tx_thread_create` 在 `TX_DISABLE_STACK_FILLING` 未定义时用
  `TX_STACK_FILL`（0xEF）覆盖整段栈。
- **修复**：水位与 canary 统一使用 0xEF；`memset` 保留但值一致。

### 6. 启动阶段命令被截断（"sstack"）

- **现象**：主机在 `READY` 后立即连发 `status`、`stack`，固件收到 `sstack`。
- **定位**：RX 历史显示只有 `sstack\n` 真正进入 USART；`STATUS` 是 monitor
  自己的首条周期状态，不是命令响应。说明 `status\n` 在 UART RX 尚未被调度时
  只锁存了第一个字节，其余因单字节 RX 溢出丢失。
- **修复**：把 `READY` 改为由 console 线程启动后打印；主机看到 READY 时
  console 已在运行，RX 中断正常服务。自检连续多次通过。

### 7. `crash` 触发的是 UNALIGNED 而不是 BusFault

- **现象**：`0xFFFFFFFF` 写入得到 `CFSR=0x01000000`（UFSR.UNALIGNED）。
- **修复**：改为 4 字节对齐的 `0xFFFFFFF0`，得到
  `CFSR=0x00008200`（PRECISERR + BFARVALID）与 `BFAR=0xFFFF_FFF0`。

## 可复用经验

1. **模板的 OpenOCD 配置要换成 AP1**：`stlink-dap.cfg` + `dapdirect_swd` +
   官方 `target/stm32h5x.cfg`，否则 examination 阶段就失败。
2. **`READY` 要在能接收命令之后再打印**：`tx_application_define` 里的
   `READY` 只代表内核即将启动，不代表 RX 已在服务。
3. **带 `vsnprintf` 的线程栈至少 2 KB**：768 B/1 KB 很容易 STKOF；用
   `stack` 命令定期看水位。
4. **ThreadX 栈填充是 0xEF**：水位/ canary 必须与 `TX_STACK_FILL` 一致。
5. **fault 记录放 `.noinit`**：能跨复位、不能跨掉电；启动时读取一次并清除
   `valid`，避免每次重启重复打印。
6. **调试时冻结 IWDG**：`DBGMCU_APB1FZR1_DBG_IWDG_STOP`，否则断点停下就会被复位。
7. **自检脚本先清串口缓冲**：烧写前的固件输出会污染断言；先 `drain_quiet`
   并只匹配重启后的新 `BOOT` 行。
