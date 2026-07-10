# UE5.4 Mobile VR「透明物体之后渲染指定物体」方案分析（CX/MiniMax）

> 针对 `Docs/Plan.md` 的计划进行可行性评估、问题排查与遗漏补全。引擎版本 UE5.4，目标平台 Android VR，渲染路径 Mobile。以下结论均与当前引擎源码逐条核对。

## 一、结论摘要

- **方向可行**：在透明之后追加一次「不透明式」绘制来覆盖透明物体，思路成立，且与引擎现有 MeshPass 机制兼容。
- **但当前计划按原文无法工作**，存在 6 处阻断性缺陷（编译错误 / 运行时崩溃），以及 1 个最致命的遗漏：**新 Pass 从未在可见性系统中注册**，导致 `ParallelMeshDrawCommandPasses[MobileAfterTranslucencyPass]` 永远为空，`DispatchDraw` 一笔都不画。
- 即便修复编译问题，**深度状态**也用错了（在深度只读 subpass 里请求深度写入），在 Vulkan/Metal 上会触发校验错误或渲染异常。
- 还需注意 **Forward vs Deferred 路径**：本方案只在 Forward 路径下直接成立；若设备走 Mobile Deferred，需要额外处理（Deferred 路径下不透明物体的光照在单独的 `MobileDeferredShadingPass` 完成，之后补画不会被正确光照）。
- 下文给出逐条评估与修正后的最小可行方案。

## 二、已核实的源码事实（当前引擎）

- `Source/Runtime/Renderer/Public/MeshPassProcessor.h`：`EMeshPass::Type : uint8` 枚举，非编辑器项共 **32 个**（`DepthPass`…`WaterInfoTexturePass`），编辑器 4 个（`HitProxy` 等），故 `Num = 32`（非编辑器）/ `36`（编辑器）。`NumBits = 6`。文件中的断言是：
    - `static_assert(EMeshPass::Num <= (1 << EMeshPass::NumBits), ...)`
    - `static_assert(EMeshPass::NumBits <= sizeof(EMeshPass::Type) * 8, ...)`
    - **并非** 计划里写的 `EMeshPass::Num == 33 + 4` / `== 33`（该断言在当前引擎不存在，且数值与实际枚举不符）。
- `Source/Runtime/Renderer/Private/MobileBasePass.cpp`：
    - 构造函数签名为 7 个参数：`(EMeshPass::Type, const FScene*, const FSceneView*, const FMeshPassProcessorRenderState&, FMeshPassDrawListContext*, EFlags, ETranslucencyPass::Type)`，**第 7 个参数是 `ETranslucencyPass::Type`，不是 bool**。初始化列表里 `bTranslucentBasePass(InTranslucencyPassType != ETranslucencyPass::TPT_MAX)`、`bPassUsesDeferredShading(bDeferredShading && !bTranslucentBasePass)`。
    - `AddMeshBatch` 开头有判空保护：`(PrimitiveSceneProxy && !PrimitiveSceneProxy->ShouldRenderInMainPass())`，即调用 `ShouldRenderInMainPass()` 前先判空（说明 `PrimitiveSceneProxy` 可能为空）。
    - `ShouldDraw`：当 `!bTranslucentBasePass` 时走不透明分支 `return !bIsTranslucent;`（只画不透明材质）。
    - `CreateMobileBasePassProcessor` 用 6 个参数 new；`CreateMobileTranslucencyStandardPassProcessor` 等用 7 个参数，第 7 个为 `ETranslucencyPass::TPT_*`。
    - `REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR`：`BasePass`/`MobileBasePassCSM` 用 `EMeshPassFlags::CachedMeshCommands | EMeshPassFlags::MainView`；各半透明 pass 用 `EMeshPassFlags::MainView`。
- `Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp`：
    - `RenderForwardSinglePass` 存在。`RenderTranslucency(RHICmdList, View)` 在 `GraphBuilder.AddPass(RDG_EVENT_NAME("SceneColorRendering"), …)` 的 lambda 内部，且在 `RHICmdList.NextSubpass()` 之后——即处于**深度只读的 subpass**。同 lambda 内 `RenderMobileBasePass` 使用 `&PassParameters->InstanceCullingDrawParams`。
    - 选择逻辑：`if (bRequiresMultiPass) RenderForwardMultiPass(...) else RenderForwardSinglePass(...)`。
    - 同时存在 `RenderDeferredSinglePass` / `RenderDeferredMultiPass`（Mobile Deferred 路径，含 `MobileDeferredShadingPass` 与多次 `NextSubpass`）。`RequiresMultiPass()` 对 Vulkan、Metal framebuffer-fetch 等返回 false。
