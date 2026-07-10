# 方案分析报告

**分析日期**: 2026/06/29
**分析对象**: Android VR 移动端 Forward 渲染路径下 "Render After Translucency" 功能修改方案
**分析结论**: 方案整体思路正确，但存在若干**错误**和**潜在问题**，以及**未修改的遗漏部分**

---

## 1. 错误 (需立即修正)

### 1.1 static_assert 当前值描述错误

**位置**: `Engine/Source/Runtime/Renderer/Public/MeshPassProcessor.h:128-130`

**现状代码**:
```c++
static_assert(EMeshPass::Num == 32 + 4, ...);  // editor
static_assert(EMeshPass::Num == 32, ...);       // non-editor
```

**方案描述** (有误):
> 并更新底部断言 EMeshPass::Num == 34 + 4 与 EMeshPass::Num == 34

方案假设"当前是 34"，但**当前实际是 32** (非编辑器) / 36 (编辑器)。新增 2 个枚举后, 正确的目标值应为 `34` (非编辑器) / `38` (编辑器), 即 `34+4`。

**修正方案**: 将 static_assert 改为:
```c++
static_assert(EMeshPass::Num == 34 + 4, ...);  // editor
static_assert(EMeshPass::Num == 34, ...);       // non-editor
```

### 1.2 AddMeshBatch 的 "bRenderInMainPass" 提前 return 会让 AfterTranslucencyPass 失效

**位置**: `Engine/Source/Runtime/Renderer/Private/MobileBasePass.cpp:867` 附近的 `FMobileBasePassMeshProcessor::AddMeshBatch`

**问题代码**:
```c++
if (!MeshBatch.bUseForMaterial ||
    (Flags & ...DoNotCache) == ...DoNotCache ||
    (PrimitiveSceneProxy && !PrimitiveSceneProxy->ShouldRenderInMainPass()))  // <-- 问题
{
    return;
}
//RenderAfterTranslucency Added
const bool bAfterTranslucencyBasePass = ...;
...
```

**问题**: 当一个组件设置 `bRenderInMainPass=false` + `bRenderAfterTranslucency=true` (即只想在 Translucency 之后渲染, 不参与常规 BasePass), 该组件在 AddMeshBatch 的**第一道关卡就被 return**, 导致**根本进不去** `MobileAfterTranslucencyPass` / `MobileAfterTranslucencyDepthPass` 的逻辑分支。

**正确写法** (首道关卡应排除两种 pass 都不需要渲染的情况):
```c++
if (!MeshBatch.bUseForMaterial ||
    (Flags & ...DoNotCache) == ...DoNotCache ||
    (PrimitiveSceneProxy && !PrimitiveSceneProxy->ShouldRenderInMainPass() 
        && !PrimitiveSceneProxy->ShouldRenderAfterTranslucency()))  // <-- 同时放行 AfterTranslucency
{
    return;
}
```

### 1.3 SceneVisibility 静态网格的 bRenderAfterTranslucency 块未进入主 if

**位置**: `Engine/Source/Runtime/Renderer/Private/SceneVisibility.cpp:1556`

**现状外层 if**:
```c++
if (StaticMeshRelevance.bUseForMaterial && (ViewRelevance.bRenderInMainPass || ViewRelevance.bRenderCustomDepth))
```

**方案修改** (在 if 内部 else 分支处理 AfterTranslucency):
```c++
if (StaticMeshRelevance.bUseForMaterial && (ViewRelevance.bRenderInMainPass || ViewRelevance.bRenderCustomDepth))
{
    if (ShadingPath == EShadingPath::Mobile)
    {
        if (!StaticMeshRelevance.bUseSkyMaterial)
        {
            if (ViewRelevance.bRenderAfterTranslucency)
            {
                // AfterTranslucency ...
            }
            else
            {
                // BasePass ...
            }
            ...
        }
    }
    ...
}
```

**问题**: 标记物体如果设置 `bRenderInMainPass=false` + `bRenderAfterTranslucency=true`, **整个 if 块不会进入**, AfterTranslucency 路径失效。同 1.2 的问题。

