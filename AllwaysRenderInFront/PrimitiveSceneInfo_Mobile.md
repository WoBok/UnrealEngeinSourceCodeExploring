# `FPrimitiveSceneInfo` 深度分析 — 在移动渲染管线中的作用

## 0. 摘要

| 项目 | 结论 |
|---|---|
| **本质** | `UPrimitiveComponent` 在**渲染线程**的镜像，包含所有渲染线程需要的持久化状态 |
| **1:1 关系** | 与 `FPrimitiveSceneProxy` 一一对应；与 `UPrimitiveComponent` 一一对应 |
| **线程归属** | 构造于渲染线程，被 `FScene` 持有，全渲染帧生命周期内常驻 |
| **移动端特化字段** | `NumMobileDynamicLocalLights`、`bVisibleInRealTimeSkyCapture`、`CachedReflectionCaptureProxies[3]`、`bNeedsCachedReflectionCaptureUpdate` |
| **与 `bShouldRenderAfterTranslucency` 计划的关系** | 缓存该字段**非必要**（与 `bRenderInMainPass` 同理，运行时直接查代理即可） |

---

## 1. 数据流全景：组件 → 代理 → 场景信息

```
┌─────────────────────────┐    创建/同步     ┌──────────────────────────┐    创建/初始化    ┌─────────────────────────┐
│ UPrimitiveComponent     │ ───────────────► │ FPrimitiveSceneProxy     │ ───────────────► │ FPrimitiveSceneInfo     │
│ (Game Thread)           │  (MarkRenderState│ (Rendering Thread)       │  (FScene::Add   │ (Rendering Thread)      │
│                         │   Dirty → 创建    │                          │   Primitive)     │                         │
│ - UPROPERTY 字段        │   Proxy)         │ - 渲染状态字段            │                  │ - 缓存的代理属性         │
│ - bRenderInMainPass     │                  │ - bRenderInMainPass       │                  │ - bShouldRenderInMainPass│
│ - bRenderAfterTrans...  │                  │ - bRenderAfterTrans...    │                  │   (但**未被移动端使用**)  │
│                         │                  │                          │                  │ - NumMobileDynamic...    │
│                         │                  │                          │                  │ - 反射缓存数组           │
└─────────────────────────┘                  └──────────────────────────┘                  └─────────────────────────┘
        ▲                                              ▲                                          ▲
        │ MarkRenderStateDirty                         │ AddMeshBatch 时查询                      │ Scene 级查询
        │ (运行时改属性)                                │ ShouldRenderInMainPass()                 │ (剔除/PathTracing)
        │                                              │ ShouldRenderAfterTranslucency()          │
        │                                              │                                          │
        └───────────── GameThread ─────────────────────┴────────── Rendering/RendererThread ─────┘
```

**关键事实**：`FPrimitiveSceneInfo` 是一份**冗余的副本**，目的是让渲染线程**无需访问 `UPrimitiveComponent`** 即可拿到渲染决策所需的信息。这是 UE 的核心多线程设计 — GameThread 可以在任何时刻修改组件，而渲染线程通过这份缓存快照工作。

---

## 2. 字段分类（按用途）

### 2.1 缓存来自 Proxy 的渲染状态位（uint8 : 1 位域）

| 字段 | 行号 | 来源 | 主要消费者 |
|---|---|---|---|
| `bShouldRenderInMainPass` | `PrimitiveSceneInfo.h:675` | `Proxy->ShouldRenderInMainPass()` | **仅 PathTracing**（`RendererScene.cpp:5170`） |
| `bVisibleInRealTimeSkyCapture` | `PrimitiveSceneInfo.h:678` | `Proxy->IsVisibleInRealTimeSkyCaptures()` | `SceneVisibility.cpp:1722, 2391`（剔除） |
| `bDrawInGame` | `PrimitiveSceneInfo.h:681` | `Proxy->IsDrawnInGame()` | PathTracing、scene capture |
| `bIsVisibleInRayTracing` | `PrimitiveSceneInfo.h:687` | `Proxy->IsVisibleInRayTracing()` | RT 决策 |
| `bCacheShadowAsStatic` | `PrimitiveSceneInfo.h:656` | 由 Mobility + `GetShadowCacheInvalidationBehavior()` 推导 | 阴影 |
| `bNeedsCachedReflectionCaptureUpdate` | `PrimitiveSceneInfo.h:672` | 初始 true | `PrimitiveSceneInfo.cpp:2326` |