- `Source/Runtime/Renderer/Private/RendererStats.h`：**该文件不存在**（读取报「Cannot find path」）。

## 三、严重问题（阻断编译或运行时崩溃）

### 3.1【致命/崩溃】新枚举放在 `Num` 之后 → 数组越界

计划把 `MobileAfterTranslucencyPass` 放在 `Num, NumBits = 6,` 之后：

```c++
		Num,
		NumBits = 6,
		MobileAfterTranslucencyPass, // ← 放在 Num 之后
```

后果：`MobileAfterTranslucencyPass` 的值 == `Num`（32/36）。而 `FViewInfo::ParallelMeshDrawCommandPasses` 是按 `EMeshPass::Num` 定长的数组，`ParallelMeshDrawCommandPasses[EMeshPass::MobileAfterTranslucencyPass]` 即 `ParallelMeshDrawCommandPasses[Num]`，**越界一个元素** → 堆破坏/崩溃。且 `static_assert(Num <= (1<<NumBits))` 仍成立（32 ≤ 64），编译期抓不到。

**修正**：把新枚举移到 `Num` 之前（建议放在 `WaterInfoTexturePass` 之后、`#if WITH_EDITOR` 之前）：

```c++
		WaterInfoTexturePass,
		MobileAfterTranslucencyPass,   // ← 移到 Num 之前
#if WITH_EDITOR
		HitProxy,
		...
#endif
		Num,
		NumBits = 6,
```

此时 `Num = 33/37`，`NumBits=6` 仍够（33 ≤ 64），无需改动那两条真实断言。

### 3.2【致命】构造参数错位：`true` 被传给 `ETranslucencyPass::Type`

计划里：

```c++
return new FMobileBasePassMeshProcessor(EMeshPass::MobileAfterTranslucencyPass, Scene, InViewIfDynamicMeshCommand, PassDrawRenderState, InDrawListContext, Flags, true);
```

真实构造函数第 7 个参数是 `ETranslucencyPass::Type`（枚举），第 8 个才是计划新增的 `bool bAfterTranslucencyBasePass`。`true`（bool）无法隐式转换为该枚举 → **编译错误**（UE 以 `/permissive-` 编译）；即便个别编译器放行，`true`→1 会等于某个 `TPT_*`，使 `bTranslucentBasePass = (1 != TPT_MAX) = true`，于是 `ShouldDraw` 走半透明分支、不透明材质直接不画，且渲染状态被当成半透明处理。

**修正**：第 7 个显式传 `ETranslucencyPass::TPT_MAX`（保持 `bTranslucentBasePass=false`，走不透明分支），第 8 个传 `true`：

```c++
return new FMobileBasePassMeshProcessor(EMeshPass::MobileAfterTranslucencyPass, Scene, InViewIfDynamicMeshCommand, PassDrawRenderState, InDrawListContext, Flags, ETranslucencyPass::TPT_MAX, true);
```

### 3.3【编译错误】构造函数初始化列表少了一个右括号

计划写法：

```c++
       , bPassUsesDeferredShading(bDeferredShading && !bTranslucentBasePass
       , bAfterTranslucencyBasePass(IsAfterTranslucencyBasePass))
```

真实代码是 `bPassUsesDeferredShading(bDeferredShading && !bTranslucentBasePass)`，计划漏了 `)`，把两个成员初始化粘在一起 → **编译错误**。

**修正**：

```c++
	, bPassUsesDeferredShading(bDeferredShading && !bTranslucentBasePass)
	, bAfterTranslucencyBasePass(bInAfterTranslucencyBasePass)
```

### 3.4【崩溃】`AddMeshBatch` 里 `PrimitiveSceneProxy` 空指针解引用

计划在 `AddMeshBatch` 开头直接：

```c++
bool bShouldRenderAfterTranslucency = PrimitiveSceneProxy->ShouldRenderAfterTranslucency();
```

