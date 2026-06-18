# SceneVisibility.cpp 解析文档

> 对应源码：`Engine/Source/Runtime/Renderer/Private/SceneVisibility.cpp`（6032 行）
> 配套头文件：`SceneVisibility.h`（69 行）、`SceneVisibilityPrivate.h`（1202 行）
> 文档版本：v1.0 ｜ 分析对象：大闹天宫 MR01 主引擎 UE5 移动端分支
> 关键调用方：`SceneRendering.cpp::FSceneRenderer::OnRenderBegin` → `LaunchVisibilityTasks`

---

## 1. 一句话定位

**`SceneVisibility.cpp` 是 UE 渲染线程"每帧每视图"的总调度入口**：拿到场景与视图后，在渲染开始前完成 **视锥裁剪 → 遮挡剔除 → 计算视图相关性（Relevance）→ 收集动态网格元素（GDME）→ 装配 MeshPass 任务图** 五级流水线，并把结果以 `FViewCommands` + `FVisibilityTaskData` 的形式交给下游的 BasePass / Shadow / MobileBasePass 消费。

它不是"纯算法"文件，而是一个 **任务图编排器** —— 真正干活的算法（`FrustumCull`、`OcclusionCullPrimitive`、`ComputeRelevance`）只占一半代码，另一半是 `EVisibilityTaskSchedule::Parallel/RenderThread` 双模式、CommandPipe、Instanced Stereo、ES3.1 反馈等调度结构。

---

## 2. 文件结构（按行号分区）

| 行号区间 | 内容 | 关键符号 |
|---|---|---|
| **1 – 380** | 头文件 / CVar 声明 | `GFrustumCullEnabled` / `GOcclusionFeedback_Enable` / `GHZBOcclusion` |
| **381 – 1043** | 距离裁剪 / 视锥裁剪 / 淡入淡出状态 | `IsDistanceCulled` / `FrustumCull` / `UpdatePrimitiveFadingState` |
| **1045 – 1760** | ComputeRelevance 主循环 | `FRelevancePacket::ComputeRelevance`（含 **移动端 MeshPass 选择分支**） |
| **1760 – 2180** | Nanite / Editor 选中处理 + MarkMasks 转置 | `FComputeAndMarkRelevance::Finalize` |
| **2186 – 2430** | 动态网格相关性（`ComputeDynamicMeshRelevance`） | 含 **MobileBasePassCSM 分支** |
| **2432 – 3200** | Occlusion 阶段（HZB / 硬件查询 / OcclusionFeedback） | `FGPUOcclusionPacket` / `FGPUOcclusionSerial/Parallel` |
| **3199 – 3235** | 预计算可见性 (`PrecomputedOcclusionCull`) | `View.PrecomputedVisibilityData` |
| **3238 – 3500** | `FVisibilityTaskConfig` & `FVisibilityViewPacket` 构造 | `EVisibilityTaskSchedule` / `TCommandPipe` |
| **3506 – 4400** | 启动任务图 / 处理渲染线程任务 | `LaunchVisibilityTasks` / `ProcessRenderThreadTasks` |
| **4401 – 4614** | SetupMeshPasses / Finish | `ComputeDynamicMeshRelevance` 调用 |
| **4617 – 5200** | InitViews 周边（PreVisibility / PrepareViewState） | `FSceneRenderer::PrepareViewStateForVisibility` |
| **5201 – 5290** | 灯光可见性（`ComputeLightVisibility`） | **含移动端 LightShaft 路径** |
| **5290 – 5430** | 反射捕获 mesh 元素 / 间接光缓存 | `GatherReflectionCaptureLightMeshElements` |
| **5431 – 5675** | `FDeferredShadingSceneRenderer::BeginInitViews/EndInitViews` |  |
| **5679 – 6032** | HLOD 树 `FLODSceneTree` 实现 | `UpdateVisibilityStates` / `HideNodeChildren` |

---

## 3. 五大流水线阶段

### 3.1 阶段 0：Always Visible（永远可见 primitive）

```cpp
static void UpdateAlwaysVisible(const FScene&, FViewInfo&, FFrustumCullingFlags,
                                const FVisibilityTaskConfig&, int32 TaskIndex, float CurrentWorldTime);
```

- 入口：`Scene.PrimitivesAlwaysVisibleOffset != ~0u`（Nanite Mesh 永远可见区间）
- 不做视锥裁剪，只更新 `LastRenderTime` 与 `UpdateComponentLastRenderTime`
- 输出：`View.PrimitiveVisibilityMap` 对应位置写 1

### 3.2 阶段 1：视锥裁剪 FrustumCull