**关键观察**：`bShouldRenderInMainPass` 这个字段虽然存在，但在**整个渲染管线（移动端 / Deferred / 任何 PSO / 任何剔除）中实际上不被使用**。它的唯一消费者是 `IsPrimitiveRelevantToPathTracing` 这个 path tracing 相关函数。这意味着 `bRenderInMainPass` 在渲染流程中的作用**完全是通过 Proxy 在运行时直接查询**（如 `AddMeshBatch` 中的 `PrimitiveSceneProxy->ShouldRenderInMainPass()`）。

### 2.2 移动端特化字段

#### 2.2.1 `int32 NumMobileDynamicLocalLights`（`PrimitiveSceneInfo.h:378`）

```cpp
/** The number of local lights with dynamic lighting for mobile */
int32 NumMobileDynamicLocalLights;
```

**用途**：在 mobile forward 着色器排列选择中作为开关（`MobileBasePass.cpp:924`）：

```cpp
// MobileBasePass.cpp:918-928
EMobileLocalLightSetting LocalLightSetting = EMobileLocalLightSetting::LOCAL_LIGHTS_DISABLED;
if (Scene && PrimitiveSceneProxy && ShadingModels.IsLit())
{
    if (!bPassUsesDeferredShading &&
        (MobileLocalLightsUseSinglePermutation() ||
         PrimitiveSceneProxy->GetPrimitiveSceneInfo()->NumMobileDynamicLocalLights > 0))
    {
        LocalLightSetting = GetMobileForwardLocalLightSetting(Scene->GetShaderPlatform());
    }
}
```

**维护**：`SceneCore.cpp:263` 在 `FLightPrimitiveInteraction` 构造时递增，`SceneCore.cpp:336` 在析构时递减。当计数从 0 变为 1（或反向）时，调用 `RequestStaticMeshUpdate()` 触发 mesh draw command 重新缓存（不同 shader 排列）。

#### 2.2.2 `const FReflectionCaptureProxy* CachedReflectionCaptureProxies[3]`（`PrimitiveSceneInfo.h:357`）

```cpp
static const uint32 MaxCachedReflectionCaptureProxies = 3;
const FReflectionCaptureProxy* CachedReflectionCaptureProxies[MaxCachedReflectionCaptureProxies];
```

**用途**：移动端 HQ reflections 使用 — 移动端在 base pass 中实际使用 3 个最近的 reflection capture 做反射插值（而不是 deferred 路径中的完整反射探针数组）。`CacheReflectionCaptures` 在 `PrimitiveSceneInfo.cpp:2340-2344` 实现：

```cpp
void FPrimitiveSceneInfo::CacheReflectionCaptures()
{
    FBoxSphereBounds BoxSphereBounds = Proxy->GetBounds(); 
    
    CachedReflectionCaptureProxy = Scene->FindClosestReflectionCapture(BoxSphereBounds.Origin);
    CachedPlanarReflectionProxy = Scene->FindClosestPlanarReflection(BoxSphereBounds);
    if (Scene->GetShadingPath() == EShadingPath::Mobile)
    {
        // mobile HQ reflections
        Scene->FindClosestReflectionCaptures(BoxSphereBounds.Origin, CachedReflectionCaptureProxies);
    }
    
    bNeedsCachedReflectionCaptureUpdate = false;
}
```

**Mobile 专属**：桌面 deferred 只用 1 个 `CachedReflectionCaptureProxy`，移动端额外缓存 3 个用于插值。

#### 2.2.3 `bool bNeedsCachedReflectionCaptureUpdate`（`PrimitiveSceneInfo.h:672`）

**配合** `NeedsReflectionCaptureUpdate()`（`PrimitiveSceneInfo.cpp:2326-2331`）：

