# TrackPoint 独立空闲休眠分析

## 问题描述

当前方案：TP 的 power_down 绑定在 ZMK 全局 `activity_state_changed` 事件上，只有整个键盘进入 idle 才会断 TP 电源。

**痛点**：用户一直打字（键盘活跃）但不碰 TrackPoint 时，TP 仍然全速运行（3-5mA），白白耗电。只有停止所有操作等 idle timeout 到了才省电。

**期望**：TP 有自己独立的"不用就关"逻辑，跟键盘活跃状态解耦。

## 方案分析

### 方案 A：TP 独立 idle 计时器

**思路**：TP 驱动自己维护一个"最后一次 TP 活动"时间戳。如果超过 N 秒没有 TP 移动/点击，就自动 power_down。任何 TP 活动（或键盘按键）唤醒它。

**实现要点**：
1. 在 `activity_move_mouse()` 和 `activity_click_buttons()` 里更新 `last_tp_activity_time`
2. 用一个 `k_work_delayable` 做定时检查（比如每 5 秒检查一次）
3. 如果 `now - last_tp_activity_time > TP_IDLE_TIMEOUT`，调 `power_down()`
4. 唤醒触发：
   - TP 活动：不可能（已经断电了）
   - **键盘按键**：订阅 `zmk_position_state_changed`，任何按键按下时检查 TP 是否 down，如果是就 `power_up()`
   - 或者更简单：只在用户碰 TP 时才唤醒（但 TP 断电后无法感知触碰）

**难点**：
- TP 断电后**无法自己唤醒**（不像有 MOTION 引脚的 I2C TP）
- 必须靠外部事件（键盘按键）来唤醒
- 用户打字时每次按键都要检查"TP 是否需要唤醒"，如果是就要等 1.5 秒 POR —— **打字时突然想用 TP 会有明显延迟**

**优点**：
- 打字期间 TP 完全断电，省 3-5mA
- 逻辑简单清晰

**缺点**：
- 唤醒延迟 1.5 秒（POR + init），用户体验差
- 每次按键都要检查 TP 状态，增加按键处理开销（虽然很小）
- 如果用户频繁在打字和 TP 之间切换，会频繁断电/上电，反而更耗电（POR 期间 TP 也在吃电）

### 方案 B：TP 独立 idle 但不断 VCC，只停 reporting

**思路**：不切 VCC，只发 `0xF5`（Disable Reporting）+ 关 UART RX IRQ。TP 仍然有电但不发数据，MCU 不处理。

**实现要点**：
1. 同方案 A 的计时器逻辑
2. 超时后：`activity_reporting_disable()` + `uart_irq_rx_disable()`
3. 唤醒时：`uart_irq_rx_enable()` + `activity_reporting_enable()`（发 `0xF4`）
4. 唤醒触发：同方案 A（键盘按键）

**优点**：
- 唤醒**极快**（~10ms，只需发一个 `0xF4` 命令）
- 不需要 POR，不需要重新 init，不需要重下 TP 设置
- 不会有"偏移/乱飞"问题（TP 内部状态完整保留）
- 省掉 UART 持续中断的 CPU 唤醒功耗（约 0.5-1mA）

**缺点**：
- TP 自身仍然耗电（约 3mA），只省了 MCU 侧的 UART 处理开销
- 省电效果有限（从 4-5mA 降到约 3mA）

**适用场景**：
- 如果你主要想省的是"UART 持续唤醒 CPU"的那部分功耗
- 如果你不能接受 1.5 秒唤醒延迟

### 方案 C：TP 独立 idle + VCC 断电 + 按键预唤醒

**思路**：方案 A 的改进版。TP 独立计时断电，但唤醒不是"按键时才开始 POR"，而是**提前预判**。

**实现要点**：
1. TP 独立计时器，超时后 `power_down()`
2. 订阅 `zmk_position_state_changed`（任何按键事件）
3. 按键时立即 `k_work_submit(power_up_work)` 开始唤醒
4. 唤醒在后台 work queue 执行（1.5 秒）
5. 用户打字期间 TP 在后台默默上电
6. 等用户真正碰 TP 时，大概率已经 ready 了