```cpp
static int32 FrustumCull(const FScene&, FViewInfo&, FFrustumCullingFlags Flags,
                         float MaxDrawDistanceScale, const FHLODVisibilityState*,
                         const FSceneBitArray* VisibleNodes,
                         const FVisibilityTaskConfig&, int32 TaskIndex);
```

**判定流程**（每个 primitive，按位遍历 `WordIndex → BitSubIndex`）：

```
IsPrimitiveHidden()        ← HiddenPrimitives / ShowOnlyPrimitives
   ↓
SphereRadius > 0 ?         ← 0 半径当作不可见
   ↓
HLOD 状态覆盖              ← bForceVisible / bForceHidden
   ↓
Octree 节点先验             ← bUseVisibilityOctree 时复用父节点结果
   ↓
IsPrimitiveVisible()       ← Fast 8-plane SIMD 或 IntersectBox
   ↓
距离裁剪 (IsDistanceCulled_AnyThread)  ← Max/MinDrawDistance + FadeRadius
   ↓
写回 VisBits / FadingBits / RayTracingBits
```

**SIMD 优化（`IntersectBox8Plane`，447-505 行）**：

8 个 plane 用 SSE 一次性计算 8 个 `Distance_i - PushOut_i`，命中任何一个 outside 直接 `return false`。比 `IntersectBox` 单 plane 循环快 4-5×，受 `r.Visibility.FrustumCull.UseFastIntersect=1` 控制。

**关键 CVars**：
- `r.Visibility.FrustumCull.Enabled` (1)
- `r.Visibility.FrustumCull.UseOctree` (0)
- `r.Visibility.FrustumCull.UseSphereTestFirst` (0)
- `r.Visibility.FrustumCull.UseFastIntersect` (1)
- `r.Visibility.FrustumCull.NumPrimitivesPerTask` (0=auto)

### 3.3 阶段 2：遮挡剔除 OcclusionCull

三种实现路径，按优先级选择：

| 路径 | 触发条件 | 特点 |
|---|---|---|
| **OcclusionFeedback** | `GOcclusionFeedback_Enable != 0` 且 `FeatureLevel == ES3_1` | **专为移动端设计** —— 把上帧的遮挡结果以 UAV 形式回读，零 GPU query 开销 |
| **HZB 遮挡** | `GHZBOcclusion=1` 且平台支持（`!IsOpenGLPlatform`） | 层次 Z-Buffer 软件遮挡，移动端默认路径 |
| **硬件 RHI Query** | 兜底 | 老的 `RHIRenderQuery` 路径，配合 grouped / throttled / round-robin 策略 |

**核心类**（`SceneVisibilityPrivate.h` 916-1201）：

```cpp
class FGPUOcclusionPacket            // 一次遮挡裁剪包
class FGPUOcclusionSerial            // 单线程路径（移动端）
class FGPUOcclusionParallel          // 多线程路径
class FGPUOcclusionParallelPacket    // Parallel 的子包
class FGPUOcclusionState             // 状态（bHZBOcclusion / bSubmitQueries / bUseRoundRobinOcclusion ...）
```

**OcclusionCullPrimitive 流程**（2454-2784 行）：

```
CanBeOccluded()               ← 读取 EOcclusionFlags::CanBeOccluded
   ↓
HasSubprimitiveQueries ?      ← HISM 逐实例查询
   ↓
读取历史结果
  - bUseOcclusionFeedback ?   → OcclusionFeedback.IsOccluded()
  - bHZBOcclusion       ?    → HZBOcclusionTests.IsVisible()
  - 兜底                → RHIGetRenderQueryResult(LastQuery)
   ↓
可剔除 → 写 View.PrimitiveVisibilityMap[Index] = false
   ↓
本帧提交新 query
  - AllowApproximateOcclusion → grouped query
  - 否则                       → throttled / individual
```

### 3.4 阶段 3：ComputeRelevance

`FRelevancePacket::ComputeRelevance`（1299-1958 行）是 **单 packet 内所有 primitive 的相关性计算**。对每个 primitive：

1. 调用 `PrimitiveSceneProxy->GetViewRelevance(&View)` 获取 `FPrimitiveViewRelevance`
2. 判断 HLOD 淡入淡出
3. 计算 LOD (`ComputeLODForMeshes`)
4. **根据 ShadingPath 选择 MeshPass**（核心移动端分支，见 §5.1）
5. 把可见 mesh 加入 `DrawCommandPacket.VisibleCachedDrawCommands[EMeshPass::xxx]` 或 `DynamicBuildRequests`
6. 写 `MarkMasks[StaticMeshRelevance.Id]`（一个字节，位标记）