**修正方案**: 外层 if 应包含 `bRenderAfterTranslucency`:
```c++
if (StaticMeshRelevance.bUseForMaterial 
    && (ViewRelevance.bRenderInMainPass || ViewRelevance.bRenderCustomDepth || ViewRelevance.bRenderAfterTranslucency))
```

---

## 2. 潜在问题 (可能导致功能异常)

### 2.1 标记物体仍会进入 Velocity、Anisotropy、Decal 等 Pass

**位置**: `Engine/Source/Runtime/Renderer/Private/SceneVisibility.cpp:1506-1613`

**问题**: 标记物体只要 `bRenderInMainPass=true` (默认), 在进入用户修改的 BasePass 分支之前, **已经被添加到** `EMeshPass::Velocity` (1508-1519)、`EMeshPass::AnisotropyPass` (1594-1597)、`EMeshPass::CustomDepth` (1599-1602)、`EMeshPass::LightmapDensity` / `DebugViewMode` (1604-1612) 等 Pass。

**潜在后果**:
- Velocity 写入会导致运动模糊异常
- CustomDepth 写入意味着即便 bRenderCustomDepth=false, 仍会进入该 Pass

**建议**: 标记物体需排除这些 Pass, 或者将 bRenderInMainPass 设计为 false + bRenderAfterTranslucency=true, 并同步修改其他 Pass 的条件 (工作量大)。

### 2.2 标记物体仍会进入 DepthPass (深度预 Pass)

**位置**: `Engine/Source/Runtime/Renderer/Private/SceneVisibility.cpp:1501`

**问题**: 外层 if `(bRenderInMainPass || bRenderCustomDepth || bRenderInDepthPass)` 默认通过 `bRenderInDepthPass=true`, 标记物体仍会被加到 `EMeshPass::DepthPass`。然后 BasePass 块中虽然被 AfterTranslucency 替代, 但**深度已经被写一次**。

**影响**: AfterTranslucencyDepthPass 仍需写一次 (此时无 BasePass 写), 但 DepthPass 已经写过了, 会有冗余。功能正常, 但有性能浪费。

**建议**: 标记物体应排除 DepthPass。可在外层 if 加 `&& !ViewRelevance.bRenderAfterTranslucency`。

### 2.3 MobileBasePassCSM Pass 仍被错误进入

**位置**: `Engine/Source/Runtime/Renderer/Private/SceneVisibility.cpp:568` 附近的 ComputeDynamicMeshRelevance

**问题**: 用户仅在 BasePass 设置处 (`PassMask.Set(EMeshPass::BasePass)`) 加了 bRenderAfterTranslucency 判断, 但**没有同步处理** `EMeshPass::MobileBasePassCSM` (line 569) 的 `PassMask.Set`。方案中静态网格路径同时清掉了 BasePass 和 MobileBasePassCSM, 但动态网格路径没有清掉 MobileBasePassCSM。

**影响**: 动态标记物体 (主要是 SkeletalMesh) 仍会被加入 MobileBasePassCSM Pass, 然后在 AddMeshBatch 中被排除 (功能正常), 但 PacketMask 已脏。属于逻辑不一致, 不影响渲染, 但建议同步处理:
```c++
if(ShadingPath == EShadingPath::Mobile && ViewRelevance.bRenderAfterTranslucency)
{
    PassMask.Set(EMeshPass::MobileAfterTranslucencyDepthPass);
    ...
    PassMask.Set(EMeshPass::MobileAfterTranslucencyPass);
    ...
    // 注意: 不要 Set(MobileBasePassCSM)
}
```

### 2.4 MobileAfterTranslucencyDepthPass 使用 Opaque 着色器会执行像素着色

**位置**: `Engine/Source/Runtime/Renderer/Private/MobileBasePass.cpp` 中 `CreateMobileAfterTranslucencyDepthPassProcessor`

**问题**: 用户使用 `FMobileBasePassMeshProcessor(EMeshPass::MobileAfterTranslucencyDepthPass, ...)` 缺省第 5 参数, `bTranslucentBasePass=false`, 会走 Opaque 着色器分支。Opaque 着色器**会执行像素着色器**, 只是通过 `CW_NONE` blend state 丢弃 Color 输出。

