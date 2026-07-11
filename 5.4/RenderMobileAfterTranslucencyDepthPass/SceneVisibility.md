# UE5.4 SceneVisibility 源码深度解析

> **目标平台**: UE5.4 源码版 (`E:/Unreal Engine Work Projects/MR01_DaNaoTianGong_Main/Engine`),桌面 + Mobile (OpenGL ES 3.2 / Vulkan)
> **文档版本**: v1.0 / 2026-06-18
> **作者**: Claude Code (MiniMax-M3,ultracode 模式)
> **范围**: `Engine/Source/Runtime/Renderer/Private/SceneVisibility.{cpp,h,Private.h}`

---

## 0. 文档导读

`SceneVisibility.cpp` 是 UE 渲染器中**最核心、也是最庞大**的 CPU 端模块之一。它负责在 RDG 调度之前,完成"哪些物体在哪些 View 的哪些 Mesh Pass 中需要被绘制"的全部判定。`ComputeViewVisibility`、`MarkPrimitivesAsVisible`、`InitViews` 等关键调用都汇聚于此文件。

本文档面向:

- **引擎开发者**:要在 UE 中加新的 Mesh Pass / 新的剔除规则 / 新的视口筛选。
- **Mobile 端优化者**:要搞清 Mobile 端哪些路径被简化,哪些路径被绕过。
- **性能分析者**:要找到遮挡剔除、HiZ、距离剔除的瓶颈与配置点。

---

## 1. 文件概述 (File Overview)

| 项 | 值 |
|---|---|
| 主文件 | `Engine/Source/Runtime/Renderer/Private/SceneVisibility.cpp` |
| 行数 | **6032 行** |
| 头文件 | `Engine/Source/Runtime/Renderer/Private/SceneVisibility.h` (69 行) |
| 私有头 | `Engine/Source/Runtime/Renderer/Private/SceneVisibilityPrivate.h` (1202 行) |
| 所属模块 | `Renderer` (UE Runtime) |
| 编译条件 | `WITH_ENGINE` (几乎总被定义,Shipping 同样包含) |

### 1.1 在 Unreal 渲染管线中的角色

`SceneVisibility.cpp` 处于**每帧 RDG 调度的最前端**,作用如同"前哨站":

```
GameThread 提交一帧
        │
        ▼
FSceneRenderer::Render()                  ← 入口
        │
        ├── PreVisibilityFrameSetup         ← SceneVisibility.cpp:4635
        ├── BeginInitViews (Deferred)       ← SceneVisibility.cpp:5453
        ├── BeginInitViews (Mobile, 共享)
        │
        ├── LaunchVisibilityTasks          ← SceneVisibility.cpp:383 顶层入口
        │       ├── UpdateAlwaysVisible     ← SceneVisibility.cpp:634
        │       ├── FrustumCull             ← SceneVisibility.cpp:687
        │       ├── PrecomputedOcclusionCull← SceneVisibility.cpp:3199
        │       ├── GPUOcclusion (HZB)      ← SceneVisibility.cpp:2454-3200
        │       ├── ComputeRelevance        ← SceneVisibility.cpp:1299
        │       ├── ComputeLightVisibility  ← SceneVisibility.cpp:5192/5201
        │       └── GatherDynamicMeshElements ← SceneVisibility.cpp:4288
        │
        ├── EndInitViews
        ├── ComputeLightVisibility
        ├── PostVisibilityFrameSetup
        │
        ▼
RDG Passes 调度(BasePass / Translucency / Shadow / PostProcess)
```

**关键定位**:此文件**只做 CPU 端判定**,**不直接提交任何 GPU 绘制**。所有 GPU 绘制在 `SetupMeshPass` 之后被 `ParallelMeshDrawCommandPasses[EMeshPass::Xxx]` 队列接管。

### 1.2 与其它 Renderer 文件的关系

| 上下游文件 | 关系 |
|---|---|
| `SceneRendering.h` (Public) | 定义 `FViewInfo` / `FSceneRenderer` 接口 |
| `ScenePrivate.h` (Private) | 持有 `FScene::Primitives`、`FScene::PrimitiveSceneProxies` 等 |
| `MeshPassProcessor.h` | 引用 `EMeshPass::Type` 枚举,本文件 `PassMask` 按它设置位 |
| `MobileSceneRenderer.cpp` | Mobile 端入口,`FSceneRenderer` 派生类,调用 `LaunchVisibilityTasks` |
| `DeferredShadingRenderer.cpp` | 桌面端入口,`FSceneRenderer` 派生类,调用 `LaunchVisibilityTasks` |
| `PrimitiveViewRelevance.h` | 提供 `FPrimitiveViewRelevance` 标志位 |
| `StaticMeshSceneProxy.cpp` | 在 `GetViewRelevance` 中向 `FPrimitiveViewRelevance` 写位,SceneVisibility 读位 |
| `SceneRenderTargetParameters.h` | HZB 的输入输出资源 |

---

## 2. 核心职责 (Core Responsibilities)

`SceneVisibility.cpp` 同时管理 **GPU 场景 (Gpu Scene) 与 CPU 场景 (PrimitiveSceneProxies)** 的可见性判定。具体职责可拆为以下子模块:

### 2.1 视锥剔除 (Frustum Culling)

- 函数: `FrustumCull` (`:687`)
- 输入: `FScene`, `FViewInfo`, `FFrustumCullingFlags`
- 输出: 修改 `View.PrimitiveVisibilityMap`
- 关键 CVar:
  - `r.AllowFrustumCull` → `GFrustumCullEnabled` (`:254`)
  - `r.CullWithOctree` → `GFrustumCullUseOctree` (`:262`)
  - `r.CullWithSphereTestFirst` → `GFrustumCullUseSphereTestFirst` (`:270`)
  - `r.Cull.WithFastIntersect` → `GFrustumCullUseFastIntersect` (`:278`)
  - `r.FrustumCullNumPrimitivesPerTask` (`:244`)

支持两种空间结构:

1. **平面 OBB 扫描** (默认):遍历 `Scene.Primitives` 数组,逐个 `BoxSphereBounds` 与 `View.ViewFrustum` 8 平面求交。
2. **Octree 加速** (`GFrustumCullUseOctree`):用 `FScene::Octree` (`Scene.Octree`) 递归收集可见节点,函数 `CullOctree` (`:590`)。

### 2.2 遮挡剔除 (Occlusion Culling)

遮挡剔除在 UE 中有**两套实现**:

| 实现 | 函数 | 适用平台 | 触发条件 |
|---|---|---|---|
| **Precomputed Occlusion** | `PrecomputedOcclusionCull` (`:3199`) | 全平台 | `Scene.PrecomputedVisibilityData` 存在 |
| **GPU HiZ Occlusion** | `FGPUOcclusionPacket::OcclusionCullPrimitive` (`:2454`) | 桌面 (OpenGL/Vulkan/D3D) | `DoOcclusionQueries()` (`:323`) 返回 true |
| **Mobile 单 Pass 串行** | `FGPUOcclusionSerial` (`:3169`) | Mobile (无并行 query 支持) | `IsMobilePlatform(...)` 为真 |

**关键判断逻辑** (`:3244`):

```cpp
if (!FApp::ShouldUseThreadingForPerformance()
 || !GIsThreadedRendering
 || !GSupportsParallelOcclusionQueries
 || GVisualizeOccludedPrimitives > 0
 || IsMobilePlatform(Scene.GetShaderPlatform()))
{
    // 走 FGPUOcclusionSerial 串行路径
}
```

### 2.3 距离剔除 (Distance Culling)

- 函数: `FViewInfo::IsDistanceCulled` (`:392`)、`IsDistanceCulled_AnyThread` (`:406`)
- 字段: `MinDrawDistance` / `MaxDrawDistance` (来自 `UPrimitiveComponent`)
- LOD 淡入淡出: `UpdatePrimitiveFading` (`:980`)、`UpdatePrimitiveFadingState` (`:1023`)、`ClearStalePrimitiveFadingStates` (`:893`)
- CVar:
  - `r.DisableLODFade` (`:223`)
  - `r.LODFadeTime` (`:226`)
  - `r.DistanceFadeMaxTravel` (`:229`)

距离剔除的判定比较"早",位于 `FrustumCull` 内部,通过 `MaxDrawDistanceScale` (来自 `ViewDistanceScale * CalculateFieldOfViewDistanceScale`) 实现:

```cpp
const float MaxDrawDistanceScale = GetCachedScalabilityCVars().ViewDistanceScale
                                 * GetCachedScalabilityCVars().CalculateFieldOfViewDistanceScale(View.DesiredFOV);
```

### 2.4 层级剔除 (Hierarchical Culling)

- **HLOD**: `FLODSceneTree::UpdateVisibilityStates` (`:5758`)
- **LOD 树本身**: `FLODSceneTree::AddChildNode` (`:5679`)、`RemoveChildNode` (`:5703`)、`UpdateNodeSceneInfo` (`:5720`)
- HLOD 在 `BeginInitVisibility` (`:3560`) 中更新,先于 frustum cull。

### 2.5 显示/隐藏过滤 (Show Only / Hidden)

- 函数: `BeginInitVisibility` 中检查 `View.HiddenPrimitives` 与 `View.ShowOnlyPrimitives`
- 字段: `FFrustumCullingFlags::bHasHiddenPrimitives`、`bHasShowOnlyPrimitives` (`:3588-3589`)
- 影响: 写入 `View.PrimitiveVisibilityMap` 之前就被屏蔽掉。

### 2.6 冻结可见性 (Frozen Visibility, Editor Only)

- `ViewState->bIsFrozen` 时,**关闭 frustum cull**,仅用 `ViewState->FrozenPrimitives` 集合 (`:3542`)
- `ViewState->bIsFreezing` 时,记录当前帧所有可见的 PrimitiveComponentId 到 `FrozenPrimitives` (`:4448`)

### 2.7 与 GPU 场景的交互 (ComputeViewVisibility on GPU)

UE5.4 引入了**Instance Culling on GPU**。`SceneVisibility.cpp` 仍然负责 CPU 端 **per-primitive** 的判断,但**每个 instance** 的判断可以下放到 GPU Compute Shader (`InstanceCulling.usf`)。

- 关键对象: `FInstanceCullingManager` (在头文件中前置声明)
- 关键函数: `SetupMeshPasses` (`:4401`) 接受 `FInstanceCullingManager&`
- 关键调用: `SceneRenderer.SetupMeshPass(View, ..., InstanceCullingManager)` (`:4468`)

**与 GPU 场景的边界**:

- CPU 端: `View.PrimitiveVisibilityMap[PrimitiveIndex]` = bool,逐 primitive。
- GPU 端: `View.InstanceCullingDrawParams` = 间接绘制参数,逐 instance。

### 2.8 与 CPU 场景的交互

CPU 端代表: `FScene::PrimitiveSceneProxies[]`、`FScene::Primitives[]`、`FScene::StaticMeshes[]`、FScene 的 Octree 加速结构 (`FScene::Octree`)。

主要交互点:

| 操作 | 函数 | 说明 |
|---|---|---|
| 标记可见 | `View.PrimitiveVisibilityMap[Index] = true` | 多个位置:FrustumCull、PrecomputedOcclusionCull、ComputeAndMarkRelevance |
| 标记不可见 | `View.PrimitiveVisibilityMap[Index] = false` | OcclusionCull、HZB |
| 计算相关性 | `FDrawCommandRelevancePacket::AddCommandsForMesh` (`:1074`) | 把 primitive 加入 `FRelevancePacket` |
| 准备 Mesh Pass | `FComputeAndMarkRelevance::ComputeRelevance` (`:1299`) | 决定进 BasePass / Translucency / CustomDepth 等 |

### 2.9 动态/静态/InstancedStaticMesh 的处理

| 类型 | 处理路径 |
|---|---|
| **静态 (Static)** | `FStaticMeshSceneProxy` → `FStaticMeshBatch` → `FRelevancePacket::AddCommandsForMesh` 直接进 Mesh Pass |
| **动态 (Dynamic)** | `FPrimitiveSceneProxy::GetDynamicMeshElements` 在 `GatherDynamicMeshElements` (`:4288/4341`) 中调用,把生成的 `FMeshBatch` 推到 `View.DynamicMeshElements` |
| **InstancedStaticMesh (ISM/HISM)** | CPU 端: 走 `FRelevancePacket`,但每个 instance 单独做 `GetViewRelevance` 判定;GPU 端: 进入 `FInstanceCullingManager`,Compute Shader 做 HiZ 二次剔除 |
| **Niagara / Particle** | 走 `FNiagaraSceneProxy` 自身的 `GetDynamicMeshElements`,进 `GatherDynamicMeshElements` 路径 |
| **Niagara GPU Emitter** | 进 `GPUEmitterCache`,不走 SceneVisibility (本文件不涉及) |
| **Lumen / Nanite** | 走 `Nanite::IRenderer` 接口,与本文件并行,不重复判定 |

### 2.10 光源可见性 (Light Visibility)

- 函数: `FSceneRenderer::ComputeLightVisibility` (`:5201`)
- 函数: `FDeferredShadingSceneRenderer::ComputeLightVisibility` (`:5192`):桌面端覆盖,处理阴影/反射探针。
- Mobile 端: `bSetupMobileLightShafts` 标志 (`:5218`),只处理 light shaft,无阴影相关。
- 时机: `Tasks.LightVisibility.Trigger()` (`:3580`) 紧跟 HLOD 更新。

---

## 3. 主要数据结构 (Key Data Structures)

### 3.1 `FViewInfo` (在 `SceneRendering.h` 中定义,本文件重度使用)

| 字段 | 类型 | 用途 |
|---|---|---|
| `PrimitiveVisibilityMap` | `FSceneBitArray` | 每个 primitive 一个 bit,是否对当前 View 可见 |
| `PrimitiveRayTracingVisibilityMap` | `FSceneBitArray` | 同步到 ray tracing |
| `PotentiallyFadingPrimitiveMap` | `FSceneBitArray` | LOD 淡入淡出中的 primitive |
| `PrimitiveFadeUniformBufferMap` | `FSceneBitArray` | 有淡入淡出 uniform buffer 的 primitive |
| `PrimitiveViewRelevanceMap` | `TArray<FPrimitiveViewRelevance>` | 每个 primitive 的 view 相关性 |
| `StaticMeshVisibilityMap` | `FSceneBitArray` | 静态 mesh 可见性(对 `FStaticMeshBatch` 的二次筛选) |
| `StaticMeshFadeOutDitheredLODMap` | `FSceneBitArray` | 静态 mesh LOD 淡出 |
| `StaticMeshFadeInDitheredLODMap` | `FSceneBitArray` | 静态 mesh LOD 淡入 |
| `PrimitivesLODMask` | `TArray<FLODMask>` | 每个 primitive 的 LOD mask |
| `DynamicMeshElementRanges` | `TArray<uint32>` | Dynamic mesh element 在 `DynamicMeshElements` 数组中的范围 |
| `PrimitiveFadeUniformBuffers` | `TArray<FUniformBufferRHIRef>` | 淡入淡出 uniform buffer |
| `HiddenPrimitives` / `ShowOnlyPrimitives` | `TSet<FPrimitiveComponentId>` | 显式隐藏/显示集 |
| `MeshDecalBatches` | `TArray<FMeshDecalBatch>` | mesh decal batch |
| `DynamicMeshElementsPassRelevance` | `TArray<FMeshPassMask>` | Dynamic mesh 的 pass 掩码 |
| `ViewElementPDI` | `FViewElementPDI` | editor-only,view element primitives |
| `NumVisibleDynamicMeshElements[EMeshPass::Num]` | `TStaticArray<int32, ...>` | 每个 pass 的 dynamic mesh 数 |
| `bIsMobileMultiViewEnabled` | `bool` | Mobile Multi-View 标志 |

**FSceneBitArray 的来源**: `SceneVisibility.cpp:3514-3526` 全部以 `Init(bool, Num)` 初始化,占位大小由 `Scene.Primitives.Num()` 决定。

### 3.2 `FVisibilityViewPacket` (SceneVisibilityPrivate.h:419)

每个 View 独占一个 packet,持有:

- 任务事件集合 `Tasks`: `{AlwaysVisible, FrustumCull, OcclusionCull, ComputeRelevance, LightVisibility}`
- `Relevance.Context` (指向 `FComputeAndMarkRelevance`)
- `OcclusionCull.ContextIfSerial` / `ContextIfParallel` (二选一)
- `ViewElementPDI` (editor)
- `ViewState` 指针

