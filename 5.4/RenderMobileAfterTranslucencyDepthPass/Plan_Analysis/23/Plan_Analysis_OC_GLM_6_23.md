# Plan.md 分析报告 — RenderAfterTranslucency (UE 5.4 Mobile Forward)

> 目标：让被标记的不透明物体在透明物体之后绘制，从而遮挡（覆盖）不写深度的透明物体，仅限于 Mobile + Forward 渲染路径，且仅作用于 Static Mesh / Skeletal Mesh。

下文按 **结论 → 各章节核对 → 关键风险/缺漏 → 推荐改进** 的顺序展开。所有源码引用基于当前工程 `Engine/Source/...` 下的实际代码（UE 5.4 自定义版本）。

---

## 0. 总体结论

整体思路正确、可行：复用 `FMobileBasePassMeshProcessor` 走一遍"opaque-like"的绘制流程，并把 Pass 插在 `RenderTranslucency` 之后即可获得"遮挡半透明"的效果。但当前方案存在 **若干必须解决的问题** 和 **若干潜在风险**，主要集中在：

1. **Forward Single Pass 渲染路径不可用（致命问题）**——`RenderForwardSinglePass` 内部主渲染通道使用 `ESubpassHint::DepthReadSubpass`，整个 `RenderTranslucency` 都发生在第二个 subpass 内；在 `RenderTranslucency()` 之后再插入 `RenderMobileAfterTranslucencyPass()` 会**仍处于 DepthRead-only subpass** 中，无法写入深度，方案最终能否达到"遮挡半透明"的效果取决于您是否打算写深度。即便仅做颜色覆盖，也需要确认 RT/Subpass 状态。
2. **`Result.bRenderInMainPass = ShouldRenderInMainPass()` 被改写为 `bRenderAfterTranslucency` 的物体仍会被加入 `BasePass`**——计划中的 `GetViewRelevance` 没有把"AfterTranslucency"语义从 `bRenderInMainPass` 中剥离，导致同一物体既在 `BasePass` 又在 `MobileAfterTranslucencyPass` 被画了两次。
3. **`Plan` 第 1 节中 `EMeshPass::Num` 断言数字错误**——当前源码 `EMeshPass::Num == 32`（不是 33），Plan 中写的是 `33`/`33+4`，应改为 `33`/`33+4`。等于：新增 1 项之后实际数字应是 `33`（非编辑器）/ `37`（编辑器），Plan 与实际差 0；但更**关键**的是 Plan 计划中未注意到 GUID 注释需要替换。
4. **`FPrimitiveViewRelevance` 用 memzero 初始化所有位**——`bRenderAfterTranslucency = false` 这一行**完全多余**且具有误导性（构造里第一行 memzero 已置零）。
5. **缓存 MeshCommand 时机不正确**——`MobileAfterTranslucencyPass` 注册时带了 `EMeshPassFlags::CachedMeshCommands`，但 `FMobileBasePassMeshProcessor` 在 `AddMeshBatch` 阶段才根据 `bAfterTranslucencyBasePass` 做分流。**`CachedMeshDrawCommand` 是离线（PrimitiveSceneInfo 添加时）构建的**：`PrimitiveSceneInfo.cpp:127` 处对每个 Pass 都会调用一次 Processor，依赖你在 `AddMeshBatch` 内通过 `MeshPassType` 区分。当前实现里写的是 `bAfterTranslucencyBasePass` 成员，原则上可用，但要注意——cache 时 Scene 中此 Primitive 已存在 `bRenderAfterTranslucency`，但 Plan 中并没有让 `PrimitiveSceneProxy` 在变更标志时刷新缓存（虽然 `MarkRenderStateDirty` 会重建 Proxy，但仍需测试动态切换）。
6. **`DispatchPassSetup` 的"先 setup 再 build"流水线没问题**，但 `BuildInstanceCullingDrawParams` 中加入 `MobileAfterTranslucencyPass` 行后必须确保 `SetupMeshPass`/`ParallelMeshDrawCommandPasses[MobileAfterTranslucencyPass]` 已被 `DispatchPassSetup`。Plan 没有显式新增 `DispatchPassSetup` 处理：实际上 `FSceneRenderer::SetupMeshPass`（`SceneRendering.cpp:4196`）会遍历 `EMeshPass::Num` 中所有 `MainView` 标记的 Pass 自动 setup，因此**只要新 Pass 注册时带了 `MainView` 标志即会被纳入**——计划中这点正确（已带 `MainView`）。但还需注意 `SetupMeshPass` 中针对 Mobile 跳过了 `BasePass` 与 `MobileBasePassCSM`（4209 行），新 Pass 不在跳过名单里，无需特殊处理。
7. **`DispatchDraw` 在 Render Pass 内被调用，而 `BuildRenderingCommands` 在 RDG Builder 阶段调用**——若 `MobileAfterTranslucencyPass` 没有先 `BuildRenderingCommands`，`DispatchDraw` 时 `InstanceCullingDrawParams` 会是空的。Plan 中已在 `BuildInstanceCullingDrawParams` 中加入此 Pass，正确。