而真实代码在调用 `ShouldRenderInMainPass()` 前是先判空的（`PrimitiveSceneProxy &&`），说明 `PrimitiveSceneProxy` 可能为空。计划这里未判空 → **空指针崩溃**。

**修正**：加判空（见第七节完整片段）。

### 3.5【编译错误】统计宏未声明，且 `RendererStats.h` 不存在

计划在 `RenderMobileAfterTranslucencyPass` 中使用：

```c++
SCOPE_CYCLE_COUNTER(STAT_AfterTranslucencyDrawTime);
SCOPED_GPU_STAT(RHICmdList, AfterTranslucency);
```

- `SCOPE_CYCLE_COUNTER(STAT_AfterTranslucencyDrawTime)` 需要对应的 `DECLARE_CYCLE_STAT(..., STAT_AfterTranslucencyDrawTime, ...)`；
- `SCOPED_GPU_STAT(RHICmdList, AfterTranslucency)` 需要对应的 `DECLARE_GPU_STAT(AfterTranslucency)`；
- 计划未添加这些声明；而它假设存在的 `RendererStats.h` 在当前引擎里**并不存在**。

→ **编译错误**。

**修正**：在声明 `STAT_BasePassDrawTime` / `DECLARE_GPU_STAT(Basepass)` 的同一处（用 `rg`/`Select-String` 搜索 `STAT_BasePassDrawTime` 与 `DECLARE_GPU_STAT` 定位）补上：

```c++
DECLARE_CYCLE_STAT(TEXT("After Translucency Draw Time"), STAT_AfterTranslucencyDrawTime, STATGROUP_Renderer);
DECLARE_GPU_STAT(AfterTranslucency);
```

或更省事：直接复用已有统计（`STAT_BasePassDrawTime` / `SCOPED_GPU_STAT(RHICmdList, Basepass)`），不引入新名字。`CSV_SCOPED_TIMING_STAT_EXCLUSIVE(RenderAfterTranslucency)` 与 `SCOPED_DRAW_EVENT(...)` 用的是字符串名，无需预声明，可保留。

### 3.6【版本不符/编译错误】`static_assert` 指令基于错误版本

计划要求「更新底部断言 `EMeshPass::Num == 33 + 4` 与 `== 33`」。当前引擎根本没有这条断言（见第二节），真实断言是 `Num <= (1<<NumBits)` 等。若照计划写入 `static_assert(EMeshPass::Num == 33, ...)`，因真实 `Num=32`（按 3.1 修正后为 33）会**直接编译失败**或与真实计数冲突。

**修正**：删除计划里那条 `==33/+4` 断言的改动；保持引擎原有的 `Num <= (1<<NumBits)` 断言即可（修正 3.1 后 33 ≤ 64 仍成立）。`GetMeshPassName` 的 switch 里补 `case EMeshPass::MobileAfterTranslucencyPass` 即可（计划已做）。

## 四、关键遗漏（导致功能不生效）

### 4.1【最致命】未在可见性系统注册新 Pass → 什么都不画

新增 `EMeshPass::MobileAfterTranslucencyPass` 与 `REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR` 只是注册了「处理器工厂 / PSO 收集器 / 静态命令缓存」。但**每帧每视图**真正填充 `View.ParallelMeshDrawCommandPasses[EMeshPass::MobileAfterTranslucencyPass]` 的是 `SceneVisibility.cpp` 中的可见性收集与 `DispatchPassSetup`。计划完全没改这里 → 该 pass 命令列表恒为空 → `RenderMobileAfterTranslucencyPass` 里的 `DispatchDraw` 一笔不画。

**必须补充**（`Source/Runtime/Renderer/Private/SceneVisibility.cpp`）：

1. 在 `FSceneRenderer::ComputeViewVisibility` 相关的 `PassMask` 设置里，移动路径下启用新 pass：

   ```c++
   PassMask.Set(EMeshPass::MobileAfterTranslucencyPass);
   ```

2. 在 `FSceneRenderer::SetupMeshPass`（移动路径分支，与 `EMeshPass::BasePass` 同处）补：

   ```c++
   View.ParallelMeshDrawCommandPasses[EMeshPass::MobileAfterTranslucencyPass].DispatchPassSetup(
       Scene, View, ... /* CreateMobileAfterTranslucencyPassProcessor, BasePassDepthStencilAccess */);
   ```

   （参考同文件中 `EMeshPass::BasePass` / `EMeshPass::MobileBasePassCSM` 的写法。）

