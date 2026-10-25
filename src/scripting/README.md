# Scripting

独立的 C++20 / Lua 运行时，加上 renderer 的场景适配和编辑器接入。五个阶段已分别实现：生命周期和属性元数据、节点绑定、Inspector / 播放控制、手动重载、配置保存。

## 模块边界

- `script_types.h`：普通 C++ 数据结构，包含属性描述、状态、日志和操作结果。
- `script_system.h`：创建、更新、启停、销毁、属性访问与状态查询。
- `script_host.h`：节点和灯光操作的窄接口，不暴露 renderer 类型。
- `script_system.cpp` / `script_bindings.cpp`：Lua 执行、受保护调用、场景接口绑定。
- `script_config.cpp`：带类型的 JSON 配置读写，不保存运行状态。
- `examples/`：计时器、节点移动、旋转、灯光强度动画。
- `tests/script_system_tests.cpp`：脱离 Vulkan 的核心验证。
- `../scene_script_host.*`：节点注册、弱引用句柄、变换与灯光修改、快照恢复。
- `../scene_scripts.*`：挂载配置、Play / Pause / Stop、文件重载与场景切换。
- `../script_inspector.*`：ImGui 界面，只使用控制层的公开接口。

运行时的公开接口只依赖标准库和 GLM，内部另外使用 Lua、sol2 和现有 nlohmann/json。`scripting` 目标不引用 Vulkan、SDL、ImGui、`Node` 或 `VulkanEngine`，也没有事件总线、自动反射或场景所有权。

`ScriptSystem` 通过一个私有实现隐藏 VM 和 Lua 引用。`SceneScriptHost` 实现唯一的场景接口，`SceneScripts` 管理编辑和运行状态；引擎只负责连接模块与安排调用顺序。

## 构建和运行

从仓库根目录，在已初始化编译器环境的终端执行：

```powershell
cmake -S src/scripting -B build-scripting -DCMAKE_BUILD_TYPE=Debug
cmake --build build-scripting --config Debug
ctest --test-dir build-scripting -C Debug --output-on-failure
```

Ninja 构建的示例路径是 `build-scripting/scripting_demo.exe`；Visual Studio 多配置生成器使用 `build-scripting/Debug/scripting_demo.exe`。可传入自定义脚本文件路径；示例程序要求脚本声明 `name` 和 `speed` 属性。

依赖由 CMake 从上游下载并校验 SHA-256：

