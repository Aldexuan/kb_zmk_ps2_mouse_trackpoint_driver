# TrackPoint 空闲低功耗问题分析

## 一、问题描述

使用此驱动驱动 TrackPoint 时：

- 键盘正常使用：TrackPoint 持续消耗 **4~5 mA**
- 键盘空闲 60s（ZMK 进入 idle 状态）：其他外设功耗下降，**但 TrackPoint 仍然 4~5 mA**
- 只有整个键盘进入 **sleep** 模式后，TrackPoint 才真正断电降下来
- 目标：空闲（不打字、不动 TP）时也能让 TP 进入低功耗

## 二、项目整体运行方式（速览）

项目是 zmk 模块（非 fork），把 TrackPoint/PS2 鼠标以 Zephyr `input` 子系统方式接入 zmk。

### 代码分层

| 层 | 文件 | 作用 |
|---|---|---|
| PS/2 物理层（二选一） | `src/drivers/ps2/ps2_uart.c` | **推荐**：借用 nRF52 UART 硬件收发 PS/2 帧 |
| PS/2 物理层 | `src/drivers/ps2/ps2_gpio.c` | 备用：纯 GPIO 中断位操作 |
| 鼠标/TP 驱动 | `src/drivers/input/input_mouse_ps2.c` | PS/2 协议命令、TP 初始化、数据包解析、settings 持久化 |
| ZMK 事件适配 | `src/mouse/input_listener_ps2.c` | 接收 Zephyr input 事件，可选 Auto Layer Toggle |
| 行为 | `src/behaviors/behavior_mouse_setting.c` | 运行时用键位调节灵敏度等 |

### 关键调用链（移动时）

```
TP → UART RX IRQ (ps2_uart.c)
   → 解析出一个 byte
   → ps2_callback_t data->callback_isr()
   → zmk_mouse_ps2_activity_callback()  (input_mouse_ps2.c)
   → 累到 3/4 字节 → 解析包 → input_report_rel(…)
   → input_listener_ps2.c → ZMK HID / Layer Toggle
```

### README 4.3 节官方结论

- 已知 TP 耗 3~3.85 mA，110 mAh 电池约 30h
- 理论上 TP 有 low-power mode（约 890uA）
- **作者说"还没尝试过，不确定是否所有 TP 都支持"** —— 等于这条路没做

## 三、代码侧调查结论

### 3.1 已有但不完整的"半成品"

驱动里已经有低功耗相关的痕迹，说明有人开始做但没接上 ZMK 的活动事件：

- `struct zmk_mouse_ps2_data` 有字段 `wake_up_packets_to_discard`（注释 `Power saving: Track wake-up packets to discard initial drift`）
- `zmk_mouse_ps2_activity_reporting_enable()` / `_disable()` 函数已经存在并可用
- `enable` 会发 `0xF4`（Enable Reporting）+ 标志位置 on + 丢弃开头 20 个包的初始噪声
- `disable` 会发 `0xF5`（Disable Reporting）+ 标志位置 off

### 3.2 问题所在：reporting disable 没省电

`ps2_uart.c` 中：

```c
static int ps2_uart_disable_callback(const struct device *dev) {
    ps2_uart_data_queue_empty();
    data->callback_enabled = false;  // 仅仅是标志位
    return 0;
}
```

**`disable_callback` 根本没有关 UART、没有关 GPIO、也没有切断 TP 电源**：
- UART RX IRQ 一直开着（`uart_irq_rx_enable` 没被 disable）
- TP VCC/RST 没有被拉低
- 即使发了 `0xF5`（Disable Reporting），TP 仍处于 Stream Mode 的 idle，只是不发数据而已，功耗不会明显下降

### 3.3 完全没有订阅 ZMK 活动事件

grep 搜索 `ZMK_SUBSCRIPTION / ZMK_LISTENER / zmk_activity_state / activity_state_changed` 在 src/ 下 **0 命中**。

结论：驱动不知道 ZMK 什么时候进入 idle / sleep，所以 idle 时什么都不做。

### 3.4 电源相关硬件钩子

- DTS 支持 `rst-gpios`（TP Power-On-Reset 脚）
- 目前 `rst_gpio` 只在初始化时用了一次 POR 时序，之后从未再操作
- 没有 Kconfig 选项控制 VCC / VDD 开关 GPIO（需要硬件预留一个 GPIO 驱动 P-MOSFET 才能真·断电）

