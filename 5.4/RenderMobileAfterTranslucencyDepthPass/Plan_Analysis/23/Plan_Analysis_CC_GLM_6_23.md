# UE5.4 移动端 Forward 渲染路径 After-Translucency Pass 修改方案 — 完整分析报告

> 分析日期：2026/06/23
> 分析对象：`Docs/Plan.md`
> 工程根：`E:\Unreal Engine Work Projects\MR01_DaNaoTianGong_Main\Engine`

---

## 总体评估

该方案整体框架设计合理，核心思路（新增 `EMeshPass` 枚举、复制 `MobileBasePassMeshProcessor` 逻辑、在 `SceneVisibility` 中分流、在 `RenderForward` 管线中新增 Pass 调用）符合 UE5.4 移动端渲染架构。但存在多处**严格错误**（将导致编译失败、运行时崩溃或功能不正确）以及若干**缺失修改**和**潜在语义风险**。以下逐项详述。

---

## 一、严格错误（必须修复，否则编译失败或功能错误）

### 1.1 AddMeshBatch 中 PrimitiveSceneProxy 空指针崩溃

**文件**：`Engine/Source/Runtime/Renderer/Private/MobileBasePass.cpp` 第 867 行 `AddMeshBatch`

**方案代码**：
```cpp
bool bShouldRenderAfterTranslucency = PrimitiveSceneProxy->ShouldRenderAfterTranslucency();
```

**问题**：`PrimitiveSceneProxy` 可能为 `nullptr`。原始代码已有保护性检查：
```cpp
(PrimitiveSceneProxy && !PrimitiveSceneProxy->ShouldRenderInMainPass())
```
但方案新增的代码直接解引用 `PrimitiveSceneProxy` 未做空指针检查。当编辑器辅助元素或某些动态 MeshBatch（`PrimitiveSceneProxy` 为 `nullptr`）经过此处时，会直接崩溃。

**修正**：
```cpp
const bool bShouldRenderAfterTranslucency =
    PrimitiveSceneProxy && PrimitiveSceneProxy->ShouldRenderAfterTranslucency();
```

---

### 1.2 静态 Mesh 未从 MobileBasePassCSM 排除

**文件**：`Engine/Source/Runtime/Renderer/Private/SceneVisibility.cpp` 第 1556–1592 行

**方案修改后的代码（节选）**：
```cpp
if (ViewRelevance.bRenderAfterTranslucency)
{
    DrawCommandPacket.AddCommandsForMesh(..., EMeshPass::MobileAfterTranslucencyPass);
}
else
{
    DrawCommandPacket.AddCommandsForMesh(..., EMeshPass::BasePass);
}
if (!bMobileBasePassAlwaysUsesCSM)
{
    DrawCommandPacket.AddCommandsForMesh(..., EMeshPass::MobileBasePassCSM);  // ← 仍会被加入
}
```

**问题**：`MobileBasePassCSM` 的 `AddCommandsForMesh` 调用在 `if/else` 块**外部**，意味着标记了 `bRenderAfterTranslucency` 的静态网格仍然会被加入 `EMeshPass::MobileBasePassCSM`。该 Pass 在不透明阶段被绘制（`RenderMobileBasePass` 中），导致 After 物体在**不透明阶段被绘制一次**（在 CSM 路径下接收阴影），又在 After Pass 中再绘制一次，**完全违背了本需求"在不透明阶段不渲染"的核心目标**。

**修正**：
```cpp
if (ViewRelevance.bRenderAfterTranslucency)
{
    DrawCommandPacket.AddCommandsForMesh(..., EMeshPass::MobileAfterTranslucencyPass);
}
else
{
    DrawCommandPacket.AddCommandsForMesh(..., EMeshPass::BasePass);
    if (!bMobileBasePassAlwaysUsesCSM)
    {
        DrawCommandPacket.AddCommandsForMesh(..., EMeshPass::MobileBasePassCSM);
    }
}
```

---

### 1.3 ComputeDynamicMeshRelevance 未分流 After 物体（动态网格）

**文件**：`Engine/Source/Runtime/Renderer/Private/SceneVisibility.cpp` 第 2186–2264 行 `ComputeDynamicMeshRelevance`

