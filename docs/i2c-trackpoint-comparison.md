# I2C TrackPoint 驱动对比分析

## 一、项目概况

| 项目 | 接口 | 文件 | 特点 |
|---|---|---|---|
| 我们的 PS/2 驱动 | PS/2 (UART 模拟) | `input_mouse_ps2.c` (~2000行) | 完整 PS/2 协议栈、TP 命令、settings 持久化 |
| I2C 驱动 (参考) | I2C | `trackpoint_0x15.c` (~250行) | 极简、中断驱动、纯轮询读包 |

## 二、I2C 驱动的架构

```
TP MOTION 引脚 (GPIO0.14, 低有效)
    ↓ 边沿中断
motion_isr()
    ↓ k_work_submit
trackpoint_work_cb()  [系统工作队列]
    ↓ while(motion_pin == HIGH)
    ↓   i2c_read_dt() 读 7 字节包
    ↓   解析 dx, dy
    ↓   input_report_rel()
    ↓ 循环直到 motion_pin 变低
```

## 三、值得借鉴的设计点

### 3.1 ★ 中断驱动 + MOTION 引脚（最关键差异）

I2C 驱动用了一个 **MOTION GPIO 引脚**（TP 的 MOTION/INT 输出）：
- TP 有数据时拉高 MOTION 引脚
- MCU 配置为边沿中断
- 只有 MOTION 引脚触发时才去读数据

**对我们的意义**：
- PS/2 协议没有独立的 MOTION 引脚（数据直接在 CLK/DATA 线上传输）
- 但这个思路可以用在**唤醒场景**：如果 TP 有 MOTION 引脚可用，idle 时可以只监听 MOTION 中断作为唤醒源，而不需要保持 UART 活跃
- 不过大多数 PS/2 TrackPoint 没有暴露 MOTION 引脚，所以对我们不直接适用

### 3.2 ★ 指数加速算法

```c
static inline float trackpoint_exponential_factor(int8_t dx, int8_t dy, uint32_t delta_ms) {
    float speed = (float)dist / (float)delta_ms;
    float mult = expf(speed * 1.307357f);
    return (mult > TP_MAX_MULT) ? TP_MAX_MULT : mult;
}
```

- 基于"速度 = 位移/时间"计算指数加速因子
- 慢速移动 → 精确（乘数≈1）
- 快速移动 → 加速（乘数最大 2.0）
- 用 `delta_ms`（两包间隔时间）参与计算

**对我们的意义**：
- 我们的 PS/2 驱动完全依赖 TP 内部的加速算法（sensitivity/neg-inertia/value6）
- 可以考虑在 MCU 侧也加一层软件加速，作为 TP 内部算法的补充
- 但这不是当前问题的重点

### 3.3 ★ 滚轮模式的死区 + 累加器设计

```c
#define SCROLL_DEADZONE 2  // 死区=2

static inline void process_scroll_axis(...) {
    if (abs_delta <= SCROLL_DEADZONE) {
        *residue = 0;  // 死区内清零累加器
        return;
    }
    // 动态除数：慢速用大除数（粗粒度），快速用小除数（细粒度）
    int divisor = SCROLL_DIVISOR_SLOW - 
                  ((SCROLL_DIVISOR_SLOW - SCROLL_DIVISOR_FAST) * abs_delta) / SCROLL_INPUT_MAX;
    *residue += (delta * dir_mult);
    int16_t scroll_ticks = *residue / divisor;
    if (scroll_ticks != 0) {
        input_report_rel(dev, input_code, scroll_ticks, true, K_FOREVER);
        *residue %= divisor;
    }
}
```

**对我们的意义**：
- **死区概念**直接适用于我们的问题！I2C 驱动用 `SCROLL_DEADZONE=2` 过滤噪声
- 我们刚加的 `deadzone=1` 就是同样的思路，但只用在了 move_mouse 上
- 他们的死区内还会**清零累加器**，防止噪声慢慢累积最终触发一次移动

### 3.4 ★ 主导轴锁定（防误触）