## 四、问题根因

1. **驱动完全没监听 ZMK activity state**（idle/sleep 事件不通知 TP）
2. 即便调用 `activity_reporting_disable()`，也只是让 TP 不发数据，UART 外设+TP 自身逻辑功耗并未下降
3. 从"停发数据"到"真正省电"缺三件事：
   - 关 UART RX IRQ（释放 nRF52 UART 外设，让 HFCLK 可以关）
   - 让 TP 本身进入低功耗（发 `0xF5` + 可能需要硬件切电）
   - 唤醒路径（让 TP 的动作能把 MCU 唤回来）

## 五、可行的解决思路（按侵入度从小到大）

### 方案 A：软件 idle（最小改动，效果中等）

- 订阅 `zmk_activity_state_changed` 事件
- 进入 `ZMK_ACTIVITY_IDLE` 时：
  - 调 `zmk_mouse_ps2_activity_reporting_disable()`（已有）
  - 额外调 `uart_irq_rx_disable()` 关 UART RX
  - 关闭 UART 设备的 pm：`pm_device_action_run(uart_dev, PM_DEVICE_ACTION_SUSPEND)`
- 回 `ZMK_ACTIVITY_ACTIVE` 时反向恢复
- 预期：省掉 UART + CPU 频繁唤醒的电流，TP 自身约 3mA 仍在
- 难点：TP 进入 idle 后用户一动 TP 无法立即唤醒（需要额外的 GPIO 中断做 wake），可能要配合键盘按键来唤醒

### 方案 B：TP 低功耗命令（中等改动，效果更好）

部分 TP 支持通过 PS/2 命令进入低功耗 / 挂起：
- 发 `0xF5` Disable Reporting（已做）
- 进一步尝试 `0xEC`（Reset Wrap Mode）或 Set Remote Mode (`0xF0`)，让 TP 只在被 poll 时才工作
- 组合方案 A 的 UART 挂起

效果受限于具体 TP 型号（T430 / T460S / Sprintek 等行为不同），README 作者没测就是因为这个。

### 方案 C：硬件切断 VCC（最大改动，效果最好）

- PCB 上加一个 P-MOSFET 用 GPIO 控制 TP 的 VCC
- idle 时驱动拉该 GPIO 让 TP 完全断电（省掉 3mA 全部）
- 唤醒时重新 POR
- 需要：硬件改板 + 驱动加 `vcc-gpios` 属性 + 唤醒源（例如一个键盘按键专门作为 wake）

### 方案 D：只关 UART（最小方案，不碰 TP）

- idle 时仅 `uart_irq_rx_disable()` + `pm_device suspend`
- TP 仍会往线上发数据但 MCU 不再响应
- 省 UART/CPU 的耗电，TP 3mA 省不下来
- 最安全、兼容所有 TP，也是"至少先做起来"的路径

## 六、推荐实施顺序

1. **先做方案 D**（稳妥、兼容性好）：加 activity listener + UART RX 挂起，验证能省多少
2. **叠加方案 A**：加上 TP 0xF5 + wake-up 时发 0xF4
3. **若仍不够低**：尝试方案 B（不同 TP 测试命令）
4. **如果用户能改硬件**：方案 C 最彻底

## 七、下一步要看的代码点

- ZMK 侧：`zmk/events/activity_state_changed.h`、`zmk_activity_state` API（确认事件名）
- 本项目 `input_mouse_ps2.c` 的 init 流程（找合适位置注册 listener）
- `ps2_uart.c` 的 `uart_irq_rx_enable` 位置，看能否安全 toggle
- 确认 `CONFIG_PM_DEVICE` 是否已启用或需要用户加

---

## 八、用户采用方案 C（硬件切 VCC）后的实测问题分析

### 8.1 用户硬件情况

- 主控：**nice!nano v2**（板上 P0.13 控制板载 LDO 的 EN，拉低即关断 `VCC` 输出，官方就是给外设做低功耗用的）
- TrackPoint VCC 接在该 VCC 轨上
- 同时还接了 POR 脚（例子 DTS 里 `MOUSE_PS2_PIN_RST_PRO_MICRO = <&gpio1 6 ...>`）
- SCL/SDA 用 UART 驱动：SCL=P0.06，SDA=P0.08
- 现象：**切 VCC → 再给 VCC，其他按键正常，TP 不再工作**

