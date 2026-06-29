# FDepthPassMeshProcessor 复用分析与方案优化

## 一、结论先行

**复用 `FDepthPassMeshProcessor` 完全可行，且相比当前方案用 `FMobileBasePassMeshProcessor` 做深度 pass 更合适。**

| 维度 | 当前方案：`FMobileBasePassMeshProcessor` 做深度 | 优化方案：`FDepthPassMeshProcessor` 做深度 |
|------|--------------------------------------|--------------------------------------|
| 着色器开销 | 走 BasePass 完整像素着色器路径（CSM、Lighting 全部要算），只通过 `CW_NONE` 关掉颜色写 | 走深度专用简化路径 (`Process<bPositionOnly>`)，像素着色器极简（甚至可以走 position-only path） |
| 移动端 stencil 布局 | 没设置，stencil 写为 0 | `SetMobileDepthPassRenderState` 自动设置移动端 stencil（Decal mask、Shading model mask、ContactShadow mask） |
| 移动端 cull 优化 | 无 | 自动应用 `EarlyZPassMode`/`bEarlyZPassMovable`/`GMinScreenRadiusForDepthPrepass` 等 cull 规则 |
| 改动量 | — | 增量约 30-50 行：1 个新 factory、1 个新 ctor 参数、AddMeshBatch 中 5-10 行分支 |
| 风险 | 低（都是新代码） | 极低（FDepthPassMeshProcessor 已经被 MobileDepthPass 用过，模式现成） |

**强烈建议切换到 `FDepthPassMeshProcessor` 做深度 pass。** `FMobileBasePassMeshProcessor` 留作颜色 pass（`MobileAfterTranslucencyPass`）使用——这才是它该干的事。

---

## 二、为什么 `FDepthPassMeshProcessor` 是正确选择

`FDepthPassMeshProcessor` 类的设计就是参数化、按 MeshPassType 派生的（见 `DepthRendering.h:176`）：

```cpp
class FDepthPassMeshProcessor : public FSceneRenderingAllocatorObject<FDepthPassMeshProcessor>, public FMeshPassProcessor
{
    // 构造时已经接受 InMeshPassType + 各种 variant 标志
    FDepthPassMeshProcessor(
        EMeshPass::Type InMeshPassType,
        ...
        const bool InbRespectUseAsOccluderFlag,
        const EDepthDrawingMode InEarlyZPassMode,
        const bool InbEarlyZPassMovable,
        const bool bDitheredLODFadingOutMaskPass,
        ...
        const bool bShadowProjection = false,        // 已有 variant 标志
        const bool bSecondStageDepthPass = false);  // 已有 variant 标志
```

UE 自家已经用这种模式挂了 4 个 MeshPass：
- `EMeshPass::DepthPass`（移动端+延迟）
- `EMeshPass::SecondStageDepthPass`（仅延迟端，移动端被注释：`//Secondary depth pass is not implemented on mobile so far`）
- `EMeshPass::DitheredLODFadingOutMaskPass`（仅延迟端）
- 以及 `bShadowProjection=true` 的阴影深度变体

新增第 5 个 `EMeshPass::MobileAfterTranslucencyDepthPass` 走的是完全相同模式，照葫芦画瓢即可。

---

## 三、具体改动方案（`FDepthPassMeshProcessor` 复用版）

### 3.1 `Engine/Source/Runtime/Renderer/Public/MeshPassProcessor.h`

**当前 Plan 已包含的内容**（已正确）：添加 `EMeshPass::MobileAfterTranslucencyPass` 和 `EMeshPass::MobileAfterTranslucencyDepthPass` 到枚举、`GetMeshPassName` switch case、`static_assert(EMeshPass::Num == 34 + 4, ...)`。

### 3.2 `Engine/Source/Runtime/Renderer/Private/DepthRendering.cpp`（**新增**）

#### 3.2.1 构造函数增加一个 variant 标志

在第 1217-1218 行附近，构造函数加一个参数 `bInAfterTranslucencyDepthPass`：

