# NUCLEO-H563ZI_THREADX_Tx-Reliability-Monitor

P1 可靠性 / 可观测性层，从
[`ThreadX Secure 模板`](../NUCLEO-H563ZI_THREADX-SECURE_TEMPLATE/README.md) 派生，
对应 [Application Note](../Application_note.md) 的方向 G 与缺口 #1–#4、#10、#12。

目标：让 ThreadX 应用从"能跑"变成"可以长期挂着跑、出问题能查"。

## 已实现

| 能力 | 实现 | 板上证据 |
| --- | --- | --- |
| 独立看门狗 | IWDG，LSI，4.0 s 超时，监护线程每 500 ms 喂狗 | `WDG timeout_ms=4000 feed_ms=500`；长跑 `wdg_feed` 与 uptime 同步增长 |
| 线程心跳 | 5 路心跳计数（monitor / logger / console / prod_a / prod_b），1.5 s 无进展即停喂 | `stall 1` 后打印 `WATCHDOG stall=prod_a no-heartbeat=1500ms`，约 4 s 后 `reset=iwdg` |
| 故障捕获 | HardFault / MemManage / BusFault / UsageFault 记录 `CFSR/HFSR/MMFAR/BFAR/PC/LR/xPSR/EXC_RETURN` 到 `.noinit`，随后复位 | `crash` → `BOOT reset=software fault=yes`，`cfsr=0x00008200 bfar=0xfffffff0` |
| 复位原因 | 读取/清除 `RCC->RSR`，区分 iwdg / wwdg / software / brown-out / low-power / pin | `BOOT reset=iwdg`、`BOOT reset=software` 均实测 |
| 日志系统 | 4 KB 环形缓冲 + 独立 logger 线程 + USART3；`vsnprintf` 与入队串行化；满时计数丢弃 | `drop=0`；`log <text>` 回显正常 |
| 状态台 | USART3 115200 8N1，命令见下表 | `STATUS/STACK/FAULT/WDG` 均实测 |
| 栈水位 | 线程栈统一用 ThreadX 的 `0xEF` 填充，启动时注册、运行时扫描；栈底 canary 被改写即停喂 | `STACK monitor=1000/2048 logger=516/1024 console=1040/3072 prod_a=920/2048 prod_b=920/2048` |
| 一键自检 | build → 符号检查 → flash → 串口断言 | `./scripts/selfcheck.sh all` 全通过 |

## 板上已验证（实测，不是"应该能跑"）

- **构建**：`-O0` 下 `text 90,716 B / data 2,252 B / bss 20,192 B`（含 1.5 KB heap+stack），`.bin` 92,976 B；`-O2` 下 `.bin` 72,104 B。
- **稳定运行**：连续 205 s 无复位，`wdg_feed=410`（2 Hz），monitor/console/prod 心跳分别为 2051 / 20500 / 410。
- **看门狗闭环**：`stall 1` → 1.5 s 检出 → 停止喂狗 → IWDG 复位；重启后 `BOOT reset=iwdg fault=none`，系统恢复。
- **故障闭环**：`crash` → BusFault（PRECISERR + BFARVALID）→ 记录写入 `.noinit` → 复位；重启后自动打印记录，`fault` 命令可再次读取。
- **自检套件**：`basic` 9 项、`watchdog` 4 项、`fault` 5 项全部 PASS。

## 串口命令

USART3（ST-LINK 虚拟串口，PD8/PD9，115200 8N1）：

| 命令 | 作用 |
| --- | --- |
| `help` | 列出命令 |
| `status` | uptime、复位原因、喂狗次数、5 路心跳、日志丢弃、RX 丢弃 |
| `stack` | 各线程栈已用 / 总大小 |
| `fault` | 上一次故障记录（无则 `FAULT none`） |
| `wdg` | IWDG 参数与累计喂狗次数 |
| `log <text>` | 写一条日志，验证日志链路 |
| `stall <0\|1\|2>` | 0=清除；1=模拟 prod_a 卡死；2=模拟 prod_b 卡死（破坏性） |
| `crash` | 触发一次对齐地址 BusFault（破坏性，验证故障记录） |
| `reboot` | 软件复位 |

## 构建与烧写

```sh
make clean
make -j4
make flash
```

需要 `CUBE_FW`（默认 `/mnt/study/mcu_fw/stm32CubeH5/STM32Cube_FW_H5_V1.7.0`）、
ARM GNU 工具链和 OpenOCD。OpenOCD 使用 `dapdirect_swd` + 官方
`target/stm32h5x.cfg`（CPU 在 AP1），本地 `openocd/` 已配置好。

优化等级可切换：

```sh
make clean && make OPT=-O2
```

## 一键自检

```sh
./scripts/selfcheck.sh basic     # 构建 + 符号 + 烧写 + 状态/栈/故障/日志断言
./scripts/selfcheck.sh watchdog  # 模拟线程卡死，断言 IWDG 复位与恢复
./scripts/selfcheck.sh fault     # 触发 BusFault，断言故障记录与重启报告
./scripts/selfcheck.sh all       # 三组顺序执行
```

可用 `PORT=/dev/ttyACM0` 覆盖串口。

## 目录结构

```text
Core/Inc/     app_config.h / app_log.h / app_fault.h / app_watchdog.h /
              app_stacks.h / app_console.h
Core/Src/     main.c, app_threadx.c, app_log.c, app_console.c,
              app_fault.c, app_watchdog.c, app_stacks.c
openocd/      st_nucleo_h5.cfg (dapdirect) + stm32h5x.cfg (官方 target)
scripts/      selfcheck.sh, serial_check.py
```

## 设计说明

- **心跳阈值**：监护线程每 100 ms 轮询；任一已注册线程 1.5 s 计数不变即停止喂狗。IWDG 4 s 超时，留出约 2.5 s 让 logger 把告警发完。
- **栈填充**：ThreadX `tx_thread_create` 会把栈填成 `TX_STACK_FILL`（0xEF）。水位扫描和 canary 必须用同一字节，否则会误报"栈溢出"。
- **日志线程**：`app_log_printf` 在日志锁内完成格式化与入队，避免 newlib `_impure_ptr` 的并发问题；logger 线程只负责搬运。
- **故障记录**：放在链接脚本新增的 `.noinit (NOLOAD)` 段，**能跨复位、不能跨掉电**；上电后 `app_fault_init` 读取一次并清除 `valid`，`fault` 命令继续可查。
- **READY 时机**：`READY` 由 console 线程启动后打印，而不是在 `tx_application_define` 里打印。否则主机可能在 UART RX 尚未被调度时就连发命令，单字节 RX 会溢出，出现半条命令。
- **调试友好**：IWDG 通过 `DBGMCU->APB1FZR1` 在核被调试器 halt 时冻结，避免单步调试被复位。

## 已知限制

- `stall` / `crash` 是**实验室破坏性命令**，量产固件应通过编译开关裁掉。
- 故障记录只跨复位；需要跨掉电请写入备份寄存器或 Flash。
- 单镜像 Secure 模板，不含 NonSecure 启动链、MPU 隔离和低功耗 tickless。
- 日志 4 KB，长时间高频日志会触发 `drop` 计数；现场可按需调大。
- 线程栈已按实测留余量；新增 `vsnprintf`/USB/FileX 调用时要重新看 `stack` 命令。

## 文档

- [Application Note](../Application_note.md)：方向、路线与验收标准
- [NUCLEO-H563ZI_THREADX_TIPS.md](./NUCLEO-H563ZI_THREADX_TIPS.md)：模板配置与踩坑
- [report.md](./report.md)：本项目的实现与问题定位复盘
