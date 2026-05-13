# 从 I2C TrackPoint 驱动可借鉴到 PS/2 驱动的功能

## 背景

参考项目：`zmk_config_sofle_macintosch_dongle-expert_amateur_set` 中的 `trackpoint_0x15.c`
- 接口：I2C + MOTION GPIO 中断
- 约 250 行，极简但功能完整
- 有几个设计思路值得移植到我们的 PS/2 驱动

---

## 一、指数加速算法（推荐移植 ⭐⭐⭐）

### I2C 驱动的做法

```c
float speed = (float)(abs(dx) + abs(dy)) / (float)delta_ms;
float mult = expf(speed * 1.307357f);  // e^(speed * k)
mult = MIN(mult, 2.0f);               // 上限 2x

fx = dx * BASE_SPEED * sensitivity * mult;
```

- 慢速精确（乘数≈1.0），快速加速（乘数最大 2.0）
- 用两包之间的时间间隔 `delta_ms` 计算瞬时速度
- 加速曲线是指数型，手感比线性好

### 对我们的价值

我们目前完全依赖 TP 内部的加速算法（sensitivity + neg-inertia + value6），MCU 侧是"原样透传"。但：
- TP 内部算法是 30 年前 IBM 设计的，参数调节有限
- 不同型号 TP（T430 / T460S / Sprintek）内部算法行为不一致
- MCU 侧加一层软件加速可以**统一手感**，不依赖具体 TP 型号

### 移植方案

在 `zmk_mouse_ps2_activity_move_mouse()` 里，在 `input_report_rel()` 之前：
1. 记录上一包时间戳 `last_packet_time`
2. 计算 `delta_ms = now - last_packet_time`
3. 计算加速因子 `mult = exp(speed * k)`，k 可配置
4. `mov_x *= mult; mov_y *= mult;`

需要加的 Kconfig：
- `CONFIG_ZMK_INPUT_MOUSE_PS2_EXPONENTIAL_ACCEL` (bool, default n)
- `CONFIG_ZMK_INPUT_MOUSE_PS2_ACCEL_MAX_MULT` (int, 百分比, default 200 = 2.0x)
- `CONFIG_ZMK_INPUT_MOUSE_PS2_ACCEL_FACTOR` (int, 千分比, default 1307 = 1.307)

### 注意事项

- 需要 `#include <math.h>` 或用整数近似（nRF52 没有 FPU，浮点很慢）
- 建议用整数查表或分段线性近似代替 `expf()`
- 要和 TP 内部加速叠加，可能需要把 TP 的 sensitivity 调低让 MCU 侧主导

---

## 二、滚轮模式增强（推荐移植 ⭐⭐⭐）

### I2C 驱动的做法

```c
// 1. 死区过滤
if (abs_delta <= SCROLL_DEADZONE) { *residue = 0; return; }

// 2. 动态除数（慢=粗粒度，快=细粒度）
int divisor = SLOW - ((SLOW - FAST) * abs_delta) / INPUT_MAX;

// 3. 累加器 + 整除
*residue += delta;
int16_t ticks = *residue / divisor;
if (ticks != 0) {
    input_report_rel(dev, WHEEL, ticks, true, K_FOREVER);
    *residue %= divisor;
}
```

### 对我们的价值

我们的 `input_listener_ps2.c` 里 scroll layer 的实现是硬编码的分段阈值：
```c
if (abs(val) >= 128) scaled_val = val / 24;
else if (abs(val) >= 64) scaled_val = val / 16;
// ...
```

I2C 驱动的方案更优雅：
- **累加器**：小位移不会丢失，慢慢累积到一个 tick
- **动态除数**：自动适应速度，不需要硬编码分段
- **死区**：滚轮模式下的噪声过滤

### 移植方案

替换 `input_listener_ps2.c` 里 scroll layer 的处理逻辑：
1. 加 `scroll_residue_x` / `scroll_residue_y` 到 `input_listener_ps2_data`
2. 用 I2C 驱动的 `process_scroll_axis()` 逻辑替换现有分段代码
3. Kconfig 配置：`SCROLL_DEADZONE`、`SCROLL_DIVISOR_SLOW`、`SCROLL_DIVISOR_FAST`

---

## 三、主导轴锁定（推荐移植 ⭐⭐）

### I2C 驱动的做法

```c
if (abs_dy * DENOM > abs_dx * NUMER) {
    dx = 0; // 纯竖向滚动
} else if (abs_dx * DENOM > abs_dy * NUMER) {
    dy = 0; // 纯横向滚动
} else {
    dx = 0; dy = 0; // 对角线死区
}
```

默认 `NUMER=3, DENOM=2`，即一个轴的位移必须是另一个轴的 1.5 倍才算"主导"。

### 对我们的价值

滚轮模式下用户通常想要纯竖向或纯横向滚动，但 TP 操作时手指很难完美直线。主导轴锁定可以：
- 消除斜向滚动的"抖动感"
- 让滚轮操作更精确

### 移植方案

在 scroll layer 处理逻辑里，`input_report_rel` 之前加主导轴判断。
DTS 或 Kconfig 配置比例参数。

---

## 四、按键切换鼠标/滚轮模式（可选 ⭐）

### I2C 驱动的做法

```c
ZMK_LISTENER(trackpoint_toggle_listener, toggle_listener_cb);
ZMK_SUBSCRIPTION(trackpoint_toggle_listener, zmk_position_state_changed);

// 按住某个键时切换模式
if (ev->position == TOGGLE_POSITION_CODE) {
    toggle_key_pressed = ev->state;
}
```

### 对我们的价值

我们已经有 layer toggle 机制（通过 `scroll-layer` DTS 属性），功能上等价。
但 I2C 驱动的方式更灵活：不需要专门的 layer，任何按键都能临时切换模式。

### 移植优先级：低

已有 layer toggle 满足需求，除非用户明确要求"按住某键变滚轮"的交互方式。

---

## 五、LED 亮度联动灵敏度（不推荐 ⭐）

I2C 驱动用 LED 亮度等级作为灵敏度的视觉反馈。有趣但：
- 需要额外硬件（LED）
- 我们已有 `&mms` 行为做运行时调节
- 不适合通用驱动模块

---

## 六、实施优先级建议

| 优先级 | 功能 | 工作量 | 收益 |
|---|---|---|---|
| 1 | 滚轮模式增强（累加器+动态除数+死区） | 中 | 滚轮手感大幅提升 |
| 2 | 主导轴锁定 | 小 | 滚轮精度提升 |
| 3 | 指数加速 | 中-大 | 鼠标手感统一化 |
| 4 | 按键切换模式 | 小 | 灵活性提升（已有替代） |

**建议先做 1+2**（都在 `input_listener_ps2.c` 的 scroll 处理里），改动集中、风险低、收益明显。
