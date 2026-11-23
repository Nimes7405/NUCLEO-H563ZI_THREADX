# NUCLEO-H563ZI_THREADX — ThreadX 应用 workspace

本目录用于容纳 NUCLEO-H563ZI（STM32H563ZI，TrustZone）上的 ThreadX 应用，
每个项目是独立目录、独立构建。方向与路线的完整说明见
[Application_note.md](./Application_note.md)。

| 项目 | 说明 | 状态 |
| --- | --- | --- |
| [`NUCLEO-H563ZI_THREADX-SECURE_TEMPLATE`](./NUCLEO-H563ZI_THREADX-SECURE_TEMPLATE/README.md) | 单镜像 Secure 模板：ThreadX 6.4.0 + HAL，SysTick 100 Hz、HAL 1 ms 走 TIM6、静态线程 + LED3 心跳 | 已复核构建（`-O0`：Flash 15.0 KB / RAM 4.3 KB）；板上心跳待目视确认 |
| [`NUCLEO-H563ZI_THREADX_Tx-Reliability-Monitor`](./NUCLEO-H563ZI_THREADX_Tx-Reliability-Monitor/README.md) | P1 可靠性层：IWDG + 线程心跳、故障记录、环形日志、USART3 状态台、栈水位、一键自检 | 已在板上验证（`selfcheck` basic/watchdog/fault 全通过；205 s 稳定运行） |
| [`NUCLEO-H563ZI_THREADX_Tx-Control-Node`](./NUCLEO-H563ZI_THREADX_Tx-Control-Node/README.md) | 路线 1：TIM1 PWM + ADC 注入同步、10 kHz 前馈 + PI 控制环、DWT 抖动实测、软件/ADC 双反馈；P3-lite 静态 MPU + 栈 guard + 安全模式 + 分级 WWDG；P4 release profile + `cal` 参数校准 + 片内 Flash 掉电保存 | 已在板上验证（`selfcheck` 七组全通过；1 h O0 soak PASS；O2 `selfcheck all` 七组 PASS；`release_check.sh` PASS；校准值重启/重烧后保持） |
| [`NUCLEO-H563ZI_THREADX_Tx-FileX-SimDisk`](./NUCLEO-H563ZI_THREADX_Tx-FileX-SimDisk/README.md) | 路线 2 软件栈：RAM 内模拟 NOR（擦除 + 只允许 1→0）承载 FileX + LevelX；TIM3 触发 ADC1 → EOC 中断或 GPDMA 循环 → 环形缓冲 → CSV 落盘 → 2 KB 轮转 → 回读校验，无需 SD 卡或外部 Flash | 已在板上验证（`selfcheck` basic/fs/acq/dma/watchdog/fault 六组全通过；`fs stress` 触发真实块回收与擦除，1→0 违规为 0；GPDMA 100 kHz 采集 422,000 样本零丢样、`acq check` 逐行校验通过；`dma test` 定位到"计数但不落内存"的根因） |
| [`NUCLEO-H563ZI_THREADX_Tx-Data-Logger`](./NUCLEO-H563ZI_THREADX_Tx-Data-Logger/README.md) | 路线 2 多通道：从 SimDisk 派生，ADC 序列扫描 1–4 通道 → GPDMA 交织双半缓冲 → 每通道窗口统计 → CSV；新增确定性解交织自检 | 已在板上验证（`selfcheck` 七组全通过；4 通道 100 kHz = 400 k 样本/秒、`drop=0 dma_ovr=0`；`acq demux` 四路斜坡全部命中） |
| [`NUCLEO-H563ZI_THREADX_Tx-Usb-Data-Logger`](./NUCLEO-H563ZI_THREADX_Tx-Usb-Data-Logger/README.md) | 路线 2 P3：从 Data-Logger 派生，加 USBX 复合设备（MSC 数据盘 + CDC 命令通道），把"拔卡取数 + UART 命令"合并到一根 USB-C | 基线迁移完成（构建产物与上游二进制一致）；USBX 待接入 |
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
- **掉电保存的小数据（校准值、阈值、设备 ID）优先用片内 Flash 参数页**，
  不依赖外部介质：复用 [`Tx-Control-Node/Core/Src/app_param.c`](./NUCLEO-H563ZI_THREADX_Tx-Control-Node/Core/Src/app_param.c)
  （16 B 记录 + CRC32 + 追加日志）。注意三点：Makefile 需 `-mcmse`、
  避开 `0x081FE000` 的 EDATA 区、编程时临时关 MPU 只读分区。
- 新项目派生后先读
  [模板 TIPS §7「写内部 Flash」](./NUCLEO-H563ZI_THREADX-SECURE_TEMPLATE/NUCLEO-H563ZI_THREADX_TIPS.md)。
- 文档入口：[Application_note.md](./Application_note.md)（方向与路线）、
  [NUCLEO-H563ZI_THREADX_TIPS.md](./NUCLEO-H563ZI_THREADX-SECURE_TEMPLATE/NUCLEO-H563ZI_THREADX_TIPS.md)（模板配置）、
  [New_project_naming.md](./NUCLEO-H563ZI_THREADX-SECURE_TEMPLATE/New_project_naming.md)（新建项目流程）。
