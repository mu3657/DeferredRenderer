# 五阶段交付与验证记录

验证日期：2026-10-26。以下结果来自本次实际编译与运行。

| 阶段 | 已实现内容 |
| --- | --- |
| 1 | 独立 Lua VM、实例生命周期、环境隔离、属性声明和错误处理 |
| 2 | 节点代理、失效句柄检查、局部位置/旋转、父子变换和逐帧接入 |
| 3 | 灯光接口、Inspector 挂载和参数、Play/Pause/Resume/Stop、快照恢复 |
| 4 | 手动文件重载、全部同路径实例预检、参数保留和私有状态重置 |
| 5 | 场景旁 JSON 配置、导入节点持久 ID、相对路径、类型迁移和原子替换 |

## 结果

| 验证 | 结果 |
| --- | --- |
| 独立 Debug 核心验证 | 372 checks, 0 failures |
| RelWithDebInfo + MSVC AddressSanitizer | 372 checks, 0 failures；未报告内存访问错误 |
| 完整 engine 构建 | 成功生成 `bin/engine.exe` |
| `engine.exe --scripting-validation` | 85 checks, 0 failures |
| 原有 `engine.exe --taa-validation` | 70 checks, 0 failures |

核心验证覆盖生命周期、实例隔离、元数据、严格类型/范围检查、错误调用栈、场景代理、加载期间禁止写场景、失效句柄、全文件重载和带类型的配置读写。ASan 结果不等同于泄漏检测结论。

集成验证实际重新读取已有 prefab 到新的节点对象，以持久 ID 恢复脚本，并在节点改名后再次加载配置。另一个独立场景使用两个移动方块、父子变换和一盏强度变化的点光源，验证暂停、恢复、停止、切场景、缺失资源、参数迁移和脚本错误隔离。

GPU 验证检查了场景遍历的矩阵、当前光追实例和已提交 TLAS 实例的一致性，以及光源收集、TAA 历史、DDGI 更新和实际图像。GeometryPass 执行后会清空不透明绘制列表，因此光栅矩阵检查在暂停状态下重用场景遍历路径；光追/TLAS 检查使用已渲染帧保留的记录。

## 可查看的产物

- [集成报告](../../.runtime/scripting/full/report.json)
- [逐项检查](../../.runtime/scripting/full/checks.txt)
- [静止场景](../../.runtime/scripting/full/stopped.png)
- [脚本运行后的场景](../../.runtime/scripting/full/playing.png)
- [Stop 恢复后的场景](../../.runtime/scripting/full/restored.png)

图像直接从实际 GPU 图像读回，1280 × 800。已目视检查静止和运行图像，能看到方块运动、阴影和亮度变化。这些是渲染验证场景，不是正式展示场景或性能基准。

ImGui Inspector 和播放控件已在真实渲染帧中生成绘制数据；原生窗口自动检查在截图步骤被中断，未完成逐个按钮的点击验证，不将其计为通过项目。

## 仍然明确保留的边界

- 配置保存挂载关系与参数，不保存运行时新增节点的几何和场景结构。重启后未重建的节点会被报告缺失。
- 持久 ID 依赖原 prefab 的节点 ID；重新烘焙若改变该 ID，需要重新绑定。
- 重载预检失败保留旧实例。提交后的 on_create 错误使新实例进入 Faulted，生命周期回调不提供事务回滚；Stop 仍恢复播放前快照。
- 不支持脚本创建/删除节点、物理、输入绑定、文件监听、任意 Lua 状态迁移或不可信脚本沙箱。

这些边界及使用方法见 [README](README.md)。