```cpp
FDepthPassMeshProcessor(
    EMeshPass::Type InMeshPassType, 
    const FScene* Scene,
    ERHIFeatureLevel::Type FeatureLevel,
    const FSceneView* InViewIfDynamicMeshCommand,
    const FMeshPassProcessorRenderState& InPassDrawRenderState,
    const bool InbRespectUseAsOccluderFlag,
    const EDepthDrawingMode InEarlyZPassMode,
    const bool InbEarlyZPassMovable,
    const bool bDitheredLODFadingOutMaskPass,
    FMeshPassDrawListContext* InDrawListContext,
    const bool bInShadowProjection = false,
    const bool bInSecondStageDepthPass = false,
    const bool bInAfterTranslucencyDepthPass = false);  // RenderAfterTranslucency Added
```

类私有成员同步加一个：

```cpp
const bool bAfterTranslucencyDepthPass;  // RenderAfterTranslucency Added
```

构造函数初始化列表第 1225 行后加：

```cpp
, bAfterTranslucencyDepthPass(bInAfterTranslucencyDepthPass)
```

#### 3.2.2 `AddMeshBatch` 中做区分（核心问题答复）

在第 1021 行 `FDepthPassMeshProcessor::AddMeshBatch` 开头加入分流逻辑：

```cpp
void FDepthPassMeshProcessor::AddMeshBatch(const FMeshBatch& RESTRICT MeshBatch, uint64 BatchElementMask, const FPrimitiveSceneProxy* RESTRICT PrimitiveSceneProxy, int32 StaticMeshId)
{
    // RenderAfterTranslucency Added: 这个 pass 只渲染标记过的物体
    if (bAfterTranslucencyDepthPass)
    {
        if (!PrimitiveSceneProxy || !PrimitiveSceneProxy->ShouldRenderAfterTranslucency())
        {
            return;
        }
        // 跳过下面的 occluder / velocity 过滤逻辑（这个 pass 不需要）
        // 直接走下面的材质遍历
        if (MeshBatch.bUseForDepthPass)
        {
            const FMaterialRenderProxy* MaterialRenderProxy = MeshBatch.MaterialRenderProxy;
            while (MaterialRenderProxy)
            {
                const FMaterial* Material = MaterialRenderProxy->GetMaterialNoFallback(FeatureLevel);
                if (Material && Material->GetRenderingThreadShaderMap())
                {
                    if (TryAddMeshBatch(MeshBatch, BatchElementMask, PrimitiveSceneProxy, StaticMeshId, *MaterialRenderProxy, *Material))
                    {
                        break;
                    }
                }
                MaterialRenderProxy = MaterialRenderProxy->GetFallback(FeatureLevel);
            }
        }
        return;
    }

    bool bDraw = MeshBatch.bUseForDepthPass;
    // ... 原有逻辑保持不变
```

**关键点**：
- 新 pass 的 `bRespectUseAsOccluderFlag` 在 factory 里传 `false`，occluder 检查天然被跳过（看原 line 1026 的条件是 `bDraw && bRespectUseAsOccluderFlag && !MeshBatch.bUseAsOccluder && EarlyZPassMode < DDM_AllOpaque`，把第一个标志设为 false 就走不进去了）。但为了更干净（避免 `EarlyZPassMode < DDM_AllOpaque` 这条路径带来意外行为），更稳妥的做法就是上面那样早 return 单独走一遍。
- 不需要 `TryAddMeshBatch` 内部做任何改动——它已经处理了 `PrimitiveSceneProxy->ShouldRenderInDepthPass()` 和材质 domain 过滤，对普通 Mesh 足够。

#### 3.2.3 添加新 Factory 和注册

在第 1281 行 `CreateDitheredLODFadingOutMaskPassProcessor` 之后添加：