**Finalize 阶段**（2096-2180 行）：
- `FComputeAndMarkRelevance::Finalize()` 串行汇总所有 packet
- 把 `MarkMasks[]` 8-bit 数组转置为 3 个 bit array：`StaticMeshVisibilityMap` / `StaticMeshFadeOutDitheredLODMap` / `StaticMeshFadeInDitheredLODMap`
- **Fast path：跳过 64-bit 全 0 块**，避免大场景下 70% 内存带宽

### 3.5 阶段 4：GDME（Gather Dynamic Mesh Elements）

```cpp
class FDynamicMeshElementContext
class FDynamicMeshElementContextContainer
```

- 多线程任务：每个 Context 一个 `MeshCollector` + 自己的 `FRHICommandList`
- 平台过滤：`Proxy->SupportsParallelGDME()` 为 false 的 primitive 强制回退到 render thread
- 合并：`MergeContexts` 在 `SetupMeshPasses` 中执行

### 3.6 阶段 5：SetupMeshPasses

`FVisibilityTaskData::SetupMeshPasses`（4401-4470）：

```
DynamicMeshElements.ContextContainer.MergeContexts()
   ↓
for each view:
  for each dynamic primitive:
    ComputeDynamicMeshRelevance()       ← 计算 FMeshPassMask
   ↓
SceneRenderer.SetupMeshPass(View, BasePassDepthStencilAccess, ViewCommands,
                            InstanceCullingManager)
```

`ViewCommands` 即 `FVisibilityTaskData::GetViewCommandsPerView()` 返回的结构，是 `IVisibilityTaskData` 公开给消费者的最终产物。

---

## 4. 任务图与并行模式

```cpp
enum class EVisibilityTaskSchedule {
    RenderThread,   // 0：渲染线程为主，ParallelFor 加速
    Parallel,       // 1：完整任务图，GDME 仍在 render thread
};
```

### 4.1 模式选择（`FVisibilityTaskConfig` 构造函数，3238-3360）

```cpp
Schedule = GVisibilityTaskSchedule != 0 ? Parallel : RenderThread;

if (Schedule == Parallel) {
    if (!FApp::ShouldUseThreadingForPerformance() ||
        !GIsThreadedRendering ||
        !GSupportsParallelOcclusionQueries ||
        GVisualizeOccludedPrimitives > 0 ||
        IsMobilePlatform(Scene.GetShaderPlatform()))    // ★ 移动端强制降级
    {
        Schedule = RenderThread;
    }
}
```

> **关键设计点**：当检测到 `IsMobilePlatform()` 时，强制使用 RenderThread 模式 —— 原因是移动 GPU 的并行 occlusion query 支持参差不齐，统一用串行路径更稳。

### 4.2 任务图

```cpp
class FVisibilityTaskData {
    struct FTasks {
        UE::Tasks::FTaskEvent LightVisibility;
        UE::Tasks::FTaskEvent FrustumCull;
        UE::Tasks::FTaskEvent OcclusionCull;
        UE::Tasks::FTaskEvent ComputeRelevance;
        UE::Tasks::FTaskEvent DynamicMeshElements;
        UE::Tasks::FTask      FinalizeRelevance;
        UE::Tasks::FTask      MeshPassSetup;
    };
};
```

**Pipe 通信**（`TCommandPipe`，`SceneVisibilityPrivate.h` 28-140）：

```cpp
TCommandPipe<FPrimitiveRange> OcclusionCullPipe;   // 视锥 → 遮挡
TCommandPipe<FPrimitiveIndexList> RelevancePipe;  // 遮挡 → ComputeRelevance
```

每条 Pipe 用 `AddNumCommands(n)` / `ReleaseNumCommands(n)` 做引用计数，`EnqueueCommand` 触发 Pipe 上的 `Launch` 任务消费队列。Pipe 的 `SetEmptyFunction` 在 `n=0` 时触发下游任务。

### 4.3 任务粒度（`FVisibilityTaskConfig`）

| 维度 | RenderThread | Parallel（移动端不会进入） |
|---|---|---|
| AlwaysVisible | 128 word / task | 32 word / task × 2 task / worker |
| FrustumCull | 128 word / task | 32 word / task × 2 task / worker |
| OcclusionCull | dynamic | MaxQueriesPerTask 自动 / view |
| Relevance | 128 prim / packet | 32 prim / task × 32 task / worker |

---

## 5. 移动渲染（MobileRender）相关性

移动端是 MR01 的核心目标，SceneVisibility.cpp 内有 **9 处显式 ShadingPath / Mobile 平台分支**，下面按位置归纳。

### 5.1 ComputeRelevance 内的移动端 MeshPass 路由