**影响**:
- 功能正确 (深度仍写入)
- 但**性能浪费**: 像素着色器实际跑了一遍, 又被丢弃
- 移动端 VR 对性能敏感, 建议使用专门的 Z-Pass 材质 / VertexFactory 路径

**建议优化方案**: 复用 `MobileBasePassDepth` / `FMobileDepthPassMeshProcessor` 路径, 或者使用 `EarlyZPass` 的 mobile 变体, 或者使用材质 Mask 强制走 `MaterialDomain_DeferredDecal` 之外的深度路径。

### 2.5 Subpass 切换与 LoadOp 行为需验证

**位置**: `Engine/Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp:1609, 1623` 附近

**问题**: 用户在 RenderForwardSinglePass 中:
- Subpass 0: `RenderMobileBasePass` + `RenderMobileAfterTranslucencyDepthPass` (line 1609 之后)
- Subpass 1: `RenderDecals` + `RenderTranslucency` + `RenderMobileAfterTranslucencyPass` (line 1623 之后)

**潜在问题**:
- Depth 在 Subpass 0 写入, Subpass 1 通过 `ERenderTargetLoadAction::ELoad` 读取, 行为正确
- 但 Subpass 1 的 `RenderTargets.DepthStencil.SetDepthStencilAccess` 默认是 `DepthRead_StencilRead` (由 RenderTranslucency 决定), 用户的 `MobileAfterTranslucencyPass` 也设了 `DepthRead_StencilRead`, **但没有重新设置 `DepthStencilState`**, Subpass 内部多 pass 共享 depth state, OK
- `MobileAfterTranslucencyPass` 的 depth-stencil access 应当与 `RenderTranslucency` 一致 (DepthRead_StencilRead), 用户的设置正确

**隐藏问题**: 用户的 `RenderMobileAfterTranslucencyPass` 中**没有**调用 `RHICmdList.SetViewport()`, 也不会有问题, 因为是同一个 Subpass 内的连续绘制, Viewport 不变。

### 2.6 ShouldDraw 检查会过滤 Masked 材质 (但实际上 OK)

**位置**: `Engine/Source/Runtime/Renderer/Private/MobileBasePass.cpp:828-848` `FMobileBasePassMeshProcessor::ShouldDraw`

**逻辑**:
- `bTranslucentBasePass=false` (用户的两个新 processor 都满足) → 走 `return !bIsTranslucent`
- Masked 材质 `IsMaskedBlendMode=true`, `IsTranslucentBlendMode=false` → `bIsTranslucent=false` → `!bIsTranslucent=true` → 会被绘制

**结论**: 标记物体的 Masked 材质能正常进入 AfterTranslucencyDepthPass 和 AfterTranslucencyPass。**功能正常, 无问题。**

### 2.7 AfterTranslucencyPass 在 Multi-View / Stereo 下的行为

**位置**: `Engine/Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp`

**问题**: VR 移动端通常启用 MultiView (`bIsMobileMultiViewEnabled=true`)。`RenderTargets.MultiViewCount` 设置后, 一次 draw call 写两个 view。

**潜在问题**:
- 用户的 `RenderMobileAfterTranslucencyDepthPass` 和 `RenderMobileAfterTranslucencyPass` 在 Subpass 0 / Subpass 1 内, MultiView 设置沿用 Subpass 启动时的配置
- **没有显式调用 SetViewport** 也没有问题, 因为 MultiView 是由 RenderTargets 上的 MultiViewCount 决定, 不会因为单个 DrawCall 而失效

**结论**: Multi-View 行为正常, **无问题**, 但建议测试覆盖 `vr.MobileMultiView=0/1` 两种场景。

---

## 3. 遗漏的修改 (功能完整所必需)

### 3.1 多渲染路径 (Multi-Pass / Deferred) 未被修改

