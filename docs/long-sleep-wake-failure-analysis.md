# 长时间睡眠后唤醒 TrackPoint 无响应分析

## 现象

- 键盘进入睡眠（System OFF）**很长时间**后唤醒
- 主手和副手按键正常工作
- TrackPoint 完全无响应（不移动、不点击）
- 必须整机断电重启才能恢复
- 短时间睡眠唤醒正常，只有**长时间**才出问题

## 关键区别：idle vs sleep 的唤醒路径

### idle 唤醒（正常工作）

```
ZMK_ACTIVITY_IDLE → power_down() → 断 VCC
按键 → ZMK_ACTIVITY_ACTIVE → power_up() → VCC + POR + init → 正常
```

`power_up()` 在系统工作队列里执行，MCU 一直在运行，所有外设状态完整。

### sleep 唤醒（出问题的路径）

```
ZMK_ACTIVITY_SLEEP → power_down() → 断 VCC → MCU 进入 System OFF
                                                    ↓
                                              所有 RAM 丢失
                                              所有外设状态丢失
                                              GPIO 回到复位默认态
                                                    ↓
按键唤醒 → MCU 冷启动（等同于上电复位）→ Zephyr 重新 init 所有设备
```

**System OFF 是真正的"断电"**：MCU 的 RAM、寄存器、外设状态全部丢失。唤醒等同于按了一次 reset 按钮。

## 问题根因分析

### 根因 1：`mouse_ps2_is_down` 状态丢失（最可能）

`mouse_ps2_idle_pm.c` 里有一个关键变量：

```c
static bool mouse_ps2_is_down = false;
```

**正常流程**：
1. 进 idle → `power_down()` → `mouse_ps2_is_down = true`
2. 进 sleep → MCU System OFF → **RAM 全部丢失**
3. 唤醒 → MCU 冷启动 → `mouse_ps2_is_down` 重新初始化为 `false`
4. ZMK 发出 `ZMK_ACTIVITY_ACTIVE` 事件
5. `power_up_work_cb` 检查 `if (!mouse_ps2_is_down) return;` → **直接 return！**

**结果**：`power_up()` 根本没被调用。TP 的 VCC 虽然被 ZMK 的 System OFF 恢复流程重新拉高了（nice!nano v2 上电默认 P0.13=高），但：
- 没人跑 POR
- 没人发 `0xF4` 开启 reporting
- 没人配置 ps2_callback
- TP 处于"上电但无人理睬"状态

### 根因 2：UART 外设状态不一致

System OFF 唤醒后，Zephyr 会重新 init 所有设备（包括 UART 和 ps2_uart 驱动）。但 `input_mouse_ps2.c` 的 init 流程是：

```c
static int zmk_mouse_ps2_init(const struct device *dev) {
    // 创建 init_thread，延迟 1000ms 后执行
    k_thread_create(..., zmk_mouse_ps2_init_thread, ..., K_MSEC(1000));
    return 0;
}
```

`init_thread` 会：
1. 跑 POR
2. `wait_for_mouse()`
3. 配置 TP 设置
4. `activity_reporting_enable()`

**这个流程在冷启动时应该正常工作**。但问题可能出在：

#### 场景 A：idle PM 的 power_down 和 init_thread 竞争

时间线：
1. MCU 冷启动
2. `zmk_mouse_ps2_init()` 创建 init_thread（延迟 1000ms）
3. ZMK 事件系统初始化
4. ZMK 发出 `ZMK_ACTIVITY_ACTIVE`（因为刚唤醒）
5. `mouse_ps2_activity_listener` 收到 ACTIVE → `power_up_work` 被 submit
6. `power_up_work_cb` 检查 `mouse_ps2_is_down == false` → **return（不执行）** ✅ 这没问题
7. 1000ms 后 `init_thread` 开始执行 → POR + init → 正常

**但如果 ZMK 在 init_thread 完成之前又发了一次 IDLE 事件呢？**

可能的竞争：
1. init_thread 还在跑（POR 600ms + wait_for_mouse）
2. ZMK idle timeout 到了（如果 timeout 设得很短）
3. `ZMK_ACTIVITY_IDLE` 事件到达
4. `power_down_work_cb` 执行 → `power_down()` → 断 VCC
5. init_thread 还在 `wait_for_mouse()` 里等 → 永远等不到 → 超时失败
6. init_thread 放弃 → TP 永远不工作

**但这个场景需要 idle timeout < init 时间（~2 秒），不太可能。**

#### 场景 B：VCC 上电时序问题（最可能的真正原因）

System OFF 唤醒后：
1. nRF52 冷启动
2. nice!nano v2 的 P0.13 **默认状态是什么？**

关键问题：**P0.13 在 nRF52 复位后的默认状态是 input（高阻）**。

nice!nano v2 的 LDO EN 引脚（P0.13）有一个**外部上拉电阻**到 VBAT，所以复位后 P0.13 = 高阻 → 被上拉拉高 → LDO 输出 VCC → TP 上电。

**但时序是这样的**：
1. MCU 复位 → P0.13 高阻 → LDO EN 被上拉 → VCC 开始爬坡
2. TP 上电 → 开始自测 → 发 0xAA 0x00
3. **此时 UART 还没 init 完**（Zephyr 设备 init 有优先级顺序）
4. 0xAA 0x00 发到 UART RX 引脚 → **没人接收** → 丢失
5. 1000ms 后 init_thread 开始 → 跑 POR（RST 脉冲）
6. TP 收到 POR → 重新自测 → 发 0xAA 0x00
7. `wait_for_mouse()` 尝试 `ps2_read()` → 读到 0xAA → 成功

