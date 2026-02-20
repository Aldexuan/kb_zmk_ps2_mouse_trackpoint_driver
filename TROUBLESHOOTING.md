# TrackPoint 功耗优化 - 问题诊断与解决

## 当前问题分析

### 症状
- TrackPoint 1分钟无操作后仍未进入休眠状态
- 功耗仍然维持在较高水平 (~5mA)

### 根本原因
TrackPoint 设备的工作特性：
1. **持续数据流**: 即使物理上没有移动，TrackPoint 也会持续发送数据包
2. **噪声信号**: 设备内部电子噪声会产生微小的移动数据
3. **原始逻辑缺陷**: 原代码在每次接收到数据包时都更新活动时间，导致永远无法满足空闲条件

## 已实施的修复

### 1. 修改活动时间更新逻辑
**原逻辑**: 每次收到数据包就更新活动时间
```c
// 问题代码 - 每次接收数据都更新
void zmk_mouse_ps2_activity_callback(...) {
    zmk_mouse_ps2_update_activity_time(); // 这导致永不空闲
}
```

**新逻辑**: 只在处理完整数据包且有显著活动时更新
```c
// 修复后 - 只在有意义的活动时更新
void zmk_mouse_ps2_activity_process_cmd(...) {
    if (has_significant_movement || button_events) {
        zmk_mouse_ps2_update_activity_time(); // 仅在真实活动时更新
    }
}
```

### 2. 改进初始化逻辑
- 初始化时 `last_activity_time` 设置为 0
- 首次空闲检查时建立基准时间
- 避免系统启动时的假活动状态

### 3. 增强空闲检测
- 处理初始状态的特殊情况
- 更准确的时间计算
- 改进的日志记录用于调试

## 预期行为

### 正常工作流程
1. **系统启动**: `last_activity_time = 0`
2. **首次检查**: 建立当前时间作为基准
3. **正常使用**: 只有显著移动或按钮操作才更新活动时间
4. **空闲检测**: 连续30秒(默认)无显著活动后进入休眠
5. **快速恢复**: 任何数据到达立即退出休眠

### 日志输出示例
```
[DBG] Initial idle check setup
[DBG] Idle check: time since last activity = 5000 ms (threshold: 30000 ms)
[DBG] Idle check: time since last activity = 10000 ms (threshold: 30000 ms)
...
[DBG] Idle check: time since last activity = 30000 ms (threshold: 30000 ms)
[INF] Entering idle mode to reduce power consumption
```

## 测试验证方法

### 1. 启用调试日志
在配置文件中添加：
```
CONFIG_ZMK_LOG_LEVEL_DBG=y
```

### 2. 观察预期日志
- 查看定期的空闲检查日志
- 确认时间累积正确
- 验证进入/退出休眠的消息

### 3. 功耗测量
- 使用电流表测量实际功耗变化
- 对比休眠前后的电流读数
- 验证功耗降低效果

## 故障排除

### 如果仍不进入休眠

1. **检查配置**
   ```
   CONFIG_ZMK_INPUT_MOUSE_PS2_IDLE_THRESHOLD_MS=30000
   ```

2. **查看日志**
   - 是否有空闲检查日志输出？
   - 时间累积是否正常？
   - 是否有意外的活动更新？

3. **可能的原因**
   - TrackPoint 物理振动
   - 电磁干扰
   - 配置未生效
   - 其他组件产生干扰

### 调整建议

**缩短空闲时间**（更快响应）：
```
CONFIG_ZMK_INPUT_MOUSE_PS2_IDLE_THRESHOLD_MS=15000
```

**延长空闲时间**（减少误触发）：
```
CONFIG_ZMK_INPUT_MOUSE_PS2_IDLE_THRESHOLD_MS=60000
```

## 技术细节

### 关键改动点

1. **数据接收回调** (`zmk_mouse_ps2_activity_callback`)
   - 移除了每次接收数据的活动时间更新
   - 保留了退出休眠的逻辑

2. **数据包处理** (`zmk_mouse_ps2_activity_process_cmd`)
   - 保留了有意义活动的活动时间更新
   - 过滤掉噪声和微小移动

3. **空闲检测** (`zmk_mouse_ps2_idle_check_handler`)
   - 处理初始状态特殊情况
   - 改进时间计算逻辑

这个修复应该能解决 TrackPoint 无法进入休眠的根本问题！