```cpp
bool FPrimitiveSceneInfo::NeedsReflectionCaptureUpdate() const
{
    return bNeedsCachedReflectionCaptureUpdate && 
        // For mobile, the per-object reflection is used for everything
        (Scene->GetShadingPath() == EShadingPath::Mobile || IsForwardShadingEnabled(Scene->GetShaderPlatform()));
}
```

**Mobile 专属**：注释明确说"For mobile, the per-object reflection is used for everything"。这意味着移动端**总是**需要缓存 reflection captures（不像 deferred 桌面端可以延迟到某些条件下）。

### 2.3 静态网格相关数组

| 字段 | 行号 | 用途 |
|---|---|---|
| `TArray<FCachedMeshDrawCommandInfo> StaticMeshCommandInfos` | `PrimitiveSceneInfo.h:311` | 每个静态网格×每个 Pass 的 MDC 信息（cache lookup key） |
| `TArray<FStaticMeshBatchRelevance> StaticMeshRelevances` | `PrimitiveSceneInfo.h:314` | 静态网格相关性（包括 `CommandInfosMask`、`bSupportsNaniteRendering` 等） |
| `TArray<FStaticMeshBatch> StaticMeshes` | `PrimitiveSceneInfo.h:317` | 实际的 mesh batch 数据 |

**大小公式**：`StaticMeshCommandInfos.Num() == StaticMeshes.Num() * EMeshPass::Num`，最多到第一次 mesh draw command 缓存完成时压缩（`PrimitiveSceneInfo.cpp:565`）。

**对新 Pass 的影响**：因为 `EMeshPass::MobileAfterTranslucencyPass` 加进枚举后 `EMeshPass::Num` 从 32 增加到 33，`CacheMeshDrawCommands` 中循环 `for (int32 PassIndex = 0; PassIndex < EMeshPass::Num; PassIndex++)`（`PrimitiveSceneInfo.cpp:476`）会自动遍历新 Pass；`SceneInfo->StaticMeshCommandInfos.AddDefaulted(EMeshPass::Num * SceneInfo->StaticMeshes.Num())`（`PrimitiveSceneInfo.cpp:459`）会自动按新尺寸分配。**编译时由 `static_assert(sizeof(MeshRelevance.CommandInfosMask) * 8 >= EMeshPass::Num)`（`PrimitiveSceneInfo.cpp:505`）保护**。

### 2.4 索引与场景注册

| 字段 | 行号 | 用途 |
|---|---|---|
| `int32 PackedIndex` | `PrimitiveSceneInfo.h:620` | 在 `Scene->Primitives` 数组中的索引（**会变**，add/remove 时重排） |
| `FPersistentPrimitiveIndex PersistentIndex` | `PrimitiveSceneInfo.h:625` | 持久索引（用于 GPU 反射 / 跨重建追踪） |
| `FOctreeElementId2 OctreeId` | `PrimitiveSceneInfo.h:325` | 在 `Scene->PrimitiveOctree` 中的 ID |
| `FPrimitiveComponentId PrimitiveComponentId` | `PrimitiveSceneInfo.h:291` | 组件级 ID（跨重建稳定） |

**注意 `GetIndexAddress()`**（`PrimitiveSceneInfo.h:475`）：返回 `&PackedIndex`，便于把指针持久化到 GPU buffer 中。这是 GPU Scene 的基础。

### 2.5 HitProxy（编辑器专用）

`TArray<TRefCountPtr<HHitProxy>> HitProxies` + `HHitProxy* DefaultDynamicHitProxy` — 用于编辑器中的 primitive 选择。**移动端 shipping 不使用**。

### 2.6 Nanite 专用

`NaniteRasterBins[ENaniteMeshPass::Num]`、`NaniteShadingBins[ENaniteMeshPass::Num]`、`NaniteCommandInfos[ENaniteMeshPass::Num]`、`NaniteMaterialSlots[ENaniteMeshPass::Num]` — 4 个数组，索引 `ENaniteMeshPass::BasePass` 和 `ENaniteMeshPass::LumenCardCapture`。**移动端不使用 Nanite**（desktop-only），这些字段在移动端不参与。