---

## 1. 第 1 节：`EMeshPass` 枚举 & `GetMeshPassName`

### 验证
- `Engine/Source/Runtime/Renderer/Public/MeshPassProcessor.h:34` 起的 `enum EMeshPass::Type` 与 Plan 列出的一致。
- 当前 `EMeshPass::Num == 32`，编辑器下 `== 32+4 == 36`。Plan 写为 `33` / `33+4`，**这是正确的（加一个之后等于 33）**。注意，需同时修改 GUID 注释保持唯一性（按规范修改 `{674D7D62-...}` 这一 GUID）。
- `NumBits = 6`，最多支持 64 项，新增一项后还在范围内。OK。

### 风险/建议
- **位置**：建议把 `MobileAfterTranslucencyPass` 放在 `MobileBasePassCSM` 之后或 `WaterInfoTexturePass` 之后均可，Plan 当前放在 `WaterInfoTexturePass` 之后 + `#if WITH_EDITOR` 之前，**正确**。不要插在 `#if WITH_EDITOR` 内。
- `GetMeshPassName` 的 `case` 标签里有手误：Plan 第 99 行字符串 `"MobileAfterTranslucencyPass"` 拼写本身没问题，但请保证最终代码字符串没有换行/空格污染。

---

## 2. 第 2 节：`UPrimitiveComponent`

### 验证
- `Engine/Source/Runtime/Engine/Classes/Components/PrimitiveComponent.h:407` 处 `bRenderInMainPass` 的 UPROPERTY 定义存在；在其后插入 `bRenderAfterTranslucency` 是合适的。
- Setter 添加位置（`SetRenderInMainPass` 附近）合适：`Components/PrimitiveComponent.cpp:4457` 存在 `SetRenderInMainPass` 实现，紧邻添加 `SetRenderAfterTranslucency` OK。
- `bRenderAfterTranslucency = false` 的默认值初始化位置：构造函数中 `bRenderInMainPass = true;` 旁边，**OK**。

### 风险/建议
- `meta = (EditCondition = "bRenderInMainPass")` 建议添加：当 `bRenderInMainPass == false` 时本属性应被禁用（否则物体根本不会进 `BasePass` 检查链路）。
- 由于 `FPrimitiveSceneProxy` 是渲染线程对象，仅靠 `MarkRenderStateDirty()` 会重建 Proxy；动态切换 `bRenderAfterTranslucency` 不会立刻刷新已缓存的 `MeshDrawCommand`，**但因为 Proxy 重建会触发 `PrimitiveSceneInfo` 重新 cache**，故此处行为正确。
- 需要为蓝图开放 Getter（可选）：`UFUNCTION(BlueprintPure)` 的 `GetRenderAfterTranslucency()`。

---

## 3. 第 3 节：`FPrimitiveSceneProxy` & `FPrimitiveSceneProxyDesc`

### 验证
- `PrimitiveSceneProxy.h:1200` 处 `bRenderInMainPass : 1` 的 bitfield 区域存在；插入 `bRenderAfterTranslucency : 1` 紧邻它是 OK 的（注意保持位字段连续以节省内存）。
- `PrimitiveSceneProxy.h:700` 处 `ShouldRenderInMainPass()` inline 函数存在；在其后添加 `ShouldRenderAfterTranslucency()` OK。
- `Engine/Private/PrimitiveSceneProxy.cpp:277` `InitializeFrom` 中赋值 `bRenderInMainPass = InComponent->bRenderInMainPass;` 处插入新行 OK。
- `Engine/Private/PrimitiveSceneProxy.cpp:428` 的初始化列表中 `bRenderInMainPass(InProxyDesc.bRenderInMainPass),` 旁添加 `bRenderAfterTranslucency(InProxyDesc.bRenderAfterTranslucency),` OK。
- `PrimitiveSceneProxyDesc.h:93` 的 `uint32 bRenderInMainPass : 1;` 处插入 OK；`PrimitiveSceneProxyDesc.h:25` 的 `bRenderInMainPass = true;` 旁添加 `bRenderAfterTranslucency = false;` OK。