```cpp
// RenderAfterTranslucency Added
FMeshPassProcessor* CreateMobileAfterTranslucencyDepthPassProcessor(ERHIFeatureLevel::Type FeatureLevel, const FScene* Scene, const FSceneView* InViewIfDynamicMeshCommand, FMeshPassDrawListContext* InDrawListContext)
{
    EDepthDrawingMode EarlyZPassMode;
    bool bEarlyZPassMovable;
    FScene::GetEarlyZPassMode(FeatureLevel, EarlyZPassMode, bEarlyZPassMovable);

    FMeshPassProcessorRenderState DepthPassState;
    SetupDepthPassState(DepthPassState);

    return new FDepthPassMeshProcessor(
        EMeshPass::MobileAfterTranslucencyDepthPass, Scene, FeatureLevel,
        InViewIfDynamicMeshCommand, DepthPassState,
        /*bRespectUseAsOccluderFlag=*/false, EarlyZPassMode, bEarlyZPassMovable,
        /*bDitheredLODFadingOutMaskPass=*/false, InDrawListContext,
        /*bShadowProjection=*/false, /*bSecondStageDepthPass=*/false,
        /*bAfterTranslucencyDepthPass=*/true);  // 关键
}

REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR(MobileAfterTranslucencyDepthPass,
    CreateMobileAfterTranslucencyDepthPassProcessor, EShadingPath::Mobile,
    EMeshPass::MobileAfterTranslucencyDepthPass,
    EMeshPassFlags::CachedMeshCommands | EMeshPassFlags::MainView);
```

> `SetupDepthPassState` 已经会把深度状态设成 `DepthWrite_StencilWrite`、`TStaticDepthStencilState<true, CF_DepthNearOrEqual>`、blend = `CW_NONE`，正是我们想要的。所以这个 factory 实际上可以很简洁，不用像当前 Plan 那样手写 `PassDrawRenderState` 三件套。

### 3.3 `Engine/Source/Runtime/Renderer/Private/MobileBasePass.cpp`

**当前 Plan 已包含的 `CreateMobileAfterTranslucencyPassProcessor`（颜色 pass）保持不变。** 但需要把 `MobileAfterTranslucencyDepthPass` 的注册从 MobileBasePass.cpp **移除**（depth pass 不再在这里注册，改在 DepthRendering.cpp 注册）。

需要修改的部分：
- **删除**当前 Plan 中 `REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR(MobileAfterTranslucencyDepthPass, ...)` 这一行（已经移到 DepthRendering.cpp）
- **删除**当前 Plan 中 `CreateMobileAfterTranslucencyDepthPassProcessor` factory（已经移到 DepthRendering.cpp）
- **保留**`CreateMobileAfterTranslucencyPassProcessor` 和它的注册（颜色 pass 仍然需要用 `FMobileBasePassMeshProcessor`）

### 3.4 `Engine/Source/Runtime/Renderer/Private/MobileBasePass.cpp` 的 `AddMeshBatch` 修改

**当前 Plan 的修改需要相应调整**——`FMobileBasePassMeshProcessor::AddMeshBatch` 不再需要处理 `MobileAfterTranslucencyDepthPass`，只需要处理 `MobileAfterTranslucencyPass`（颜色 pass）：

```cpp
void FMobileBasePassMeshProcessor::AddMeshBatch(const FMeshBatch& RESTRICT MeshBatch, uint64 BatchElementMask, const FPrimitiveSceneProxy* RESTRICT PrimitiveSceneProxy, int32 StaticMeshId)
{
    if (!MeshBatch.bUseForMaterial ||
        (Flags & FMobileBasePassMeshProcessor::EFlags::DoNotCache) == FMobileBasePassMeshProcessor::EFlags::DoNotCache ||
        (PrimitiveSceneProxy && !PrimitiveSceneProxy->ShouldRenderInMainPass()))
    {
        return;
    }
    // RenderAfterTranslucency Added: MobileAfterTranslucencyPass（颜色 pass）只渲染标记过的物体
    // 注意 MobileAfterTranslucencyDepthPass 不走这里，由 FDepthPassMeshProcessor 处理
    const bool bIsAfterTranslucencyPass = (MeshPassType == EMeshPass::MobileAfterTranslucencyPass);
    if (bIsAfterTranslucencyPass && (!PrimitiveSceneProxy || !PrimitiveSceneProxy->ShouldRenderAfterTranslucency()))
    {
        return;
    }
    if (!bIsAfterTranslucencyPass && PrimitiveSceneProxy && PrimitiveSceneProxy->ShouldRenderAfterTranslucency())
    {
        // 标记过的物体不进标准 BasePass
        return;
    }
    // ... 其余逻辑保持不变
```