**现状**:
- `Engine/Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp` 中 `RenderMobileBasePass` 至少在 5 处被调用:
  - line 896 (`RenderCustomRenderPassBasePass`)
  - line 1609 (`RenderForwardSinglePass`) ← 方案已修改
  - line 1682 (`RenderForwardMultiPass`)
  - line 1968 (`RenderDeferredSinglePass`)
  - line 2011 (`RenderDeferredMultiPass`)
- `RenderTranslucency` 在 4 处被调用 (1623, 1735, 1985, 2068)

**VR 移动端 Forward 路径**:
- 默认走 `RenderForwardSinglePass` (line 1573 条件分支)
- 若 `vr.MobileMultiView=0` (单眼) 且 MSAA 启用, 可能走 `RenderForwardMultiPass`
- 若启用 `MobileDeferredShading`, 走 `RenderDeferred` 路径 (虽然 VR Forward 不太可能走, 但需要确认项目设置)

**建议**:
- 至少在 `RenderForwardMultiPass` 中也加入 `RenderMobileAfterTranslucencyDepthPass` 和 `RenderMobileAfterTranslucencyPass` 调用
- 若项目可能启用 Deferred 路径 (虽然对 VR 不推荐), 同样需要修改

### 3.2 EFlags 缺少 CanReceiveCSM 在 AfterTranslucencyPass (光照)

**位置**: `Engine/Source/Runtime/Renderer/Private/MobileBasePass.cpp` 中 `CreateMobileAfterTranslucencyPassProcessor`

**问题**: 标记物体是有光照的 Opaque 物体, 走 AfterTranslucencyPass 时也应当接收 CSM 阴影。用户的 `EFlags` 只设了 `CanUseDepthStencil`, 没有 `CanReceiveCSM`。

**后果**:
- 若 `MobileBasePassAlwaysUsesCSM(GShaderPlatformForFeatureLevel[FeatureLevel])=true` (该平台默认 BasePass 走 CSM), 标记物体在 AfterTranslucencyPass 不会接收 CSM 阴影, 视觉与在 BasePass 渲染时**不一致**

**建议修改**:
```c++
const FMobileBasePassMeshProcessor::EFlags Flags = FMobileBasePassMeshProcessor::EFlags::CanUseDepthStencil
    | (MobileBasePassAlwaysUsesCSM(GShaderPlatformForFeatureLevel[FeatureLevel]) 
        ? FMobileBasePassMeshProcessor::EFlags::CanReceiveCSM 
        : FMobileBasePassMeshProcessor::EFlags::None);
```

### 3.3 MobileAfterTranslucencyDepthPass 应使用 CanReceiveCSM=false

**位置**: `Engine/Source/Runtime/Renderer/Private/MobileBasePass.cpp` 中 `CreateMobileAfterTranslucencyDepthPassProcessor`

**问题**: 用户已经设了 `CanUseDepthStencil` (正确, DepthPass 不需要 CSM), **没有 CanReceiveCSM, 这是对的**。但若想保持和 BasePass 行为完全一致, 应考虑 `bIsMasked` 材质的深度处理。当前缺省的 `Flags = CanUseDepthStencil` 已 OK。

**结论**: 无需修改, **OK**。

### 3.4 bRenderAfterTranslucency 未在 SetupPrecachePSOParams 中处理

**位置**: `Engine/Source/Runtime/Engine/Private/Components/PrimitiveComponent.cpp:4620`

**现状**:
```c++
void UPrimitiveComponent::SetupPrecachePSOParams(FPSOPrecacheParams& Params)
{
    Params.bRenderInMainPass = bRenderInMainPass;
    Params.bRenderInDepthPass = bRenderInDepthPass;
    ...
}
```

**问题**: 若未将 `bRenderAfterTranslucency` 传入 PSO Precache, 则标记物体的 PSO 不会被预编译, 运行时首次绘制会出现 PSO 编译卡顿 (移动端尤为明显, 容易卡顿 50-200ms)。