### 2.7 RayTracing 专用（`#if RHI_RAYTRACING`）

整个 `CachedRayTracingMeshCommandIndicesPerLOD`、`CachedRayTracingMeshCommandsHashPerLOD`、`CachedRayTracingInstance` 等等都包裹在 `RHI_RAYTRACING` 预处理中。**移动端默认不开启 RT**，这些字段在移动端不参与。

---

## 3. 生命周期

### 3.1 构造

**位置**：`PrimitiveSceneInfo.cpp:368-376`

```cpp
FPrimitiveSceneInfo::FPrimitiveSceneInfo(UPrimitiveComponent* InPrimitive, FScene* InScene)
    : FPrimitiveSceneInfo(FPrimitiveSceneInfoAdapter(InPrimitive), InScene)
{ }

FPrimitiveSceneInfo::FPrimitiveSceneInfo(FPrimitiveSceneDesc* InPrimitiveSceneDesc, FScene* InScene)
    : FPrimitiveSceneInfo(FPrimitiveSceneInfoAdapter(InPrimitiveSceneDesc), InScene)
{ }
```

**两种入口**：
- `UPrimitiveComponent*` — 标准的"GameThread 创建组件 → 重建 render state"路径
- `FPrimitiveSceneDesc*` — 用于 procedural / runtime-created primitives（无对应 `UObject`）

**适配器**：`FPrimitiveSceneInfoAdapter`（`PrimitiveSceneInfo.cpp:183-276`）从这两种源统一提取数据。

**关键构造步骤**（位于 `PrimitiveSceneInfo.cpp:278-366` 的 private ctor）：
1. `Proxy = InAdapter.SceneProxy` — 持有 Proxy 指针
2. `PrimitiveComponentId = InAdapter.ComponentId`
3. `bShouldRenderInMainPass(InAdapter.SceneProxy->ShouldRenderInMainPass())` — **缓存位**
4. `bVisibleInRealTimeSkyCapture(InAdapter.SceneProxy->IsVisibleInRealTimeSkyCaptures())` — 移动端用
5. `bCacheShadowAsStatic(...)` — 由 Mobility + ShadowCacheInvalidationBehavior 推导
6. 初始化 GPU Scene 索引（`InstanceSceneDataOffset = INDEX_NONE` 等）

### 3.2 添加到场景：`AddToScene`（`PrimitiveSceneInfo.cpp:1518-1719`）

这是 **GameThread → RenderThread** 命令触发后，在渲染线程上实际"挂载"到 FScene 的步骤：

| 子步骤 | 行号 | 内容 | 移动端相关？ |
|---|---|---|---|
| `AddToScene_IndirectLightingCacheUniformBuffer` | 1523-1552 | 为有需要的 primitive 创建 indirect lighting cache UB | 否 |
| `AddToScene_IndirectLightingCacheAllocation` | 1554-1581 | 分配 indirect lighting cache 槽位 | 否 |
| `AddToScene_LightmapDataOffset` | 1583-1597 | 分配 lightmap data GPU buffer 偏移 | 是（移动端也用 lightmap） |
| `AddToScene_ReflectionCaptures` | 1600-1610 | **调用 `CacheReflectionCaptures()`** | **是**（移动端 cache 3 个 reflection captures） |
| `AddToScene_AddToPrimitiveOctree` | 1612-1624 | 加入 octree | 否 |
| `AddToScene_UpdateBounds` | 1626-1705 | 更新 Scene->PrimitiveBounds, PrimitiveFlagsCompact 等 | 否（但 `bShouldRenderInMainPass` 是位域，未复制到 Scene 级） |
| `AddToScene_LevelNotifyPrimitives` | 1707-1718 | 通知关卡 | 否 |

**移动端亮点**：步骤 4（reflection captures）是移动端必备的。在 `AddStaticMeshes` 之后的 `CacheMeshDrawCommands` 步骤中，移动端的 `BasePass`、`MobileBasePassCSM`、未来的 `MobileAfterTranslucencyPass` 的 MDC 都会被构建。

