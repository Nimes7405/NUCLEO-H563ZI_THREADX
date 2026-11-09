# NUCLEO-H563ZI_THREADX — ThreadX 应用 workspace

本目录用于容纳 NUCLEO-H563ZI（STM32H563ZI，TrustZone）上的 ThreadX 应用，
每个项目是独立目录、独立构建。方向与路线的完整说明见
[Application_note.md](./Application_note.md)。

| 项目 | 说明 | 状态 |
| --- | --- | --- |
| [`NUCLEO-H563ZI_THREADX-SECURE_TEMPLATE`](./NUCLEO-H563ZI_THREADX-SECURE_TEMPLATE/README.md) | 单镜像 Secure 模板：ThreadX 6.4.0 + HAL，SysTick 100 Hz、HAL 1 ms 走 TIM6、静态线程 + LED3 心跳 | 已复核构建（`-O0`：Flash 15.0 KB / RAM 4.3 KB）；板上心跳待目视确认 |
| [`NUCLEO-H563ZI_THREADX_Tx-Reliability-Monitor`](./NUCLEO-H563ZI_THREADX_Tx-Reliability-Monitor/README.md) | P1 可靠性层：IWDG + 线程心跳、故障记录、环形日志、USART3 状态台、栈水位、一键自检 | 已在板上验证（`selfcheck` basic/watchdog/fault 全通过；205 s 稳定运行） |
| [`NUCLEO-H563ZI_THREADX_Tx-Control-Node`](./NUCLEO-H563ZI_THREADX_Tx-Control-Node/README.md) | 路线 1：TIM1 PWM + ADC 注入同步、10 kHz 前馈 + PI 控制环、DWT 抖动实测、软件/ADC 双反馈；P3-lite 静态 MPU + 栈 guard + 安全模式 + 分级 WWDG；P4 release profile + `cal` 参数校准 + 片内 Flash 掉电保存 | 已在板上验证（`selfcheck` 七组全通过；1 h O0 soak PASS；O2 `selfcheck all` 七组 PASS；`release_check.sh` PASS；校准值重启/重烧后保持） |
| `NUCLEO-H563ZI_THREADX_Tx-Data-Logger`（规划） | 路线 2：ADC/DMA + FileX/LevelX 本地记录 + USB 导出 | 规划中 |
| `NUCLEO-H563ZI_THREADX_Tx-Bench-Tool`（规划，主机侧） | 串口日志抓取、抖动/栈水位分析、长跑报告 | 规划中 |

相关（不在本 workspace 内）：

- `../NUCLEO-H563ZI_THREADX_Tx-Thread-Sync`：线程、互斥量/信号量、USART3 `printf`（已板上验证）。
- `../NUCLEO-H563ZI_THREADX_Tx-FreeRTOS-Wrapper`：CMSIS-RTOS2 兼容层。
- `../NUCLEO-H563ZI_THREADX-SECURE_Tx-SecureLED-TrustZone`：双镜像、NSC、ThreadX 安全栈。
- `../NUCLEO-H563ZI_USBX_Ux-Device-Human-Interface`：USBX HID + USB-PD，ThreadX 调度已在板上验证。
- `../NUCLEO-H563ZI_NetXDuo/`：网络应用 workspace；TLS、网络 OTA、自检与长跑工具从这里复用。

公共约定：

- 单镜像项目直接 `make` / `make flash`；双镜像项目按模板 README 的
  option bytes → Secure → NonSecure 顺序烧写。
- 新增外设/中断前先查：ThreadX 模式宏（`TX_SINGLE_MODE_SECURE`）、端口完整性、
  SysTick 与 TIM6 的分工、NVIC 配置、线程栈大小。
- 验证优先用各项目的 `scripts/selfcheck.sh`，避免手工步骤漂移；长跑用
  Control-Node 的 `scripts/soak.py`，先 1 h 再逐步加长，不要一上来跑 72 h。
- 文档入口：[Application_note.md](./Application_note.md)（方向与路线）、
  [NUCLEO-H563ZI_THREADX_TIPS.md](./NUCLEO-H563ZI_THREADX-SECURE_TEMPLATE/NUCLEO-H563ZI_THREADX_TIPS.md)（模板配置）、
  [New_project_naming.md](./NUCLEO-H563ZI_THREADX-SECURE_TEMPLATE/New_project_naming.md)（新建项目流程）。