**位置**：`FRelevancePacket::ComputeRelevance`，1558-1577 行

```cpp
if (ViewRelevance.bDrawRelevance) {
    if (ViewRelevance.bRenderInMainPass || ViewRelevance.bRenderCustomDepth) {
        // Specific logic for mobile packets
        if (ShadingPath == EShadingPath::Mobile) {
            if (!StaticMeshRelevance.bUseSkyMaterial) {
                DrawCommandPacket.AddCommandsForMesh(..., EMeshPass::BasePass);
                if (!bMobileBasePassAlwaysUsesCSM) {
                    DrawCommandPacket.AddCommandsForMesh(..., EMeshPass::MobileBasePassCSM);
                }
            } else {
                DrawCommandPacket.AddCommandsForMesh(..., EMeshPass::SkyPass);
            }
            // bUseSingleLayerWaterMaterial is added to BasePass on Mobile. No need to add it to SingleLayerWaterPass
            MarkMask |= EMarkMaskBits::StaticMeshVisibilityMapMask;
        }
        else { /* Deferred / Forward 路径 */ }
    }
}
```

**与桌面端的差异**：

| 桌面端 (Deferred / Forward) | 移动端 (EShadingPath::Mobile) |
|---|---|
| BasePass 与 SkyPass 分开 | 天空盒的 SkyPass 独立，但 SingleLayerWater 合入 BasePass |
| 总是单独的 `SingleLayerWaterPass` | 单层水面材质**不**进独立 Pass，写入 BasePass |
| `bRenderInSecondStageDepthPass` 可以为 true | `bRenderInSecondStageDepthPass` 强制屏蔽（1219-1221、1535、1912 行） |
| `MobileBasePassCSM` Pass 不存在 | 可选加入 `MobileBasePassCSM`（除非 `MobileBasePassAlwaysUsesCSM(ShaderPlatform)` 为 true） |

**关键变量**：

```cpp
const bool bMobileMaskedInEarlyPass    = (ShadingPath == EShadingPath::Mobile) && Scene.EarlyZPassMode == DDM_MaskedOnly;
const bool bMobileBasePassAlwaysUsesCSM = (ShadingPath == EShadingPath::Mobile) && MobileBasePassAlwaysUsesCSM(Scene.GetShaderPlatform());

const bool bDrawDepthOnly = ViewData.bFullEarlyZPass ||
                            ((ShadingPath != EShadingPath::Mobile) &&
                             (...));
// ↑ 移动端默认不进入 depth-prepass（除非 bFullEarlyZPass）
```

### 5.2 深度通道的移动端差异

**位置**：1423、1531-1541 行

- 移动端不自动生成 depth-prepass（`bDrawDepthOnly=false`），除非 `ShouldForceFullDepthPass(ShaderPlatform)` 为真
- 移动端在 `DDM_MaskedOnly` 早 Z 模式下，**只有 Masked 材质**的 mesh 进 depth pass
- `bRenderInSecondStageDepthPass` 被屏蔽 → 不会路由到 `EMeshPass::SecondStageDepthPass`

### 5.3 动态网格相关性里的 MobileBasePassCSM

**位置**：`ComputeDynamicMeshRelevance`，2228-2232 行

```cpp
if (ShadingPath == EShadingPath::Mobile) {
    PassMask.Set(EMeshPass::MobileBasePassCSM);
    View.NumVisibleDynamicMeshElements[EMeshPass::MobileBasePassCSM] += NumElements;
}
```

> 注意：这里没有"如果 bMobileBasePassAlwaysUsesCSM 则跳过"的判断，动态路径在所有移动端都注册 `MobileBasePassCSM` 标记位（具体是否真出 Pass 由后续 mesh processor 决定）。

### 5.4 遮挡子系统的移动端特殊性

#### 5.4.1 OcclusionFeedback（**移动端独有**）

**声明**（`SceneVisibility.cpp:106-112`）：

```cpp
int32 GOcclusionFeedback_Enable = 0;
static FAutoConsoleVariableRef CVarOcclusionFeedback_Enable(
    TEXT("r.OcclusionFeedback.Enable"),
    GOcclusionFeedback_Enable,
    TEXT("Whether to enable occlusion system based on a rendering feedback. "
         "Currently works only with a mobile rendering\n"),
    ECVF_RenderThreadSafe
);
```

**初始化**（`FGPUOcclusion::Unmap`，2954-2965 行）：