**方案修改**仅处理了 `BasePass` 分支（在 if/else 中切换），但**忽略了紧随其后的 `MobileBasePassCSM` 添加**（约第 2228 行附近）：

```cpp
if (ShadingPath == EShadingPath::Mobile)
{
    PassMask.Set(EMeshPass::MobileBasePassCSM);  // ← 即便 bRenderAfterTranslucency = true 也会被加入
    View.NumVisibleDynamicMeshElements[EMeshPass::MobileBasePassCSM] += NumElements;
}
```

**问题**：动态网格（SkeletalMesh 等）标记了 After 后，仍会被加入 `MobileBasePassCSM`，导致与 1.2 相同的双重绘制。

**修正**：在 Mobile 分支中，将 `MobileBasePassCSM` 也包进 `!ViewRelevance.bRenderAfterTranslucency` 条件内。

---

### 1.4 `CreateMobileAfterTranslucencyPassProcessor` 最后一个参数 `true` 必须依赖签名修改先合入

**文件**：`Engine/Source/Runtime/Renderer/Private/MobileBasePass.cpp` 第 1224 行附近

**方案代码**：
```cpp
return new FMobileBasePassMeshProcessor(
    EMeshPass::MobileAfterTranslucencyPass, Scene, InViewIfDynamicMeshCommand,
    PassDrawRenderState, InDrawListContext, Flags, true);
```

如果**还没**给 `FMobileBasePassMeshProcessor` 加上 `bool bAfterTranslucencyBasePass` 参数，编译器会把 `true` 作为 `ETranslucencyPass::Type` 的隐式转换（→ `ETranslucencyPass::Type(1)`，即 `TPT_TranslucencyAfterDOF` 或类似值），从而把 Processor 当作**透明 Pass** 处理（`bTranslucentBasePass = true`），产生严重错误。

**修正建议**：必须把 header 中加 `bAfterTranslucencyBasePass` 参数、cpp 构造函数实现修改、以及 `CreateMobileAfterTranslucencyPassProcessor` 调用**作为同一原子提交**，避免中间态错误。为更安全可以用具名参数（命名约定）或显式 `ETranslucencyPass::TPT_MAX, true` 调用风格：

```cpp
return new FMobileBasePassMeshProcessor(
    EMeshPass::MobileAfterTranslucencyPass, Scene, InViewIfDynamicMeshCommand,
    PassDrawRenderState, InDrawListContext, Flags,
    /*InTranslucencyPassType=*/ETranslucencyPass::TPT_MAX,
    /*IsAfterTranslucencyBasePass=*/true);
```

---

### 1.5 构造函数初始化列表里的右括号笔误

**文件**：`Engine/Source/Runtime/Renderer/Private/MobileBasePass.cpp` 第 810 行附近

**方案代码**：
```cpp
, bPassUsesDeferredShading(bDeferredShading && !bTranslucentBasePass)
, bAfterTranslucencyBasePass(IsAfterTranslucencyBasePass))   // ← 末尾多了一个 )
{
}
```

末尾多了一个右括号，**编译失败**。应该是：

```cpp
, bAfterTranslucencyBasePass(IsAfterTranslucencyBasePass)
{
}
```

（这是文档拼写错误，但仍需指出，避免照抄。）

---

## 二、潜在问题与语义风险

### 2.1 深度写入为 false → After 物体之间无法互相遮挡

**方案**：
```cpp
PassDrawRenderState.SetDepthStencilState(TStaticDepthStencilState<false, CF_DepthNearOrEqual>::GetRHI());
```

**分析**：
- 透明物体不写深度。After Pass 渲染时，深度 buffer 中只有**不透明物体**的深度。
- `CF_DepthNearOrEqual` 深度测试：After 物体与不透明物体深度比较正确，能正确被不透明物体遮挡。
- **但不写深度** → 多个 After 物体之间无法正确遮挡。后画的 After 物体即使空间上在前面 After 之后（更远），也会覆盖（视觉错误）。

**对核心需求的影响**：
- "把透明物体遮挡住"：不需要深度写入即可达成（透明物体已经在 SceneColor 里，After 直接覆盖颜色就行）。
- "沿用不透明物体渲染深度测试等逻辑"：不透明物体是写深度的，按这句应该开。