### 8.2 为什么 VCC 切回来后 TP 废掉

不改代码直接切 VCC 会坏掉，是因为**驱动里有状态机／硬件时序强假设"TP 只上电一次"**。具体踩了 4 个坑：

#### 坑 1：Power-On-Reset 时序只在 init 时跑一次

`zmk_mouse_ps2_init_power_on_reset()` 在 `DEVICE_DT_INST_DEFINE` 的 init 流程中只调用一次，之后从未再调用。TP 重新上电后需要的 600ms POR 时序完全没人做。

结果：TP 实际已经做自测并发了 `0xAA`，但主机（MCU）没给它"电源已稳"的 RST 脉冲，有些 TP 固件会卡在 reset 态。

#### 坑 2：Reporting 开关状态不同步

TP 断电重启后内部一定是 **reporting=disabled（F5 状态）** —— 这是 PS/2 协议默认值。
但驱动里 `data->activity_reporting_on` 还是 `true`（根本没人改它），之后谁也不会再发 `0xF4` 命令。

结果：**TP 根本不发数据**，UART 收不到字节 → 鼠标不动。

#### 坑 3：包解析状态机错位

TP 上电会先发：
```
0xAA  (self-test passed)
0x00  (device id = mouse)
```
这 2 个字节一定会进 UART FIFO。

但驱动里 `data->packet_idx`、`data->packet_buffer` 保持着断电前的状态，这两个自测字节会被当成"一个鼠标数据包的中间字节"处理，然后：
- 若 `packet_idx == 0`：检查 `alignment_bit`，0xAA bit3=1 刚好过校验 → 被当成一个合法包头，后面全乱
- 若 `packet_idx > 0`：直接塞进 buffer 某个位置，之后的 stream 全错位

即使 TP 奇迹般地开始发数据包，MCU 也永远对不齐帧。

#### 坑 4：切 VCC 时 SCL/SDA 被 MCU 反向供电

TP 的 VCC 被断掉时，MCU 的 GPIO（SCL/SDA）仍然输出高电平（3.3V），电流会通过 TP 内部的 ESD 保护二极管倒灌回 TP 的 VCC 轨：
- 让 TP"假活"：VCC 被寄生电压抬到 1.5~2V，不够真正运行，但足以让复位电路失效
- 下次给正 VCC 时 TP 可能不会正确执行 Power-On 时序
- 还会漏电抵消省电收益

这一点在 PS/2 / I²C 这种开漏总线 + 外部上拉的方案里很常见。

#### 坑 5（可能）：UART 外设 pinctrl 不切 sleep

切 VCC 的同时若没把 UART pinctrl 切到 `sleep/low-power-enable` 状态，RX 引脚会持续从（已掉电的）TP 线上读到浮空噪声，不仅浪费电，还可能让 UART FIFO 塞满错误字节。

### 8.3 要把"硬切 VCC"做对，必须做的 5 件事

按顺序：

1. **下电前**（进 idle）：
   1. `zmk_mouse_ps2_activity_reporting_disable()` 发 `0xF5`
   2. `uart_irq_rx_disable(uart_dev)` 关 UART RX 中断
   3. 把 UART pinctrl 切到 `sleep` 状态（或者直接把 SCL/SDA 配成 GPIO input + no-pull），**防止 MCU 反向供电**
   4. `pm_device_action_run(uart_dev, PM_DEVICE_ACTION_SUSPEND)` 挂起 UART 外设
   5. 拉低 VCC 控制 GPIO（nice!nano v2 的 P0.13）→ TP 真正断电
   6. 复位驱动状态：`packet_idx=0`、`memset(packet_buffer)`、`activity_reporting_on=false`、`wake_up_packets_to_discard=20`
2. **保持 idle**：不做任何 PS/2 操作
3. **唤醒触发**：键盘某个按键按下、split 侧有输入、或者任何 ZMK 把 activity state 切回 ACTIVE 的源
4. **上电时**（回 active）：
   1. 恢复 UART pinctrl 到 `default`
   2. `pm_device_action_run(uart_dev, PM_DEVICE_ACTION_RESUME)`
   3. 拉高 VCC GPIO（P0.13）→ TP 上电
   4. **重跑 `zmk_mouse_ps2_init_power_on_reset()`**：POR 线低→等 600ms→高
   5. **清 UART RX 队列 + 包缓冲**（扔掉 0xAA/0x00 或任何垃圾）
   6. 可选：重新发一次 `0xFF` (Reset) 等 `0xAA 0x00`，让两边对齐
   7. **重新发 `0xF4`** 开启 reporting
   8. `uart_irq_rx_enable(uart_dev)`
   9. 设 `wake_up_packets_to_discard = 20`，丢掉上电噪声