```c
if (abs_dy * DOMINANT_DENOMINATOR > abs_dx * DOMINANT_NUMERATOR) {
    dx = 0; // 纯竖向
} else if (abs_dx * DOMINANT_DENOMINATOR > abs_dy * DOMINANT_NUMERATOR) {
    dy = 0; // 纯横向
} else {
    dx = 0; dy = 0; // 对角线死区
}
```

- 滚轮模式下锁定主导轴，防止斜向滚动
- 对角线区域直接丢弃（双轴死区）

**对我们的意义**：
- 如果我们的 scroll layer 有类似需求可以借鉴
- 但跟当前低功耗问题无关

### 3.5 模式切换用按键位置监听

```c
ZMK_LISTENER(trackpoint_toggle_listener, toggle_listener_cb);
ZMK_SUBSCRIPTION(trackpoint_toggle_listener, zmk_position_state_changed);
```

- 监听特定按键位置来切换鼠标/滚轮模式
- 不需要 layer toggle，直接用 ZMK 事件系统

**对我们的意义**：
- 我们的 idle PM 模块也用了 `ZMK_LISTENER` / `ZMK_SUBSCRIPTION`，思路一致
- 可以考虑用类似方式做"手动触发 TP 重校准"的按键

### 3.6 LED 亮度联动灵敏度

```c
uint8_t tp_led_brt = custom_led_get_last_valid_brightness();
float tp_factor = MOUSE_SENS_BASE + MOUSE_SENS_STEP * tp_led_brt;
```

- 用 LED 亮度等级作为灵敏度调节的视觉反馈
- 有趣但跟我们的问题无关

## 四、对我们当前问题的启发

### 4.1 关于"不进 idle"的问题

I2C 驱动的关键区别：**它只在 MOTION 引脚触发时才读数据**。如果 TP 没有数据，MOTION 引脚保持低电平，MCU 完全不会被打扰。

我们的 PS/2 驱动：**UART RX 一直在监听**。TP 发的每一个字节（包括噪声）都会触发 UART IRQ → 进入 callback → 解析 → `input_report_rel()` → ZMK 认为有活动。

**这就是为什么 deadzone 过滤是必要的**：我们没有 MOTION 引脚来区分"TP 真的被碰了"和"TP 在发噪声"。

### 4.2 关于"唤醒后偏移"的问题

I2C 驱动完全没有处理这个问题（没有 POR、没有 reset、没有 discard）。可能原因：
- I2C TrackPoint（如 Sprintek SK8707）的 Z-force 校准比老 IBM/Lenovo PS/2 TP 更稳定
- 或者他们的 TP 从不断电（没有 idle power saving）
- 或者他们的用户也遇到了但没修

### 4.3 关于 `SCROLL_DEADZONE` 的启发

他们用 `SCROLL_DEADZONE=2`，说明 I2C TP 的噪声也能到 ±2。我们的 PS/2 TP 用 `deadzone=1` 可能不够。

**建议**：如果测试发现 deadzone=1 还是偶尔阻止 idle，可以提到 2。

## 五、可以借鉴但暂不实施的改进

| 改进 | 来源 | 优先级 | 说明 |
|---|---|---|---|
| 死区过滤 | `SCROLL_DEADZONE` | ✅ 已做 | 我们加了 deadzone=1 |
| 指数加速 | `trackpoint_exponential_factor` | 低 | TP 内部已有加速算法 |
| 主导轴锁定 | `DOMINANT_NUMERATOR/DENOMINATOR` | 低 | scroll layer 可选 |
| MOTION 引脚唤醒 | `motion_gpio` | 不适用 | PS/2 TP 没有此引脚 |
| 按键切换模式 | `ZMK_LISTENER` | 低 | 已有 layer toggle |

## 六、结论

I2C 驱动最大的优势是**中断驱动 + MOTION 引脚**，让它天然不受噪声困扰（没数据就不读）。我们的 PS/2 驱动因为协议限制必须持续监听 UART，所以需要软件层面的 deadzone 过滤来达到类似效果。

对于当前的"不进 idle"问题，deadzone 过滤是正确且必要的方案。I2C 驱动的 `SCROLL_DEADZONE=2` 也验证了这个思路的合理性。