### 3.5 其余部分（与当前 Plan 相同，无需调整）

- `PrimitiveComponent.h/cpp`：`bRenderAfterTranslucency` 字段、Setter、默认初始化
- `PrimitiveSceneProxy.h/cpp`、`PrimitiveSceneProxyDesc.h`：`bRenderAfterTranslucency` 字段
- `PrimitiveViewRelevance.h`：`bRenderAfterTranslucency` 字段、构造函数初始化
- `StaticMeshRender.cpp:2055`、`SkeletalMesh.cpp:7107`：`GetViewRelevance` 设置 `Result.bRenderAfterTranslucency`
- `SceneVisibility.cpp:1564` 静态 mesh 分流 + `SceneVisibility.cpp:2211` 动态 mesh 分流
- `MobileShadingRenderer.cpp`：调用 `RenderMobileAfterTranslucencyDepthPass` 和 `RenderMobileAfterTranslucencyPass`
- 各种 stat 声明

---

## 四、当前 Plan 中需要修正/补充的部分

### 4.1 严重：动态 mesh 路径中 `MobileBasePassCSM` 的遗漏

**当前 Plan 的 SceneVisibility.cpp 静态 mesh 分流（line 1564 附近）只加了两个新 pass，但没有处理 `EMeshPass::MobileBasePassCSM` 的情况。** 看 `SceneRendering.cpp:4209`：

```cpp
if (ShadingPath == EShadingPath::Mobile && (PassType == EMeshPass::BasePass || PassType == EMeshPass::MobileBasePassCSM))
{
    continue;  // handled separately in MobileShadingRenderer
}
```

移动端 base pass 实际上注册了**两个** MeshPass：`EMeshPass::BasePass` 和 `EMeshPass::MobileBasePassCSM`（CSM 着色时用）。你的分流逻辑应该同时处理两者：

```cpp
if (ShadingPath == EShadingPath::Mobile)
{
    if (!StaticMeshRelevance.bUseSkyMaterial)
    {
        if (ViewRelevance.bRenderAfterTranslucency)
        {
            DrawCommandPacket.AddCommandsForMesh(..., EMeshPass::MobileAfterTranslucencyDepthPass);
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
    }
    // ...
}
```

**这意味着标记过的物体在 CSM 阴影下也会正确地走 AfterTranslucency 路径。**

### 4.2 严重：动态 mesh 路径的 `ComputeDynamicMeshRelevance` 同样需要处理 `MobileBasePassCSM`

`SceneVisibility.cpp:2211` 附近 `ComputeDynamicMeshRelevance` 的当前 Plan 修改：

```cpp
if (ViewRelevance.bRenderInMainPass || ViewRelevance.bRenderCustomDepth)
{
    if (ShadingPath == EShadingPath::Mobile && ViewRelevance.bRenderAfterTranslucency)
    {
        PassMask.Set(EMeshPass::MobileAfterTranslucencyDepthPass);
        PassMask.Set(EMeshPass::MobileAfterTranslucencyPass);
    }
    else
    {
        PassMask.Set(EMeshPass::BasePass);
        if (ShadingPath == EShadingPath::Mobile)
        {
            PassMask.Set(EMeshPass::MobileBasePassCSM);  // ← 原本的代码，不要漏
        }
    }
}
```

**务必保留 `MobileBasePassCSM` 在非 after-translucency 分支的设置。**

### 4.3 中等：`FMobileSceneRenderer::SetupMobileBasePass` 是否需要改

`FMobileSceneRenderer::SetupMobileBasePass`（MobileShadingRenderer.cpp ~line 380）会把 BasePass + MobileBasePassCSM 两个 mesh processor 一起跑过 `View.DynamicMeshElements`，用的是 `View.DynamicMeshElementsPassRelevance` 这个 mask 数组。只要 `ComputeDynamicMeshRelevance` 给对应元素设置了正确的 mask bit，dynamic mesh 元素会被自动派发到正确的 processor。**所以你不需要改 SetupMobileBasePass，但前提是 mask bit 设置正确（见 4.2）。**