**任务图语义**:

```
AlwaysVisible ──→ FrustumCull ──→ OcclusionCull ──→ ComputeRelevance ──→ (RDG)
                                                              ↑
                                                       LightVisibility
```

### 3.3 `FVisibilityTaskData` (SceneVisibilityPrivate.h:499)

全局 packet,聚合所有 View:

- `Views[]` / `ViewPackets[]` (每个 View 一个 packet)
- `Tasks` (跨所有 View 的合并事件)
- `TaskConfig` (任务配置,见下)
- `DynamicMeshElements` (GDME 状态)
- `RHICmdList` 引用

### 3.4 `FVisibilityTaskConfig` (SceneVisibilityPrivate.h:345)

任务粒度配置,`LaunchVisibilityTasks` 之前计算:

| 子结构 | 字段 | 含义 |
|---|---|---|
| 顶层 | `Schedule` | `EVisibilityTaskSchedule::Parallel/RenderThread` |
| `AlwaysVisible` | `NumTasks`, `NumWordsPerTask`, `NumPrimitivesPerTask` | 始终可见 prim 的任务划分 |
| `FrustumCull` | `NumTasks`, `NumWordsPerTask`, `NumPrimitivesPerTask` | 视锥剔除任务划分 |
| `OcclusionCull` | `MaxQueriesPerTask` | occlusion query 任务划分 |
| `Relevance` | `NumEstimatedPackets`, `NumPrimitivesPerPacket` | relevance 任务划分 |

### 3.5 `FComputeAndMarkRelevance` (SceneVisibilityPrivate.h:792)

执行 "compute view relevance + mark pass mask" 的核心类:

- `AddPrimitive(int32 Index)`: 把 primitive 加入待处理队列
- `AddPrimitives(FPrimitiveIndexList&&)`: 批量加入
- `Finalize()`: 完成本 packet
- `Finish(UE::Tasks::FTaskEvent&)`: 调度 compute task
- `LaunchComputeRelevanceTask()` (在 `FRelevancePacket` 中, `:1161`)

### 3.6 `FRelevancePacket` (SceneVisibilityPrivate.h:700)

`FDrawCommandRelevancePacket` 的容器,管理多个 mesh batch 的相关性计算:

- `AddCommandsForMesh(PrimitiveIndex, ...)` (`:1074`): 为单个静态 mesh 准备所有 pass 的 draw command
- `LaunchComputeRelevanceTask()`: 并行调度
- `Finalize()`: 完成

### 3.7 `FGPUOcclusion` 系列 (SceneVisibilityPrivate.h:1089-1186)

```cpp
class FGPUOcclusion {
    // 基类
};
class FGPUOcclusionSerial final : public FGPUOcclusion {
    // 串行:用于 Mobile / 单线程
};
class FGPUOcclusionParallel final : public FGPUOcclusion {
    // 并行:多线程同时 query
};
class FGPUOcclusionPacket {
    // 单个 view 的 occlusion 任务
    bool OcclusionCullPrimitive(...);  // :2454
};
```

**HZB 输入**: 通过 `View.HZBMipmap0Size` 等字段传递 mip 0 的 size,实际 HZB 数据在 `FSceneTextures` 中持有,本文件只负责 dispatch query。

### 3.8 `FFrustumCullingFlags` (匿名结构,`SceneVisibilityPrivate.h` 内)

```cpp
struct FFrustumCullingFlags
{
    bool bShouldVisibilityCull;     // 总开关
    bool bUseCustomCulling;         // CustomVisibilityQuery
    bool bUseSphereTestFirst;       // 球测试优先(快路径)
    bool bUseFastIntersect;         // 8 平面 + fast intersect
    bool bUseVisibilityOctree;      // Octree 加速
    bool bHasHiddenPrimitives;      // 是否有 Hidden
    bool bHasShowOnlyPrimitives;    // 是否有 ShowOnly
};
```

### 3.9 `FPrimitiveRange` (SceneVisibilityPrivate.h:151)

```cpp
struct FPrimitiveRange
{
    uint32 StartIndex = 0;
    uint32 EndIndex = 0;  // 索引区间 [Start, End)
};
```

### 3.10 `FDynamicPrimitiveIndexList` (SceneVisibilityPrivate.h:178)

GPU 端把 dynamic mesh 的 primitive 索引打包,通过 `TCommandPipe<FDynamicPrimitiveIndexList>` 在 RT 消费,见 `:4288`。

### 3.11 `EMarkMaskBits` (SceneVisibilityPrivate.h:665)

`ComputeAndMarkRelevance` 内部用 MarkMask 把 primitive 标记进 `FViewInfo` 的状态:

```cpp
namespace EMarkMaskBits {
    enum Type { ... };  // MarkVisibility, MarkShadowVisibility, ...
}
```

### 3.12 `FViewCommands` (SceneVisibility.h:15)

```cpp
class FViewCommands
{
    TStaticArray<FMeshCommandOneFrameArray, EMeshPass::Num> MeshCommands;
    TStaticArray<int32, EMeshPass::Num> NumDynamicMeshCommandBuildRequestElements;
    TStaticArray<TArray<const FStaticMeshBatch*>, EMeshPass::Num> DynamicMeshCommandBuildRequests;
    TStaticArray<TArray<EMeshDrawCommandCullingPayloadFlags>, EMeshPass::Num> DynamicMeshCommandBuildFlags;
};
```

每个 View 持有 1 个,记录当前帧的 draw command 与待 build 的 dynamic request。

### 3.13 `IVisibilityTaskData` (SceneVisibility.h:32)

接口,封装跨线程可见性任务的"消费者"视图。`LaunchVisibilityTasks` 顶层入口返回这个指针。

---

## 4. 主要函数列表 (Main Functions)

### 4.1 顶层入口

| 函数 | 行号 | 说明 | Mobile 调用? |
|---|---|---|---|
| `LaunchVisibilityTasks(FRHICommandListImmediate&, FSceneRenderer&, FTask&)` | `:383` | **整个文件的入口**,在 RT 端被 `FSceneRenderer::BeginInitViews` 调用,产出 `IVisibilityTaskData*` | Yes |
| `FSceneRenderer::PreVisibilityFrameSetup(FRDGBuilder&)` | `:4635` | 一帧最早期,分配 RDG 资源、创建 SceneTextures | Yes |
| `FSceneRenderer::PostVisibilityFrameSetup(FILCUpdatePrimTaskData*&)` | `:5398` | 一帧最末期,ILC update、refl capture、light sort | Yes |
| `FDeferredShadingSceneRenderer::PreVisibilityFrameSetup` | `:5437` | 桌面端 override | No (Deferred only) |
| `FDeferredShadingSceneRenderer::BeginInitViews` | `:5453` | 桌面端 BeginInitViews | No |
| `FDeferredShadingSceneRenderer::EndInitViews` | `:5627` | 桌面端 EndInitViews | No |
| `FSceneRenderer::PrepareViewStateForVisibility` | `:4726` | 准备 view state(临时资源) | Yes |
| `IsLargeCameraMovement` | `:4620` | 相机位移检测,影响 occlusion 提交策略 | Yes |

### 4.2 Compute 系列 (compute relevance / pass mask)

| 函数 | 行号 | 说明 | Mobile 调用? |
|---|---|---|---|
| `FComputeAndMarkRelevance::ComputeRelevance` | `:1299` | **核心**:根据 `FPrimitiveViewRelevance` 把 primitive 分配到各个 `EMeshPass`,写 `PassMask` | Yes |
| `ComputeDynamicMeshRelevance` | `:2186` | 动态 mesh 的相关性计算 | Yes |
| `FDrawCommandRelevancePacket::AddCommandsForMesh` | `:1074` | 为单个静态 mesh 生成所有 pass 的 draw command | Yes |
| `FRelevancePacket::LaunchComputeRelevanceTask` | `:1161` | 调度 relevance 任务 | Yes |
| `FRelevancePacket::Finalize` | `:1196` | 完成 relevance packet | Yes |
| `FComputeAndMarkRelevance::AddPrimitive` / `AddPrimitives` | `:2021` / `:1981` | 入队 | Yes |
| `FComputeAndMarkRelevance::Finish` | `:2035` | 调度 compute task,等前置完成 | Yes |
| `FComputeAndMarkRelevance::Finalize` | `:2055` | 写结果到 view | Yes |