- [Lua 5.4.9](https://www.lua.org/ftp/)：官方源码包，构建为私有静态库。
- [sol2 v3.3.1 tag](https://github.com/ThePhD/sol2/releases/tag/v3.3.1)：只使用头文件，不运行其子工程配置。
- GLM：复用仓库中现有的 `third_party/glm`。

离线构建可以指定已解压的源码路径：

```powershell
cmake -S src/scripting -B build-scripting `
  -DFETCHCONTENT_SOURCE_DIR_SCRIPTING_LUA_SOURCE=/path/to/lua-5.4.9 `
  -DFETCHCONTENT_SOURCE_DIR_SCRIPTING_SOL2_SOURCE=/path/to/sol2
```

主工程的 `engine` 已链接 `scripting`，不再使用第一阶段的可选构建开关。主工程中的独立核心验证和命令行计时器示例默认关闭，可分别设置 `SCRIPTING_BUILD_TESTS=ON`、`SCRIPTING_BUILD_EXAMPLE=ON`。

真实场景、GPU 和编辑器集成验证从 `bin` 目录运行：

```powershell
./engine.exe --scripting-validation
```

结果输出到 `.runtime/scripting/full/`，包括检查记录、JSON 报告和静止 / 运行 / 恢复三张实际 GPU 图像。验证使用独立测试场景和配置路径，不修改原始 prefab 或其脚本配置。

本机 MSVC 若出现既有的 PDB 问题，按项目惯例初始化 `vcvars64.bat` 并设置 `_CL_=/Z7`。Ninja 配置和构建时应使用相同的 Windows 代码页，避免 `/showIncludes` 依赖解析失效。

## C++ 使用

```cpp
#include <scripting/script_system.h>

scripting::ScriptSystem scripts;
const auto timer = scripts.create_instance("timer.lua", {
    {"name", std::string("A")},
    {"speed", 2.0}
});

if (timer) {
    scripts.update(0.016);
    const auto metadata = scripts.inspect(timer.id);
    const auto result = scripts.set_property(timer.id, "speed", 3.0);
    scripts.destroy_instance(timer.id);
}
```

`number` 在 C++ 中使用 `double`，字符串使用 `std::string`；`vec3` 使用 `glm::vec3`。公开接口不返回 Lua 表或函数，`inspect()` 返回可独立持有的元数据副本。

创建失败通过 `CreateResult.error` 返回原因，不留下活动实例。未知实例、未知属性和错误的参数值通过 `ScriptResult` 或空 `optional` 报告。属性修改失败时保留原值。

## Lua 约定

文件返回且只返回一张定义表：

```lua
return {
    properties = {
        speed = { type = "number", default = 1.0, min = 0.0, max = 10.0 },
        enabled = { type = "bool", default = true },
        name = { type = "string", default = "timer" },
        offset = { type = "vec3", default = { x = 0, y = 0, z = 0 } }
    },

    on_create = function(self)
        self.state.elapsed = 0
    end,

    on_update = function(self, dt)
        self.state.elapsed = self.state.elapsed + dt * self.properties.speed
        log(string.format("elapsed=%.2f", self.state.elapsed))
    end,

    on_destroy = function(self)
        log("timer destroyed")
    end
}
```

- `properties` 声明和三个回调都可省略；提供的回调必须是函数。
- 定义表中的 `properties` 是元数据，回调中的 `self.properties` 是当前实例的实际值。
- `self.state` 是实例私有的运行数据，不出现在属性描述中，不作为配置保存。
- 每个实例重新执行文件，拥有独立的顶层局部变量、全局环境、属性表、状态表和标准库表副本。
- 顶层负责定义参数和回调；行为放进生命周期回调，方便后续实现重载。
- 属性仅支持 `bool`、`number`、`string`、`vec3`。元数据按属性名排序，供后续编辑器稳定显示。
- 数值必须有限；`min` / `max` 可整体省略，提供时必须同时出现且只能用于 `number`。
- 默认值、创建时的参数覆盖和 C++ 属性修改均检查类型和范围，不把字符串隐式转为数字。
- Lua 可以修改已声明属性；每次生命周期回调结束时检查实际值，非法修改会让该实例进入 `Faulted`。

Lua 的配置声明是当前阶段的属性反射来源：同一份 `PropertyDesc` 可供 Inspector、参数校验和配置保存复用。这里没有增加 C++ 成员反射框架。

## 生命周期与错误语义

1. 加载文本 Lua，在独立环境中执行定义。
2. 检查属性描述、可选回调与参数覆盖。
3. 创建 `self.properties` / `self.state`，执行一次 `on_create`。
4. `update(dt)` 按创建顺序调用 `Running` 实例。
5. `destroy_instance()`、`clear()` 或系统析构时尝试一次 `on_destroy`，随后释放引用。

`Disabled` 只暂停更新，恢复后继续使用原状态。回调出错会令该实例进入 `Faulted`，其他实例继续更新；Faulted 实例需要重新创建或手动重载，不能通过启用操作直接恢复。

`on_create` 已进入但失败时也会尝试一次 `on_destroy`，供脚本清理部分初始化状态。Disabled / Faulted 实例仍会执行销毁回调。销毁回调报错不阻止实例移除或其他实例清理。

加载与回调均使用受保护调用；Lua 运行错误保留源文件、行号和调用栈。模式错误（例如缺少属性类型）提供属性名和错误原因。错误写入日志，并保留在 `CreateResult.error` 或 `InstanceInfo.lastError` 中。

默认日志写入 `std::clog`，也可注入 `LogSink`。日志回调必须在 `ScriptSystem` 析构完成前保持其外部依赖有效；日志回调自身抛异常不会打断脚本执行。日志器的内部存活时间覆盖 VM 关闭，以允许 Lua finalizer 输出日志。

系统只在单线程使用，接口不可重入；日志回调不能再次调用同一个系统。`dt` 必须是非负有限秒数，否则在更新任何实例前抛出 `std::invalid_argument`。

## 场景 API 和更新顺序

挂载到节点的脚本在回调中使用 `self.node`：

```lua
local p = self.node.transform:get_local_position()
self.node.transform:set_local_position({ x = p.x + dt, y = p.y, z = p.z })
self.node.transform:rotate_local_y_radians(math.rad(45) * dt)
self.node.light:set_intensity(20)
self.node.light:set_color({ x = 1, y = 0.7, z = 0.3 })
self.node.light:set_range(12)
```

变换接口使用局部空间；位置修改保留原矩阵的旋转、缩放和剪切，旋转通过矩阵右乘组合。灯光数值必须有限且非负，`range = 0` 继续表示无限范围。只有 LightNode 能使用灯光接口。

`scene.find("name")` 返回节点代理；使用 `node:is_valid()` 检查。查找不到或名称重复时返回无效代理，后续读写会产生可定位的脚本错误。代理没有 C++ 指针、Vulkan 资源或可写的世界矩阵。

场景写操作只在生命周期回调期间开放。顶层定义、元数据检查、重载预检和 Lua finalizer 都不能修改场景。

主循环显式执行：输入 / 编辑器 → 脚本更新 → 世界变换刷新 → 绘制对象和灯光收集 → TLAS 同步 → 渲染。脚本不注册进 RenderPass，也不依赖 GPU 提交是否成功。最小化窗口期间暂停脚本更新，恢复时继续正常帧步长。

宿主每个逻辑更新同步活动节点集合；节点从树中移除后，即使外部仍持有 shared_ptr，旧句柄也会失效。切场景先停止并恢复旧场景，再撤销所有旧句柄；返回该场景时重新分配句柄。

## Inspector 和播放控制

1. 在 Hierarchy 选择一个节点。
2. 打开 Inspector 的 Selection 页面，在 Scripts 中选择示例或输入 Lua 文件路径，点击 Attach script。
3. Inspector 根据属性元数据生成 bool、number、string、vec3 控件。
4. 使用 Inspector 顶部的 Play / Pause / Resume / Stop 控制脚本。
5. 修改脚本后点击对应组件的 Reload；同一文件的实例一起重载。
6. 点击 Save scripts 保存，Load scripts 重新读取活动场景旁的配置文件。

Play 保存活动场景节点的局部变换、可见性，以及灯光颜色 / 强度 / 范围，然后创建启用的实例。Pause 保留实例状态；Resume 继续；Stop 先清理实例，再恢复快照。渲染资源不复制。初始禁用的组件在首次启用时才执行 on_create。

播放和暂停期间，场景结构、变换、灯光属性、可见性及挂载关系不可编辑；相机导航、脚本参数和组件启停仍可用。参数编辑同时更新实例和编辑配置，Stop 不撤销这类显式参数编辑。脚本运行中自行修改属性值不会自动变成配置覆盖。

切换场景会保留该场景尚未保存的挂载配置和 dirty 状态；关闭程序前仍需显式 Save scripts。界面的 Script messages 展示加载、参数迁移和生命周期错误。

## 手动重载语义

- 先在独立实例环境中检查新文件的语法、声明和回调类型；不执行生命周期，不允许场景写入。
- 预检失败时保留旧代码和私有状态，旧实例继续运行。
- 提交后旧实例执行 on_destroy，新实例执行 on_create；私有运行状态重置。
- 保留仍存在、类型相同且符合新范围的属性值；其余属性使用新默认值并记录诊断。
- 禁用状态保留。Faulted 实例可通过修正文件并 Reload 恢复。
- 提交后的 on_create 如果发生运行错误，新实例进入 Faulted；生命周期回调不提供场景事务回滚。Stop 仍可恢复 Play 前的场景快照。

## 配置和持久 ID

默认文件名为 `<scene>.pfb.scripts.json`，与 prefab 并排放置：

```json
{
  "version": 1,
  "scripts": [
    {
      "node_id": "prefab/123",
      "path": "../scripts/move.lua",
      "enabled": true,
      "properties": {
        "speed": { "type": "number", "value": 2.0 },
        "axis": { "type": "vec3", "value": [1, 0, 0] }
      }
    }
  ]
}
```

导入节点使用 prefab 中的数值节点 ID 构造 `prefab/<id>`；显示名称、运行时句柄和 TAA temporalID 都不参与保存。改名或重新加载同一 prefab 不影响绑定。重新烘焙若改变了原始节点 ID，需要重新绑定；当前未修改资产烘焙协议。

运行时新增节点获得独立随机 ID。在当前场景对象存活期间可稳定挂载，但脚本配置不负责保存新增节点本身；程序重启后未由场景重建的节点会被报告为缺失。缺失或重复 ID 的记录保留在配置中并跳过执行，不绑定到同名节点。

路径相对于配置文件存储；只保存挂载关系、启用标志和参数覆盖，不保存 Lua 函数、VM、运行计时器或 GPU 数据。写文件先生成临时文件，完整写入后替换目标；失败时保留原文件。损坏的 JSON 不替换当前挂载配置。

## 当前边界

- 实例 ID、节点句柄和持久 ID 各司其职；前两者不跨进程保存。
- 只开放基本函数和 `math` / `string` / `table` / `utf8`，以及 `log(string)`。未开放 `require`、文件和系统操作。
- 环境隔离用于普通行为脚本，不是执行不可信脚本的安全沙箱；共享 VM 的内部资源（例如随机数状态）仍共享。
- 尚无指令预算，死循环不会被受保护调用自动中断。
- 当前脚本只修改现有节点和灯光；尚无动态创建 / 删除 / 换父节点、物理、输入绑定、文件监听或任意运行状态迁移。