**建议修改**:
```c++
Params.bRenderInMainPass = bRenderInMainPass;  // 这里需传 bRenderAfterTranslucency
```
或者更准确地在 `FMobileBasePassMeshProcessor::CollectPSOInitializers` 中处理:
- 用户的 plan 7 在 `CollectPSOInitializers` 开头直接 return, **完全跳过了 PSO Precache**:
```c++
void FMobileBasePassMeshProcessor::CollectPSOInitializers(...)
{
    if (MeshPassType == EMeshPass::MobileAfterTranslucencyPass || 
        MeshPassType == EMeshPass::MobileAfterTranslucencyDepthPass)
    {
        return;  // <-- 完全跳过 PSO 预编译
    }
    ...
}
```

**潜在后果**: 标记物体的 PSO 不会被预编译, 运行时首帧卡顿。

**建议**: 至少让 `MobileAfterTranslucencyPass` 走 Precache 路径, 但应**仅**针对 `bRenderAfterTranslucency=true` 的物体做 Precache。可修改为:
```c++
if (MeshPassType == EMeshPass::MobileAfterTranslucencyPass || 
    MeshPassType == EMeshPass::MobileAfterTranslucencyDepthPass)
{
    if (!PreCacheParams.bRenderAfterTranslucency)  // 假设 Params 也有这个标志
    {
        return;
    }
}
```

否则, 可保持 return, 但用户需要接受运行时卡顿。

### 3.5 `FStaticPrimitiveDrawCommand` 缓存是否被影响

**位置**: `Engine/Source/Runtime/Renderer/Private/MobileBasePass.cpp` 附近, `REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR` 宏

**分析**: 用户使用了 `EMeshPassFlags::CachedMeshCommands | EMeshPassFlags::MainView`, 这与 `MobileBasePass` 一致。MDC (Mesh Draw Commands) 会被缓存。**OK, 无问题。**

### 3.6 `MeshPassProcessor.h` 中 `NumBits` 容量检查

**位置**: `Engine/Source/Runtime/Renderer/Public/MeshPassProcessor.h:77,80`

**现状**:
```c++
NumBits = 6,  // 即 2^6 = 64 个 pass
static_assert(EMeshPass::Num <= (1 << EMeshPass::NumBits), ...);  // 64
```

**计算**: 新增 2 个后, Num = 34 (non-editor), 仍 <= 64。**OK, 无问题。**

### 3.7 PrimitiveComponentDesc.h 中缺少 bRenderAfterTranslucency 字段声明位置 (在 1)

**位置**: `Engine/Source/Runtime/Engine/Public/PrimitiveSceneProxyDesc.h:93` 附近

**方案修改**:
```c++
uint32 bRenderAfterTranslucency : 1;
```

**校验**: PrimitiveSceneProxy.h line 1200 附近也加了, 同步。PrimitiveSceneProxy.cpp InitializeFrom 和构造函数初始化列表都加了。**OK, 完整。**

### 3.8 缺失 View.ParallelMeshDrawCommandPasses 初始化确认

**位置**: `Engine/Source/Runtime/Renderer/Public/SceneRendering.h` 附近的 `FSceneView` 中 `ParallelMeshDrawCommandPasses`

**分析**: 该数组是按 `EMeshPass::Num` 大小静态分配的。新增 2 个枚举, 数组大小自动扩展。`REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR` 宏会调用 `CreateMobileAfterTranslucencyPassProcessor` 来填充对应 slot。**OK, 无需额外修改。**

### 3.9 缺少对 HiStencil、SceneDepthAux、Velocity 等 texture 的处理

**分析**: AfterTranslucencyDepthPass 只写深度, 不需要 SceneColor。AfterTranslucencyPass 需要 SceneColor 写入, 沿用 Subpass 1 配置即可。**OK, 无需修改。**

### 3.10 在 Editor 下的处理

**位置**: `RenderMobileBasePass` 还会调用 `RenderMobileEditorPrimitives` (line 487-490)

**问题**: 编辑器基元 (BSP 等) 不在标记物体范围, 不会被影响。**OK, 无需修改。**

### 3.11 阴影投射 (CastShadow) 是否受影响

**分析**: 标记物体的阴影投射 (CSMShadowDepth) 走独立 Pass, 不会被用户修改影响。**OK, 阴影功能正常。**