### 4.3 Init / Update 系列

| 函数 | 行号 | 说明 | Mobile 调用? |
|---|---|---|---|
| `FVisibilityViewPacket::BeginInitVisibility` | `:3506` | 单 View 的可见性初始化:分配 visibility map、更新 HLOD、构建 frustum flags、启动 frustum cull task | Yes |
| `FVisibilityTaskData::LaunchVisibilityTasks` | `:4089` | 全局入口,创建所有 View 的 packet,串接任务图 | Yes |
| `FVisibilityTaskData::SetupMeshPasses` | `:4401` | 把 visibility 结果转入 `FViewCommands`,调用 `SceneRenderer.SetupMeshPass` | Yes |
| `FVisibilityTaskData::ProcessRenderThreadTasks` | `:4472` | RT 端串行模式的主循环,处理 occlusion cull、relevance finalize、GDME | Yes |
| `FVisibilityTaskData::MergeSecondaryViewVisibility` | `:4263` | 多 viewport / 立体声场景下,合并次视图的可见性到主视图 | Yes |
| `FVisibilityTaskData::Finish` | `:4600` | 等待任务图、清理 | Yes |
| `UpdateAlwaysVisible` | `:634` | 永远可见 primitive(全屏 skybox 等)的可见性更新 | Yes |
| `FrustumCull` | `:687` | 视锥剔除主体 | Yes |
| `UpdatePrimitiveFading` | `:980` | LOD 淡入淡出状态更新 | Yes |
| `FViewInfo::UpdatePrimitiveFadingState` | `:1023` | 单 primitive 的淡入淡出状态机 | Yes |
| `ClearStalePrimitiveFadingStates` | `:893` | 清理不再可见的 primitive 的淡入淡出状态 | Yes |
| `FLODSceneTree::UpdateVisibilityStates` | `:5758` | HLOD 可见性状态机 | Yes |
| `FLODSceneTree::ClearVisibilityState` | `:5728` | 重置 HLOD 状态 | Yes |
| `FLODSceneTree::AddChildNode` / `RemoveChildNode` / `UpdateNodeSceneInfo` | `:5679/5703/5720` | HLOD 树结构维护 | Yes |
| `FSceneViewState::UpdateMotionBlurTimeScale` | `:5154` | 运动模糊时间尺度更新 | Yes |

### 4.4 Gather 系列 (动态 mesh 元素收集)

| 函数 | 行号 | 说明 | Mobile 调用? |
|---|---|---|---|
| `FVisibilityTaskData::GatherDynamicMeshElements(FDynamicPrimitiveIndexList&&)` | `:4288` | 通过 command pipe 调度的 GDME | Yes |
| `FVisibilityTaskData::GatherDynamicMeshElements(FDynamicPrimitiveViewMasks&)` | `:4341` | RT 直接调度版本 | Yes |
| `FDynamicMeshElementContext::GatherDynamicMeshElementsForPrimitive` | `:3844` | 单 primitive 的 GDME 入口 | Yes |
| `FDynamicMeshElementContext::GatherDynamicMeshElementsForEditorPrimitive` | `:3873` | Editor primitive 路径 | Yes (WITH_EDITOR) |
| `FDynamicMeshElementContext::Finish` | `:3881` | 提交 GDME 任务 | Yes |
| `FDynamicMeshElementContextContainer::Init` | `:3912` | 初始化 GDME 容器,创建 N 个 context | Yes |
| `FDynamicMeshElementContextContainer::MergeContexts` | `:3928` | 合并所有 context 的结果 | Yes |
| `FDynamicMeshElementContextContainer::Submit` | `:4062` | 提交 GDME 任务到 RT | Yes |

### 4.5 Mark 系列 (标记可见性)

| 函数 | 行号 | 说明 | Mobile 调用? |
|---|---|---|---|
| `FViewInfo::IsDistanceCulled` | `:392` | 距离剔除(单线程) | Yes |
| `FViewInfo::IsDistanceCulled_AnyThread` | `:406` | 距离剔除(任意线程) | Yes |
| `FrustumCull` (内部) | `:687` | 写 `View.PrimitiveVisibilityMap[i] = true/false` | Yes |
| `OcclusionCullPrimitive` (内部) | `:2454` | 写 `View.PrimitiveVisibilityMap[i] = false`(被遮挡) | Yes |
| `PrecomputedOcclusionCull` | `:3199` | 静态预计算遮挡 | Yes |
| `FSceneRenderer::DoOcclusionQueries` | `:323` | 是否做 GPU occlusion 的总开关 | Yes |

### 4.6 ComputeLightVisibility 系列

| 函数 | 行号 | 说明 | Mobile 调用? |
|---|---|---|---|
| `FSceneRenderer::ComputeLightVisibility` | `:5201` | 基类实现,无 shadow | Yes |
| `FDeferredShadingSceneRenderer::ComputeLightVisibility` | `:5192` | 桌面端 override,含 shadow | No (Deferred only) |
| `FSceneRenderer::GatherReflectionCaptureLightMeshElements` | `:5290` | 反射探针的 mesh elements 收集 | Yes |

### 4.7 Precomputed Occlusion

| 函数 | 行号 | 说明 | Mobile 调用? |
|---|---|---|---|
| `PrecomputedOcclusionCull` | `:3199` | 用 `View.PrecomputedVisibilityData` 做静态遮挡 | Yes |
| `STAT_PrecomputedOcclusionCull` | `:3209` | 性能标记 | Yes |

---

## 5. 与 MobileRender 的关联 (MobileRender Relationship)

### 5.1 守护代码的几种形式

`SceneVisibility.cpp` **不** 使用 `WITH_MOBILE_RENDERING` 宏守护(那是个 feature-level 宏,在编译时即定),而是用以下运行时判断:

| 判断 | 行号 | 用途 |
|---|---|---|
| `ShadingPath == EShadingPath::Mobile` | 多处 (`:1221/1320/1321/1423/1535/1559/1565/1912/2200/2228/2231/2236`) | 区分 pass 分配策略 |
| `IsMobilePlatform(Scene.GetShaderPlatform())` | `:3244/5582/5595` | 区分 occlusion / reflection capture 路径 |
| `View.bIsMobileMultiViewEnabled` | `:4211` | 校验多 View 路径兼容性 |
| `FeatureLevel <= ERHIFeatureLevel::ES3_1` | `:5218` | light shaft 等 ES3.1 限定功能 |

### 5.2 Mobile 端特定的简化

#### 5.2.1 Pass 分配简化

桌面端与 Mobile 端共享同一份 `FPrimitiveViewRelevance`,但 `ComputeRelevance` 内部会按 `ShadingPath` 调整:

```cpp
// :1221  - SecondStageDepthPass:桌面才有
WriteView.bUsesSecondStageDepthPass |= bUsesSecondStageDepthPass && ShadingPath != EShadingPath::Mobile;

// :1535 - 第二阶段深度
if (ViewRelevance.bRenderInSecondStageDepthPass && ShadingPath != EShadingPath::Mobile) { ... }

// :1559 - MobileBasePassCSM
if (ShadingPath == EShadingPath::Mobile) {
    if (StaticMeshRelevance.bUseForCSMDepthPass) {
        // 进 EMeshPass::MobileBasePassCSM
    }
}

// :2200 - 同 1535 的另一个位置
if (ViewRelevance.bRenderInSecondStageDepthPass && ShadingPath != EShadingPath::Mobile) { ... }

// :2228-2236 - MobileBasePassCSM 在 PassMask 阶段也单独设位
if (ShadingPath == EShadingPath::Mobile) {
    PassMask.Set(EMeshPass::MobileBasePassCSM);
    View.NumVisibleDynamicMeshElements[EMeshPass::MobileBasePassCSM] += NumElements;
}
```

#### 5.2.2 Occlusion Cull 串行化

```cpp
// :3244
if (!FApp::ShouldUseThreadingForPerformance() || !GIsThreadedRendering
 || !GSupportsParallelOcclusionQueries
 || GVisualizeOccludedPrimitives > 0
 || IsMobilePlatform(Scene.GetShaderPlatform()))
{
    // 走 FGPUOcclusionSerial
}
```

**Mobile 端强制用 `FGPUOcclusionSerial`**,原因是多数 Mobile GPU (尤其是 GLES) 不支持并行 occlusion query。

#### 5.2.3 反射捕获缓冲区分