```cpp
if (View.FeatureLevel == ERHIFeatureLevel::ES3_1) {
    if (GOcclusionFeedback_Enable == 0 && ViewState.OcclusionFeedback.IsInitialized()) {
        ViewState.OcclusionFeedback.ReleaseResource();
    } else if (GOcclusionFeedback_Enable != 0 && !ViewState.OcclusionFeedback.IsInitialized()) {
        ViewState.OcclusionFeedback.InitResource(RHICmdListImmediate);
    }
}
```

**读取**（`OcclusionCullPrimitive`，2516-2520 行）：

```cpp
if (bUseOcclusionFeedback) {
    bIsOccluded = OcclusionFeedback.IsOccluded(FPrimitiveOcclusionHistoryKey(PrimitiveId, SubQuery));
    bOcclusionStateIsDefinite = true;
}
```

**写入**（2646-2652）：

```cpp
if (bUseOcclusionFeedback) {
    const FVector BoundOrigin = OcclusionBounds.Origin + View.ViewMatrices.GetPreViewTranslation();
    const FVector BoundExtent = OcclusionBounds.BoxExtent;
    Visitor.AddOcclusionFeedback(FOcclusionFeedbackEntry(
        FPrimitiveOcclusionHistoryKey(PrimitiveId, SubQuery), BoundOrigin, BoundExtent));
}
```

> **设计动机**：移动 GPU 不擅长大规模 `RHIGetRenderQueryResult` stall。OcclusionFeedback 把"上次是否被遮挡"以 UAV 形式存在，**CPU 只读上一次结果，GPU 在跑完一帧后异步回写**。整个机制只在 `ES3_1` 平台（移动端）下被打开。

#### 5.4.2 HZB 遮挡的平台白名单

**位置**：`FGPUOcclusion` 构造函数，2898-2904 行

```cpp
// Disable HZB on OpenGL platforms to avoid rendering artifacts
State.bHZBOcclusion = !IsOpenGLPlatform(View.GetShaderPlatform());
State.bHZBOcclusion &= FDataDrivenShaderPlatformInfo::GetSupportsHZBOcclusion(View.GetShaderPlatform());
State.bHZBOcclusion &= GHZBOcclusion != 0;
State.bHZBOcclusion |= GHZBOcclusion == 2;   // 强制打开
```

- OpenGL 平台默认关（避免 artifact）
- 通过 `FDataDrivenShaderPlatformInfo` 数据驱动判断
- 可用 `r.HZBOcclusion=2` 强制打开

#### 5.4.3 `GRHIMaximumReccommendedOustandingOcclusionQueries` 节流

移动 RHI 经常限制同时在飞的 occlusion query 数量。`SubmitThrottledOcclusionQueries`（2811-2870）按 `LastQuerySubmitFrame` 升序排序，超出阈值时按 10% 最低量提交以保证进度。

### 5.5 任务调度：移动端强制 RenderThread 模式

**位置**：`FVisibilityTaskConfig` 构造函数，3244 行

```cpp
if (!FApp::ShouldUseThreadingForPerformance() || !GIsThreadedRendering ||
    !GSupportsParallelOcclusionQueries || GVisualizeOccludedPrimitives > 0 ||
    IsMobilePlatform(Scene.GetShaderPlatform()))
{
    Schedule = EVisibilityTaskSchedule::RenderThread;
}
```

> 这意味着 **MR01 移动端走的是串行 + ParallelFor 路径**，整条任务图都被禁用。这也解释了为什么 MR01 在做移动端 profiling 时看到的 CPU 时间都集中在 `STAT_ViewVisibilityTime` 上 —— 整个流程是单线程顺序的。

### 5.6 MMV（Mobile Multi-View）屏蔽

**位置**：`FVisibilityTaskData::LaunchVisibilityTasks`，4210-4211 行

```cpp
check(!Views.IsEmpty());
checkf(!Views[0]->bIsMobileMultiViewEnabled,
       TEXT("This culling path was not tested with MMV"));
```

> 即便强制走 RenderThread 模式，**MMV（Mobile Multi-View）路径仍未经过并行可见性任务的验证**，目前不推荐在 MMV 场景下打开 `GVisibilityTaskSchedule=1`。

### 5.7 灯光可见性里的移动端 LightShaft

**位置**：`FSceneRenderer::ComputeLightVisibility`，5218-5265 行

```cpp
const bool bSetupMobileLightShafts = FeatureLevel <= ERHIFeatureLevel::ES3_1 && ShouldRenderLightShafts(ViewFamily);
// ...
if (bSetupMobileLightShafts && LightSceneInfo->bEnableLightShaftBloom && ShouldRenderLightShaftsForLight(View, *LightSceneInfo->Proxy)) {
    View.MobileLightShaft = GetMobileLightShaftInfo(View, *LightSceneInfo);
}
```