5. **TP 设置重下发**（关键，很多人漏）：
   - TP 断电后 sensitivity / neg-inertia / value6 / PTS / invert-x,y / xy-swap 这些 RAM 设置**全部丢失**
   - 必须按 `zmk_mouse_ps2_init_thread()` 中那一大段重新调用 `zmk_mouse_ps2_tp_*_set()` 把用户设置下发回 TP
   - 建议把这段逻辑抽成独立函数 `zmk_mouse_ps2_apply_tp_settings()`，init 和 wake 都调

### 8.4 最小可行 patch 草图（给下一步实施看）

```c
// 新增 API（input_mouse_ps2.h）
int zmk_mouse_ps2_power_down(void);    // 进 idle 调用
int zmk_mouse_ps2_power_up(void);      // 回 active 调用

// input_mouse_ps2.c 里：
// - 把 init_thread 的 TP 设置部分抽成 apply_tp_settings(cfg)
// - power_down: reporting_disable + uart_rx_disable + pinctrl sleep
//   + pm_suspend + pull P0.13 low + 重置解析状态
// - power_up: pull P0.13 high + POR 600ms + pm_resume + pinctrl default
//   + uart_rx_enable + 清 FIFO + wait_for_mouse + apply_tp_settings
//   + reporting_enable

// 新文件：src/pm/mouse_ps2_pm.c
// - 订阅 zmk_activity_state_changed 事件
// - IDLE → power_down；ACTIVE → power_up
// - 在工作队列里做，避免在事件回调里阻塞 600ms
```

### 8.5 硬件侧补充建议

如果切 VCC 时 SCL/SDA 仍会反向供电，除了软件在 `power_down` 里把 pin 改成 input no-pull 外，更干净的做法是：

- 在 TP 的 SCL/SDA 信号线上各串一只串流控（例如一颗 N-MOSFET 或一组小信号开关）
- 或用电平转换芯片（带 OE 脚）统一控制
- 或 TP 的 VCC 用 **高侧 P-MOSFET + 上拉电阻到 VCC（不是到 VDD_MCU）**，这样 VCC 掉了上拉也跟着没了

但通常最简单就是在软件里把 pinctrl 切到 sleep / low-power-enable（用户 DTS 里已经定义了 `uart0_ps2_sleep`，目前没人用过它），让 UART RX 引脚真的进 disconnected 状态。

### 8.6 一个提醒：nice!nano v2 的 P0.13 特性

nice!nano v2 的 P0.13 驱动的是板载 3.3V LDO 的 EN：
- 电平 **高**：VCC 输出（正常供电）
- 电平 **低**：VCC 关断
- **注意极性**：DTS 里写 `GPIO_ACTIVE_HIGH` 时 `gpio_pin_set(…, 1)` 才是"供电"
- 整机 sleep 时 ZMK 会自动把该 LDO 关掉，这就是为什么用户说"只有 sleep 时 TP 才不耗电"——同一个机制我们现在手动用在 idle 里
- P0.13 同时也是 nRF52840 的普通 GPIO，复位 / 掉电后默认是输入高阻，所以**在 power_down 前一定要先 `gpio_pin_configure(OUTPUT)`**，不要依赖复位态


---

## 九、已实施的改动汇总（patch）

### 9.1 新增 / 修改的文件              

| 文件 | 变更 |
|---|---|
| `dts/bindings/input/zmk,input-mouse-ps2.yaml` | 加 `vcc-gpios` 属性 |
| `src/drivers/input/Kconfig` | 加 `ZMK_INPUT_MOUSE_PS2_IDLE_POWER_SAVING` + `WAKE_DELAY_MS` |
| `include/zmk/input_mouse_ps2.h` | 导出 `zmk_mouse_ps2_power_down/up()` |
| `src/drivers/input/input_mouse_ps2.c` | config 加 `vcc_gpio`；抽出 `apply_tp_settings()`；实现 `power_down/up` + `transport_suspend/resume` + `vcc_set` |
| `src/pm/mouse_ps2_idle_pm.c` | 新文件：订阅 `zmk_activity_state_changed`，丢 work queue |
| `CMakeLists.txt` | 按 Kconfig 编入新文件 |