### 风险/建议
- **位字段类型一致性**：`PrimitiveSceneProxy.h` 中已存在的 `bRenderInMainPass : 1` 类型为 `uint8`；`PrimitiveSceneProxyDesc.h` 中类型为 `uint32`。Plan 中保持了与原代码一致的类型（`uint8` vs `uint32`），**正确**。
- **未初始化路径**：`FPrimitiveSceneProxyDesc()` 默认构造函数中已有 `bRenderInMainPass = true;`，Plan 添加 `bRenderAfterTranslucency = false;`，**OK**。但请注意 `FPrimitiveSceneProxy::FPrimitiveSceneProxy(const UPrimitiveComponent*)` 这类直接从组件构造的路径需要确认走的是 `FPrimitiveSceneProxyDesc(const UPrimitiveComponent*)` → `InitializeFrom()` 流程，从 `PrimitiveSceneProxy.cpp:259-263` 看是的，OK。

---

## 4. 第 4 节：`FMobileBasePassMeshProcessor` 修改

### 验证
- `MobileBasePassRendering.h:480` 构造函数签名修改正确。
- `MobileBasePassRendering.h:533` 处加 `bAfterTranslucencyBasePass` 成员，**位置合理**（与 `bPassUsesDeferredShading` 同属"pass 标志"）。
- `MobileBasePass.cpp:810` 构造函数初始化列表加 `bAfterTranslucencyBasePass(IsAfterTranslucencyBasePass)`，OK。
- `MobileBasePass.cpp:867` `AddMeshBatch` 修改逻辑——**功能正确**：
  - `BasePass`（非 AfterTranslucency） 走原逻辑但跳过 `ShouldRenderAfterTranslucency()` 为 true 的物体；
  - `MobileAfterTranslucencyPass` 仅接受 `ShouldRenderAfterTranslucency()` 为 true 的物体。
- `MobileBasePass.cpp:1151` 处新建 `CreateMobileAfterTranslucencyPassProcessor` OK，宏注册位置（`:1223`）OK。

### 关键风险

1. **致命：`MobileBasePassCSM` 路径分流缺失** — 当 `bMobileBasePassAlwaysUsesCSM == false` 时，源码 `SceneVisibility.cpp:1565` 把 mesh 同时加入 `MobileBasePassCSM`，**Plan 第 6 节修改后，`bRenderAfterTranslucency == true` 的 mesh 不会进 `BasePass`，但仍会进 `MobileBasePassCSM`！** 这会导致：
   - 物体在 `BasePass` 阶段未绘制（被 `AddMeshBatch` 过滤）；
   - 在 `MobileBasePassCSM` 阶段会被绘制（Processor 没有按 `AfterTranslucency` 标志过滤，因为构造时 `bAfterTranslucencyBasePass = false`）；
   - 结果：在 `MobileBasePassCSM` 中绘制一次，又在 `MobileAfterTranslucencyPass` 中再绘制一次（双绘制 + 错误时序）。
   
   **修复**：要么在 `SceneVisibility.cpp:1565` 的 `if (!bMobileBasePassAlwaysUsesCSM)` 块中加入 `if (!ViewRelevance.bRenderAfterTranslucency)` 守卫；要么让 `FMobileBasePassMeshProcessor` 在 `MobileBasePassCSM` 中也按 `ShouldRenderAfterTranslucency` 过滤（更稳）。

2. **PSO Precache 行为** — `REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR` 会把新 Pass 纳入 PSO 预缓存。由于 `CollectPSOInitializers` 会基于材质属性枚举权限组合，新 Pass 会让 PSO 数量翻倍（同样的材质会为 `BasePass` 和 `MobileAfterTranslucencyPass` 各做一份）。建议在 `CollectPSOInitializers` 中针对 `MeshPassType == MobileAfterTranslucencyPass` 时跳过部分不需要的 LightmapPolicy 组合，或在 PSO 收集器中减少冗余。