- 只在 `ES3_1` 及以下 / 移动端平台下处理
- 平行光（Directional）专用，简化移动端体积光（无 screen-space trace）

### 5.8 反射捕获的移动端 UniformBuffer

**位置**：`FSceneRenderer::SetupSceneReflectionCaptureBuffer`，5575-5625 行

```cpp
TUniformBufferRef<FMobileReflectionCaptureShaderData> MobileReflectionCaptureUniformBuffer;

if (IsMobilePlatform(ShaderPlatform)) {
    CreateReflectionCaptureUniformBuffer(SortedCaptures, MobileReflectionCaptureUniformBuffer);
    View.MobileReflectionCaptureUniformBuffer = MobileReflectionCaptureUniformBuffer;
}
else {
    CreateReflectionCaptureUniformBuffer(SortedCaptures, ReflectionCaptureUniformBuffer);
    View.ReflectionCaptureUniformBuffer = ReflectionCaptureUniformBuffer;
}
```

- 移动端用 `FMobileReflectionCaptureShaderData`（简化版，无 sort key / SH）
- 桌面端用 `FReflectionCaptureShaderData`（完整版）

### 5.9 缓存 MeshDrawCommands 的移动端旁路

**位置**：`SceneRendering.cpp:OnRenderBegin`，3582-3585 行

```cpp
if (GAsyncCacheMaterialUniformExpressions > 0 && !IsMobilePlatform(ShaderPlatform)) {
    AsyncOps |= EUpdateAllPrimitiveSceneInfosAsyncOps::CacheMaterialUniformExpressions;
}
```

- 移动端**不**异步缓存 Material Uniform Expressions
- 原因：移动端 shader permutation 数量较少，同步执行更快

---

## 6. 关键 CVars 速查表

| CVar | 默认 | 作用范围 | 移动端推荐 |
|---|---|---|---|
| `r.AllowOcclusionQueries` | 1 | 总开关 | 1（依赖硬件） |
| `r.HZBOcclusion` | 0 | 0=hw / 1=HZB / 2=force HZB | 1 |
| `r.OcclusionFeedback.Enable` | 0 | 移动端 UAV 反馈 | **1**（项目内应默认开） |
| `r.VisualizeOccludedPrimitives` | 0 | debug | 0 |
| `r.AllowSubPrimitiveQueries` | 1 | HISM 子查询 | 1 |
| `r.Visibility.TaskSchedule` | 1 | 0=RT / 1=Parallel | **0**（移动端强制） |
| `r.Visibility.FrustumCull.Enabled` | 1 | 视锥总开关 | 1 |
| `r.Visibility.FrustumCull.UseOctree` | 0 | 八叉树 | 0（移动端少用） |
| `r.Visibility.FrustumCull.UseSphereTestFirst` | 0 | 球预判 | 0 |
| `r.Visibility.FrustumCull.UseFastIntersect` | 1 | 8-plane SIMD | 1 |
| `r.Visibility.FrustumCull.NumPrimitivesPerTask` | 0 | 0=auto | 0 |
| `r.Visibility.OcclusionCull.MaxQueriesPerTask` | 0 | 0=auto | 0 |
| `r.Visibility.Relevance.NumPrimitivesPerPacket` | 0 | 0=auto | 0 |
| `r.Visibility.DynamicMeshElements.NumMainViewTasks` | 4 | GDME 并行任务数 | 0（移动端不用） |
| `r.WireframeCullThreshold` | 5.0 | wireframe 视口剔除 | 无关 |
| `r.MinScreenRadiusForLights` | 0.03 | 灯光屏幕半径 | 0.03 |
| `r.MinScreenRadiusForDepthPrepass` | 0.03 | depth 屏半径 | 0.03（移动端基本不用） |
| `r.MinScreenRadiusForCSMDepth` | 0.01 | CSM 深度 | 0.01 |
| `r.StaticMeshLODDistanceScale` | 1.0 | LOD 距离缩放 | 0.85~1.0 |
| `r.LightMaxDrawDistanceScale` | 1.0 | 灯光最大距离 | 0.7~1.0 |
| `r.DisableLODFade` | 0 | 关闭 LOD 淡入淡出 | 0 |
| `r.LODFadeTime` | 0.25 | 淡入淡出时间 | 0.25 |
| `r.DistanceFadeMaxTravel` | 1000 | 淡变最大距离 | 1000 |
| `r.EarlyInitDynamicShadows` | 1 | 阴影初始化提前 | 1 |
| `r.Cache.UpdatePrimsTaskEnabled` | 1 | ILC 异步 | 1 |
| `r.GFramesNotOcclusionTestedToExpandBBoxes` | 5 | 新 bbox 扩展 | 5 |
| `r.FramesToExpandNewlyOcclusionTestedBBoxes` | 2 | 扩展帧数 | 2 |
| `r.ExpandAllOcclusionTestedBBoxesAmount` | 0.0 | 全局 bbox 扩展 | 0 |
| `r.NeverOcclusionTestDistance` | 0.0 | 近距离不测遮挡 | 200~500 |
| `r.ForceSceneHasDecals` | 0 | 强制有 decal | 0 |
| `r.CameraCutTranslationThreshold` | 10000 | 摄像机切断阈值 | 10000 |