**建议**：改为 `<true, CF_DepthNearOrEqual>` 开启深度写入。这样 After 物体之间能正确互相遮挡。
**但是有副作用**：当前 After 调用点位于 `RenderTranslucency` 之后，那时已经位于 SinglePass 的 subpass 1 / MultiPass 的 Pass 2，深度访问模式是 `DepthRead_StencilRead`（只读），**写深度会触发 RHI 校验失败或被驱动忽略**。详见 2.2。

---

### 2.2 渲染时序与 RenderPass 作用域

**SinglePass 流程**（`MobileShadingRenderer.cpp::RenderForwardSinglePass`）：
```
subpass 0: MaskedPrePass → RenderMobileBasePass → DebugView → PostRenderBasePass
subpass 1: NextSubpass() → Decals → Shadows → Fog → RenderTranslucency
           [方案在此插入 RenderMobileAfterTranslucencyPass]
```

subpass 1 的深度访问模式为 `DepthRead_StencilRead`。

**MultiPass 流程**：
```
Pass 1 (SceneColorRendering): MaskedPrePass → RenderMobileBasePass → ...
[Resolve MSAA depth]
Pass 2 (DecalsAndTranslucency): Decals → Shadows → Fog → RenderTranslucency
                                 [方案在此插入 RenderMobileAfterTranslucencyPass]
```

Pass 2 的深度访问模式同样是 `DepthRead_StencilRead`。

**结论**：
- 若坚持 `<false, CF_DepthNearOrEqual>`（不写深度），现有插入点 OK。
- 若改为 `<true, ...>`（写深度），需要在 `MultiPass` 中**新增一个 Pass 3**（带 `DepthWrite_StencilWrite` 访问），或者在 `SinglePass` 中重新设计 subpass。这是相当大的改动，建议**保持不写深度**，并接受 After 物体之间相互遮挡可能错误的语义限制。

> 折中方案：如果 After 物体彼此空间关系简单（例如只有一个 After 物体，或它们彼此不相交），不写深度也工作得很好。VR 场景里"标记一个物体让它最后画"的典型用法通常如此。

---

### 2.3 After 物体在透明物体之后时的视觉限制（固有语义）

绘制顺序：不透明 → 半透明 → After。

- After 物体空间上**在透明物体之前**（更近）：覆盖透明颜色，视觉**正确**。
- After 物体空间上**在透明物体之后**（更远）：深度测试基于不透明深度，能通过测试，但视觉上 After 会覆盖前面的半透明物体，**视觉错误**。

这是此方案的**固有语义限制**，无法通过代码修复。建议在使用规范里明确：把 After 标记仅给"需要无视半透明遮挡叠加在最上层"的对象（典型场景：VR 手部、UI 网格、关键道具等）。

---

### 2.4 移动延迟着色路径的副作用

`FMobileBasePassMeshProcessor` 构造函数中：
```cpp
bDeferredShading = IsMobileDeferredShadingEnabled(...);
bPassUsesDeferredShading = bDeferredShading && !bTranslucentBasePass;
```

After Pass 的 `bTranslucentBasePass = false`，所以 `bPassUsesDeferredShading = bDeferredShading`。如果平台启用了 Mobile Deferred，After Pass 会按 Deferred 路径处理（GBuffer 输出等），这与"在透明物体之后再画一次"的位置不兼容（GBuffer 早已被消费）。

**建议**：在 `CreateMobileAfterTranslucencyPassProcessor` 中显式断言或在 `AddMeshBatch` 中跳过 Deferred 路径，例如：

```cpp
if (bAfterTranslucencyBasePass && bPassUsesDeferredShading)
{
    // After Pass 在 Forward 模式下才有意义
    return;
}
```

或者更直接：在 `FMobileBasePassMeshProcessor` 中，当 `bAfterTranslucencyBasePass` 为真时强制走 Forward shader permutation。用户只关心 Forward 路径，最简做法是在 `Create*Processor` 注册前判断 `IsMobileDeferredShadingEnabled`，若是 Deferred 则跳过整套 After Pass 注册和分流。

---

### 2.5 阴影投射 / 接收