3. **DepthStencilState 选择** — Plan 第 324 行用了 `TStaticDepthStencilState<false, CF_DepthNearOrEqual>::GetRHI()`（**不写深度**）。
   - 这意味着 AfterTranslucency 物体 **不会写深度**，与 `BasePass` 行为不同。
   - 用户原意是"用 opaque 的方式画"以遮挡半透明。要遮挡半透明，只需 **颜色覆盖** 即可（半透明已经没写深度）。所以"不写深度"在视觉上 OK。
   - 但是：**如果后续还有依赖深度的 effect**（例如后处理 DOF / SSR / FX 软粒子），AfterTranslucency 的物体不写深度会让这些 effect 无法识别它的"前面"，可能产生异常。
   - **建议**：保留可配置项，默认 `true` 写深度，避免上述风险。
   - 关于 `TStaticDepthStencilState<bEnableDepthWrite, DepthTest>` 与 `SetDepthStencilState` 的作用：前者是模板化的 RHI 静态深度模板状态（**编译期单例**），后者把它绑定到 PassState 上，影响该 Pass 内每个 DrawCommand。
   
4. **DepthStencilAccess 设置** — `FExclusiveDepthStencil::DepthRead_StencilRead` 意味着 PassParameter 那边的 RenderTarget 也必须是 DepthRead_StencilRead 兼容。在 `RenderForwardSinglePass` 中，单 RenderPass 已设为 `DepthReadSubpass`，是 read-only depth，与之兼容。但若你想写深度，则需让 `RenderForwardSinglePass` 第二段（translucency 之后）使用一个支持 depth-write 的 RenderTarget binding —— 这又涉及 subpass 拆分，难度较大。**结论：默认不写深度的方案是安全的，但功能上意味着无法挡住后续不透明（也没有"后续不透明"，OK）。**

5. **Plan 设计冗余建议（Plan 自身第 572-580 行）** — 您自己提到 `bAfterTranslucencyBasePass` 与 `MeshPassType` 重复，**建议采纳**：直接用 `GetMeshPassType() == EMeshPass::MobileAfterTranslucencyPass` 判断，去掉成员变量，减少状态不一致风险。

---

## 5. 第 5 节：`RenderMobileAfterTranslucencyPass` 与 RDG 集成

### 验证
- `RenderMobileAfterTranslucencyPass` 实现 OK；`MobileBasePassRendering.cpp:470` 处 `RenderMobileBasePass` 函数体是模仿目标。
- `SceneRendering.h:2695` 声明位置 OK。
- `RenderCore.cpp:65` 处加 `DEFINE_STAT(STAT_AfterTranslucencyDrawTime)` OK；`RenderCore.h:44` 声明 OK。
- `BasePassRendering.h:144` 处 `DECLARE_GPU_DRAWCALL_STAT_EXTERN(AfterTranslucency)` OK。**但 Plan 漏掉了 `DEFINE_GPU_DRAWCALL_STAT(AfterTranslucency)`** — 在 `BasePassRendering.cpp:184` 附近需要补上 `DEFINE_GPU_DRAWCALL_STAT(AfterTranslucency);`，否则链接失败。
- `SceneRendering.h:2796` 添加 `FInstanceCullingDrawParams AfterTranslucencyInstanceCullingDrawParams;` OK。
- `MobileShadingRenderer.cpp:1433` `BuildInstanceCullingDrawParams` 中加入新 Pass `BuildRenderingCommands` OK。

### 关键风险

1. **Forward Single Pass：Subpass 限制（致命/重要）**
   - `RenderForwardSinglePass` 内部把整个 `[Opaque][Subpass1][NextSubpass → Translucency][NextSubpass → CustomResolve]` 装进**一个 GraphBuilder.AddPass**，且 SubpassHint 已经在 line 1586 设定为 `DepthReadSubpass`。
   - 在 `NextSubpass()` 之后调用 `RenderTranslucency`，然后再 `RenderMobileAfterTranslucencyPass`，**两者都在同一个 DepthRead-only subpass 内**。这对您的方案而言：
     - 颜色覆盖：✅ 可以做到。
     - 写深度：❌ 不能（subpass 已经标记 depth read-only）。
     - 您写的是 `TStaticDepthStencilState<false, ...>` 即不写深度，**实际可以运行**。
   - 但若您之后想写深度，需要把 AfterTranslucency 拆到 `NextSubpass()` 之后或独立 AddPass，并修改 `SubpassHint`、`RenderTargets` 的 `DepthStencilAccess`。当前 Plan **不支持深度写入**，请在文档/代码注释中明确告知此限制。

2. **Forward Multi Pass：subpass 是独立的 AddPass**
   - `RenderForwardMultiPass` 中 `RenderTranslucency` 位于 `DecalsAndTranslucency` 的 `GraphBuilder.AddPass` 内（`MobileShadingRenderer.cpp:1721`），其 `SecondPassParameters->RenderTargets.DepthStencil.SetDepthStencilAccess(ExclusiveDepthStencil)` 设置为 `DepthRead_StencilRead` 或 `DepthRead_StencilWrite`。同样**不支持深度写入**。
   - 您把 `RenderMobileAfterTranslucencyPass` 调用塞在 `RenderTranslucency` 后面，仍在该 AddPass 的 lambda 内，**OK**（颜色覆盖可以工作）。