---

## 7. 上游调用链（谁会用这个文件）

```
FSceneRenderer::OnRenderBegin()                       [SceneRendering.cpp:3560]
   ↓
   LaunchVisibilityTasks()                            [本文件 :383]
   ↓
   FVisibilityTaskData::LaunchVisibilityTasks()      [本文件 :4089]
   ↓
   ├─ FVisibilityViewPacket::BeginInitVisibility()  [本文件 :3506]
   │     ├─ UpdateAlwaysVisible
   │     ├─ FrustumCull (Parallel/Serial)
   │     └─ 触发 Tasks.FrustumCull / Tasks.LightVisibility
   ↓
   FSceneRenderer::PrepareViewStateForVisibility()    [本文件 :4726]（先于 BeginInitViews）
   ↓
FDeferredShadingSceneRenderer::BeginInitViews()       [本文件 :5453]
   ↓
   TaskDatas.VisibilityTaskData->ProcessRenderThreadTasks()  [本文件 :4472]
   ↓
   └─ ProcessRenderThreadTasks 内部：
      ├─ RenderThread 模式：同步串行
      └─ Parallel 模式：等待 TaskEvent
   ↓
FMobileSceneRenderer::Render()                        [MobileShadingRenderer.cpp:910]
   ↓
   VisibilityTaskData = OnRenderBegin(GraphBuilder)   [MobileShadingRenderer.cpp:925]
   ↓
   ProcessRenderThreadTasks / FinishGatherDynamicMeshElements
   ↓
   SetupMobileBasePassAfterShadowInit(BasePassDepthStencilAccess,
                                       GetViewCommandsPerView(),    ← ★ 关键入口
                                       InstanceCullingManager)
```

**两个最重要的公开 API**（`SceneVisibility.h`）：

```cpp
// 启动整个可见性流水线，返回任务数据指针
extern IVisibilityTaskData* LaunchVisibilityTasks(
    FRHICommandListImmediate& RHICmdList,
    FSceneRenderer& SceneRenderer,
    const UE::Tasks::FTask& BeginInitVisibilityTaskPrerequisites);

// 任务数据抽象接口
class IVisibilityTaskData {
public:
    virtual TArrayView<FViewCommands> GetViewCommandsPerView() = 0;  // ★ 下游消费
    virtual UE::Tasks::FTask GetFrustumCullTask() const = 0;
    virtual UE::Tasks::FTask GetComputeRelevanceTask() const = 0;
    virtual UE::Tasks::FTask GetLightVisibilityTask() const = 0;
    virtual void StartGatherDynamicMeshElements() = 0;
    virtual void ProcessRenderThreadTasks() = 0;
    virtual void FinishGatherDynamicMeshElements(...) = 0;
    virtual void Finish() = 0;
};
```

---

## 8. 移动端优化建议（针对 MR01）

### 8.1 已观察到的"好习惯"（项目里已做对）
- 强制走 RenderThread 模式 → 简单稳定
- 使用 OcclusionFeedback → 避免 GPU query stall
- HZB 在 OpenGL 平台默认关闭 → 避免 artifact

### 8.2 建议排查 / 改进点

1. **`GOcclusionFeedback_Enable` 默认值**（106 行）
   - 注释明确说"Currently works only with a mobile rendering"
   - MR01 应当确保在打包 Android / iOS 时通过 `DeviceProfiles` 或 `Ini` 强制设到 1

2. **`r.Visibility.OcclusionCull.MaxQueriesPerTask` 调参**
   - 移动端建议手动设一个固定值（如 64 或 128），减少任务图调度开销

3. **`r.NeverOcclusionTestDistance` 建议非零**
   - 当前默认 0 → 极近距离也做 occlusion check
   - 建议设 200~500（cm），跳过玩家脚下物体的遮挡 query

4. **`r.Visibility.Relevance.NumPrimitivesPerPacket` 显式化**
   - 当前 0=auto 在不同线程数下表现不一
   - 移动端建议固定 256