- **阴影投射**：阴影深度图由 `CSMShadowDepth` / `VSMShadowDepth` 独立绘制，与 BasePass 无关。After 物体**仍可投射阴影**。
- **阴影接收**：After Pass 走 `FMobileBasePassMeshProcessor` 的 shader，包含 CSM/接收阴影逻辑，**可以接收阴影**。
- **但注意 1.2/1.3**：如果未从 `MobileBasePassCSM` 排除，会在不透明阶段提前画一遍，破坏需求。

---

### 2.6 PrimitiveSceneProxyDesc 类型不一致（uint32 vs uint8）

`PrimitiveComponent.h` 用 `uint8 bRenderAfterTranslucency : 1`，而 `PrimitiveSceneProxyDesc.h` 用 `uint32 bRenderAfterTranslucency : 1`。位字段宽度差异不影响功能（位 field 总是 1 bit），但与既有风格保持一致更好。**保持 Desc 用 `uint32`** 即与原 `bRenderInMainPass` 等字段一致，方案当前写法 OK。

---

## 三、缺失或不完整的修改

### 3.1 数组大小自动扩展（无需手动改）

- `int32 NumVisibleDynamicMeshElements[EMeshPass::Num];`（SceneRendering.h）
- `TStaticArray<FParallelMeshDrawCommandPass, EMeshPass::Num> ParallelMeshDrawCommandPasses;`

随 `EMeshPass::Num` 自动扩展，**无需修改**。

### 3.2 `FMeshPassMask`（uint64 位掩码）

新增后 Num=33（非编辑器）/ 37（编辑器），都 ≤ 64，**无需修改**。

### 3.3 `EMeshPass::NumBits = 6`

允许最多 64 个枚举值，**无需修改**。

### 3.4 `static_assert` 数值

方案将断言从 `32 → 33`、`32+4 → 33+4`，正确。

### 3.5 PSO 预缓存 / Cached Mesh Commands

`REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR(...EMeshPassFlags::CachedMeshCommands | EMeshPassFlags::MainView)` 与 BasePass 注册一致。`FScene::AddPrimitive` 时会自动调用 `CreateMobileAfterTranslucencyPassProcessor`，PSO precache 也通过既有 `CollectPSOInitializers` 自动覆盖。**无需额外修改**。

### 3.6 `BuildInstanceCullingDrawParams` 中关于 `bIsFullDepthPrepassEnabled` 的分支

方案只在 `if (Scene->GPUScene.IsEnabled())` 内添加了 After 的 `BuildRenderingCommands`，没有像 DepthPass 那样根据 `bIsFullDepthPrepassEnabled` 条件化。这里**不需要**——After Pass 与 DepthPass 无关，无论是否有 prepass 都要构建命令。**OK**。

### 3.7 `AfterTranslucencyInstanceCullingDrawParams` 的成员变量声明

声明为 `FMobileSceneRenderer` 成员（与 `TranslucencyInstanceCullingDrawParams` 同级），生命周期与 Renderer 一致；引用在 `BuildRenderingCommands` 内被 RDG Lambda 捕获，生命周期足够。模式与已有 `TranslucencyInstanceCullingDrawParams` 一致，**OK**。

### 3.8 编辑器图元 / Decal / SingleLayerWater 等其他 Pass 的分流

需求里明确只要 Mesh / SkeletalMesh，这些方案**不动**是合理的。

### 3.9 在 `MaterialInterface::GetShaderMap` 之类的 Permutation 表

不需要新增——After Pass 复用 BasePass shader permutation，无新 shader。**OK**。

### 3.10 `SetupMobileBasePassAfterShadowInit` / `Init*` 流程

新增 Pass 的 ProcessorFactory 已由 `REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR` 注册到全局表，`ComputeDynamicMeshRelevance` 与 `SceneVisibility` 静态 mesh 分流后，命令会被自动加入 `ParallelMeshDrawCommandPasses[MobileAfterTranslucencyPass]`。**无需额外的 Setup 调用**。

### 3.11 编辑器 HitProxy / 选中高亮

After 物体在编辑器中点选/高亮可能不准确（HitProxy 路径默认走 BasePass 流程）。VR 项目通常不在意，但需提一句。如果将来要修，可以在 HitProxy Processor 里也加分流。