3. **Deferred 路径未修改**
   - Plan 只针对 Forward，但 `RenderDeferredSinglePass`/`RenderDeferredMultiPass`（`MobileShadingRenderer.cpp:1947, 1996`）也会在 mobile deferred 下被调用。如果在 deferred shading 下意外启用 `bRenderAfterTranslucency`：物体会进 `MobileAfterTranslucencyPass` 但 **从不被绘制**（因为没在 deferred 路径里 dispatch）。这是符合"只支持 forward"的预期，但要保证用户对 deferred 启用时不会触发"物体消失"——建议在 `SetRenderAfterTranslucency` Setter 中加 `ensureMsg` 或 `IsMobileDeferredShadingEnabled` 检查，给用户提示。

4. **多视图（VR / Instanced Stereo）**
   - 您是 VR 项目。`RenderForwardSinglePass` 在 ISR 下 `RenderViews` 仍是 1 个 view，AfterTranslucency 也只调用一次，配合 `EInstanceCullingMode::Stereo` 路径正确处理两眼实例化。
   - **Multi-View（vr.MobileMultiView=2）**：`BasePassRenderTargets.MultiViewCount` 已经在 `InitRenderTargetBindings_Forward` 中处理，新 Pass 跟随同一 RenderTargets binding，**OK**。
   - **MultiPass（每眼独立 view）**：会 loop 2 次，AfterTranslucency 也会被各画一次，OK。

5. **`AfterTranslucencyInstanceCullingDrawParams` 的作用域**
   - Plan 把它放在 `FMobileSceneRenderer` 成员中（`SceneRendering.h:2796` 附近），与 `TranslucencyInstanceCullingDrawParams` 同级。这是**正确的**，因为它需要在 `BuildInstanceCullingDrawParams` 调用后、`RenderTranslucency`/`RenderMobileAfterTranslucencyPass` lambda 内（捕获 `this`）被读取。
   - 但需要注意它**不是 RDG-tracked uniform buffer**——`InstanceCullingDrawParams` 是 `RDG_PARAMETER` 类型，包含 `RDGBuffer` 等。把它存为 SceneRenderer 成员变量并跨 RDG Pass 引用，**这点在工程现存代码中已经在用**（参见 `TranslucencyInstanceCullingDrawParams`），可以照搬。
   - **但是！** —— 在 `RenderForwardSinglePass` 的 lambda 里 `&AfterTranslucencyInstanceCullingDrawParams`，需要按引用捕获 `this`。Plan 第 427 行写法 `RenderMobileAfterTranslucencyPass(RHICmdList, View, &AfterTranslucencyInstanceCullingDrawParams);` 内层 lambda 已经 `[this, PassParameters, ViewContext, &SceneTextures]`，可访问 `this->AfterTranslucencyInstanceCullingDrawParams`，**OK**。

6. **多视图渲染时 `BuildRenderingCommands` 仅对 0 号 view 调用一次**
   - `BuildInstanceCullingDrawParams` 在 `RenderForward` 的 view loop 内被调用（`MobileShadingRenderer.cpp:1564`），每个 view 都会刷新一次 `PassParameters->InstanceCullingDrawParams`（BasePass）和 `this->*OtherInstanceCullingDrawParams`（SkyPass/Translucency/...）。
   - **这意味着多视图下每个 view loop 都会覆盖 `AfterTranslucencyInstanceCullingDrawParams`**，但 `RenderForwardSinglePass` 是在同 view loop 内立刻调用 lambda，时序上 OK。

---

## 6. 第 6 节：`FPrimitiveViewRelevance` & GetViewRelevance & SceneVisibility