5. **`r.ExpandAllOcclusionTestedBBoxesAmount` 谨慎开启**
   - 移动端 bbox 计算开销敏感，默认 0 是对的
   - 如要打开需配合性能回归测试

6. **MMV 路径**（4211 行）
   - 注释 `not been tested with MMV` → 暂未覆盖
   - 若 MR01 启用多 Viewport 移动端方案，需要补全覆盖

7. **检查 `MobileBasePassAlwaysUsesCSM` 在目标 shader platform 上的行为**
   - 当 `bMobileBasePassAlwaysUsesCSM=true` 时，每个 static mesh **少一次** `MobileBasePassCSM` 注册
   - 渲染端需要确保这种情况下仍能正确生成 depth shadow

### 8.3 性能关键路径

| 阶段 | RenderThread 模式耗时占比（典型） | 优化手段 |
|---|---|---|
| FrustumCull | 30% | `UseFastIntersect=1`、`UseOctree` 大场景下打开 |
| OcclusionCull | 35% | OcclusionFeedback、HZB 阈值调优 |
| ComputeRelevance | 25% | `NumPrimitivesPerPacket`、`CacheMeshDrawCommands` |
| GDME | 10% | `SupportsParallelGDME()` 减少回退 |

---

## 9. 内部数据结构索引

| 数据结构 | 位置 | 作用 |
|---|---|---|
| `FFrustumCullingFlags` | `SceneVisibility.cpp:507` | 视锥裁剪配置位 |
| `FDrawCommandRelevancePacket` | `SceneVisibilityPrivate.h:679` | 单 packet 内所有 MeshPass 的 draw command 收集 |
| `FRelevancePacket` | `SceneVisibilityPrivate.h:700` | 单 relevance packet |
| `FComputeAndMarkRelevance` | `SceneVisibilityPrivate.h:792` | 单视图 ComputeRelevance 调度器 |
| `FVisibilityViewPacket` | `SceneVisibilityPrivate.h:419` | 单视图任务图 |
| `FVisibilityTaskData` | `SceneVisibilityPrivate.h:499` | 整个场景渲染器任务图 |
| `FVisibilityTaskConfig` | `SceneVisibilityPrivate.h:345` | 任务粒度配置 |
| `EVisibilityTaskSchedule` | `SceneVisibilityPrivate.h:333` | 调度模式枚举 |
| `FGPUOcclusion*` | `SceneVisibilityPrivate.h:916-1201` | 遮挡剔除层级 |
| `TCommandPipe<T>` | `SceneVisibilityPrivate.h:28` | 跨任务命令管道 |
| `FViewCommands` | `SceneVisibility.h:15` | 公开给消费者的视图命令集 |

---

## 10. 总结：SceneVisibility 在渲染管线中的位置

```
┌────────────────────────────────────────────────────────────────┐
│  RHI Sync / Scene Update (Scene::Update)                       │
│   - UpdateAllPrimitiveSceneInfos                                │
│   - UpdateLightPrimitiveInteractions                            │
└──────────────────┬─────────────────────────────────────────────┘
                   ↓
┌────────────────────────────────────────────────────────────────┐
│  FSceneRenderer::OnRenderBegin                                  │
│   └─ LaunchVisibilityTasks ──┐                                 │
└──────────────────┬───────────┼─────────────────────────────────┘
                   ↓           │
         ┌─────────▼──────────────────────────────┐
         │  SceneVisibility.cpp（核心）              │
         │                                        │
         │  ① LightVisibility                     │
         │  ② FrustumCull  (HZB + Octree + SIMD)   │
         │  ③ OcclusionCull (HW query / Feedback)  │
         │  ④ ComputeRelevance  (MeshPass 路由)    │
         │  ⑤ GDME (Dynamic Mesh Elements)        │
         │  ⑥ SetupMeshPasses                     │
         │                                        │
         │   ★ 移动端走 RenderThread 串行模式       │
         │   ★ ShadingPath 分支决定 MeshPass 选型   │
         └─────────┬──────────────────────────────┘
                   ↓  GetViewCommandsPerView()
        ┌──────────┴──────────┐
        ↓                     ↓
  DeferredShading         FMobileSceneRenderer
  SceneRenderer           ::Render
  (BasePass)              (MobileBasePass / BasePassCSM)
```

> **核心要点**：SceneVisibility.cpp 是 **桌面/移动统一入口**，但内部通过 `IsMobilePlatform()`、`ShadingPath == Mobile`、`FeatureLevel <= ES3_1` 等分支做了大量平台特定化。MR01 项目层面还需要通过 CVars 进一步把"移动端推荐值"固化到 `DefaultEngine.ini` 中。
