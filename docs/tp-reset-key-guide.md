# TrackPoint 按键重置功能

## 功能说明

新增 `MS_TP_RESET` 键值，按下后执行完整的 TrackPoint 硬件重置（等同于断电重启）。

**解决的问题**：
- 唤醒后光标一直向右/右上偏移
- 唤醒后光标满屏乱飞
- 唤醒后 TrackPoint 完全无响应
- 任何其他 TrackPoint 异常状态

**恢复时间**：约 1.5-2 秒

## 使用方法

在 keymap 中绑定 `&mms MS_TP_RESET`：

```c
#include <dt-bindings/zmk/mouse_settings.h>

/ {
    keymap {
        compatible = "zmk,keymap";
        
        adjust_layer {
            bindings = <
                // ... 其他按键 ...
                &mms MS_TP_RESET    // TrackPoint 重置键
                // ... 其他按键 ...
            >;
        };
    };
};
```

## 建议放置位置

- 放在不容易误触的 layer 上（如 adjust/fn layer）
- 或用组合键触发（如 `MO(adjust)` + 某键）
- 不建议放在默认 layer 上（避免误触导致 TP 中断 1.5 秒）

## 工作原理

按下后在后台工作队列执行以下流程：

### 如果启用了 `CONFIG_ZMK_INPUT_MOUSE_PS2_IDLE_POWER_SAVING`（有 VCC 控制）：

1. `power_down()`：停止 reporting → 挂起 UART → 断 VCC
2. 等待 200ms（让 TP 内部电容放电）
3. `power_up()`：恢复 VCC → POR 600ms → 检测设备 → 重下设置 → 开启 reporting

### 如果没有 VCC 控制：

1. 停止 reporting（发 `0xF5`）
2. 发 PS/2 Reset 命令（`0xFF`）
3. 等待 500ms（TP 自测）
4. 重跑 POR（如果有 RST 引脚）
5. 重新检测设备
6. 重下所有 TP 设置
7. 重新开启 reporting

## 键值定义

| 键值 | 宏名 | 值 | 说明 |
|---|---|---|---|
| `MS_TP_RESET` | `MS_TP_RESET` | 20 | 完整硬件重置 |

## 与其他键值的关系

| 键值 | 作用 | 区别 |
|---|---|---|
| `MS_RESET` (1) | 重置 TP **设置**到默认值 | 只改灵敏度等参数，不重启硬件 |
| `MS_TP_RESET` (20) | 重置 TP **硬件** | 完整断电重启，等同于拔电源 |

## 注意事项

1. **按下后 TP 会有 1.5-2 秒无响应**（正在重启），这是正常的
2. **不影响键盘按键**：重置在后台工作队列执行，按键功能不受影响
3. **重置后 TP 设置会保留**：灵敏度等运行时调节的值会被重新下发
4. **可以连续按**：如果第一次没恢复，再按一次（虽然理论上一次就够）

## 产品说明书建议文案

> **TrackPoint 异常恢复**
> 
> 如果 TrackPoint 出现光标漂移、乱飞或无响应等异常，请按下 [你的键位] 进行重置。
> 重置过程约 2 秒，期间 TrackPoint 暂时不可用，完成后自动恢复正常。