### 3.3 添加静态网格：`AddStaticMeshes`（`PrimitiveSceneInfo.cpp:1303-...`）

**调用者**：`FScene::AddPrimitiveSceneInfo_RenderThread`（`RendererScene.cpp:1551`）

**关键步骤**：
1. 每个 SceneInfo 调用 `Proxy->DrawStaticElements(&BatchingSPDI)`（`PrimitiveSceneInfo.cpp:1316`）— Proxy 把自己的 `FMeshBatch` 数据通过 `FBatchingSPDI::DrawMesh` 写入 `SceneInfo->StaticMeshes`
2. `BatchingSPDI::DrawMesh`（`PrimitiveSceneInfo.cpp:105-152`）同时构造 `FStaticMeshBatch` 和 `FStaticMeshBatchRelevance`
3. `FStaticMeshBatchRelevance`（`StaticMeshBatch.h`）包含 `bSupportsNaniteRendering`、`bSupportsGPUScene`、`bUseSkyMaterial` 等位
4. 在所有 primitive 添加完后，`CacheMeshDrawCommands`（`PrimitiveSceneInfo.cpp:429`）按 Pass 维度生成 MDC

### 3.4 缓存 Mesh Draw Commands：`CacheMeshDrawCommands`（`PrimitiveSceneInfo.cpp:429-628`）

这是 MDC 的核心构建循环，对新 Pass **至关重要**：

```cpp
// PrimitiveSceneInfo.cpp:476-521 (核心循环)
for (int32 PassIndex = 0; PassIndex < EMeshPass::Num; PassIndex++)
{
    const EShadingPath ShadingPath = GetFeatureLevelShadingPath(Scene->GetFeatureLevel());
    EMeshPass::Type PassType = (EMeshPass::Type)PassIndex;

    if ((FPassProcessorManager::GetPassFlags(ShadingPath, PassType) & EMeshPassFlags::CachedMeshCommands) != EMeshPassFlags::None)
    {
        FCachedPassMeshDrawListContext::FMeshPassScope MeshPassScope(DrawListContext, PassType);

        FMeshPassProcessor* PassMeshProcessor = FPassProcessorManager::CreateMeshPassProcessor(
            ShadingPath, PassType, Scene->GetFeatureLevel(), Scene, nullptr, &DrawListContext);

        if (PassMeshProcessor != nullptr)
        {
            for (const FMeshInfoAndIndex& MeshAndInfo : MeshBatches)
            {
                FPrimitiveSceneInfo* SceneInfo = SceneInfos[MeshAndInfo.InfoIndex];
                FStaticMeshBatch& Mesh = SceneInfo->StaticMeshes[MeshAndInfo.MeshIndex];
                FStaticMeshBatchRelevance& MeshRelevance = SceneInfo->StaticMeshRelevances[MeshAndInfo.MeshIndex];

                uint64 BatchElementMask = ~0ull;
                // 调用 AddMeshBatch → 内部调用 PrimitiveSceneProxy->ShouldRenderInMainPass() 判断
                PassMeshProcessor->AddMeshBatch(Mesh, BatchElementMask, SceneInfo->Proxy);

                FCachedMeshDrawCommandInfo CommandInfo = DrawListContext.GetCommandInfoAndReset();
                if (CommandInfo.CommandIndex != -1 || CommandInfo.StateBucketId != -1)
                {
                    MeshRelevance.CommandInfosMask.Set(PassType);
                    MeshRelevance.CommandInfosBase++;
                    // ... 存储到 SceneInfo->StaticMeshCommandInfos
                }
            }
            delete PassMeshProcessor;
        }
    }
}
```

**对新 Pass 的关键影响**：