**这个流程在短时间睡眠后应该正常**。但长时间睡眠后为什么不行？

### 根因 3：长时间断电后 TP 内部电容完全放电（最可能的硬件原因）

**短时间睡眠**（几分钟）：
- TP 的内部去耦电容还有残余电荷
- VCC 恢复后 TP 快速启动（< 100ms）
- POR 时序 600ms 绰绰有余

**长时间睡眠**（几小时/过夜）：
- TP 内部电容完全放电到 0V
- VCC 恢复后 TP 需要更长时间才能稳定（内部 LDO/稳压器需要给所有电容充电）
- TP 的自测可能需要更长时间
- **POR 的 600ms 可能不够**

IBM TrackPoint 规格书说 POR 应该是 "600ms ± 20%"，但这是在"正常上电"条件下。从完全放电状态冷启动，某些 TP 可能需要更长时间。

### 根因 4：`wait_for_mouse()` 超时放弃

```c
int zmk_mouse_ps2_init_wait_for_mouse(const struct device *dev) {
    for (int i = 0; i < MOUSE_PS2_INIT_ATTEMPTS; i++) {  // 10 次
        err = ps2_read(config->ps2_device, &read_val);   // 2 秒超时
        if (err == 0) {
            // 检查 0xAA ...
        }
        if (i % 2 == 0) {
            zmk_mouse_ps2_reset(config->ps2_device);     // 发 0xFF
        }
        k_sleep(K_SECONDS(5));                           // 等 5 秒
    }
    return 1;  // 放弃
}
```

总等待时间：10 × (2s read + 5s sleep) = **~70 秒**。

如果 TP 在 70 秒内都没响应，`init_thread` 就放弃了，打印：
```
"Could not init a mouse in 10 attempts. Giving up."
```

**之后再也没有任何代码会尝试重新初始化 TP。**

### 根因 5：idle PM 的 power_up 不会被触发

从 System OFF 唤醒后：
1. `mouse_ps2_is_down = false`（RAM 重置）
2. ZMK 发 `ACTIVE` 事件
3. `power_up_work_cb` 看到 `!mouse_ps2_is_down` → return
4. **power_up() 永远不会被调用**

所以从 System OFF 唤醒后，**唯一的初始化路径就是 `init_thread`**。如果 `init_thread` 失败了，TP 就永远死了。

## 综合诊断

**最可能的故障链**：

```
长时间 System OFF
    → TP 内部电容完全放电
    → 唤醒后 VCC 恢复
    → TP 冷启动比正常慢（需要给内部电容充电）
    → init_thread 的 POR 600ms 不够 / wait_for_mouse 的时序窗口错过
    → wait_for_mouse 10 次重试全部超时
    → init_thread 放弃
    → TP 永远不工作
    → idle PM 的 power_up 不会触发（因为 mouse_ps2_is_down=false）
    → 只有断电重启才能重新跑 init_thread
```

## 可能的解决方向

### 方向 1：增大 POR 等待时间

`CONFIG_ZMK_INPUT_MOUSE_PS2_POWER_ON_RESET_TIME` 从 600ms 调到 1000-1500ms。给 TP 更多时间从完全放电状态冷启动。

### 方向 2：增加 init 重试或延迟

在 `init_thread` 里，如果 `wait_for_mouse` 失败，不要立即放弃，而是：
- 再等 5-10 秒
- 重新跑一次 POR
- 再试一轮 `wait_for_mouse`

### 方向 3：让 idle PM 感知 System OFF 唤醒

在 `mouse_ps2_idle_pm.c` 里，不要只看 `mouse_ps2_is_down` 标志。可以加一个"启动后首次 ACTIVE 事件"的特殊处理：
- 如果是冷启动后的第一次 ACTIVE 事件，不要 short-circuit
- 或者：在 init_thread 成功后设一个 `init_complete` 标志
- idle PM 在收到 ACTIVE 时检查：如果 `!init_complete`，说明 init 可能失败了，主动调 `power_up()` 重试

### 方向 4：按键触发重试（最简单的兜底）

之前分析过的"按键重置 TP"方案。如果 init 失败了，用户按一下重置键，手动触发完整的 power_down + power_up 循环。

### 方向 5：后台定时重试

如果 `init_thread` 失败，启动一个定时器每 30 秒重试一次 `wait_for_mouse`，直到成功。

## 验证方法

1. **开启 USB 日志**（`CONFIG_ZMK_USB_LOGGING=y`），复现问题后看日志：
   - 是否有 "Could not init a mouse in 10 attempts. Giving up." ？
   - POR 是否成功？
   - `wait_for_mouse` 读到了什么？

2. **调大 POR 时间**测试：
   - `CONFIG_ZMK_INPUT_MOUSE_PS2_POWER_ON_RESET_TIME=1200`
   - 看是否能解决

3. **对比短睡眠 vs 长睡眠的日志差异**

## 总结

| 可能原因 | 概率 | 说明 |
|---|---|---|
| TP 冷启动慢 + POR 时间不够 | **高** | 长时间断电后电容放空，TP 需要更久才能响应 |
| init_thread 失败后无重试 | **高** | 一旦放弃就永远不会再尝试 |
| idle PM 不感知 System OFF 唤醒 | **中** | `mouse_ps2_is_down=false` 导致 power_up 不触发 |
| UART 状态不一致 | **低** | Zephyr 冷启动会重新 init 所有设备 |