### 3.12 阴影接收 (bCastShadow) 在 AfterTranslucencyPass 中需检查

**分析**: 同 3.2, 若 EFlags 缺少 `CanReceiveCSM`, 标记物体在 AfterTranslucencyPass 不会接收 CSM 阴影。这与方案 3.2 重复, 已列出。

### 3.13 反射捕获 (Reflection Capture) 处理

**分析**: 标记物体在 AfterTranslucencyPass 中仍会使用 `Scene->UniformBuffers.MobileReflectionCaptureUniformBuffer` (从 Subpass 启动时的 View 上下文继承)。**OK, 无需修改。**

### 3.14 Instance Culling 的正确性

**位置**: `Engine/Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp:1433` 附近

**分析**: 用户的 BuildInstanceCullingDrawParams 修改正确, 在 GPUScene 启用时调用了 `BuildRenderingCommands`。**OK。**

但需注意: 若 `Scene->GPUScene.IsEnabled()=false` (老旧 Android 设备, 不太可能但理论上存在), 新 Pass 的 InstanceCulling 不会被构建, 渲染时没有 DrawCommand。

**建议**: 在非 GPUScene 路径下, 也确保新 Pass 走 `DrawDynamicMeshPass` 路径 (legacy path)。这需要修改 `ComputeDynamicMeshRelevance` (已包含) 和验证 mobile non-GPUScene 路径。当前 UE5.4 中 mobile 一般默认开 GPUScene, 但**建议测试 non-GPUScene 设备**。

### 3.15 缺少 SceneRendering.h 中 Render 函数声明 (line 2695 附近)

**位置**: `Engine/Source/Runtime/Renderer/Private/SceneRendering.h:2695`

**方案修改**:
```c++
void RenderMobileBasePass(...);
void RenderMobileAfterTranslucencyPass(...);
void RenderMobileAfterTranslucencyDepthPass(...);
```

**校验**: line 2695 已经有 `RenderMobileBasePass`, 用户在附近加新声明, **OK**。

### 3.16 缺少 FInstanceCullingDrawParams 成员变量声明

**位置**: `Engine/Source/Runtime/Renderer/Private/SceneRendering.h:2796` 附近

**方案修改**:
```c++
FInstanceCullingDrawParams AfterTranslucencyInstanceCullingDrawParams;
FInstanceCullingDrawParams AfterTranslucencyDepthInstanceCullingDrawParams;
```

**校验**: line 2793-2796 已有 `DepthPassInstanceCullingDrawParams` 等, 用户在附近加, **OK**。

### 3.17 缺少 RenderCore.h / RenderCore.cpp 中的 STAT 声明

**位置**: `Engine/Source/Runtime/RenderCore/Public/RenderCore.h:44` 附近, `Private/RenderCore.cpp:65` 附近

**校验**: 已有 `STAT_BasePassDrawTime`, 用户加 `STAT_AfterTranslucencyDrawTime` 和 `STAT_AfterTranslucencyDepthDrawTime`, **OK**。

### 3.18 缺少 BasePassRendering.h 中的 GPU STAT 声明

**位置**: `Engine/Source/Runtime/Renderer/Private/BasePassRendering.h:144` 附近

**校验**: 已有 `DECLARE_GPU_DRAWCALL_STAT_EXTERN(Basepass)`, 用户加 `AfterTranslucency` 和 `AfterTranslucencyDepth`, **OK**。

### 3.19 PrimitiveViewRelevance.h 中的 bRenderAfterTranslucency 字段位置

**位置**: `Engine/Source/Runtime/Engine/Public/PrimitiveViewRelevance.h:54` 附近

**校验**: 已有 `bRenderInMainPass`, `bRenderInDepthPass`, 用户加 `bRenderAfterTranslucency`, 位置正确, **OK**。

注意: 在 `:90-104` 构造函数中, 用户加 `bRenderAfterTranslucency = false` 是冗余的 (因为 memzero 已经清零), 但为了明确性, 加上是 OK 的。

