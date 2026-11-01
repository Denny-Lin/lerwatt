# Lerwatt — 會自我學習的 Linux 電源管理器

Lerwatt 是實驗性的 Linux 電源管理器，設計目標是學習 CPU 頻率上下限、idle 深度限制與溫度功率預算，並和 [Lertick](https://github.com/Denny-Lin/lertick) 這個 sched_ext 排程器合作，讓排程與電源決策不再互相拉扯。

[English](README.md) · [設計文件](docs/DESIGN.md) · [模擬說明](docs/SIMULATION.md) · [命名](docs/NAMING.md) · [Lertick](https://github.com/Denny-Lin/lertick)

## 目前狀態

**第 1 階段：規則版控制器與 RK3588 模擬器。** `lerwattd`（控制器）與 `lerwatt-sim`（模擬的 RK3588 板子）都能編譯，單元測試、閉迴路測試與端對端測試全部通過。目前還沒有學習功能，也還沒在真實硬體上跑過。

第一個平台是 ARM64，以 RK3588 板子為模型。硬體層只讀寫標準的 Linux 介面（cpufreq、cpuidle、thermal、hwmon），所以 `lerwattd` 不綁定 ARM64；要模擬別的板子，只需在 `sim/` 加一份平台描述。

對照 [Linux v7.3](https://github.com/torvalds/linux/tree/v7.3) 原始碼的可行性研究結論是：兩個控制器的原型可以只用 upstream 介面、不改 kernel 做出來，但原始構想要修正四點：

| 原始構想 | Linux v7.3 的實際情況 | Lerwatt 的設計 |
| --- | --- | --- |
| Power AI 決定每顆 CPU 的頻率 | sched_ext 接管所有任務時，schedutil 直接以 BPF 排程器透過 `scx_bpf_cpuperf_set()` 設定的目標為輸入 | 逐事件的效能需求由 Lertick 在 BPF 內給；Lerwatt 管上下限、預算與能源價格 |
| Power AI 決定電壓 | OPP core 把電壓綁在頻率上；SCMI 與 Qualcomm 硬體 DVFS 的電壓由韌體決定 | Lerwatt 只選 OPP（頻率點），從不碰 regulator |
| Power AI 選 idle 狀態 | menu/teo 在每次進入 idle 時於微秒內選擇 | Lerwatt 只設延遲上限、停用特定 state；逐次選擇留在 kernel |
| Power AI 取代熱管理 | critical trip 由 thermal core 處理；硬體節流回報 `hw_pressure` | Lerwatt 取代 IPA 這類預算分配 policy，所有安全路徑保持不變 |

VM 沒有真實的 cpufreq、cpuidle、溫度感測器、Energy Model 或大小核。在接上 RK3588 板子之前，由 `lerwatt-sim` 透過與真實板子相同的 kernel 檔案提供這些介面，所以 `lerwattd` 在兩邊都不用改，接上真實硬體時會自己切換。模擬結果只能證明控制迴路的行為；省電的結論必須靠板子與外接功率計。哪些部分是模擬的，見 [docs/SIMULATION.md](docs/SIMULATION.md)。

實驗性的學習版策略（`lerwattd --policy mpc`）會在執行中線上擬合板子的溫度模型，再用它規劃頻率上限。在模擬中它是安全的，但還沒有比規則版好，見 [docs/SIMULATION.md](docs/SIMULATION.md)。

**學習帶來的優勢尚未被證明。** 在宣稱任何效果之前，Lerwatt 必須和 EAS + schedutil + power-allocator 熱 governor，以及 [scx_lavd](https://github.com/sched-ext/scx/tree/main/scheds/rust/scx_lavd) 的省電模式比較。

## Lerwatt 決定什麼、絕不碰什麼

| 決策 | 負責者 | Linux v7.3 介面 |
| --- | --- | --- |
| 逐事件的效能需求 | Lertick（BPF） | `scx_bpf_cpuperf_set()` → schedutil |
| 大小核間的任務放置 | Lertick（BPF） | sched_ext 的 `select_cpu`、`dispatch` |
| 每個叢集的頻率下限與上限 | Lerwatt | `scaling_min_freq` / `scaling_max_freq`（freq QoS） |
| 每個叢集的功率預算 | Lerwatt | 依 Energy Model 換算成頻率上限（DTPM 的作法） |
| idle 深度 | Lerwatt | `power/pm_qos_resume_latency_us`、`cpuidle/stateN/disable` |
| 能耗與效能偏好 | Lerwatt | driver 支援時的 `energy_performance_preference` |
| 溫度功率預算 | Lerwatt | 低於 kernel passive trip 的自有預算 |
| 電壓 | 無人 | 只由 OPP core 與韌體處理 |
| critical trip 與硬體節流 | kernel 與硬體 | 不變 |

## 運作方式

兩個控制器在不同時間尺度運作：Lertick 在 BPF 內處理每一個排程事件；Lerwatt 在 userspace 以 100 ms 到 1 s 為週期。它們用「價格」而不是「命令」協調：某個叢集超出功率或溫度預算時，Lerwatt 調高它的能源價格，Lertick 在放置與效能決策中把價格算進去。架構圖與協定細節見 [docs/DESIGN.md](docs/DESIGN.md)。

## 快速開始

```sh
make            # 產生 build/lerwattd 與 build/lerwatt-sim
make test       # 模型單元測試與 900 秒閉迴路情境
make e2e        # 模擬器＋控制器經由檔案互動、還原與當機復原

# 在 VM 裡：用 VM 的真實負載驅動模擬板子，再啟動控制器。
sudo build/lerwatt-sim --load procstat --cooling passive &
sudo build/lerwattd --telemetry lerwatt.jsonl

# 在真實板子上：同一個控制器指令，它會自己找到 /sys。
sudo build/lerwattd --describe
```

| 路徑 | 角色 |
| --- | --- |
| `include/lerwatt/contract.h` | Lerwatt 與 Lertick 共用 map 的格式，唯一的共用 ABI |
| `include/lerwatt/hal.h`、`src/hal_*.c` | 真實機器介面：探測、取樣、寫入、還原日誌 |
| `include/lerwatt/controller.h`、`src/controller.c` | 純控制律，不做 I/O |
| `src/lerwattd.c` | 守護程序 |
| `sim/` | 模擬器：RK3588 資料、物理模型、kernel policy 模擬、假 ABI、負載來源 |
| `tests/` | 模型測試、閉迴路測試、端對端腳本 |

## 路線圖

| 階段 | 目標 | 完成條件 |
| --- | --- | --- |
| 0 | 研究與設計 | 可行性研究與本設計（已完成） |
| 1 | 模擬器與規則版 Lerwatt（進行中） | 已完成：模擬器、控制器、契約格式、測試。下一步：在 ARM64 VM 用真實負載執行；Lertick 寫 `lt_demand`、讀 `lw_contract` |
| 2 | 硬體與基準 | RK3588 上跑 `CONFIG_SCHED_CLASS_EXT=y` 的主線 kernel，外接量測能耗，校正模擬器，重現 EEVDF + schedutil 與 scx_lavd 基準 |
| 3 | 學習版 Lerwatt | 在固定安全範圍內，用 contextual bandit 或 MPC 搭配學到的溫度與功率模型 |
| 4 | 五組對照評估 | 基準、各自單獨、兩者不合作、兩者合作 |
| 5 | 釋出與 upstream 討論 | 結果可重現；只為證明必要的介面提出 patch |

## 安全

- Lerwatt 只寫可還原的設定：頻率上下限與 resume latency 限制。第一次寫入前先把原值記進日誌，絕不放寬它啟動時看到的限制，結束時全部還原。當機後執行 `lerwattd --restore`（或下次啟動時）會把原值寫回。
- 從不寫 regulator、從不改 trip 溫度、從不停用 thermal zone。
- Lerwatt 停止時由 kernel 原本的 policy 接手；Lertick 停止時 sched_ext 退回 fair class，schedutil 也自動改回讀 fair 的 utilization。

請在可以斷電重開、有序列埠 console 的開發板上實驗。

## 授權

程式與文件採 [MIT License](LICENSE)。未來的 BPF 程式會採 GPL-2.0-only，和 Lertick 一致。