| 关注点 | 当前行为 | 加入新 Pass 后 |
|---|---|---|
| 循环次数 | `EMeshPass::Num = 32` | 自动 33 |
| `FPassProcessorManager::GetPassFlags` 返回 | 取决于 `REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR` 注册时的 flags | 计划用 `EMeshPassFlags::CachedMeshCommands | EMeshPassFlags::MainView` ✓ |
| `CreateMeshPassProcessor` 返回 | 依赖是否注册 | 计划在 `MobileBasePass.cpp:1223` 注册 → 返回非 null |
| `AddMeshBatch` 内部 | 检查 `ShouldRenderInMainPass` 等 | 计划已加入 `bAfterTranslucencyBasePass` 分支 |
| `StaticMeshCommandInfos` 数组大小 | `MeshNum * 32` | `MeshNum * 33`（自动） |

**结论**：只要新 Pass 的注册正确（`REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR` 在 `MobileBasePass.cpp:1218-1222` 附近），`CacheMeshDrawCommands` 会**自动**为新 Pass 缓存 MDC，不需要修改 `PrimitiveSceneInfo` 的逻辑。

### 3.5 移除：`RemoveFromScene` / `RemoveCachedMeshDrawCommands`

`RemoveCachedMeshDrawCommands`（`PrimitiveSceneInfo.cpp:630-680`）逐个命令移除 MDC：
- 减少 `CachedMeshDrawCommandStateBuckets[PassIndex]` 中的引用计数
- 从 `CachedDrawLists[MeshPass]` 中删除
- 释放持久 PSO ID

**新 Pass 同样**自动参与此流程（因为循环用 `EMeshPass::Num`）。

### 3.6 反射捕获更新：`CacheReflectionCaptures`（`PrimitiveSceneInfo.cpp:2333-2347`）

```cpp
void FPrimitiveSceneInfo::CacheReflectionCaptures()
{
    FBoxSphereBounds BoxSphereBounds = Proxy->GetBounds(); 
    
    CachedReflectionCaptureProxy = Scene->FindClosestReflectionCapture(BoxSphereBounds.Origin);
    CachedPlanarReflectionProxy = Scene->FindClosestPlanarReflection(BoxSphereBounds);
    if (Scene->GetShadingPath() == EShadingPath::Mobile)
    {
        // mobile HQ reflections
        Scene->FindClosestReflectionCaptures(BoxSphereBounds.Origin, CachedReflectionCaptureProxies);
    }
    
    bNeedsCachedReflectionCaptureUpdate = false;
}
```

**移动端特异性**：
- 只有 `EShadingPath::Mobile` 或 forward 着色时才执行（`NeedsReflectionCaptureUpdate` 中判断）
- 缓存**3 个**最近的 reflection captures 用于 mobile HQ reflections
- 这些缓存会在以下时机刷新：
  - `AddToScene` 时（`PrimitiveSceneInfo.cpp:1605-1608`）
  - `RemoveCachedReflectionCaptures` 后被置脏
  - 移动端每帧可能调用（`SceneVisibility.cpp:1947`、`RendererScene.cpp:5410`）

### 3.7 析构

`PrimitiveSceneInfo.cpp:378-385` 检查 `OctreeId` 无效 + `StaticMeshCommandInfos` 已清空。`~FPrimitiveSceneInfo` 不释放 PSO ID — `RemoveCachedMeshDrawCommands` 已处理。

---

## 4. 与移动渲染管线的接口

### 4.1 移动端的 SceneRenderer 入口：`FMobileSceneRenderer`

`SceneVisibility.cpp` 中的静态/动态网格相关性计算会**直接查询 Proxy**：

```cpp
// SceneVisibility.cpp:1556 (静态)
if (StaticMeshRelevance.bUseForMaterial && (ViewRelevance.bRenderInMainPass || ViewRelevance.bRenderCustomDepth))

// SceneVisibility.cpp:2211 (动态)
if (ViewRelevance.bRenderInMainPass || ViewRelevance.bRenderCustomDepth)
```

注意这里用的是 `ViewRelevance`（来自 `FPrimitiveViewRelevance`，由 `GetViewRelevance` 设置），**不是** `FPrimitiveSceneInfo::bShouldRenderInMainPass`。

**新 Pass 在 `ViewRelevance` 中的体现**：`ViewRelevance.bRenderAfterTranslucency`（计划加在 `FPrimitiveViewRelevance`）会被 `SceneVisibility.cpp:1564, 2228` 读取，用于把 primitive 分流到 `MobileAfterTranslucencyPass`。