> 这是整个方案「能不能画出来」的关键。不做这一步，前面所有改动都看不到效果。

### 4.2 深度状态用错：在深度只读 subpass 里请求深度写入

`RenderForwardSinglePass` 中，半透明绘制发生在 `NextSubpass()` 之后的**深度只读 subpass**。计划的 `CreateMobileAfterTranslucencyPassProcessor` 直接抄了 `CreateMobileBasePassProcessor` 的深度状态：

```c++
PassDrawRenderState.SetDepthStencilAccess(DefaultBasePassDepthStencilAccess); // DepthWrite
PassDrawRenderState.SetDepthStencilState(TStaticDepthStencilState<true, CF_DepthNearOrEqual>::GetRHI()); // 写深度
```

在只读 subpass 里请求深度写入，与渲染通道的 attachment 访问权限冲突，Vulkan/Metal 会报校验错误或行为异常。

**修正**：与半透明处理器一致，改成深度只读（深度测试保留、深度写关闭）：

```c++
PassDrawRenderState.SetDepthStencilAccess(FExclusiveDepthStencil::DepthRead_StencilRead);
PassDrawRenderState.SetDepthStencilState(TStaticDepthStencilState<false, CF_DepthNearOrEqual>::GetRHI());
```

这同时契合你「不写入深度、只做深度测试覆盖透明」的本意（见 5.2）。

### 4.3 渲染路径：Forward vs Deferred

本引擎同时存在 `RenderForwardSinglePass/MultiPass`（Forward）与 `RenderDeferredSinglePass/MultiPass`（Deferred）。`RenderMobileAfterTranslucencyPass` 的调用位置必须放在**运行时实际走的那条路径**里：

- **Forward 路径**：可行。在 `RenderForwardSinglePass` / `RenderForwardMultiPass` 的 `SceneColorRendering` AddPass lambda 内、`RenderTranslucency` 之后插入即可（计划方向正确，但要确认插入点在 lambda 内部）。
- **Deferred 路径**：**不适用**。Deferred 下不透明物体先写 GBuffer，再由 `MobileDeferredShadingPass` 统一光照；若在透明之后再补画一遍「不透明 base pass」材质，写入的是未光照的 GBuffer/原色，不会被光照，显示错误。若你的 Android VR 设备启用 `r.Mobile.DeferredShading`（Vulkan 多见），需先确认路径，或改用前向着色。

请用 `r.Mobile.DeferredShading` / `IsMobileDeferredShadingEnabled` 确认目标设备走哪条路径，再决定改哪个函数。

### 4.4 VR 立体 / MultiView 路径

Android VR 常用立体/实例化 Multiview 渲染。如果存在单独的 Multiview 渲染入口（与 `RenderForwardSinglePass` 并列的 multiview 变体），需同步在那里插入 `RenderMobileAfterTranslucencyPass`，否则单眼/某一视图路径下看不到效果。请核对项目实际使用的 VR 渲染器入口。

### 4.5 文件名存疑：`MobileBasePassRendering.cpp`

计划第 5 点把 `RenderMobileBasePass` 归到 `MobileBasePassRendering.cpp:492`。但当前引擎里 `MobileBasePass.cpp` 的文件头注释就误标为 "MobileBasePassRendering.cpp"，且 `RenderMobileBasePass`（行 ~492，早于构造函数 ~810）很可能实际位于 `MobileBasePass.cpp`。请确认 `MobileBasePassRendering.cpp` 是否真的存在；若不存在，新增 `RenderMobileAfterTranslucencyPass` 应放在 `MobileBasePass.cpp`（与 `RenderMobileBasePass` 同文件），并在对应头文件 `MobileBasePassRendering.h`（已确认存在）声明。

## 五、设计 / 逻辑问题

### 5.1 `bRenderInMainPass` 与 `bRenderAfterTranslucency` 的交互（易踩坑）

`AddMeshBatch` 里原有的判空保护同时检查 `!ShouldRenderInMainPass()`，且该检查在计划新增的分流逻辑**之后**仍会执行。结论：