```cpp
// :5582
if (IsMobilePlatform(ShaderPlatform))
{
    CreateReflectionCaptureUniformBuffer(SortedCaptures, MobileReflectionCaptureUniformBuffer);
}
else
{
    CreateReflectionCaptureUniformBuffer(SortedCaptures, ReflectionCaptureUniformBuffer);
}
```

Mobile 端用 `FMobileReflectionCaptureShaderData` (简化版),桌面端用 `FReflectionCaptureShaderData`。

#### 5.2.4 视口限制

```cpp
// :4211
checkf(!Views[0]->bIsMobileMultiViewEnabled, TEXT("This culling path was not tested with MMV"));
```

串行 RT 路径(`EVisibilityTaskSchedule::RenderThread`)不支持 Mobile Multi-View,使用并行路径才行。

#### 5.2.5 CSM 限制

Mobile 端只有一个方向光 CSM (`EMeshPass::MobileBasePassCSM`),且**只能从前向 view 求**:

```cpp
// :1320-1321
const bool bMobileMaskedInEarlyPass = (ShadingPath == EShadingPath::Mobile) && Scene.EarlyZPassMode == DDM_MaskedOnly;
const bool bMobileBasePassAlwaysUsesCSM = (ShadingPath == EShadingPath::Mobile) && MobileBasePassAlwaysUsesCSM(Scene.GetShaderPlatform());
```

### 5.3 共享 vs 独立的代码

| 部分 | 共享/独立 |
|---|---|
| `FrustumCull` (`:687`) | **共享**,无平台判断 |
| `PrecomputedOcclusionCull` (`:3199`) | **共享** |
| `OcclusionCullPrimitive` (`:2454`) | **共享**,但被不同 `FGPUOcclusion{Serial,Parallel}` 包装 |
| `FComputeAndMarkRelevance::ComputeRelevance` (`:1299`) | **共享**,内部按 `ShadingPath` 分支 |
| `FSceneRenderer::ComputeLightVisibility` (`:5201`) | 基类;`FDeferredShadingSceneRenderer` 覆盖 (`:5192`) |
| `BeginInitViews` | **独立**: `FDeferredShadingSceneRenderer` (`:5453`) vs `FMobileSceneRenderer` (在 `MobileShadingRenderer.cpp`) |
| `EndInitViews` | 同上 |
| `PrecomputedVisibilityBuffer` 创建 | **独立** (mobile 走精简路径) |
| `GatherDynamicMeshElements` | **共享** |

### 5.4 调用关系图

```
                ┌─────────────────────────────────────────────┐
                │     RDG Frame Graph (FSceneRenderer)         │
                └──────────┬──────────────────┬───────────────┘
                           │                  │
              ┌────────────▼───────┐  ┌───────▼────────────────┐
              │ MobileSceneRenderer│  │ DeferredShadingRenderer│
              │ .cpp               │  │ .cpp                   │
              └────────┬──────────┘  └────────┬───────────────┘
                       │                      │
                       └──────────┬───────────┘
                                  │
                                  ▼
                  FSceneRenderer::BeginInitViews (基类或覆盖)
                                  │
                                  ▼
                       LaunchVisibilityTasks  ◄── SceneVisibility.cpp:383
                                  │
                  ┌───────────────┼───────────────┐
                  ▼               ▼               ▼
          UpdateAlwaysVisible  FrustumCull   PrecomputedOcclusion
                  │               │               │
                  └─────┬─────────┴─────┬─────────┘
                        ▼               ▼
                  ComputeRelevance   GatherDynamicMeshElements
                        │               │
                        └───────┬───────┘
                                ▼
                       FSceneRenderer::SetupMeshPass
                                │
                                ▼
                  ParallelMeshDrawCommandPasses
                                │
                                ▼
                    RDG Passes (BasePass, Translucency, ...)
```

**Mobile vs 桌面 在 SceneVisibility 的分叉点**:

```
LaunchVisibilityTasks (共享)
    │
    ├── UpdateAlwaysVisible (共享)
    ├── FrustumCull (共享)
    ├── PrecomputedOcclusionCull (共享)
    ├── GPUOcclusion (分支!):
    │     ├── IsMobilePlatform  → FGPUOcclusionSerial
    │     └── 桌面              → FGPUOcclusionParallel
    ├── ComputeRelevance (共享,内部按 ShadingPath 分支)
    │     ├── Mobile  → 不设 bUsesSecondStageDepthPass
    │     ├── Mobile  → 设 EMeshPass::MobileBasePassCSM
    │     └── 桌面     → 设 EMeshPass::SecondStageDepthPass 等
    ├── ComputeLightVisibility (基类 / Deferred override)
    │     └── Mobile: bSetupMobileLightShafts 分支
    └── GatherDynamicMeshElements (共享)
```

---

## 6. 调用入口 (Call Sites)

### 6.1 哪些文件包含 SceneVisibility.h

```
Engine/Source/Runtime/Renderer/Private/SceneVisibility.h 的 #include 者:
```

| 文件 | 用途 |
|---|---|
| `Engine/Source/Runtime/Renderer/Private/MobileSceneRenderer.cpp` | 调用 `LaunchVisibilityTasks` |
| `Engine/Source/Runtime/Renderer/Private/DeferredShadingRenderer.cpp` | 同上 |
| `Engine/Source/Runtime/Renderer/Private/SceneRendering.h` (间接) | 通过 `FSceneRenderer` 接口 |
| 其它 `Renderer/Private/*.cpp` 偶尔间接依赖 | 主要是用 `FViewInfo` 字段 |

### 6.2 关键函数被谁调用

| 函数 | 调用者 |
|---|---|
| `LaunchVisibilityTasks` | `FSceneRenderer::BeginInitViews` (基类,被 Mobile/Deferred 共享);通过 `IVisibilityTaskData*` 接口回到 RT |
| `FVisibilityTaskData::ProcessRenderThreadTasks` | `IVisibilityTaskData::ProcessRenderThreadTasks` (调用者来自 `BeginInitViews` 之后) |
| `FVisibilityTaskData::FinishGatherDynamicMeshElements` | `IVisibilityTaskData::FinishGatherDynamicMeshElements` (BasePass 之前) |
| `FSceneRenderer::PreVisibilityFrameSetup` | `FSceneRenderer::Render` 内的第一阶段 |
| `FSceneRenderer::ComputeLightVisibility` | `FVisibilityTaskData::LaunchVisibilityTasks` (`:4201`) 通过 `UE::Tasks::Launch` 异步调用 |
| `FSceneRenderer::PostVisibilityFrameSetup` | `FSceneRenderer::Render` 内 RDG Pass 调度之后 |

### 6.3 在一帧中的触发时机

典型顺序(以桌面 PIE 一帧为例):

```
T0  GameThread: PrePhysicsTick / TickGroup TG_PrePhysics
T1  GameThread: TG_DuringPhysics -> TG_PostPhysics -> TG_LastDemotable
T2  GameThread: TG_PostUpdateWork -> TG_NewlySpawned
T3  GameThread: DrawGameElements (提交 Scene, BuildProxy)

--- RT 切到 RT ---

T4  RT:  FlushRenderingCommands -> ProcessSceneFamilies
T5  RT:  FSceneRenderer::Render() (总入口)
T6  RT:     PreVisibilityFrameSetup (SceneVisibility.cpp:4635)
T7  RT:     BeginInitViews (基类 + 覆盖)
                  → LaunchVisibilityTasks (SceneVisibility.cpp:383)
                     → FVisibilityTaskData::LaunchVisibilityTasks (SceneVisibility.cpp:4089)
                        调度: UpdateAlwaysVisible | FrustumCull | PrecomputedOcclusion | LightVisibility
T8  RT:     (异步) 任务图执行
T9  RT:     ProcessRenderThreadTasks (SceneVisibility.cpp:4472)
                  → 处理 OcclusionCull (HiZ query, 等 GPU)
                  → 处理 Relevance
                  → GatherDynamicMeshElements
T10 RT:     EndInitViews
T11 RT:     SetupMeshPasses (SceneVisibility.cpp:4401)
                  → 把 visible primitive 装入 ParallelMeshDrawCommandPasses
T12 RT:     RDG Passes 调度(BasePass, Lighting, Translucency, PostProcess)
T13 RT:     PostVisibilityFrameSetup (SceneVisibility.cpp:5398)
T14 RT:     提交 RHI CommandList 到 GPU
```