### 4.2 移动端 base pass 调用：`RenderMobileBasePass`

`MobileBasePassRendering.cpp:470-491`：

```cpp
void FMobileSceneRenderer::RenderMobileBasePass(FRHICommandList& RHICmdList, const FViewInfo& View, const FInstanceCullingDrawParams* InstanceCullingDrawParams)
{
    // ...
    View.ParallelMeshDrawCommandPasses[EMeshPass::BasePass].DispatchDraw(nullptr, RHICmdList, InstanceCullingDrawParams);
    
    if (View.Family->EngineShowFlags.Atmosphere)
    {
        View.ParallelMeshDrawCommandPasses[EMeshPass::SkyPass].DispatchDraw(nullptr, RHICmdList, &SkyPassInstanceCullingDrawParams);
    }
    
    // editor primitives
    FMeshPassProcessorRenderState DrawRenderState;
    // ...
    RenderMobileEditorPrimitives(RHICmdList, View, DrawRenderState, InstanceCullingDrawParams);
}
```

**与新 Pass 的关系**：计划新加的 `RenderMobileAfterTranslucencyPass` 与 `RenderMobileBasePass` 在结构上**几乎相同**，区别仅在于：
1. 调用的 MeshPass 是 `EMeshPass::MobileAfterTranslucencyPass`
2. 没有 `SkyPass` 的 `Atmosphere` 分支（除非需要）
3. 没有 `RenderMobileEditorPrimitives`（需要决策）
4. depth-stencil state 可能不同

### 4.3 移动端反射捕获访问

`PrimitiveSceneInfo::CachedReflectionCaptureProxies[3]` 数组在 mobile base pass shader 中被读取 — 用于 mobile HQ reflection 插值。

### 4.4 移动端本地光着色器排列

`PrimitiveSceneInfo::NumMobileDynamicLocalLights` 在 `MobileBasePass.cpp:924` 读取：

```cpp
PrimitiveSceneProxy->GetPrimitiveSceneInfo()->NumMobileDynamicLocalLights > 0
```

注意这里的访问路径：`PrimitiveSceneProxy->GetPrimitiveSceneInfo()` — Proxy 通过 `GetPrimitiveSceneInfo()` 访问其对应的 SceneInfo（在 `PrimitiveSceneProxy.h` 中定义），从而拿到 `NumMobileDynamicLocalLights`。

---

## 5. 与 `bRenderAfterTranslucency` 修改计划的关系

### 5.1 `bShouldRenderInMainPass` 在 `FPrimitiveSceneInfo` 中的实际作用

通过 `grep` 检索整个 Renderer 源码：

| 文件 | 行号 | 上下文 |
|---|---|---|
| `PrimitiveSceneInfo.h:675` | 定义 | `bool bShouldRenderInMainPass : 1;` |
| `PrimitiveSceneInfo.cpp:305` | 初始化 | `bShouldRenderInMainPass(InAdapter.SceneProxy->ShouldRenderInMainPass()),` |
| `RendererScene.cpp:5170` | **唯一消费者** | `IsPrimitiveRelevantToPathTracing` — 仅 path tracing 用 |

**结论**：`FPrimitiveSceneInfo::bShouldRenderInMainPass` 字段**几乎从未被消费**。它存在只是为了 path tracing 的一个边缘 case。移动端管线**从不使用它** — 移动端通过 Proxy 直接查询（`MobileBasePass.cpp:871`、`SceneVisibility.cpp:1556/2211` 等）。

### 5.2 推论：`bRenderAfterTranslucency` 不需要缓存到 `FPrimitiveSceneInfo`

既然 `bShouldRenderInMainPass`（已经被使用 6 年）都几乎不缓存到 SceneInfo，新字段更不需要。**当前所有用到 `bRenderAfterTranslucency` 的位置都是直接从 Proxy 查询**：