- **必须保持 `bRenderInMainPass = true`（默认）**。只设 `bRenderAfterTranslucency = true`。此时：普通 BasePass 经计划分流 `return` 跳过（不画），AfterTranslucency pass 经分流命中并绘制。
- 若你误把 `bRenderInMainPass` 也设成 `false`（以为「不在主不透明阶段画」），则 AfterTranslucency pass 也会被 `!ShouldRenderInMainPass()` 跳过 → 物体彻底不画。

若希望 `bRenderInMainPass=false` 也能被 AfterTranslucency pass 绘制，需让 AfterTranslucency pass 跳过 `ShouldRenderInMainPass` 检查（把分流逻辑置于该检查之前，并在 `bAfterTranslucencyBasePass` 分支里不再受其约束）。

### 5.2 「不透明物体不写入深度」的描述与实际/代码不符

需求描述说「因为不透明物体不写入深度」，但实际 BasePass **会写入深度**（`DepthWrite`）。而计划代码里 AfterTranslucency 处理器又开了深度写（`<true, …>`），与「不写入深度」的表述自相矛盾。真正能实现「覆盖透明」的是：**深度测试保留（`CF_DepthNearOrEqual`，测的是 BasePass 写入的不透明深度）+ 颜色不透明覆盖**，深度是否写入不影响覆盖效果。建议按 4.2 关闭深度写，既修正 subpass 冲突，又贴合你的本意。

### 5.3 只能画不透明材质

`ShouldDraw` 在 `!bTranslucentBasePass` 时 `return !bIsTranslucent`，即 AfterTranslucency pass 只会画**不透明**材质。被标记物体若用半透明材质则不会绘制。若你需要半透明材质也能覆盖，需额外处理（一般场景下「覆盖透明」用不透明材质即可）。

### 5.4 `InstanceCullingDrawParams` 复用问题

计划把 `&PassParameters->InstanceCullingDrawParams`（BasePass 的实例剔除参数）直接复用给 AfterTranslucency pass 的 `DispatchDraw`。每个 mesh pass 在 `DispatchPassSetup` 阶段会建立自己的实例剔除上下文；复用 BasePass 的参数可能提供错误的实例数据。正确做法是让 AfterTranslucency pass 走自己的 `DispatchPassSetup`（见 4.1），`DispatchDraw` 用其配套参数。

### 5.5 多个标记物体之间的排序

若关闭深度写（4.2），多个标记物体之间只能按提交顺序覆盖，无法互相正确遮挡。若标记物体之间有遮挡关系，可考虑：保留深度写（但需把绘制放到允许深度写的 render pass，而非只读 subpass），或对标记物体按距离排序后提交。

### 5.6 光照 UniformBuffer 一致性

- Forward SinglePass：整个 `SceneColorRendering` 用一份 `MobileBasePass = CreateMobileBasePassUniformBuffer(..., EMobileBasePass::Opaque, ...)`，AfterTranslucency 不透明绘制复用它即可，CSM/光照数据可用。
- Forward MultiPass：半透明在独立 render pass，用的是 `EMobileBasePass::Translucent` 的 UB。若把 AfterTranslucency 插在半透明 pass 内，需确认不透明 base pass 着色器在该 UB 下仍正确取值（两者结构通常一致，但需验证）。

## 六、逐条计划评估

| 计划条目 | 评价 | 主要问题 |
|---|---|---|
| 1. `EMeshPass` 加 `MobileAfterTranslucencyPass` + `GetMeshPassName` + 断言 | 方向对，但**位置错**（放 Num 之后→越界，3.1）；断言改错版本（3.6） | 必须移到 Num 之前；删 `==33/+4` 断言 |
| 2. `PrimitiveComponent` 加 `bRenderAfterTranslucency` + Setter | OK | 注意 5.1：不要同时关 `bRenderInMainPass` |
| 3. `PrimitiveSceneProxy` 加字段 + `ShouldRenderAfterTranslucency` + 初始化 | OK | 无大问题，`InProxyDesc` 结构体也要带该字段（计划已覆盖 .h 与 .cpp 两处初始化） |
| 4. `MobileBasePassRendering.h/.cpp` 处理器改造 | 多处错误：构造参数错位（3.2）、初始化列表语法（3.3）、`AddMeshBatch` 空指针（3.4）、深度状态（4.2） | 见第三、四节修正 |
| 5. `RenderMobileAfterTranslucencyPass` + 两处调用 | 方向对，但统计宏未声明（3.5）、文件名存疑（4.5）、需确认在 AddPass lambda 内 & 走对路径（4.3）、VR 多视图（4.4） | 见修正 |