但是：新 pass（`MobileAfterTranslucencyPass`、`MobileAfterTranslucencyDepthPass`）是独立的 `FParallelMeshDrawCommandPass` 实例，它们会走 `FSceneRenderer::SetupMeshPass` 的通用循环（不是 `SetupMobileBasePass`）。这个通用循环位于 `SceneRendering.cpp:4209`，已经会处理所有非 BasePass/MobileBasePassCSM 的 pass，所以**新 pass 会自动通过通用循环设置。** 不需要手动改。

### 4.4 中等：`MobileShadingRenderer.cpp` 现有 BasePass dispatch 时机的位置

`RenderMobileBasePass` 在 `RenderForwardSinglePass` 的开头被调用，**紧接着是 SkyPass、Editor primitives**。你把 `RenderMobileAfterTranslucencyDepthPass` 插在 `RenderMobileBasePass` 之后是对的，但要确认它放在 Editor primitives 之后还是之前——`RenderEditorPrimitives` 也写深度，所以新 pass 应该放在它之后：

```cpp
RenderMobileBasePass(RHICmdList, View, &PassParameters->InstanceCullingDrawParams);
// ... SkyPass 和 editor primitives 也写深度，所以新 pass 必须在它们之后
RenderMobileAfterTranslucencyDepthPass(RHICmdList, View, &AfterTranslucencyDepthInstanceCullingDrawParams);
```

**把 `RenderMobileAfterTranslucencyDepthPass` 放在 SkyPass + editor primitives 之后、Translucency 之前。**

### 4.5 轻微：`BuildInstanceCullingDrawParams` 只在 `GPUScene.IsEnabled()` 时调用

UE5.4 移动端强制走 GPUScene 路径（已确认没有非 GPUScene 移动端代码），所以 Plan 中 `if (Scene->GPUScene.IsEnabled())` 的位置是对的。**但需要确认你的目标工程在 Android 上 `r.Mobile.GPUScene.Enable=1`（默认就是 1）。**

### 4.6 轻微：`CollectPSOInitializers` 的早返回会影响 PSO 预缓存

`MobileBasePass.cpp:1056` 的 `CollectPSOInitializers` 在新 pass 上 early return 是必要的（因为新 pass 不应该走 `r.PSOPrecache.TranslucencyAllPass` 那条逻辑），但是这意味着**新 pass 的 PSO 不会在 Precache 阶段被预编译**——首次出现会有 shader compile 卡顿。

如果在意这个卡顿，需要：
- 单独为新 pass 写一个 `CollectPSOInitializers` 实现（在 `DepthRendering.cpp` 中的 `FDepthPassMeshProcessor::CollectPSOInitializers` 自然地会处理 depth pass，**但颜色 pass 走 `FMobileBasePassMeshProcessor`，没有 precache 路径**）
- 或者：临时把 `r.PSOPrecache.RequestPSOs=1` 加到项目 DefaultEngine.ini，让 runtime 自动 precache

**对你的 VR 游戏（首帧卡顿敏感）建议做 precache，或者接受首次的卡顿。**

### 4.7 轻微：stencil 在颜色 pass 中不会被用作测试

`MobileAfterTranslucencyPass` 的 `FExclusiveDepthStencil::DepthRead_StencilRead` 是 OK 的——只要深度 pass 也写 stencil 同样的位。`SetupDepthPassState` 默认的 stencil access 是 `DepthWrite_StencilWrite`，所以一致。**但要确认你的 marked object 的材质里**不依赖 stencil 标记位（比如自定义 stencil shadow、CustomDepth mask 等）——这些 stencil 位可能被移动端 base pass 写到 depth 缓冲里，再被新 pass 覆盖，从而影响后续的 CustomDepth、SSAO 等。

如果 marked object 用了特殊 stencil 标记（比如 `bUseAsOccluder` 内部用 stencil 表达 AO 遮挡），那 `MobileAfterTranslucencyDepthPass` 用 `SetupDepthPassState`（标准移动端 stencil 布局）可能会改写这些位。

**建议：把 `SetupDepthPassState` 替换为不写 stencil 的状态。** 或者在 `CreateMobileAfterTranslucencyDepthPassProcessor` 里手动设置：