**关键观察**:

- `LaunchVisibilityTasks` 在 `T7` 调度任务,任务本身在 `T8` 异步执行。
- RT 在 `T9` 才回收任务结果(`ProcessRenderThreadTasks`)。
- HiZ query 在 `T8` 期间提交,GPU 完成时 RT 才能在 `T9` 读结果(`WaitOcclusionTests`)。
- `SetupMeshPasses` 之后,`FViewCommands` 中的 draw command 才能在 RDG Pass 中被消费。

---

## 7. 关键算法 (Key Algorithms)

### 7.1 Hierarchical Z Buffer (HiZ) Occlusion

#### 7.1.1 概述

HiZ 用**已渲染的 depth buffer 降采样到 8x8 像素 tile**,每个 tile 持有最大深度,作为"被遮挡"的快速查询。

**配置**:

- 关闭: `r.AllowOcclusionQueries 0`
- 可视化: `r.VisualizeOccludedPrimitives 1` (`:114`)

#### 7.1.2 流程

```
1. BasePass / DepthPass 渲染完成
2. 把 SceneDepth 降采样到 HZB (mip 0..N),mip 0 是 8x8 像素
3. FSceneTextures 持有 HZB 资源
4. SceneVisibility.cpp:OcclusionCullPrimitive:
   a) 对每个 primitive 的 bound,转 NDC
   b) 计算该 bound 覆盖哪些 8x8 tile
   c) 拿 HZB 中对应 tile 的 max depth
   d) 如果 bound 的 min depth > tile max depth → 该 primitive 被遮挡
5. 提交 query 到 GPU,下一帧 RT 端读结果
```

#### 7.1.3 关键代码位

- `:2454` `FGPUOcclusionPacket::OcclusionCullPrimitive`:核心判定循环。
- `:2786` `FProcessVisitor::AddOcclusionQuery`:批量提交。
- `:2811` `FProcessVisitor::SubmitThrottledOcclusionQueries`:throttle 防止一次提交太多。
- `:2919` `FGPUOcclusion::Map/Unmap`:RHI buffer 映射。
- `:2973` `FGPUOcclusion::WaitForLastOcclusionQuery`:等 GPU 完成。
- `:2926` `STAT_OcclusionFeedback_ReadbackResults`:反馈 readback 计时。
- `:2932` `STAT_MapHZBResults`:HZB 映射计时。

#### 7.1.4 Software vs Hardware Occlusion

| 模式 | 触发 | 说明 |
|---|---|---|
| **Hardware (HiZ)** | 默认,桌面 + 部分 mobile | GPU query,异步 |
| **Software (Precomputed)** | `Scene.PrecomputedVisibilityData` 有效 | 用预计算数据,CPU 端查表 (`:3199`) |
| **Feedback** | `r.OcclusionFeedback.Enable 1` (`:106`) | 异步 readback |

### 7.2 View-Frustum Culling

#### 7.2.1 八平面法

```cpp
// :3586
Flags.bUseFastIntersect = (View.ViewFrustum.PermutedPlanes.Num() == 8) && GFrustumCullUseFastIntersect;
```

8 平面 = 上/下/左/右/近/远 + 修正后的斜切平面。`FConvexVolume::IntersectBox` 提供 OBB vs 8 平面求交。

#### 7.2.2 球优先快路径

`bUseSphereTestFirst` (`:270`, 球 vs 锥,O(1) 测试),若通过才进 OBB 测试。

#### 7.2.3 Octree 加速

```cpp
// :3587
Flags.bUseVisibilityOctree = GFrustumCullUseOctree;
```

`CullOctree` (`:590`) 递归把 Octree 节点与 frustum 求交,把可见节点加到 `FSceneBitArray OutVisibleNodes`,然后 `FrustumCull` (`:687`) 只对属于这些节点的 primitive 做精细判定。

#### 7.2.4 任务并行

`FrustumCull` (`:687`) 内部按 `TaskConfig.FrustumCull.NumPrimitivesPerTask` 切片,生成 N 个 task,每个 task 处理一段 `[Start, End)` 索引区间。

### 7.3 距离剔除 (Distance Culling)

```cpp
// :392-490 简化版
bool FViewInfo::IsDistanceCulled(float DistanceSquared, float MinDrawDistance, float InMaxDrawDistance, const FPrimitiveSceneInfo* PrimitiveSceneInfo) const
{
    // 1. 总是显示标记 (例如 skybox): 不剔除
    if (PrimitiveSceneInfo->bAlwaysHasVelocity) return false;
    
    // 2. MinDrawDistance: 离太近
    if (MinDrawDistanceSquared > DistanceSquared) return true;
    
    // 3. MaxDrawDistance × ViewDistanceScale: 离太远
    if (MaxDrawDistance > 0.0f)
    {
        const float ScaledMaxDrawDistance = MaxDrawDistance * MaxDrawDistanceScale;
        if (DistanceSquared > ScaledMaxDrawDistance * ScaledMaxDrawDistance) return true;
    }
    
    // 4. LOD fade: 在 fade 区间内,返回 true 表示"暂时不算可见,等淡入"
    ...
}
```

关键 CVar:

- `r.DisableLODFade` (`:223`)
- `r.LODFadeTime` (`:226`)
- `r.DistanceFadeMaxTravel` (`:229`)

`MaxDrawDistanceScale` 在 `BeginInitVisibility` (`:3556`) 计算,影响所有距离判定。

### 7.4 场景的空间结构

#### 7.4.1 Octree

`FScene::Octree` (`Scene.Octree` 类型 `FSceneOctree`),由 `FScene::AddPrimitive` 维护。`GFrustumCullUseOctree` 控制是否在视锥剔除中使用。

#### 7.4.2 HLOD Tree

`FLODSceneTree` (`Scene.SceneLODHierarchy`),由 `AddChildNode` (`:5679`) 等维护。`UpdateVisibilityStates` (`:5758`) 在每帧 `BeginInitVisibility` 中更新,产生 `FHLODVisibilityState`,该 state 被 `FrustumCull` 用来在距离剔除时把不可见的 HLOD 子节点屏蔽。

#### 7.4.3 Primitive ComponentId 索引

`Scene.PrimitiveComponentIds[i]` 数组允许按 `FPrimitiveComponentId` 反查 index,被 `FrozenPrimitives` 和 `OcclusionCullPrimitive` 使用。

### 7.5 Always Visible 快路径

```cpp
// :634-687
static void UpdateAlwaysVisible(...)
{
    // 永远可见的 primitive (sky, 一些 UI 等)
    // 直接设 View.PrimitiveVisibilityMap[i] = true
    // 不进 frustum cull task,显著减少任务数
}
```

`View.PrimitiveAlwaysVisible[i]` 或 `bAlwaysHasVelocity` 等标志位决定哪些 primitive 走此路径。

### 7.6 Show Only / Hidden 过滤

```cpp
// :3588-3589
Flags.bHasHiddenPrimitives   = View.HiddenPrimitives.Num() > 0;
Flags.bHasShowOnlyPrimitives = View.ShowOnlyPrimitives.IsSet();
```

`FrustumCull` 内部在写 `PrimitiveVisibilityMap` 之前先检查 `HiddenPrimitives.Contains(...)` 或 `ShowOnlyPrimitives.Contains(...)`。

### 7.7 Frozen Visibility (Editor)

```cpp
// :3538-3554
#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
if (ViewState && ViewState->bIsFrozen)
{
    bShouldVisibilityCull = false;
    for (...)
    {
        if (ViewState->FrozenPrimitives.Contains(...))
        {
            View.PrimitiveVisibilityMap[Index] = true;
        }
    }
}
#endif
```

冻结后,跳过所有 cull,直接用上一帧记录的可见集。

### 7.8 ComputeViewVisibility → ComputeAndMarkRelevance

**ComputeAndMarkRelevance** 是"标记到 Mesh Pass"的核心:

```cpp
// :1299-1980 简化伪代码
void FComputeAndMarkRelevance::ComputeRelevance(FDynamicPrimitiveIndexList& DynamicPrimitiveIndexList)
{
    for (each visible primitive) {
        const FPrimitiveSceneProxy* Proxy = Scene.PrimitiveSceneProxies[Index];
        const FPrimitiveViewRelevance& VR = View.PrimitiveViewRelevanceMap[Index];
        FMeshPassMask PassMask;
        
        if (VR.bRenderInMainPass || VR.bRenderCustomDepth) {
            PassMask.Set(EMeshPass::BasePass);
            ...
        }
        if (VR.bRenderInDepthPass) { ... }
        if (VR.bDrawInTranslucency) { ... }
        // ... 20+ Pass
        for (each static mesh batch) {
            FDrawCommandRelevancePacket::AddCommandsForMesh(...)
        }
    }
}
```

每个 primitive 写一个 `FMeshPassMask`,每个被 set 的 bit 对应一个 Mesh Pass 队列。最终 `SetupMeshPass` 把 `FMeshPassMask` 翻译成 `FViewCommands::MeshCommands[EMeshPass::Xxx]`。

---

## 8. 调试与性能 (Debugging & Performance)

### 8.1 关键 Stat 标记 (Cycle Counters)

| Stat | 行号 | 含义 |
|---|---|---|
| `STAT_ViewVisibilityTime` | `:4474` | 整段 visibility 时间 |
| `STAT_ComputeViewRelevance` | `:1302` | compute relevance 时间 |
| `STAT_InitViewsTime` | `:5461` | BeginInitViews 整段 |
| `STAT_InitViewsPossiblyAfterPrepass` | `:5634` | EndInitViews 整段 |
| `STAT_ComputeAndMarkRelevanceForViewParallel_Finalize` | `:2075` | relevance finalize (parallel) |
| `STAT_ComputeAndMarkRelevanceForViewParallel_TransposeMeshBits` | `:2135` | mesh bit 转置 (parallel) |
| `STAT_AlwaysVisible_Loop` | `:636` | always-visible primitive 循环 |
| `STAT_FrustumCull_Loop` | `:689` | frustum cull 循环 |
| `STAT_UpdateAlwaysVisible` | `:3623` | task 形式 always-visible |
| `STAT_UpdatePrimitiveFading` | `:987` | LOD 淡入淡出更新 |
| `STAT_OcclusionFeedback_ReadbackResults` | `:2926` | occlusion feedback readback |
| `STAT_MapHZBResults` | `:2932` | HZB 映射 |
| `STAT_PrecomputedOcclusionCull` | `:3209` | 预计算 occlusion |
| `STAT_DecompressPrecomputedOcclusion` | `:4107` | precomputed visibility 解压 |
| `STAT_PostVisibilityFrameSetup` | `:5405` | post-vis 阶段整段 |
| `STAT_PostVisibilityFrameSetup_Light_Visibility` | `:5203` | light visibility 部分 |
| `STAT_PostVisibilityFrameSetup_IndirectLightingCache_Update` | `:5648` | ILC update |
| `STAT_InitViews_InitRHIResources` | `:5524` | RHI 资源初始化 |
| `STAT_InitViews_UpdatePrimitiveIndirectLightingCacheBuffers` | `:5660` | ILC buffer 更新 |

**用法**: `stat sceneRendering` 或 `stat unit` + `stat startfile/stopfile`。

### 8.2 关键 CVar 速查

| CVar | 含义 | 推荐值 |
|---|---|---|
| `r.AllowFrustumCull` | 总开关 | 1(默认) |
| `r.CullWithOctree` | Octree 加速 | 1(场景大时) |
| `r.CullWithSphereTestFirst` | 球测试优先 | 1(经验上) |
| `r.Cull.WithFastIntersect` | 8 平面 fast intersect | 0/1 都行 |
| `r.FrustumCullNumPrimitivesPerTask` | 任务粒度 | 0(自动) |
| `r.AllowOcclusionQueries` | 总开关 | 1 |
| `r.OcclusionFeedback.Enable` | 反馈机制 | 0(默认,实验) |
| `r.VisualizeOccludedPrimitives` | 可视化 | 0/1 |
| `r.AllowSubPrimitiveQueries` | sub-primitive query | 1(默认) |
| `r.FramesNotOcclusionTestedToExpandBBoxes` | 遮挡扩展 | 5 |
| `r.FramesToExpandNewlyOcclusionTestedBBoxes` | 新增遮挡扩展 | 2 |
| `r.ExpandNewlyOcclusionTestedBBoxesAmount` | 扩展量 | 0.0 |
| `r.ExpandAllTestedBBoxesAmount` | 全局扩展量 | 0.0 |
| `r.NeverOcclusionTestDistance` | 永不测距 | 0.0 |
| `r.DisableLODFade` | 关闭 LOD fade | 0 |
| `r.LODFadeTime` | fade 时间 | 0.25s |
| `r.DistanceFadeMaxTravel` | fade 距离 | 1000.0 |
| `r.CameraCutTranslationThreshold` | 相机切割阈值 | 10000.0 |
| `r.EarlyInitDynamicShadows` | 早期 init 阴影 | 1 |
| `r.ILCUpdatePrimTaskEnabled` | ILC update 任务 | 1 |
| `r.VisibilityTaskSchedule` | 任务调度模式 | 1(parallel) |
| `r.OcclusionCullMaxQueriesPerTask` | occlusion 任务粒度 | 0(自动) |
| `r.NumDynamicMeshElementTasks` | GDME 任务数 | 4 |
| `r.RelevanceNumPrimitivesPerPacket` | relevance 任务粒度 | 0(自动) |
| `r.MinScreenRadiusForLights` | 灯最小屏幕半径 | -1 |
| `r.MinScreenRadiusForDepthPrepass` | depth prepass 最小半径 | 0.03 |
| `r.MinScreenRadiusForCSMDepth` | CSM 最小半径 | 0.05 |
| `r.WireframeCullThreshold` | 线框 cull 阈值 | 5.0 |
| `r.ForceSceneHasDecals` | 强制场景有 decals | 0 |
| `r.TemporalAASamples` | TAA 样本 | 1 |
| `r.Mobile.MinAutomaticViewMipBias` | mobile view mip bias | 0 |
| `r.Mobile.MinAutomaticViewMipBiasOffset` | mobile view mip bias offset | 0.5 |
| `r.SceneVisibility.TaskPoolSize` | (间接) | - |
| `r.ViewDistanceScale` | 全局视距缩放 | 1.0 |
| `r.Light.MaxDrawDistanceScale` | 灯最大绘制距离缩放 | 1.0 |

### 8.3 性能瓶颈位置

按典型场景的瓶颈出现频率排序:

1. **`FDrawCommandRelevancePacket::AddCommandsForMesh` (`:1074`)** — 静态 mesh 在 N 个 pass 上生成 draw command,N × Visible 静态 mesh 数 = 大量工作。**优化**:合并 material、减少 pass 数。
2. **`FComputeAndMarkRelevance::ComputeRelevance` (`:1299`)** — 标记 pass mask,逐 primitive 大量分支。**优化**:减少 irrelevant primitive 进 visibility。
3. **`FrustumCull` (`:687`)** — 视锥剔除,O(N) 遍历 Scene.Primitives。**优化**:开 octree (`r.CullWithOctree 1`)。
4. **`FGPUOcclusionPacket::OcclusionCullPrimitive` (`:2454`)** — HZB 判定,sub-primitive 时尤其重。**优化**:关 `r.AllowSubPrimitiveQueries 0`。
5. **`GatherDynamicMeshElements` (`:4288/4341`)** — 动态 mesh 同步,涉及 RT 阻塞。**优化**:减少 dynamic mesh count,优先 static + GPU cull。
6. **`PrecomputedOcclusionCull` (`:3199`)** — 预计算 occlusion,数据量大时解压慢。**优化**:减少 precomputed visibility cells。
7. **`FSceneRenderer::ComputeLightVisibility` (`:5201`)** — 灯可见性,影响阴影 cull。**优化**:减少 dynamic shadow casters。
8. **`HLOD Update` (`:5758`)** — HLOD 树遍历,场景大时慢。

### 8.4 调试视图

| 命令 | 作用 |
|---|---|
| `r.VisualizeOccludedPrimitives 1` | 把被 occlusion 剔除的 primitive 标红 |
| `Show flag "Visibility" / "Primitive Bounds"` | 显示 primitive 边界 |
| `r.AllowFrustumCull 0` | 临时关闭视锥剔除,看是否因它导致消失 |
| `r.AllowOcclusionQueries 0` | 临时关闭 occlusion |
| `stat sceneRendering` | 显示 SceneRendering stat group |
| `stat GPU` | 看 GPU 时间,HiZ 提交/读取开销 |
| `stat RHI` | RHI 层 detail |
| `profile GPU` (RenderDoc) | 单帧 detail |
| `r.ScreenPercentage` | 调小有助于定位 pixel-bound 场景 |
| `Pause` + ViewState "freeze visibility" | 冻结当前可见性,排查后续 pass |