| 消费位置 | 查询方式 |
|---|---|
| `AddMeshBatch`（运行时路由） | `PrimitiveSceneProxy->ShouldRenderAfterTranslucency()` |
| `SceneVisibility.cpp` 静态/动态（相关性 + 分流） | `ViewRelevance.bRenderAfterTranslucency`（从 `GetViewRelevance()` 复制） |

### 5.3 何时**应该**考虑在 `FPrimitiveSceneInfo` 添加缓存位？

仅在以下情况之一需要时：

1. **Scene-level 剔除决策**：在不知道具体 View 的情况下做剔除（如 broad phase culling、HiZ 生成等）
2. **GPU Scene 数据写入**：如果要把这个位写入 GPU Scene 的 instance data buffer，供 shader 读取
3. **Path Tracing 决策**：类似 `bShouldRenderInMainPass` 的位置
4. **Instance Culling Context**：如果 instance culling pass 需要这个位做剔除

**当前计划均未涉及上述场景**，因此不需要添加。如果未来 Path Tracing 也要支持 `bRenderAfterTranslucency`，可参考 `bShouldRenderInMainPass` 缓存模式。

### 5.4 如果未来要加，应当如何加

参考 `bShouldRenderInMainPass` 的模式：

```cpp
// PrimitiveSceneInfo.h
public:
    /** Set to true for the primitive to be rendered in the main pass to be visible in a view. */
    bool bShouldRenderInMainPass : 1;
    // ... 
    /** Set to true for the primitive to be rendered after translucency. */
    bool bShouldRenderAfterTranslucency : 1;  // 新增

// PrimitiveSceneInfo.cpp:305
bShouldRenderInMainPass(InAdapter.SceneProxy->ShouldRenderInMainPass()),
// ...
bShouldRenderAfterTranslucency(InAdapter.SceneProxy->ShouldRenderAfterTranslucency()),  // 新增
```

---

## 6. 关于 PSO 预缓存的最终确认（与第 2.3.1 节呼应）

`FPrimitiveSceneInfo` **不参与** PSO 预缓存流程。PSO 预缓存是 `UPrimitiveComponent::SetupPrecachePSOParams → PrecachePSOs` 的 GameThread 流程，绕过 `FPrimitiveSceneInfo`（见 `PrimitiveComponent.cpp:4620` 和分析报告 2.3.1 节的完整调用链）。

`FPrimitiveSceneInfo` 在 PSO 预缓存发生**之后**才创建（因为 PSO 编译需要先完成才能创建 SceneProxy，再创建 SceneInfo）。

---

## 7. 总结

| 问题 | 答案 |
|---|---|
| `FPrimitiveSceneInfo` 是什么？ | `UPrimitiveComponent` 在渲染线程的持久化镜像，1:1 与 `FPrimitiveSceneProxy` 对应 |
| 谁创建它？ | `FScene::AddPrimitive` → `BatchAddPrimitivesInternal` → `FPrimitiveSceneInfo::FPrimitiveSceneInfo`（渲染线程） |
| 它的生命周期？ | 从组件注册到场景，到组件从场景移除 |
| 它如何与移动端接口？ | 通过 `NumMobileDynamicLocalLights`、`CachedReflectionCaptureProxies[3]`、`bVisibleInRealTimeSkyCapture` 等移动端专属字段 |
| `bShouldRenderInMainPass` 在移动端有用吗？ | **没有**。唯一消费者是 Path Tracing |
| 计划需要缓存 `bRenderAfterTranslucency` 到 SceneInfo 吗？ | **不需要**（当前），原因与 `bRenderInMainPass` 一样 — Proxy 运行时查询已足够 |
| 新 Pass 怎么与 `FPrimitiveSceneInfo` 交互？ | 通过 `CacheMeshDrawCommands` 中的 `EMeshPass::Num` 循环自动处理；MDC 数组自动按 `MeshNum * 33` 分配 |
| 有什么潜在问题需要关注？ | 1) `StaticMeshCommandInfos.Num() == StaticMeshes.Num() * EMeshPass::Num` — 加 Pass 后内存略增；2) `static_assert(sizeof(CommandInfosMask) * 8 >= EMeshPass::Num)`（`PrimitiveSceneInfo.cpp:505`）— 自动保护 |