```cpp
FMeshPassProcessorRenderState DepthPassState;
DepthPassState.SetBlendState(TStaticBlendState<CW_NONE>::GetRHI());
DepthPassState.SetDepthStencilAccess(FExclusiveDepthStencil::DepthWrite_StencilNop);  // 不写 stencil
DepthPassState.SetDepthStencilState(TStaticDepthStencilState<true, CF_DepthNearOrEqual>::GetRHI());
```

（颜色 pass 相应改成 `DepthRead_StencilNop`。）

### 4.8 缺失：移动端 MobileAfterTranslucencyDepthPass 在 RenderFullDepthPrepass / RenderMaskedPrePass 中的处理

如果你的项目里 `r.Mobile.EarlyZPass=1`（启用 mobile PreZ），现有 PreZ 会先写一次深度，然后 base pass 在比较时使用。这意味着：
- 你的 marked object 在 PreZ 时已经写过一次深度（因为它对 PreZ 是可见的）
- 然后 `MobileAfterTranslucencyDepthPass` 会再写一次（覆盖，OK）
- 然后 `MobileAfterTranslucencyPass` 测试的是覆盖后的深度

**结果正确，但有一次冗余写入。** 如果 VR 项目移动端本来就不开 PreZ（按你说的"移动端不会使用PreZPass"），这个不是问题。**只需确认 `r.Mobile.EarlyZPass=0` 或在 DefaultEngine.ini 显式关闭。**

### 4.9 缺失：`EMeshPassFlags::MainView` 标志

`REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR` 里你的 flag 用了 `CachedMeshCommands | MainView`，这是对的（和 `MobileDepthPass` 一致）。

### 4.10 缺失：`ShouldRenderAfterTranslucency` 应当对 view 范围生效

`bRenderAfterTranslucency` 是 per-component 的 boolean，没有 view mask 概念——这与 `bRenderInMainPass` 一致，OK。**但若你希望某些 view 不渲染 marked object（比如 Reflection Capture、SceneCapture），需要在 `GetViewRelevance` 中根据 `View` 过滤。** 当前 Plan 默认所有 view 都渲染，对 VR 头显主 view 来说没问题，但 SceneCapture 也会跑一次（白白多两个 pass）。如果是 VR-only 工程这不是问题。

---

## 五、AddMeshBatch 区分的最小改动汇总

| 位置 | 区分方式 |
|------|---------|
| `FDepthPassMeshProcessor::AddMeshBatch` | 早 return：`bAfterTranslucencyDepthPass && !ShouldRenderAfterTranslucency()` → return；否则走标准路径 |
| `FMobileBasePassMeshProcessor::AddMeshBatch` | 早 return：`MeshPassType==MobileAfterTranslucencyPass` 走"只渲染标记过的"分支；其他走"过滤掉标记过的"分支 |
| `SceneVisibility.cpp` 静态 mesh（line 1564） | `ViewRelevance.bRenderAfterTranslucency` 决定 add 到 `MobileAfterTranslucencyDepthPass+MobileAfterTranslucencyPass` 还是 `BasePass+MobileBasePassCSM` |
| `SceneVisibility.cpp` 动态 mesh（line 2211） | `ViewRelevance.bRenderAfterTranslucency` 决定 `PassMask` 设为新两 pass 还是 `BasePass+MobileBasePassCSM` |

---

## 六、最终建议路线

按以下顺序修改（基于 UE5.4 + Android VR Forward 路径）：