---

## 四、各文件逐项核对

### 4.1 `Engine/Source/Runtime/Renderer/Public/MeshPassProcessor.h`

| 方案要点 | 实际位置（当前源码） | 评估 |
|---|---|---|
| `EMeshPass` 枚举新增 `MobileAfterTranslucencyPass` | 在 `WaterInfoTexturePass`（第 67 行）之后、`#if WITH_EDITOR`（第 69 行）之前 | ✅ 位置正确 |
| `NumBits = 6` 保持 | 第 77 行 | ✅ 新 Num=33/37 ≤ 64 |
| `GetMeshPassName` 添加 case | 在 `WaterInfoTexturePass`（第 118 行）之后 | ✅ 位置正确 |
| `static_assert` 改为 `Num == 33` / `Num == 33 + 4` | 第 127–131 行 | ✅ 计算正确 |

### 4.2 `PrimitiveComponent.h` / `.cpp`

| 方案要点 | 实际位置 | 评估 |
|---|---|---|
| `uint8 bRenderAfterTranslucency : 1;` | `bRenderInMainPass` 之后（约 407 行附近） | ✅ |
| `SetRenderAfterTranslucency` 声明 | `SetRenderInMainPass` 之后（约 1917 行附近） | ✅ |
| Setter 实现 | `SetRenderInMainPass` 实现之后（约 4457 行附近） | ✅ |
| 构造函数 `bRenderAfterTranslucency = false` | 与 `bRenderInMainPass = true` 同处（约 333 行附近） | ✅ |

### 4.3 `PrimitiveSceneProxy.h` / `.cpp`

| 方案要点 | 实际位置 | 评估 |
|---|---|---|
| `uint8 bRenderAfterTranslucency : 1;` 成员 | `bRenderInMainPass` 之后（约 1200 行附近） | ✅ |
| `ShouldRenderAfterTranslucency()` 内联 getter | `ShouldRenderInMainPass` 之后（约 700 行附近） | ✅ |
| `InitializeFrom`（Component → Proxy）赋值 | 约 277 行附近 | ✅ |
| 构造函数初始化列表（Desc → Proxy） | 约 428 行附近 | ✅ |

> ⚠️ **注意**：UE5.4 同时存在 `InitializeFrom(UPrimitiveComponent*)` 和 `InitializeFromPrimitiveSceneProxyDesc(const FPrimitiveSceneProxyDesc&)` 两条路径，**两边都要赋值**。方案分别在 277 行和 428 行做了——双路径都覆盖，OK。

### 4.4 `PrimitiveSceneProxyDesc.h`

| 方案要点 | 实际位置 | 评估 |
|---|---|---|
| `uint32 bRenderAfterTranslucency : 1;` | `bRenderInMainPass` 之后（约 93 行附近） | ✅ |
| 构造函数 `= false` | 约 25 行附近 | ✅ |

### 4.5 `MobileBasePassRendering.h`

| 方案要点 | 实际位置 | 评估 |
|---|---|---|
| `const bool bAfterTranslucencyBasePass;` 成员 | 在 `bPassUsesDeferredShading`（约 533 行）之后 | ✅ 声明顺序与初始化顺序一致 |
| 构造函数追加 `bool bAfterTranslucencyBasePass = false` 形参 | 形参列表最末位（约 480 行附近） | ✅ 带默认值，向后兼容 |

### 4.6 `MobileBasePass.cpp`

| 方案要点 | 实际位置 | 评估 |
|---|---|---|
| 构造函数初始化 `bAfterTranslucencyBasePass(IsAfterTranslucencyBasePass)` | 约 810 行 | ⚠️ **拼写错误**：末尾多写了一个 `)`，见 §1.5 |
| `AddMeshBatch` 分流逻辑 | 约 867 行 | ❌ **空指针风险**，见 §1.1 |
| `CreateMobileAfterTranslucencyPassProcessor` | 约 1151 行 | ⚠️ 深度状态/Flags 判断见 §2.1 |
| `REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR(...)` | 约 1223 行 | ✅ Flags 与 BasePass 对齐 |