## 七、修正后的最小可行方案要点

1. **枚举**（`MeshPassProcessor.h`）：`MobileAfterTranslucencyPass` 放在 `WaterInfoTexturePass` 之后、`#if WITH_EDITOR` 之前；`GetMeshPassName` 加 case；不动原有 `Num <= (1<<NumBits)` 断言，不要写 `==33/+4`。
2. **统计**：声明 `STAT_AfterTranslucencyDrawTime` 与 `DECLARE_GPU_STAT(AfterTranslucency)`（或复用 `Basepass` 的）。
3. **处理器**（`MobileBasePassRendering.h/.cpp`）：

   ```c++
   // .h 构造声明加第 8 参
   FMobileBasePassMeshProcessor(EMeshPass::Type, const FScene*, const FSceneView*,
       const FMeshPassProcessorRenderState&, FMeshPassDrawListContext*, EFlags,
       ETranslucencyPass::Type InTranslucencyPassType = ETranslucencyPass::TPT_MAX,
       bool bInAfterTranslucencyBasePass = false);
   // 成员
   const bool bAfterTranslucencyBasePass;
   ```

   ```c++
   // .cpp 构造初始化列表
   , bPassUsesDeferredShading(bDeferredShading && !bTranslucentBasePass)
   , bAfterTranslucencyBasePass(bInAfterTranslucencyBasePass)
   ```

   ```c++
   // AddMeshBatch：判空 + 分流（置于 ShouldRenderInMainPass 检查之前）
   if (PrimitiveSceneProxy)
   {
       const bool bWantAfter = PrimitiveSceneProxy->ShouldRenderAfterTranslucency();
       if (bAfterTranslucencyBasePass ? !bWantAfter : bWantAfter) return;
   }
   if (!MeshBatch.bUseForMaterial ||
       (Flags & EFlags::DoNotCache) == EFlags::DoNotCache ||
       (PrimitiveSceneProxy && !PrimitiveSceneProxy->ShouldRenderInMainPass()))
       return;
   ```

   ```c++
   // CreateMobileAfterTranslucencyPassProcessor：深度只读 + 正确参数顺序
   PassDrawRenderState.SetDepthStencilAccess(FExclusiveDepthStencil::DepthRead_StencilRead);
   PassDrawRenderState.SetDepthStencilState(TStaticDepthStencilState<false, CF_DepthNearOrEqual>::GetRHI());
   ...
   return new FMobileBasePassMeshProcessor(EMeshPass::MobileAfterTranslucencyPass, Scene,
       InViewIfDynamicMeshCommand, PassDrawRenderState, InDrawListContext, Flags,
       ETranslucencyPass::TPT_MAX, true);
   ```

   `REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR(MobileAfterTranslucencyPass, CreateMobileAfterTranslucencyPassProcessor, EShadingPath::Mobile, EMeshPass::MobileAfterTranslucencyPass, EMeshPassFlags::CachedMeshCommands | EMeshPassFlags::MainView);`

4. **可见性（最关键，`SceneVisibility.cpp`）**：`PassMask.Set(EMeshPass::MobileAfterTranslucencyPass)` + `SetupMeshPass` 里对该 pass 调 `DispatchPassSetup`（仿 `BasePass`）。
5. **绘制**（`MobileBasePass.cpp` 或 `MobileBasePassRendering.cpp`，先确认文件）：实现 `RenderMobileAfterTranslucencyPass`；在 `RenderForwardSinglePass`/`RenderForwardMultiPass` 的 `SceneColorRendering` AddPass lambda 内、`RenderTranslucency` 之后调用。
6. **运行时**：保持 `bRenderInMainPass=true`，仅设 `bRenderAfterTranslucency=true`。

## 八、VR 特别注意事项

- 确认设备走 Forward 还是 Deferred（4.3）；本方案仅 Forward 直接成立。
- 确认 VR 立体/Multiview 入口是否需要同步插入（4.4）。
- 性能：追加一次 base-pass 级绘制，标记物体数量少时影响可控；VR 对填充率敏感，建议标记物体尽量精简。
- 深度：按 4.2 关闭深度写后，标记物体之间无相互深度遮挡（5.5），UI 类覆盖场景通常可接受。