# Lerwatt — 會自我學習的 Linux 電源管理器

Lerwatt 是給 ARM64 Linux 的實驗性電源管理器。它學習 CPU 頻率上下限、idle 深度限制與溫度功率預算，並和 [Lertick](https://github.com/Denny-Lin/lertick) 這個 sched_ext 排程器合作，讓排程與電源決策不再互相拉扯。

[English](README.md) · [設計文件](docs/DESIGN.md) · [命名](docs/NAMING.md) · [Lertick](https://github.com/Denny-Lin/lertick)

## 目前狀態

**第 0 階段：研究與設計，尚無程式碼。** 對照 [Linux v7.3](https://github.com/torvalds/linux/tree/v7.3) 原始碼的可行性研究結論是：兩個控制器的原型可以只用 upstream 介面、不改 kernel 做出來，但原始構想要修正四點：

| 原始構想 | Linux v7.3 的實際情況 | Lerwatt 的設計 |
| --- | --- | --- |
| Power AI 決定每顆 CPU 的頻率 | sched_ext 接管所有任務時，schedutil 直接以 BPF 排程器透過 `scx_bpf_cpuperf_set()` 設定的目標為輸入 | 逐事件的效能需求由 Lertick 在 BPF 內給；Lerwatt 管上下限、預算與能源價格 |
| Power AI 決定電壓 | OPP core 把電壓綁在頻率上；SCMI 與 Qualcomm 硬體 DVFS 的電壓由韌體決定 | Lerwatt 只選 OPP（頻率點），從不碰 regulator |
| Power AI 選 idle 狀態 | menu/teo 在每次進入 idle 時於微秒內選擇 | Lerwatt 只設延遲上限、停用特定 state；逐次選擇留在 kernel |
| Power AI 取代熱管理 | critical trip 由 thermal core 處理；硬體節流回報 `hw_pressure` | Lerwatt 取代 IPA 這類預算分配 policy，所有安全路徑保持不變 |

電源研究需要真實硬體。VM 沒有真實的 cpufreq、cpuidle、溫度感測器、Energy Model 或大小核，所以 Lertick 現有的 QEMU 環境無法評估 Lerwatt。第一個目標平台是 RK3588 開發板加外接功率計。

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

## 路線圖

| 階段 | 目標 | 完成條件 |
| --- | --- | --- |
| 0 | 研究與設計 | 可行性研究與本設計（已完成） |
| 1 | 硬體與基準 | RK3588 上跑 `CONFIG_SCHED_CLASS_EXT=y` 的主線 kernel，外接量測能耗，重現 EEVDF + schedutil 與 scx_lavd 基準 |
| 2 | 規則版 Lerwatt 與契約 map | 不靠學習的控制器能穩定守住功率預算；Lertick 加入容量感知放置與 `cpuperf` 目標 |
| 3 | 學習版 Lerwatt | 在固定安全範圍內，用 contextual bandit 或 MPC 搭配學到的溫度與功率模型 |
| 4 | 五組對照評估 | 基準、各自單獨、兩者不合作、兩者合作 |
| 5 | 釋出與 upstream 討論 | 結果可重現；只為證明必要的介面提出 patch |

## 安全

- Lerwatt 只寫可還原的設定：頻率上下限、能耗偏好、resume latency 限制與 idle state 停用。啟動時記錄原值，結束時還原。
- 從不寫 regulator、從不改 trip 溫度、從不停用 thermal zone。
- Lerwatt 停止時由 kernel 原本的 policy 接手；Lertick 停止時 sched_ext 退回 fair class，schedutil 也自動改回讀 fair 的 utilization。

請在可以斷電重開、有序列埠 console 的開發板上實驗。

## 授權

文件採 [MIT License](LICENSE)，和 Lertick 相同。未來的 BPF 程式會採 GPL-2.0-only，和 Lertick 一致。