> 关于 `Create*` 函数里 `PassDrawRenderState.SetBlendState(...)` 注释为 "是否还需要？"——**需要保留**。否则会沿用未定义的 BlendState，结果不可预测。建议显式：
> ```cpp
> PassDrawRenderState.SetBlendState(TStaticBlendStateWriteMask<CW_RGBA>::GetRHI());
> ```
>
> 关于 `Flags` 判断：BasePass 还判断了 `MobileBasePassAlwaysUsesCSM` 决定是否加 `CanReceiveCSM`。**After Pass 也应该一致**，否则 After 物体不会接收 CSM 阴影：
> ```cpp
> const FMobileBasePassMeshProcessor::EFlags Flags =
>     FMobileBasePassMeshProcessor::EFlags::CanUseDepthStencil
>     | (MobileBasePassAlwaysUsesCSM(GShaderPlatformForFeatureLevel[FeatureLevel])
>         ? FMobileBasePassMeshProcessor::EFlags::CanReceiveCSM
>         : FMobileBasePassMeshProcessor::EFlags::None);
> ```
> 否则如果 `MobileBasePassAlwaysUsesCSM=true`（这是 Quest VR 项目最常见的设定），After 物体将不会接收 CSM 阴影，与"沿用不透明渲染逻辑"不符。

### 4.7 `MobileBasePassRendering.cpp` / `SceneRendering.h`

| 方案要点 | 实际位置 | 评估 |
|---|---|---|
| `RenderMobileAfterTranslucencyPass` 实现 | `RenderMobileBasePass` 之后（约 492 行附近） | ✅ |
| `SceneRendering.h` 声明 | `RenderMobileBasePass` 声明附近（约 2695 行） | ✅ |
| `AfterTranslucencyInstanceCullingDrawParams` 成员 | `TranslucencyInstanceCullingDrawParams` 同级（约 2796 行附近） | ✅ |

### 4.8 `MobileShadingRenderer.cpp`

| 方案要点 | 实际位置 | 评估 |
|---|---|---|
| `BuildInstanceCullingDrawParams` 添加 `MobileAfterTranslucencyPass` | 约 1433 行 `if (GPUScene.IsEnabled())` 内 | ✅ |
| `RenderForwardSinglePass` 中 `RenderTranslucency` 之后调用 | 约 1624 行 | ✅ 不写深度时可行，写深度时需重构 RenderPass（见 §2.2） |
| `RenderForwardMultiPass` 同位置插入 | 约 1736 行 | ✅ 同上 |

### 4.9 `RenderCore.cpp` / `RenderCore.h` / `BasePassRendering.h`

| 方案要点 | 实际位置 | 评估 |
|---|---|---|
| `DEFINE_STAT(STAT_AfterTranslucencyDrawTime)` | RenderCore.cpp 第 65 行附近 | ✅ |
| `DECLARE_CYCLE_STAT_EXTERN(...)` | RenderCore.h 第 44 行附近 | ✅ |
| `DECLARE_GPU_DRAWCALL_STAT_EXTERN(AfterTranslucency)` | BasePassRendering.h 第 144 行附近 | ✅ |
| 对应 `DEFINE_GPU_DRAWCALL_STAT(AfterTranslucency)` | **方案未提及！** | ❌ **缺失**，见 §3 |

> ⚠️ **补充**：`DECLARE_GPU_DRAWCALL_STAT_EXTERN(AfterTranslucency)` 只是声明，需要在某个 `.cpp` 里写 `DEFINE_GPU_DRAWCALL_STAT(AfterTranslucency);`，否则链接失败。BasePass 是在 `BasePassRendering.cpp` 里 `DEFINE_GPU_DRAWCALL_STAT(Basepass);` 的，建议也在那里加上 `AfterTranslucency`。

### 4.10 `PrimitiveViewRelevance.h`

| 方案要点 | 实际位置 | 评估 |
|---|---|---|
| `uint32 bRenderAfterTranslucency : 1;` | `bRenderInMainPass` 之后（约 54 行） | ✅ |
| 构造函数 `bRenderAfterTranslucency = false` | 约 103 行 | ✅（已 memset，是冗余但无害） |

### 4.11 `StaticMeshRender.cpp` / `SkeletalMesh.cpp`