### 3.20 缺失的 HeterogeneousVolume / TextRenderComponent 等其他 Proxy 的修改

**位置**: `Engine/Source/Runtime/Engine/Private/Components/HeterogeneousVolumeComponent.cpp:171` 和 `TextRenderComponent.cpp:857`

**分析**: 这些组件也设置了 `Result.bRenderInMainPass = ShouldRenderInMainPass();` 但用户的方案只修改了 `StaticMeshRender.cpp` 和 `SkeletalMesh.cpp`。

**影响**: 用户明确说"我只需要让 Mesh 和 Skeletal Mesh 生效即可", 所以这些组件**不需要**支持 `bRenderAfterTranslucency`。**OK, 无需修改。**

但若项目中有其他类型的 PrimitiveComponent (如 ProceduralMeshComponent) 也使用 bRenderInMainPass 模式, 需要相应修改, 否则 `bRenderAfterTranslucency` 标志在这些组件上不会生效。

---

## 4. 总结

### 必须修复 (功能性问题)
1. **static_assert 数值描述** (1.1) — 方案描述错误, 实际值是 32 不是 34
2. **AddMeshBatch 的 bRenderInMainPass 提前 return** (1.2) — 阻止 bRenderInMainPass=false 标记物体渲染
3. **SceneVisibility 静态网格的外层 if** (1.3) — 阻止 bRenderInMainPass=false 标记物体进入

### 强烈建议修复 (影响功能或性能)
4. **Velocity / CustomDepth / DepthPass 仍然进入** (2.1, 2.2) — 标记物体可能出现在错误 Pass
5. **MobileBasePassCSM 在动态网格中未排除** (2.3) — 逻辑不一致
6. **PSO Precache 被跳过** (3.4) — 运行时首帧卡顿
7. **AfterTranslucencyPass 缺 CanReceiveCSM** (3.2) — 阴影丢失
8. **多渲染路径 (Multi-Pass / Deferred) 未修改** (3.1) — 非默认路径下功能失效

### 建议优化 (性能相关)
9. **MobileAfterTranslucencyDepthPass 使用 Opaque Shader** (2.4) — 浪费像素着色器
10. **non-GPUScene 路径未测试** (3.14) — 兼容性问题

### 不影响功能 (但应记录)
- 变量命名 `bAfterTranslucencyBasePass` 不够清晰
- 默认参数 `ETranslucencyPass::TPT_MAX` 的隐式使用, 应显式声明

---

## 5. 推荐补充修改清单

按优先级排序:

| 优先级 | 修改内容 | 位置 | 原因 |
|--------|----------|------|------|
| P0 | 修正 AddMeshBatch 首道关卡 | MobileBasePass.cpp:867 | 阻止 bRenderInMainPass=false 物体渲染 |
| P0 | 修正 SceneVisibility 静态网格外层 if | SceneVisibility.cpp:1556 | 同上 |
| P0 | 修正 static_assert 描述 | MeshPassProcessor.h:128-130 | 编译错误 |
| P1 | Add CanReceiveCSM 到 AfterTranslucencyPass | MobileBasePass.cpp (CreateMobileAfterTranslucencyPassProcessor) | 阴影丢失 |
| P1 | 同步修改 RenderForwardMultiPass | MobileShadingRenderer.cpp:1682 | 多 Pass 路径失效 |
| P1 | 修改 ComputeDynamicMeshRelevance 的 MobileBasePassCSM 处理 | SceneVisibility.cpp:2228 | 逻辑一致 |
| P1 | 让 PSO Precache 处理 AfterTranslucency Pass | MobileBasePass.cpp:1056 (CollectPSOInitializers) | 避免运行时卡顿 |
| P2 | 决定是否排除 Velocity / CustomDepth / DepthPass | SceneVisibility.cpp:1506-1613 | 视觉正确性 |
| P2 | 测试 non-GPUScene 路径 | MobileShadingRenderer.cpp | 兼容性 |
| P3 | 优化 AfterTranslucencyDepthPass 走 Z-Pass 路径 | 新增专用 Processor | 性能 |

---

**报告完成**。