**优点**：
- 打字期间 TP 断电省 3-5mA
- 用户从打字切到 TP 时，如果之前有按键，TP 可能已经 ready
- 最差情况也只等 1.5 秒

**缺点**：
- 如果用户打完字立刻碰 TP（< 1.5 秒），还是会有延迟
- 频繁打字会频繁触发 power_up，如果 TP idle timeout 太短会反复断电/上电
- 需要防抖：连续按键只触发一次 power_up

**优化**：
- 设一个"冷却期"：power_down 后至少 N 秒内不响应按键唤醒（避免刚断就开）
- 或者：只在"第一次按键"时唤醒，后续按键不重复触发

### 方案 D：TP 独立 idle + 只在特定条件唤醒

**思路**：不是任何按键都唤醒 TP，而是只在"可能要用 TP"的信号出现时才唤醒。

**可能的唤醒信号**：
- 切换到包含鼠标按键的 layer（比如你的 scroll layer 或 mouse layer）
- 按下特定的"TP 唤醒键"（手动触发，类似之前分析的 `&tp_reset`）
- 长时间不打字（比如停顿 2 秒后第一次按键 → 可能要切到 TP）

**优点**：
- 避免频繁无意义的 power_up
- 唤醒更精准

**缺点**：
- 逻辑复杂
- 可能漏判（用户想用 TP 但没触发唤醒条件）

## 推荐方案

### 短期推荐：方案 C（独立计时 + 按键预唤醒）

理由：
1. 省电效果最好（TP 完全断电）
2. 用户体验可接受（打字时后台唤醒，大多数情况无感）
3. 实现复杂度中等（基于现有 power_down/up 基础设施）
4. 最差情况有兜底（等 1.5 秒也不是不能用）

### 参数建议

| 参数 | 建议值 | 说明 |
|---|---|---|
| TP idle timeout | 30-60 秒 | 不碰 TP 多久后断电 |
| 唤醒冷却期 | 5 秒 | 断电后至少等 5 秒才响应按键唤醒 |
| 唤醒触发 | 任意按键 | 第一次按键触发，后续忽略直到 TP ready |

### 长期考虑：方案 B 作为补充

方案 B（只停 reporting 不断 VCC）可以作为"轻度省电"模式：
- TP idle 10 秒 → 进入方案 B（停 reporting，省 UART 开销）
- TP idle 60 秒 → 进入方案 C（断 VCC，彻底省电）

两级省电，兼顾响应速度和功耗。

## 与现有方案的关系

| 触发条件 | 现有方案 | 新方案 |
|---|---|---|
| 键盘全局 idle | TP power_down | 保留（作为最终兜底） |
| TP 独立 idle | 无 | 新增（更积极的省电） |
| 键盘 sleep | ZMK 自动断电 | 保留（不变） |

新方案是**叠加**在现有方案之上的，不替换。即使 TP 独立 idle 逻辑有 bug，全局 idle 仍然是最终安全网。

## 实现难度评估

| 组件 | 工作量 | 说明 |
|---|---|---|
| TP 活动时间戳 | 小 | 在 move_mouse/click 里加一行 |
| 定时检查 work | 小 | k_work_delayable 标准用法 |
| 按键事件订阅 | 中 | ZMK_LISTENER + zmk_position_state_changed |
| 防抖/冷却逻辑 | 小 | 几个时间戳比较 |
| 与现有 idle PM 协调 | 中 | 避免两套逻辑冲突 |
| 总计 | **中等** | 约 100-150 行新代码 |

## 风险

1. **频繁 VCC 循环加速 TP 老化**：理论上有，实际上 TP 设计寿命远超键盘使用年限
2. **POR 偶发失败**：已有 `wait_for_mouse` 重试机制兜底
3. **唤醒后偏移/乱飞**：已有 `wake_up_packets_to_discard` + deadzone 兜底，加上之前分析的"按键重置"方案作为最终手段
4. **与全局 idle PM 冲突**：需要加互斥锁，避免两边同时操作 VCC/UART