### 验证
- `PrimitiveViewRelevance.h` 中位字段添加 OK，**但** 构造函数已经 memzero（line 92-97 的循环），`bRenderAfterTranslucency = false;` 这行**多余**且**无害**，建议删除以保持代码整洁。
- `StaticMeshRender.cpp:2062`、`SkeletalMesh.cpp:7115` 处赋值 OK。
- `SceneVisibility.cpp:1556`（即 Plan 中 1564）的 if 分支修改：
  ```cpp
  if (ViewRelevance.bRenderAfterTranslucency)
      DrawCommandPacket.AddCommandsForMesh(..., EMeshPass::MobileAfterTranslucencyPass);
  else
      DrawCommandPacket.AddCommandsForMesh(..., EMeshPass::BasePass);
  ```
  逻辑正确，但**遗漏处理**：
  - 同分支内紧接着的 `if (!bMobileBasePassAlwaysUsesCSM) { AddCommandsForMesh(EMeshPass::MobileBasePassCSM); }` **未做守卫**——这会导致 `bRenderAfterTranslucency == true` 的物体仍会进 `MobileBasePassCSM`（见第 4 节风险点 1）。
  - **必须加守卫**：
    ```cpp
    if (!bMobileBasePassAlwaysUsesCSM && !ViewRelevance.bRenderAfterTranslucency)
    { ... AddCommandsForMesh(EMeshPass::MobileBasePassCSM); }
    ```
  - 或者：让 `EMeshPass::MobileAfterTranslucencyPass` 也支持 CSM 物体——但您说"沿用 BasePass 的渲染逻辑"，那应再注册一个 `MobileAfterTranslucencyCSMPass`，工作量增加，**不推荐**。如果允许"AfterTranslucency 不接收 CSM"，则简单加守卫即可。

- `SceneVisibility.cpp:2211` 的 `ComputeDynamicMeshRelevance` 修改：
  - 修改后逻辑：mobile 下若 `bRenderAfterTranslucency`，**仅** 设置 `MobileAfterTranslucencyPass`，不设 `BasePass`。
  - 注意：动态网格分支下还有 `MobileBasePassCSM` 的设置（line 2228-2232），Plan 中**未守卫**——`bRenderAfterTranslucency == true` 的动态网格仍会进 `MobileBasePassCSM`，与静态网格同样存在重复绘制。
  - **必须加守卫**：
    ```cpp
    if (ShadingPath == EShadingPath::Mobile && !ViewRelevance.bRenderAfterTranslucency)
    {
        PassMask.Set(EMeshPass::MobileBasePassCSM);
        ...
    }
    ```

### Skeletal Mesh 不会 cache MeshDrawCommand
- Skeletal Mesh 默认是动态绘制（`bRenderStatic = false`，参见 `SkeletalMesh.cpp:7112` 的逻辑），意味着会走 `ComputeDynamicMeshRelevance` 而非静态网格的 `AddCommandsForMesh` cache 路径。这条路径会让 `DispatchPassSetup` 在每帧重新生成 `MeshDrawCommand`——**性能上需注意**，但功能上 OK。

---

## 7. 其他遗漏 / 隐式依赖

### 7.1 GetViewRelevance 还需扩展到其它 Proxy
Plan 仅修改 `FStaticMeshSceneProxy` 与 `FSkeletalMeshSceneProxy` 的 `GetViewRelevance`，**符合需求**（"我只需要让 Mesh 和 Skeletal Mesh 生效"）。但若日后想扩展到其它 Proxy（如 InstancedStaticMesh、HISM、GeometryCollection、HeterogeneousVolume、Particle 等），需要在它们的 `GetViewRelevance` 中加入：`Result.bRenderAfterTranslucency = ShouldRenderAfterTranslucency();`。当前实现下，这些 Proxy 的 `Result.bRenderAfterTranslucency` 保持默认 0，物体永远不会进 `MobileAfterTranslucencyPass`，符合"只 Mesh/Skeletal Mesh 生效"的初衷。

### 7.2 InstancedStaticMesh / HISM 是否继承 `FStaticMeshSceneProxy`?
是的（`FInstancedStaticMeshSceneProxy : FStaticMeshSceneProxy`），**它们会自动继承本特性**，这点对用户友好。但 ISM/HISM 的 `GetViewRelevance` 通常会调用基类，请验证：
- `Engine/Source/Runtime/Engine/Private/InstancedStaticMesh.cpp` 中 `FInstancedStaticMeshSceneProxy::GetViewRelevance` 一般不 override 或调用基类。需确认。

### 7.3 `bCanReceiveCSM` 与 `LightmapPolicyType`
- `MobileAfterTranslucencyPass` Processor 创建时未传 `CanReceiveCSM`，所以走的是"非 CSM"分支，`SelectMeshLightmapPolicy` 会选 non-CSM policy。**OK**（与"AfterTranslucency 不接收 CSM"语义一致）。
- 静态光照（lightmap）：会选择带 lightmap 的 policy，正常工作。
- 但**移动平台 SkyLight Permutation** 等仍会被纳入，PSO 增多，建议关注 Shader 编译时间与包体。