1. **`MeshPassProcessor.h`**：加 `MobileAfterTranslucencyPass` + `MobileAfterTranslucencyDepthPass` 到 `EMeshPass`、`GetMeshPassName`、`static_assert(EMeshPass::Num == 34+4)`（当前 Plan 已包含）
2. **`PrimitiveComponent.h/cpp` + `PrimitiveSceneProxy.h/cpp` + `PrimitiveSceneProxyDesc.h` + `PrimitiveViewRelevance.h`**：加 `bRenderAfterTranslucency` + Setter（当前 Plan 已包含）
3. **`StaticMeshRender.cpp:2055` + `SkeletalMesh.cpp:7107`**：`GetViewRelevance` 设置 `Result.bRenderAfterTranslucency`（当前 Plan 已包含）
4. **`DepthRendering.cpp`**：构造函数加 `bInAfterTranslucencyDepthPass` 参数 + 私有成员；`AddMeshBatch` 早 return 分支；新增 `CreateMobileAfterTranslucencyDepthPassProcessor` + `REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR`
5. **`MobileBasePass.cpp`**：移除 `CreateMobileAfterTranslucencyDepthPassProcessor` 和其注册；保留 `CreateMobileAfterTranslucencyPassProcessor` 和其注册；`AddMeshBatch` 只处理 `MobileAfterTranslucencyPass`（颜色 pass 分支）；`CollectPSOInitializers` 早返回只针对 `MobileAfterTranslucencyPass`
6. **`SceneVisibility.cpp:1564` 静态 mesh + `:2211` 动态 mesh**：分流逻辑（**注意保留 `MobileBasePassCSM` 在非 after-translucency 分支的设置**）
7. **`SceneRendering.h` + `MobileBasePassRendering.cpp`**：声明 + 实现 `RenderMobileAfterTranslucencyDepthPass` 和 `RenderMobileAfterTranslucencyPass`（当前 Plan 已包含）
8. **`MobileShadingRenderer.cpp` `BuildInstanceCullingDrawParams`**：build 新两个 pass 的 `FInstanceCullingDrawParams`（当前 Plan 已包含）
9. **`MobileShadingRenderer.cpp` `RenderForward` 路径**：在 `RenderMobileBasePass` 之后（SkyPass + editor primitives 之后）、`RenderTranslucency` 之前调用 `RenderMobileAfterTranslucencyDepthPass`；在 `RenderTranslucency` 之后调用 `RenderMobileAfterTranslucencyPass`（当前 Plan 已包含）
10. **各种 stat 声明**（当前 Plan 已包含）

---

## 七、可能存在的潜在问题

1. **粒子系统（Niagara）**：plan 只考虑 `Mesh` 和 `SkeletalMesh`（你说过只让这两个生效），但 `FParticleSystemSceneProxy` 也会走 `AddMeshBatch`。Niagara 标记的 `bRenderAfterTranslucency` 是否需要支持？
   - 如果不需要：默认 component `bRenderAfterTranslucency=false`，现有流程会过滤掉，无需改 Niagara 代码
   - 如果需要：需要类似地修改 `FParticleSystemSceneProxy::GetViewRelevance`、SceneVisibility.cpp 中 Niagara 的分流

2. **Material with Translucent domain**：标记的物体如果用了 `Translucent` 材质 domain（透明材质），那 `FDepthPassMeshProcessor::TryAddMeshBatch` 内的 blend mode 检查（`Material->GetBlendMode() != BLEND_Translucent`）会过滤掉透明材质。
   - 如果你的 marked object 是不透明的：OK
   - 如果有半透明的 marked object：需要在 `TryAddMeshBatch` 中放行，或者确保 marked object 的材质是 Opaque/Masked

3. **bRenderInMainPass=false 的 marked object**：当前 Plan 中 `FMobileBasePassMeshProcessor::AddMeshBatch` 的 `ShouldRenderInMainPass` 检查在新 pass 路径里也会触发——marked object 如果同时把 `bRenderInMainPass` 设为 false，会被新 pass 跳过。
   - 建议在使用上：**保持 `bRenderInMainPass=true`，只通过 `bRenderAfterTranslucency` 控制新 pass 路径**。或者修改 `AddMeshBatch` 在新 pass 时跳过 `ShouldRenderInMainPass` 检查

4. **InstancedStaticMesh / HierarchicalISM**：这两个走 `FInstancedStaticMeshSceneProxy::GetViewRelevance` 和 `FHierarchicalInstancedStaticMeshSceneProxy::GetViewRelevance`，需要检查它们是否正确传递了 `bRenderAfterTranslucency`。**当前 Plan 没有处理这两个 proxy**——需要在 StaticMeshRender.cpp 修改的对应位置也改这两个 proxy（同一文件相邻位置）。