### 8.5 常见问题速查

| 现象 | 可能原因 | 调试步骤 |
|---|---|---|
| 物体在画面中"消失" | Frustum cull 错误 | `r.AllowFrustumCull 0` |
| 物体突然出现 | Occlusion 误判 | `r.AllowOcclusionQueries 0` |
| LOD 切换不平滑 | Distance cull / fade | `r.DisableLODFade 0`, `r.LODFadeTime` |
| 编辑器视图正常,PIE 视图消失 | `ViewState` 差异 | 检查 `bIsFrozen` |
| 动态阴影乱跳 | Light visibility / shadow cull | `r.EarlyInitDynamicShadows 0` |
| 移动端物体闪烁 | 任务调度问题 | `r.VisibilityTaskSchedule 0` (强制串行) |
| ISM 实例消失 | GPU instance cull 阈值 | 检查 `View.InstanceCullingDrawParams` |
| 反射捕获缺失 | reflection buffer 没创建 | `r.ReflectionCapture.Enable 1` |

---

## 9. 修改扩展指南

### 9.1 新增 Mesh Pass 时

要改的位置:

1. `MeshPassProcessor.h:32-79` `EMeshPass::Type` 枚举加值(注意更新 `NumBits`)。
2. `GetMeshPassName` switch 加 case。
3. `static_assert(EMeshPass::Num == ...)` 更新。
4. **`SceneVisibility.cpp:ComputeRelevance` (`:1299`)** — 在 `PassMask.Set(EMeshPass::YourNewPass)` 写位。
5. 创建 `FYourNewMeshPassProcessor`,注册到 `REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR` 宏。
6. 在 `MobileSceneRenderer.cpp` / `DeferredShadingRenderer.cpp` 的 RDG pass 调度中调用 `View.ParallelMeshDrawCommandPasses[EMeshPass::YourNewPass].DispatchDraw(...)`。

### 9.2 新增视锥剔除规则时

1. 修改 `FFrustumCullingFlags` (匿名,在 `BeginInitVisibility` 里) 加新 flag。
2. 在 `FrustumCull` (`:687`) 内检查新 flag,执行新逻辑。
3. 如果新规则依赖 View 上的数据,加到 `FViewInfo`。

### 9.3 新增遮挡规则时

1. 派生 `FGPUOcclusion` 新子类(或扩展 `FGPUOcclusionPacket`)。
2. 在 `BeginInitVisibility` 中根据条件选 `Serial` / `Parallel` / 新类。
3. 新增 CVar 控制。

### 9.4 调试新 Mesh Pass

1. `stat sceneRendering` 找到 `STAT_ComputeViewRelevance`,确认新 pass 的 command count。
2. `profile GPU` 在 RenderDoc 中找对应 draw call 标签。
3. 在 RDG Pass 中加 `RDG_EVENT_SCOPE` 让 RenderDoc 显示名字。

---

## 10. 版本与变更记录

| 版本 | 日期 | 内容 |
|---|---|---|
| v1.0 | 2026-06-18 | 初版:基于 UE5.4 源码完整解析 SceneVisibility.cpp / .h / Private.h |

---

## 11. 附录

### 11.1 关键源码引用

| 文件:行号 | 内容 |
|---|---|
| `SceneVisibility.cpp:383` | 顶层入口 `LaunchVisibilityTasks` |
| `SceneVisibility.cpp:687` | 视锥剔除 `FrustumCull` |
| `SceneVisibility.cpp:1074` | `AddCommandsForMesh` |
| `SceneVisibility.cpp:1161` | `LaunchComputeRelevanceTask` |
| `SceneVisibility.cpp:1299` | `ComputeRelevance` 核心 |
| `SceneVisibility.cpp:2186` | `ComputeDynamicMeshRelevance` |
| `SceneVisibility.cpp:2454` | `OcclusionCullPrimitive` (HZB) |
| `SceneVisibility.cpp:2786` | `AddOcclusionQuery` |
| `SceneVisibility.cpp:3199` | `PrecomputedOcclusionCull` |
| `SceneVisibility.cpp:3506` | `BeginInitVisibility` |
| `SceneVisibility.cpp:4089` | `FVisibilityTaskData::LaunchVisibilityTasks` |
| `SceneVisibility.cpp:4288` / `:4341` | `GatherDynamicMeshElements` (两版本) |
| `SceneVisibility.cpp:4401` | `SetupMeshPasses` |
| `SceneVisibility.cpp:4472` | `ProcessRenderThreadTasks` |
| `SceneVisibility.cpp:4635` | `PreVisibilityFrameSetup` (基类) |
| `SceneVisibility.cpp:5192` | `FDeferredShadingSceneRenderer::ComputeLightVisibility` |
| `SceneVisibility.cpp:5201` | `FSceneRenderer::ComputeLightVisibility` |
| `SceneVisibility.cpp:5398` | `PostVisibilityFrameSetup` |
| `SceneVisibility.cpp:5437` | `FDeferredShadingSceneRenderer::PreVisibilityFrameSetup` |
| `SceneVisibility.cpp:5453` | `FDeferredShadingSceneRenderer::BeginInitViews` |
| `SceneVisibility.cpp:5627` | `FDeferredShadingSceneRenderer::EndInitViews` |
| `SceneVisibility.cpp:5679-5728` | `FLODSceneTree` 系列 |
| `SceneVisibility.cpp:5758` | `FLODSceneTree::UpdateVisibilityStates` |
| `SceneVisibility.h:32` | `IVisibilityTaskData` 接口 |
| `SceneVisibility.h:69` | `LaunchVisibilityTasks` 声明 |
| `SceneVisibilityPrivate.h:151` | `FPrimitiveRange` |
| `SceneVisibilityPrivate.h:178` | `FDynamicPrimitiveIndexList` |
| `SceneVisibilityPrivate.h:248` | `FDynamicMeshElementContext` |
| `SceneVisibilityPrivate.h:345` | `FVisibilityTaskConfig` |
| `SceneVisibilityPrivate.h:419` | `FVisibilityViewPacket` |
| `SceneVisibilityPrivate.h:499` | `FVisibilityTaskData` |
| `SceneVisibilityPrivate.h:665` | `EMarkMaskBits` |
| `SceneVisibilityPrivate.h:700` | `FRelevancePacket` |
| `SceneVisibilityPrivate.h:792` | `FComputeAndMarkRelevance` |
| `SceneVisibilityPrivate.h:837-916` | Occlusion 相关结构 |
| `SceneVisibilityPrivate.h:1089-1186` | `FGPUOcclusion` 三类 |

### 11.2 配套参考

- `AllwayRenderFront.md` — Mobile Forward 延后渲染方案(`PassMask.Set(EMeshPass::AllwayFrontPass)` 调用点即在 `SceneVisibility.cpp:2228` 附近)。
- `MobileShadingRenderer.cpp` — Mobile 端 RDG 调度入口。
- `DeferredShadingRenderer.cpp` — 桌面端 RDG 调度入口。
- `MeshPassProcessor.h` — `EMeshPass::Type` 枚举与 `GetMeshPassName` switch。
- `PrimitiveViewRelevance.h` — `FPrimitiveViewRelevance` 标志位。
- `ScenePrivate.h` — `FScene::Primitives/StaticMeshes/Octree/SceneLODHierarchy`。

---

**注意**: 本文档基于 UE5.4 源码 (Engine path: `E:/Unreal Engine Work Projects/MR01_DaNaoTianGong_Main/Engine`)。实施修改前请:
1. 备份 `Engine/Source/Runtime/Renderer/Private/SceneVisibility.{cpp,h,Private.h}`。
2. 在独立分支开发,避免与 Epic 上游冲突。
3. 每次改动后跑 `EngineTest`(含 `TestCull...` 系列)验证。
4. RenderDoc 抓帧对比,确认 `PrimitiveVisibilityMap` 行为符合预期。
5. Profile `STAT_ViewVisibilityTime`、`STAT_ComputeViewRelevance`,确保不退化。