### 7.4 SortKey / DrawOrder
- 您的 AfterTranslucency 物体在同 Pass 内的绘制顺序由 `FMeshDrawCommand` 的 SortKey 决定。`FMobileBasePassMeshProcessor` 走的是 opaque 排序（Front-to-Back），对您的语义（"覆盖透明"）影响不大。
- 但如果多个 AfterTranslucency 物体之间互相遮挡，且都不写深度，则**绘制顺序错误时会出现"后画的覆盖先画的"** —— 类似传统 forward 无深度物体的问题。**强烈建议在最终方案中允许深度写入**（见 4.3）。

### 7.5 阴影投射
- `bRenderAfterTranslucency` 不影响阴影（`bCastDynamicShadow` 等独立）。物体仍会出现在 ShadowDepth Pass 中，作为投射方与接收方都正常。OK。

### 7.6 SceneCapture / Custom Depth / Editor
- Plan 已声明"不需要 CustomDepth"，已正确跳过。
- SceneCapture 走的也是 Mobile Renderer，AfterTranslucency 同样会被画——通常无问题。
- Editor primitives（Editor 内部 mesh）走 `RenderMobileEditorPrimitives`，与 `MobileAfterTranslucencyPass` 无关。

### 7.7 `RenderCore.h` 与 `RENDERCORE_API`
- 添加 `DECLARE_CYCLE_STAT_EXTERN(..., RENDERCORE_API)` 是 OK 的（与 `STAT_BasePassDrawTime` 同样设置）。

### 7.8 `DEFINE_GPU_DRAWCALL_STAT(AfterTranslucency)` 必须补充
- Plan 漏掉了：在 `Engine/Source/Runtime/Renderer/Private/BasePassRendering.cpp:184` 附近加上：
  ```cpp
  DEFINE_GPU_DRAWCALL_STAT(AfterTranslucency);
  ```
- 否则 `SCOPED_GPU_STAT(RHICmdList, AfterTranslucency)` 会链接错误。

---

## 8. 推荐的修改清单（汇总）

按优先级排序：

### P0（必须修复，否则功能错误）
1. **`SceneVisibility.cpp:1565` 与 `:2230`**：为 `MobileBasePassCSM` 加 `!ViewRelevance.bRenderAfterTranslucency` 守卫（静态 & 动态网格路径）。否则物体会被画两次。
2. **`BasePassRendering.cpp` 补充 `DEFINE_GPU_DRAWCALL_STAT(AfterTranslucency);`**。
3. **`EMeshPass::Num` 注释 GUID 替换**：按约定生成新 GUID，避免与上游 merge 时冲突。

### P1（强烈建议）
1. **采纳 Plan 自身建议（572-580 行）**：移除 `bAfterTranslucencyBasePass` 成员，直接用 `MeshPassType == EMeshPass::MobileAfterTranslucencyPass` 判断，避免状态冗余。
2. **明确深度策略**：默认建议 `TStaticDepthStencilState<true, CF_DepthNearOrEqual>` 即写深度，并将 `DepthStencilAccess` 改为 `DepthWrite_StencilRead`。注意这要求 RenderPass/Subpass 允许深度写——Forward Single Pass 不支持（受 DepthReadSubpass 限制）；若要写深度，请额外把 AfterTranslucency 拆出 single-pass 之外作为独立 RenderPass（见 7.4）。
3. **`PrimitiveComponent.h`** 的 `bRenderAfterTranslucency` UPROPERTY 添加 `EditCondition = "bRenderInMainPass"`。
4. **移除 `FPrimitiveViewRelevance::FPrimitiveViewRelevance()` 中的冗余 `bRenderAfterTranslucency = false;`**（memzero 已置零）。

### P2（视情况）
1. 在 `SetRenderAfterTranslucency` 中 ensure mobile + forward 才允许设置 true。
2. 暴露 BlueprintPure `GetRenderAfterTranslucency()`。
3. 注释/文档明确：
   - 该特性仅在 Mobile Forward 下生效；
   - 不接收 CSM 阴影；
   - 默认不写深度（视您是否采纳 P1.2 而定）；
   - 仅 Static / Skeletal Mesh 类型生效。

---

## 9. 附：行号定位备忘（基于当前源码）

