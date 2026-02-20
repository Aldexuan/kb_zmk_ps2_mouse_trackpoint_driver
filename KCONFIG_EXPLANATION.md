# Kconfig 配置说明

## 为什么采用标准 Kconfig 方式

### 问题分析
之前的构建错误是因为：
1. 我们在代码中使用了 `CONFIG_ZMK_INPUT_MOUSE_PS2_IDLE_THRESHOLD_MS`
2. 但在 Kconfig 文件中没有正确定义这个配置选项
3. 导致 Zephyr 构建系统找不到这个符号而报错

### 标准做法
按照 ZMK 和 Zephyr 的标准实践：

1. **在驱动的 Kconfig 文件中定义配置选项**
   ```kconfig
   config ZMK_INPUT_MOUSE_PS2_IDLE_THRESHOLD_MS
       int "Time in milliseconds of inactivity before entering idle mode to save power"
       default 30000
       help
         Set the time threshold for automatic idle mode activation...
   ```

2. **在代码中直接使用配置符号**
   ```c
   #define MOUSE_PS2_IDLE_THRESHOLD_MS CONFIG_ZMK_INPUT_MOUSE_PS2_IDLE_THRESHOLD_MS
   ```

3. **在用户配置中启用**
   ```conf
   CONFIG_ZMK_INPUT_MOUSE_PS2_IDLE_THRESHOLD_MS=30000
   ```

### 优势对比

| 方式 | 优点 | 缺点 |
|------|------|------|
| **标准 Kconfig** | ✓ 符合 Zephyr 规范<br>✓ 有完整的帮助文本<br>✓ 可以在 menuconfig 中配置<br>✓ 类型安全 | - 需要在正确位置定义 |
| **Board 配置文件** | ✓ 简单直接<br>✓ 不需要修改驱动代码 | - 不符合标准<br>✓ 缺少文档说明<br>✗ 无法在 GUI 中配置 |

### 其他配置的参考
正如您指出的，其他的配置如：
- `ZMK_INPUT_MOUSE_PS2_POWER_ON_RESET_TIME`
- `ZMK_INPUT_MOUSE_PS2_ENABLE_UROB_COMPAT` 
- `ZMK_INPUT_MOUSE_PS2_ENABLE_ERROR_MITIGATION`

都是采用同样的标准 Kconfig 方式定义和使用的。

这种方式确保了：
1. 构建系统的完整性
2. 配置的一致性和可维护性
3. 用户友好的配置体验
4. 与 Zephyr 生态系统的兼容性

现在我们的实现完全遵循了这一标准做法，应该不会再出现构建错误了。