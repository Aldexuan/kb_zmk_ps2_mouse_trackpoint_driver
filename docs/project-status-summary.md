# 项目当前状态总结

## 一、项目概述

基于 infused-kim 的 `kb_zmk_ps2_mouse_trackpoint_driver` ZMK 模块，为 PS/2 接口的 TrackPoint 添加了空闲低功耗、滚轮增强、按键重置等功能。目标是做成产品级键盘固件。

硬件：nice!nano v2 + PS/2 TrackPoint（UART 模式）+ 分体键盘

## 二、已完成的功能

### 2.1 空闲低功耗（方案 C：硬件切 VCC）

**状态**：✅ 已实现并稳定

| 项目 | 说明 |
|---|---|
| 触发条件 | ZMK 全局 idle（`CONFIG_ZMK_IDLE_TIMEOUT`） |
| 实现方式 | 订阅 `zmk_activity_state_changed`，idle 时断 VCC（P0.13），active 时恢复 |
| 省电效果 | idle 时 TP 从 4-5mA 降到 0mA |
| 唤醒延迟 | ~1.5 秒（200ms LDO + 600ms POR + 100ms settle + init） |
| 关键文件 | `src/pm/mouse_ps2_idle_pm.c`、`input_mouse_ps2.c`（power_down/up） |
| 用户配置 | `CONFIG_ZMK_INPUT_MOUSE_PS2_IDLE_POWER_SAVING=y` + DTS `vcc-gpios` |

**关键设计决策**：
- SCL 在断电时配为 `GPIO_OUTPUT_INACTIVE`（推挽低），不用 `GPIO_PULL_DOWN`（会漏电 30µA）
- UART pinctrl 切 sleep 由 `pm_device_action_run(SUSPEND)` 自动完成
- 唤醒后用 `apply_tp_settings_impl(true)` 下发 RAM 中的运行时值（保留 mms 调速）

### 2.2 滚轮模式增强

**状态**：✅ 已实现

| 功能 | 说明 |
|---|---|
| 累加器 + 动态除数 | 替代旧版硬编码分段，滚轮更平滑 |
| 死区过滤 | `scroll-deadzone=2`，过滤噪声 |
| 主导轴锁定 | `scroll-dominant-axis`，防斜向滚动 |
| DTS 可配置 | 所有参数通过 DTS 属性调节 |
| 关键文件 | `src/mouse/input_listener_ps2.c`、`dts/bindings/zmk,input-listener.yaml` |

### 2.3 鼠标移动死区过滤

**状态**：✅ 已实现

- `activity_move_mouse()` 里 deadzone=1
- `|dx|≤1 && |dy|≤1` 的包直接丢弃不上报
- 防止 TP 噪声阻止键盘进入 idle

### 2.4 按键重置 TrackPoint（`MS_TP_RESET`）

**状态**：✅ 已实现

| 项目 | 说明 |
|---|---|
| 键值 | `&mms MS_TP_RESET`（值=20） |
| 功能 | 完整 VCC 断电重启 + 释放所有卡住的鼠标按键 |
| 恢复时间 | ~1.5-2 秒 |
| 解决问题 | 偏移、乱飞、无响应、鼠标按键卡住 |
| 关键文件 | `input_mouse_ps2.c`（`reset_device_work_cb`）、`behavior_mouse_setting.c` |

### 2.5 唤醒后 TP 设置保留（mms 调速不丢失）

**状态**：✅ 已实现

- `apply_tp_settings_impl(from_wake)` 分 init/wake 两条路径
- init 路径用 DTS 值（settings_restore 后覆盖）
- wake 路径用 `data->tp_*` RAM 运行时值

## 三、已知问题（未解决）

### 3.1 唤醒后偶发异常（概率性）

| 症状 | 频率 | 根因 | 当前解决方案 |
|---|---|---|---|
| 光标一直向右/右上偏 | 低概率 | TP Z-force 基线校准偏移 | `&mms MS_TP_RESET` |
| 光标满屏乱飞 + 鼠标按键卡住 | 低概率 | PS/2 字节流错位 + 垃圾被解析为按键 | `&mms MS_TP_RESET`（会释放卡住的按键） |
| 唤醒后 TP 完全无响应 | 极低概率 | `init_thread` 的 `wait_for_mouse` 超时放弃 | `&mms MS_TP_RESET` |