| 方案要点 | 实际位置 | 评估 |
|---|---|---|
| `FStaticMeshSceneProxy::GetViewRelevance` 中赋值 `Result.bRenderAfterTranslucency` | 约 2055 行 | ✅ |
| `FSkeletalMeshSceneProxy::GetViewRelevance` 中赋值 | 约 7107 行 | ✅（注意函数在 `SkeletalMesh.cpp` 中是 Skeletal Proxy 的 GetViewRelevance，确认是 SkeletalMeshSceneProxy 的而不是 USkeletalMesh 资产类的） |

> ⚠️ 检查项：UE5.4 中 `FSkeletalMeshSceneProxy` 的实际位置可能是在 `Engine/Private/SkeletalMesh.cpp` 中，也可能在 `SkeletalMeshSceneProxy.cpp` 等其他单元——核对时只看 **是否是 SkeletalMeshSceneProxy::GetViewRelevance** 即可。

### 4.12 `SceneVisibility.cpp`

| 方案要点 | 实际位置 | 评估 |
|---|---|---|
| 静态 Mesh 的 Mobile 分支分流 | 约 1564 行 | ❌ **遗漏 MobileBasePassCSM 排除**，见 §1.2 |
| `ComputeDynamicMeshRelevance` 的 Mobile 分支分流 | 约 2211 行 | ❌ **同上**，见 §1.3 |

---

## 五、推荐的最终修改清单（Checklist）

### 必须修复的严格错误

- [ ] **§1.1 AddMeshBatch nullptr 检查**
  `MobileBasePass.cpp`：
  ```cpp
  const bool bShouldRenderAfterTranslucency =
      PrimitiveSceneProxy && PrimitiveSceneProxy->ShouldRenderAfterTranslucency();
  ```

- [ ] **§1.2 静态 Mesh：把 `MobileBasePassCSM` 也并入 else 分支**
  `SceneVisibility.cpp` 静态 mesh Mobile 分支：
  ```cpp
  if (ViewRelevance.bRenderAfterTranslucency)
  {
      DrawCommandPacket.AddCommandsForMesh(..., EMeshPass::MobileAfterTranslucencyPass);
  }
  else
  {
      DrawCommandPacket.AddCommandsForMesh(..., EMeshPass::BasePass);
      if (!bMobileBasePassAlwaysUsesCSM)
      {
          DrawCommandPacket.AddCommandsForMesh(..., EMeshPass::MobileBasePassCSM);
      }
  }
  ```

- [ ] **§1.3 动态 Mesh：同样的修正**
  `ComputeDynamicMeshRelevance` 中：
  ```cpp
  if (ShadingPath == EShadingPath::Mobile)
  {
      if (ViewRelevance.bRenderAfterTranslucency)
      {
          PassMask.Set(EMeshPass::MobileAfterTranslucencyPass);
          View.NumVisibleDynamicMeshElements[EMeshPass::MobileAfterTranslucencyPass] += NumElements;
      }
      else
      {
          PassMask.Set(EMeshPass::BasePass);
          View.NumVisibleDynamicMeshElements[EMeshPass::BasePass] += NumElements;
          PassMask.Set(EMeshPass::MobileBasePassCSM);
          View.NumVisibleDynamicMeshElements[EMeshPass::MobileBasePassCSM] += NumElements;
      }
  }
  ```

- [ ] **§1.4 构造函数签名同步修改**
  在 header 中加 `bool bAfterTranslucencyBasePass = false` 参数前，不要先合并 `CreateMobileAfterTranslucencyPassProcessor`，否则 `true` 会被吃成 `ETranslucencyPass::Type(1)`，导致 After Pass 被当作半透明 Pass。

- [ ] **§1.5 修正初始化列表多写的 `)`**
  ```cpp
  , bAfterTranslucencyBasePass(IsAfterTranslucencyBasePass)
  ```

- [ ] **§4.9 补 `DEFINE_GPU_DRAWCALL_STAT(AfterTranslucency)`**
  在 `BasePassRendering.cpp`（与 `Basepass` 一同）。

### 强烈建议修复（语义/正确性）