### 9.2 用户侧启用步骤

1. **config/<board>.conf** 增加：
   ```
   CONFIG_ZMK_INPUT_MOUSE_PS2_IDLE_POWER_SAVING=y
   ```

2. **boards/shields/<your>/<your>_right.overlay** 在 `mouse_ps2` 节点里加一行：
   ```dts
   mouse_ps2: mouse_ps2 {
       status = "okay";
       compatible = "zmk,input-mouse-ps2";
       ps2-device = <&uart_ps2>;
       rst-gpios   = MOUSE_PS2_PIN_RST_PRO_MICRO;
       vcc-gpios   = <&gpio0 13 GPIO_ACTIVE_HIGH>;  /* nice!nano v2 VCC EN */
   };
   ```
   nice!nano v2 的板载 3.3V LDO EN 脚就是 `P0.13`，`GPIO_ACTIVE_HIGH` 意味着拉高 = 通电。

3. **确认 DTS 里已经定义了 `uart0_ps2_sleep` pinctrl 状态且带 `low-power-enable`**（用户示例 DTS 已经有），否则 idle 时 UART RX 引脚不会进 disconnected 状态。

### 9.3 运行时行为

- 进入 ZMK idle（默认 300s，可用 `CONFIG_ZMK_IDLE_TIMEOUT` 调）：
  - 发 `0xF5` → 关 UART RX IRQ → UART 外设 SUSPEND → UART pinctrl -> sleep → SCL 拉低 → `P0.13` 拉低（断 VCC）
- 回 active（任何键盘活动）：
  - `P0.13` 拉高 → 等 50ms LDO 稳定 → UART 外设 RESUME → UART pinctrl -> default → SCL 回 input
  - 跑 `init_power_on_reset()`（600ms POR）→ 跑 `wait_for_mouse()` 消耗 `0xAA 0x00` → 跑 `apply_tp_settings()` → `0xF4` 重新开 reporting

### 9.4 关于 SCL/SDA 反灌耗电的处理

三步防御：
1. UART pinctrl 切到 `sleep` 状态（带 `low-power-enable`）→ SDA 被 nRF52 设为"断开 / 无上下拉 / 无驱动"
2. SCL 显式配成 `GPIO_OUTPUT_INACTIVE`（低电平输出）→ 没有压差 → 不会倒灌
3. VCC 断开时 TP 那端没有 3V，SCL/SDA 没有外部上拉能量源 → 不漏电

这样 TP 真的是 0 mA；nRF52 侧两个 GPIO 输出低 + 浮空也几乎不耗电（低于 1µA）。唯一会消耗的是 idle 下整机 ZMK 的常规功耗。

**实现说明**：代码里 pinctrl 切 SLEEP / DEFAULT 是由 `pm_device_action_run(SUSPEND/RESUME)` 自动做的（nRF UARTE 驱动 PM 实现会帮你 apply pinctrl）。不显式调 `pinctrl_apply_state` 的原因是 Zephyr 的 `PINCTRL_DT_DEFINE()` 生成的配置变量是文件静态的，跨 TU 直接 `PINCTRL_DT_DEV_CONFIG_GET` 会链接失败（本驱动和 ps2_uart 在不同 .c 里）。SCL 是普通 GPIO，没有 pm_device 机制，所以手动 `gpio_pin_configure_dt(OUTPUT_INACTIVE)` 拉低。

### 9.5 已知限制

- 只支持 `uart-ps2` 驱动（UART 模式），不支持 `gpio-ps2`。如果你用 GPIO 驱动需要额外在 ps2_gpio.c 里实现 suspend/resume。
- `pm_device_action_run` 需要 `CONFIG_PM_DEVICE=y`；我们在新 Kconfig 里已 `select PM_DEVICE`。
- 唤醒延迟：TP 冷启动到可用约 600ms (POR) + 50ms (LDO) + 50ms (init_wait_for_mouse 首次尝试)。用户按键后第一次动 TP 可能会感觉到短暂延迟（常规写字场景不会感知）。