**产品策略**：不做自动检测（容易误判），统一用按键重置兜底。

### 3.2 打字时 TP 不省电

**现状**：只有键盘全局 idle 才断 TP 电源。一直打字时 TP 持续耗电 3-5mA。

**分析文档**：`docs/tp-independent-idle-analysis.md`

**推荐方案**：TP 独立 idle 计时器 + 按键预唤醒（方案 C），但暂未实施。

### 3.3 唤醒后微小持续移动

**现象**：POR 后 TP 基线有 1-2 units 偏移，deadzone=1 可能不够。

**解决方向**：deadzone 调到 2，或等 TP 内部滤波器自行收敛（`wake_up_packets_to_discard=50` 已在做）。

## 四、代码改动总览（相对原始 infused-kim 版本）

| 文件 | 改动类型 | 说明 |
|---|---|---|
| `include/zmk/input_mouse_ps2.h` | 修改 | 导出 power_down/up/reset_device API |
| `include/dt-bindings/zmk/mouse_settings.h` | 修改 | 加 `MS_TP_RESET` |
| `src/drivers/input/input_mouse_ps2.c` | 修改 | VCC 控制、apply_tp_settings 拆分、deadzone、reset_device |
| `src/drivers/input/Kconfig` | 修改 | IDLE_POWER_SAVING + WAKE_DELAY_MS |
| `src/behaviors/behavior_mouse_setting.c` | 修改 | 加 MS_TP_RESET case |
| `src/mouse/input_listener_ps2.c` | 修改 | 滚轮累加器+动态除数+主导轴锁定 |
| `src/pm/mouse_ps2_idle_pm.c` | **新增** | ZMK activity 事件监听 |
| `dts/bindings/input/zmk,input-mouse-ps2.yaml` | 修改 | 加 vcc-gpios |
| `dts/bindings/zmk,input-listener.yaml` | 修改 | 加 scroll 参数 |
| `CMakeLists.txt` | 修改 | 编入 idle_pm.c |

## 五、用户配置清单

### 必须配置

```conf
# <board>.conf
CONFIG_ZMK_INPUT_MOUSE_PS2_IDLE_POWER_SAVING=y
```

```dts
/* overlay */
mouse_ps2: mouse_ps2 {
    status = "okay";
    compatible = "zmk,input-mouse-ps2";
    ps2-device = <&uart_ps2>;
    rst-gpios = <&gpio1 6 GPIO_ACTIVE_HIGH>;
    vcc-gpios = <&gpio0 13 GPIO_ACTIVE_HIGH>;  /* nice!nano v2 VCC EN */
};
```

### 可选配置

```dts
/* 滚轮参数 */
mouse_ps2_input_listener {
    scroll-layer = <4>;
    scroll-deadzone = <2>;
    scroll-divisor-slow = <60>;
    scroll-divisor-fast = <20>;
    scroll-input-max = <128>;
    scroll-dominant-axis;
};
```

### keymap 中使用

```c
#include <dt-bindings/zmk/mouse_settings.h>

/* 放在 adjust layer 或其他不易误触的位置 */
&mms MS_TP_RESET       /* TrackPoint 硬件重置 */
&mms MS_TP_SENSITIVITY_INCR  /* 灵敏度+ */
&mms MS_TP_SENSITIVITY_DECR  /* 灵敏度- */
```

## 六、未来可做的优化（按优先级）

| 优先级 | 功能 | 文档 |
|---|---|---|
| 1 | TP 独立 idle 计时器（打字时也省电） | `tp-independent-idle-analysis.md` |
| 2 | 指数加速算法（MCU 侧软件加速） | `i2c-features-to-adopt.md` |
| 3 | init_thread 失败后重试 | `long-sleep-wake-failure-analysis.md` |
| 4 | deadzone 可配置化（DTS 参数） | 当前硬编码=1 |

## 七、关键经验教训

1. **nRF52 GPIO_PULL_DOWN 会漏电 30µA**：用 `GPIO_OUTPUT_INACTIVE` 代替
2. **不要发 0xFF 软复位**：会让 TP 重新校准，引入持续微小移动
3. **pinctrl 跨 TU 不可访问**：用 `pm_device_action_run` 代替手动 `pinctrl_apply_state`
4. **自动检测不如手动重置**：做产品用按键兜底最稳
5. **测功耗要用同一个电源**：不同 USB 电源/充电头读数差异可达 30-40µA