- [ ] **§4.6 `CreateMobileAfterTranslucencyPassProcessor` 显式设置 BlendState 与 CanReceiveCSM**
  ```cpp
  PassDrawRenderState.SetBlendState(TStaticBlendStateWriteMask<CW_RGBA>::GetRHI());
  PassDrawRenderState.SetDepthStencilAccess(FScene::GetDefaultBasePassDepthStencilAccess(FeatureLevel));
  PassDrawRenderState.SetDepthStencilState(TStaticDepthStencilState<false, CF_DepthNearOrEqual>::GetRHI()); // 注 §2.1
  const FMobileBasePassMeshProcessor::EFlags Flags =
      FMobileBasePassMeshProcessor::EFlags::CanUseDepthStencil
      | (MobileBasePassAlwaysUsesCSM(GShaderPlatformForFeatureLevel[FeatureLevel])
          ? FMobileBasePassMeshProcessor::EFlags::CanReceiveCSM
          : FMobileBasePassMeshProcessor::EFlags::None);
  ```

- [ ] **§2.4 Deferred 模式下跳过 After 注册或在 AddMeshBatch 中跳出**
  若项目可能切到 Mobile Deferred，需要避免 After Pass 走 GBuffer 路径。最简洁的做法是 `CreateMobileAfterTranslucencyPassProcessor` 在 Deferred 模式下返回 `nullptr`，并在调用 `RenderMobileAfterTranslucencyPass` 处加 Forward 判断。

### 关于深度写入与遮挡（设计决策）

- [ ] **§2.1 / §2.2 选择一种**：
  - **方案 A（推荐，零侵入）**：保持 `<false, CF_DepthNearOrEqual>`，接受多个 After 物体之间不互相遮挡的限制（在用户文档中说明）。
  - **方案 B（更正确，工作量大）**：改为 `<true, CF_DepthNearOrEqual>`，并在 `RenderForwardMultiPass` 中追加一个具备 `DepthWrite` 访问的新 RDG Pass；SinglePass 同样需要重构 subpass。

- [ ] **§2.3 文档化语义限制**：After 物体在半透明物体之后时会错误覆盖前面的半透明物体，使用规范里需说明只把 After 标记给"想要无视半透明叠加在最上层"的对象。

### 可以忽略的次要项

- [x] `PrimitiveViewRelevance` 构造函数中 `bRenderAfterTranslucency = false` 冗余但无害。
- [x] `PrimitiveSceneProxyDesc.h` 用 `uint32` 位字段与其他文件 `uint8` 不一致，**不影响功能**。
- [x] `EMeshPass::NumBits = 6` 无需调整。
- [x] `FMeshPassMask` / `ParallelMeshDrawCommandPasses` / `NumVisibleDynamicMeshElements` 数组随 `EMeshPass::Num` 自动扩展。

### 编辑器 / HitProxy（非关键）

- After 物体在编辑器中的 HitProxy 拾取/高亮可能不准确——VR 出包不影响，若需要可在 HitProxy Processor 中也加分流逻辑。

---

## 六、最终结论

整体思路**可行**，方案大体覆盖了 UE5.4 移动端 Forward 渲染所需的注入点。但当前文档版本要落地，必须先处理以下**5 个阻塞性问题**：

1. `AddMeshBatch` 的 nullptr 解引用（崩溃）；
2. 静态 / 动态 Mesh 分流时 **`MobileBasePassCSM` 未排除 After 物体**（双重绘制，违背核心需求）；
3. `CreateMobileAfterTranslucencyPassProcessor` 与 `FMobileBasePassMeshProcessor` 签名修改必须**同批次提交**；
4. 构造函数初始化列表的**笔误右括号**；
5. `DECLARE_GPU_DRAWCALL_STAT_EXTERN` 必须配套 `DEFINE_GPU_DRAWCALL_STAT`，否则链接失败。

完成这 5 处修正后，方案应可编译并基本满足"被标记的不透明 Mesh 在半透明之后再画一次以遮挡半透明"的核心需求。同时建议补上 **`CanReceiveCSM` 与 `BlendState` 的显式设置**（§4.6），并明示**多个 After 物体不互相遮挡**与**After 在透明之后时视觉错误**两条固有语义限制（§2.1 / §2.3）。