| 文件 | 当前实际行 | 说明 |
| --- | --- | --- |
| `Renderer/Public/MeshPassProcessor.h:34-78` | `enum EMeshPass::Type` | `Num == 32`，编辑器额外 +4 |
| `Renderer/Public/MeshPassProcessor.h:85-131` | `GetMeshPassName` switch | 含 `static_assert(EMeshPass::Num == 32)` |
| `Engine/Classes/Components/PrimitiveComponent.h:407-408` | `bRenderInMainPass` 定义 | |
| `Engine/Classes/Components/PrimitiveComponent.h:411` | `bRenderInDepthPass` | 紧邻位置 |
| `Engine/Private/Components/PrimitiveComponent.cpp:4457` | `SetRenderInMainPass` 实现 | |
| `Engine/Public/PrimitiveSceneProxy.h:700` | `ShouldRenderInMainPass` inline | |
| `Engine/Public/PrimitiveSceneProxy.h:1200` | `bRenderInMainPass` bitfield | |
| `Engine/Public/PrimitiveSceneProxyDesc.h:25` | `bRenderInMainPass = true;` | 默认值 |
| `Engine/Public/PrimitiveSceneProxyDesc.h:93` | `bRenderInMainPass : 1` 字段 | |
| `Engine/Private/PrimitiveSceneProxy.cpp:277` | `InitializeFrom` | |
| `Engine/Private/PrimitiveSceneProxy.cpp:428` | 构造函数初始化列表 | |
| `Engine/Private/StaticMeshRender.cpp:2055-2063` | `FStaticMeshSceneProxy::GetViewRelevance` | |
| `Engine/Private/SkeletalMesh.cpp:7107-7115` | `FSkeletalMeshSceneProxy::GetViewRelevance` | |
| `Renderer/Private/MobileBasePassRendering.h:480-487` | Processor 构造声明 | |
| `Renderer/Private/MobileBasePassRendering.h:533` | `bPassUsesDeferredShading` 成员 | 新成员加在此 |
| `Renderer/Private/MobileBasePass.cpp:810-826` | Processor 构造定义 | |
| `Renderer/Private/MobileBasePass.cpp:867-865` | `AddMeshBatch` | 含 `ShouldRenderInMainPass` 过滤 |
| `Renderer/Private/MobileBasePass.cpp:1151-1163` | `CreateMobileBasePassProcessor` | |
| `Renderer/Private/MobileBasePass.cpp:1218-1223` | `REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR` | |
| `Renderer/Private/MobileBasePassRendering.cpp:470-491` | `RenderMobileBasePass` | |
| `Renderer/Private/MobileShadingRenderer.cpp:1433-1446` | `BuildInstanceCullingDrawParams` | |
| `Renderer/Private/MobileShadingRenderer.cpp:1623` | Forward Single Pass `RenderTranslucency` 调用点 | |
| `Renderer/Private/MobileShadingRenderer.cpp:1735` | Forward Multi Pass `RenderTranslucency` 调用点 | |
| `Renderer/Private/SceneRendering.h:2695` | `RenderMobileBasePass` 声明 | |
| `Renderer/Private/SceneRendering.h:2796` | `TranslucencyInstanceCullingDrawParams` | |
| `Renderer/Private/SceneVisibility.cpp:1556-1577` | 静态网格 mobile 分流 | 含 BasePass + CSM 分支 |
| `Renderer/Private/SceneVisibility.cpp:2211-2232` | `ComputeDynamicMeshRelevance` | 含 BasePass + CSM 分支 |
| `RenderCore/Private/RenderCore.cpp:65` | `DEFINE_STAT(STAT_BasePassDrawTime)` | |
| `RenderCore/Public/RenderCore.h:44` | `DECLARE_CYCLE_STAT_EXTERN(STAT_BasePassDrawTime)` | |
| `Renderer/Private/BasePassRendering.h:144` | `DECLARE_GPU_DRAWCALL_STAT_EXTERN(Basepass)` | |
| `Renderer/Private/BasePassRendering.cpp:184` | `DEFINE_GPU_DRAWCALL_STAT(Basepass)` | **Plan 漏补 AfterTranslucency 的 DEFINE** |

---

## 10. 最终判断

- **方案可实现**，预计开发周期：实现 + 调试 1-2 天，性能/PSO 验证 1 天。
- **必修问题**：CSM 双绘制守卫、`DEFINE_GPU_DRAWCALL_STAT`、GUID 注释。
- **强烈建议**：移除 `bAfterTranslucencyBasePass` 成员；明确深度策略。
- **限制说明**：该特性只在 Forward 路径生效，不写深度，不接收 CSM；仅 Static/Skeletal Mesh 类型生效。
- 在 VR ISR / MultiView 下兼容性已通过现有 RDG/Subpass 机制保证，无需额外改动。

> 完成上述 P0 项后，方案可投入业务测试。
