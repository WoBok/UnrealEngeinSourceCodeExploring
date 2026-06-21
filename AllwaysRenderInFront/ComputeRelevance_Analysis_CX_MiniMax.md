# `FRelevancePacket::ComputeRelevance` 分析

> 文件: `Engine/Source/Runtime/Renderer/Private/SceneVisibility.cpp` (L1299–L1995)
> 类声明: `Engine/Source/Runtime/Renderer/Private/SceneVisibilityPrivate.h` (L700–L795)
> 调度器: `FComputeAndMarkRelevance` (同 cpp L2000+)
> 评审基准: 2026-06-21 / CX-MiniMax

## 1. 概述

`ComputeRelevance` 是 UE 渲染线程 **Visibility 阶段** 中最核心、最热的"判定/打包"函数之一。  
它的输入是一组**经 FrustumCull/遮挡测试后仍然存活的 Primitive 索引**（`Input.Prims`），输出三件事:

1. **将每个可见 Primitive 的 Mesh DrawCommand 注入到对应 MeshPass 桶**（`FDrawCommandRelevancePacket::AddCommandsForMesh`），决定每个 primitive 在本帧会走哪些 pass。
2. **生成 view 级别的状态聚合**（`CombinedShadingModelMask`、`bUsesGlobalDistanceField`、`bHasCustomDepthPrimitives`、各类 translucent 计数等），供后续 pass 判断是否需要启用整 view 的特性。
3. **累积若干"非 draw 但需要被收集"的列表**（动态 primitive、editor primitive、simple-light 列表、indirect lighting 脏列表、custom depth hit proxy 等），等待 `Finalize()` 阶段 merge 回 `FViewInfo`。

它是连接 **"Primitive 视图相关性 (Relevance)"** 与 **"MeshPass 调度"** 的桥梁：本质上把 `FPrimitiveSceneProxy::GetViewRelevance()` 返回的标志位，**展开为具体 MeshPass 的可见性决策**。

## 2. 在管线中的位置

```
FrustumCull ──► ComputeRelevance (本函数) ──► FilterStaticMeshes ──► Finalize
                       │                              │
                       └────── LaunchComputeRelevanceTask (并行 Tasks 调度)
```

- 调度入口: `FComputeAndMarkRelevance::AddPrimitive()` 攒满一包 (`NumPrimitivesPerPacket`) 后调用 `Packet->LaunchComputeRelevanceTask()` (L1161)。
- 同步点: 后续 `Finish()` 把所有 packet 的 `ComputeRelevanceTask` 通过 `FTaskEvent` 串起来,等待 `Tasks.ComputeRelevance.Wait()` (L4546, L4604) 后才能开始 static mesh filter。
- 多视图: `ViewBit`（1<<ViewIndex）作为该 view 在 `DynamicPrimitiveViewMasks` 中的掩码位，使得一次 compute 就能在 atomic 位图上同时标记多视图。

## 3. 数据载体（`FRelevancePacket`）

声明位置: `SceneVisibilityPrivate.h` L700-L795。关键成员分组如下:

| 类别 | 字段 | 用途 |
|---|---|---|
| 输入 | `Input.Prims` (`FRelevancePrimSet<int32>`) | 本 packet 待处理的 primitive 索引列表 |
| 输出-主 | `DrawCommandPacket` (`FDrawCommandRelevancePacket`) | 各 MeshPass 的 cached draw commands / 动态构建请求 |
| 输出-状态 | `CombinedShadingModelMask`、`SubstrateUintPerPixel`、`SubstrateClosureCountMask`、`bUsesComplexSpecialRenderPath` 等 ~12 个 bool/掩码 | view-level 状态汇总 |
| 输出-列表 | `NotDrawRelevant`、`TranslucentSelfShadowPrimitives`、`VisibleDynamicPrimitivesWithSimpleLights`、`DirtyIndirectLightingCacheBufferPrimitives`、`NaniteCustomDepthInstances`、`MeshDecalBatches`、`VolumetricMeshBatches`、`SkyMeshBatches`、`SortedTrianglesMeshBatches`、`PrimitivesLODMask` | 待 Finalize merge 进 `FViewInfo` 的容器 |
| 计数器 | `NumVisibleStaticMeshElements`、`NumVisibleDynamicPrimitives`、`TranslucentPrimCount` | 性能/特性统计 |
| 编辑器 | `EditorVisualizeLevelInstancesNanite`、`EditorSelectedInstancesNanite`、`EditorSelectedNaniteHitProxyIds` | 仅 `WITH_EDITOR` 下的选中 primitive 收集 |

> **注意**：`MarkMasks` 是 `uint8*` 的全局 buffer（`FComputeAndMarkRelevance` 持有,带 31 字节 padding 便于高速 transpose），`FStaticMeshBatchRelevance::Id` 索引进去，用于 `Finalize` 时将 visibility/LOD-dither 标记合并到 `FViewInfo::PrimitiveVisibilityMap`。

## 4. 执行流程（行号对应 L1299–L1995）

### 4.1 初始化（L1301–L1332）

- 重置所有 view-level 状态字段（`CombinedShadingModelMask`、`bSceneHasSkyMaterial`、`bHasSingleLayerWaterMaterial`、`bUsesGlobalDistanceField`、`bUsesLightingChannels`、`bTranslucentSurfaceLighting` 等）。
- 计算若干**本 packet 内只算一次**的常量，避免循环里重复求值:
  - `ShadingPath`（Mobile/Deferred）
  - `bHairStrandsEnabled`
  - `bMobileMaskedInEarlyPass`、`bMobileBasePassAlwaysCSM`
  - `bVelocityPassWritesDepth`、`bHLODActive`
  - `MaxDrawDistanceScale`（含 FOV 缩放）
- 预取 view 写入指针 `FViewInfo& WriteView = const_cast<FViewInfo&>(View)`（`ComputeRelevance` 在 task 中跑，需要通过 `const_cast` 写 view 的 `PrimitiveViewRelevanceMap` 等）。
- 准备两个 lambda：`AddEditorDynamicPrimitive`、`AddDynamicPrimitive`（只在 task 入口被调用一次，本函数体不直接使用，而是 `LaunchComputeRelevanceTask` 通过 `DynamicPrimitiveIndexList` 间接传出；本函数内并不消费它 — 它只是把 `DynamicPrimitiveIndexList` 作为 out-param 累积）。

### 4.2 主循环（L1340–L1916）

每个 `Input.Prims[InputPrimsIndex]` 处理一个 primitive，主要步骤:

#### (a) 软预取（L1346–L1353）
对 `InputPrimsIndex+1` 的 `Scene.Primitives[NextBitIndex]` 与 `Scene.PrimitiveSceneProxies[NextBitIndex]` 做软件 `Prefetch`，掩盖下一轮循环触发的 cache miss。

#### (b) 调用 `Proxy->GetViewRelevance()` (L1361)
```
ViewRelevance = PrimitiveSceneProxy->GetViewRelevance(&View);
ViewRelevance.bInitializedThisFrame = true;
```
获取本 primitive 在本 view 上的全部相关性位（`bDrawRelevance`、`bStaticRelevance`、`bShadowRelevance`、`bDynamicRelevance`、`HasTranslucency()`、`bRenderInMainPass`、`bRenderCustomDepth` 等），并写入 `View.PrimitiveViewRelevanceMap[BitIndex]`（`const_cast` 写入）。

#### (c) 反射捕获早退（L1373–L1377）
```cpp
if (View.bIsReflectionCapture && !PrimitiveSceneProxy->IsVisibleInReflectionCaptures())
{
    NotDrawRelevant.AddPrim(BitIndex);
    continue;
}
```
反射探针视角下，不可见的 proxy 直接丢弃。

#### (d) Static 相关性分支 — MeshPass 注入主体（L1379–L1714）

这是本函数最大、最热的代码块。`bStaticRelevance && (bDrawRelevance || bShadowRelevance)` 才进入。

1. **LOD 计算** (L1389)：`ComputeLODForMeshes(...)` 基于屏幕尺寸、distance、ForcedLODLevel 计算 `FLODMask`，并把 `(PrimitiveIndex, LODToRender)` 入 `PrimitivesLODMask`。
2. **派生标志** (L1393–L1402):
   - `bIsHLODFading / bIsHLODFadingOut` — HLOD 渐隐方向。
   - `bIsLODDithered / bIsLODRange` — LOD dither / 范围模式（GPU 上多 LOD 一次提交）。
   - `bDrawShadowDepth` — 基于 `MinScreenRadiusForCSMDepthSquared` 的球半径阈值。
   - `bDrawDepthOnly` — Deferred path 下 `bFullEarlyZPass` 或大球半径时进入 depth prepass。
3. **遍历每个 `StaticMeshBatch` (L1404)**：先做 overlay material 距离剔除 (L1406-L1417)。
4. **LOD dither / HLOD 渐隐标志位构造** (L1422-L1497)：
   - 根据 `bIsHLODFading`/`bIsLODDithered` 组合设置 `MarkMask`：
     - `EMarkMaskBits::StaticMeshFadeOutDitheredLODMapMask = 0x10`
     - `EMarkMaskBits::StaticMeshFadeInDitheredLODMapMask = 0x20`
   - 计算 `bCanCache`：若本 mesh 有 LOD dither 或距离剔除淡入淡出，**不缓存 mesh draw command**。
   - 计算 `CullingPayloadFlags`：在 GPU LOD-range 模式时附加 `MinScreenSizeCull` / `MaxScreenSizeCull`，保证范围两端单边剔除。
5. **按相关性注入到 MeshPass** (L1499-L1700)，主要分流:

   | 触发条件 | 注入 MeshPass |
   |---|---|
   | `bUseForMaterial && bRenderInMainPass && HasVelocity()` + 静态 opq 速度可用 | `Velocity`, `TranslucentVelocity` |
   | `bUseForDepthPass && (bDrawDepthOnly \|\| Mobile masked early pass)` | `DepthPass` / `SecondStageDepthPass`；Raytracing + FadeOut dither 时附加 `DitheredLODFadingOutMaskPass` |
   | Mobile + `!bUseSkyMaterial` + 材质用途 | `BasePass`；当 `!bMobileBasePassAlwaysUsesCSM` 时附加 `MobileBasePassCSM` |
   | Mobile + `bUseSkyMaterial` | `SkyPass` |
   | Deferred 路径 `bUseForMaterial` | `BasePass`；天空/水面材质分别加 `SkyPass`、`SingleLayerWaterPass`、`SingleLayerWaterDepthPrepass` |
   | `bUseAnisotropy` | `AnisotropyPass` |
   | `bRenderCustomDepth` | `CustomDepth` |
   | `bAddLightmapDensityCommands` | `LightmapDensity` |
   | `View.Family->UseDebugViewPS()`（非 Shipping） | `DebugViewMode` |
   | `WITH_EDITOR && bSelectable` | `HitProxy` 或 `HitProxyOpaqueOnly`（按 `bAllowTranslucentPrimitivesInHitProxy` 分流）|
   | `HasTranslucency() && bRenderInMainPass`（非 editor） | `TranslucencyStandard` / `StandardModulate` / `TranslucencyAfterDOF` / `AfterDOFModulate` / `AfterMotionBlur`（按 `bNormalTranslucency`/`bSeparateTranslucency`/`bPostMotionBlurTranslucency` 组合），或当 `!AllowTranslucencyAfterDOF` 时合桶到 `TranslucencyAll` |
   | `bTranslucentSurfaceLighting` | `LumenTranslucencyRadianceCacheMark`, `LumenFrontLayerTranslucencyGBuffer` |
   | `bDistortion` | `Distortion` |
   | `WITH_EDITOR && bEditorVisualizeLevelInstanceRelevance` | `EditorLevelInstance` |
   | `WITH_EDITOR && bEditorStaticSelectionRelevance` | `EditorSelection` |
   | `bHasVolumeMaterialDomain` | `VolumetricMeshBatches` / `HeterogeneousVolumesMeshBatches`（按 `ShouldRenderMeshBatchWithHeterogeneousVolumes` 分流） |
   | `bUsesSkyMaterial` | `SkyMeshBatches` |
   | `HasTranslucency() && SupportsSortedTriangles` | `SortedTrianglesMeshBatches` |
   | `bRenderInMainPass && bDecal && bUseForMaterial` | `MeshDecalBatches` |

   每次注入走 `DrawCommandPacket.AddCommandsForMesh(...)` (L1074-L1133)：优先用 cached mesh draw command（带 StateBucket / MeshDrawCommands 索引），否则作为动态构建请求（`DynamicBuildRequests` + `NumDynamicBuildRequestElements`）排队。

6. **写 `MarkMasks`** (L1702-L1705)：若 `MarkMask != 0`（即发生 LOD dither / HLOD fade），写回全局 `MarkMasks[StaticMeshRelevance.Id]` 供 `Finalize` 时合成到 `FViewInfo::PrimitiveVisibilityMap`。

#### (e) `!bDrawRelevance` 早退（L1710–L1714）
本 primitive 不需要任何 draw command，但仍要记入 `NotDrawRelevant` 以在 Finalize 阶段清掉 `View.PrimitiveVisibilityMap[BitIndex]`。

#### (f) Editor Nanite 选中 primitive 收集（L1718–L1827，`WITH_EDITOR`）
- `bEditorVisualizeLevelInstanceRelevance` → 收集所有 Nanite instance。
- `bEditorSelectionRelevance` → 收集选中 instance（带 `OutSelectedInstanceHitProxyIds` 收集 hit proxy）。
- 通过 lambda `CollectSelectedNaniteInstanceDraws` 统一从 `FInstanceSceneDataBuffers::GetReadView().InstanceEditorData` 拆包 `HitProxyColor` / `bSelected`。

#### (g) Dynamic primitive / 简单光源分类（L1829–L1843）
```cpp
if (bEditorRelevance)            AddEditorDynamicPrimitive(BitIndex);
else if (bDynamicRelevance)       { AddDynamicPrimitive(BitIndex);
                                   if (bHasSimpleLights) VisibleDynamicPrimitivesWithSimpleLights.AddPrim(...); }
else if (bHairStrandsRelevance)   AddDynamicPrimitive(BitIndex);
```

#### (h) Translucent 计数累加（L1845–L1880）
- `View.Family->AllowTranslucencyAfterDOF()` 决定分桶策略（5 桶 vs 1 桶 `TPT_AllTranslucency`）。
- 每个相关位 (`bNormalTranslucency`/`bSeparateTranslucency`/`bTranslucencyModulate`/`bPostMotionBlurTranslucency`) 决定更新哪个 `ETranslucencyPass::TPT_*` 计数，并附 `bUsesSceneColorCopy` 标记。
- `bDistortion` 翻 `bHasDistortionPrimitives`。

#### (i) view-level 状态合并（L1882–L1906）
对所有 primitive 强制执行（不论 static/dynamic），更新:
- `CombinedShadingModelMask |= ViewRelevance.ShadingModelMask`
- `SubstrateUintPerPixel = max(..., ViewRelevance.SubstrateUintPerPixel)`
- `bUsesComplexSpecialRenderPath`/`bUsesGlobalDistanceField`/`bUsesLightingChannels`/`bTranslucentSurfaceLighting` 全部 `|=`
- `bUsesCustomDepth` / `bUsesCustomStencil` 从 `CustomDepthStencilUsageMask` 解出
- `bSceneHasSkyMaterial` / `bHasSingleLayerWaterMaterial` / `bUsesSecondStageDepthPass`（注意 mobile 不算 second-stage depth）

#### (j) Custom depth 索引收集（L1908–L1924）
若 `bRenderCustomDepth`：
- 翻 `bHasCustomDepthPrimitives`
- `CustomDepthStencilValues.Add(Proxy->GetCustomDepthStencilValue())`（TSet 去重）
- Nanite mesh → 追加 `FPrimitiveInstanceRange { Index, InstanceSceneDataOffset, NumEntries }` 到 `NaniteCustomDepthInstances`

#### (k) Translucent self-shadow / LastRenderTime / 反射捕获 / 间接光脏列表（L1926–L1946）
- `GUseTranslucencyShadowDepths && bTranslucentSelfShadow` → 入 `TranslucentSelfShadowPrimitives`
- 更新 `PrimitiveSceneInfo->LastRenderTime = CurrentWorldTime` + `UpdateComponentLastRenderTime(...)`
- 反射捕获缓存 / 间接光缓存脏列表（如需）

### 4.3 循环结束 — view 计数器汇总（L1948–L1951）
```cpp
FPlatformAtomics::InterlockedAdd(
    (volatile int32*)&WriteView.NumVisibleStaticMeshElements,
    NumVisibleStaticMeshElements);
```
- 因为多 packet 并行写 view，**只有这一个累加用 `InterlockedAdd`**，其余 view 状态由 `Finalize()` 串行 merge 写入。

## 5. 关键设计点

### 5.1 并行性 / 任务依赖
- `LaunchComputeRelevanceTask` (L1161) 用 `UE::Tasks::Launch(...)` 异步执行，**前序依赖**是 `Scene.GetCacheMeshDrawCommandsTask()`（等待 cached mesh draw command 就绪）。
- 多 packet 共享同一 `MarkMasks` buffer：`MarkMasks[StaticMeshRelevance.Id]` 写操作的**线程安全性依赖**：
  - 不同 primitive 不会复用同一个 `StaticMeshBatchRelevance::Id` → 各 packet 写不同下标，无 race；
  - 同一个 primitive 的不同 mesh 之间会写不同 `Id`，无 race；
  - 多个 packet 写**不同 primitive 的不同 Id** → 完全独立。
- `DynamicPrimitiveViewMasks` 路径下，对 `Primitives[Idx]` 的写是 `InterlockedOr` 原子操作（`FPlatformAtomics::InterlockedOr((volatile int8*)&, ViewBit)`）。

### 5.2 软预取策略（L1346-L1353, L1357-L1358）
- 下一轮循环的 `Scene.Primitives[]` 和 `Scene.PrimitiveSceneProxies[]` 做 `Prefetch`。
- 紧接着 `GetViewRelevance()` 之前预先 `Prefetch(StaticMeshRelevances.GetData())` 和 `Prefetch(GetSceneData())`，掩盖跨 cache line 读延迟。
- 末尾 `InterlockedAdd(&WriteView.NumVisibleStaticMeshElements, ...)` 之前没有 barrier 显式，但 task 内的写入对 view 的可见性由 `Finalize()` 的串行 merge 隐式保证。

### 5.3 缓存 vs 动态构建分流
- `AddCommandsForMesh` (L1074) 决定 cached / dynamic 二选一：
  - cached 路径要求：`bUseCachedMeshDrawCommands` + pass flag `CachedMeshCommands` + `bSupportsCachingMeshDrawCommands` + `bCanCache`（无 LOD dither、无 distance-cull fade）。
  - 否则走 `DynamicBuildRequests[PassType].Add(&StaticMesh)`，由后续 `FilterStaticMeshes` 阶段并行构造 PSO。
- Nanite mesh (`bIsNaniteMesh` + `PrimitivesAlwaysVisibleOffset != ~0u`) 永远不入 `DrawCommandPacket`，因为 Nanite 走 `NaniteMeshPass` 自身独立处理。

### 5.4 Mobile / Deferred 路径分流
- 关键差异点 L1552-L1583：
  - **Mobile**: `BasePass` 是唯一可写 `MarkMask|=StaticMeshVisibilityMapMask` 的地方；天空/水面用不同 pass，CSM 由 `bMobileBasePassAlwaysUsesCSM` 决定是否合并到 `BasePass`。
  - **Deferred**: `BasePass`、`SkyPass`、`SingleLayerWaterPass`/`SingleLayerWaterDepthPrepass` 独立分流。
- `bUsesSecondStageDepthPass` 在 mobile 路径上**永远不开启**（L1905 显式 `ShadingPath!=EShadingPath::Mobile`）。

### 5.5 输出语义
函数没有返回值；所有结果以 **packet 成员变量** 形式承载，`Finalize()` (L1196) 在 `FilterStaticMeshes` 之后被调用，把本 packet 的所有状态 `|= / Append` 合并到 `FViewInfo` 和 `FViewCommands`。这意味着:
- `CombinedShadingModelMask` 等位字段以 `|=` 合并 → packet 间**不会丢失任何 view-level 状态**。
- 列表容器（`MeshDecalBatches`、`SkyMeshBatches` 等）以 `Append` 合并 → packet 间互相补全。
- 计数器 `NumVisibleStaticMeshElements` 用 `InterlockedAdd` 累加 → 唯一允许并发的写点。

## 6. 性能与可观察性

- `TRACE_CPUPROFILER_EVENT_SCOPE(ComputeViewRelevance)` + `SCOPE_CYCLE_COUNTER(STAT_ComputeViewRelevance)`：线程级 profiling 入口。
- `STAT_StaticMeshTriangles` 按 primitive 累加本帧 triangle 数。
- 性能上限：每帧每个 view 调一次 `ComputeRelevance` × packet 数；packet 数 ≈ `NumVisiblePrimitives / NumPrimitivesPerPacket`。
- `bAddLightmapDensityCommands` 控制是否把 `LightmapDensity` pass 注入（`TaskData.bAddLightmapDensityCommands`）；与 `View.Family->UseDebugViewPS()` 互斥（debug 优先）。

## 7. 关键不变量 / 容易踩的坑

1. **`View.PrimitiveViewRelevanceMap[BitIndex]`** 在并发 `ComputeRelevance` 中通过 `const_cast` 写，本 packet 串行，多 packet 不重叠（每个 primitive 只属于一个 packet）→ 安全。
2. **`MarkMasks[StaticMeshRelevance.Id]`** 的下标语义是 mesh-batch id 而非 primitive id；不同 primitive 的不同 mesh batch 不会冲突。`FComputeAndMarkRelevance` 构造时已经按 `NumMeshes + 31` 大小分配并 `Memzero`。
3. **Nanite** 路径：`AddCommandsForMesh` 对 Nanite mesh 提前 return（L1081-L1084），但本函数体里仍会执行 `GetViewRelevance()`、HLOD 状态计算等冗余逻辑；可在高 Netite 占比场景观察。
4. **反射捕获**（`View.bIsReflectionCapture`）下，未通过 `IsVisibleInReflectionCaptures()` 的 primitive 只入 `NotDrawRelevant`，**不参与任何 view-level 状态汇总** → 反射探针不会污染主 view 的 `bUsesGlobalDistanceField` 等聚合位。
5. **`CustomDepthStencilValues`** 用 TSet 去重 → 适合当 unique 数量远小于 primitive 数时；极端场景下 set 性能可能成为瓶颈。
6. **`bUsesSecondStageDepthPass`** 在 mobile 永假（聚合写时已经 `ShadingPath != EShadingPath::Mobile` 过滤），Finalize 合并时也再过滤一次（双重保险）。
7. **`Extern bool GUseTranslucencyShadowDepths`**（L1926）—— translucency self-shadow 的总开关，仅当其 true 时才走 self-shadow 路径。
8. **HIT proxy 流**：editor 下根据 `bAllowTranslucentPrimitivesInHitProxy` 二选一 `HitProxy` 或 `HitProxyOpaqueOnly`，影响 hit proxy pass 的桶选择。

## 8. 调用栈（精简版）

```
FComputeAndMarkRelevance::AddPrimitive  (攒满 packet)
   └─► FRelevancePacket::LaunchComputeRelevanceTask
          └─► UE::Tasks::Launch → [FOptionalTaskTagScope(EParallelRenderingThread)]
                 └─► FRelevancePacket::ComputeRelevance     ◀── 本函数
                       ├─► PrimitiveSceneProxy->GetViewRelevance
                       ├─► ComputeLODForMeshes
                       ├─► (Static mesh loop) DrawCommandPacket.AddCommandsForMesh
                       │       ├─► Cached path  (FPassProcessorManager / StateBucketId)
                       │       └─► Dynamic path (DynamicBuildRequests)
                       ├─► AddDynamicPrimitive/AddEditorDynamicPrimitive (写 DynamicPrimitiveIndexList)
                       ├─► TranslucentPrimCount.Add (按 ETranslucencyPass 分桶)
                       ├─► (Editor) CollectSelectedNaniteInstanceDraws
                       └─► InterlockedAdd(WriteView.NumVisibleStaticMeshElements)
```

## 9. 一句话总结

`ComputeRelevance` 是一个**以 `FPrimitiveSceneProxy::GetViewRelevance()` 输出为输入，按 view/feature level/MeshPass 规则把 mesh draw command 投递到对应桶，并把多 primitive 状态聚合到 packet 级 view-state** 的高吞吐并行函数；它的所有状态输出都由 `FRelevancePacket::Finalize()` 串行 merge 到 `FViewInfo`，并发安全由"不同 primitive 不同下标"+"TSet 去重"+"Interlocked 累加"三件套保证。

---

## 1. 分发机制: DrawCommandPacket.AddCommandsForMesh

ComputeRelevance 不直接写 view 的 MeshCommands 数组。它把所有 mesh 投递动作委托给
一个核心函数 FDrawCommandRelevancePacket::AddCommandsForMesh (L1074-L1133)。

### 1.1 数据结构 (SceneVisibilityPrivate.h L683-L696)

struct FDrawCommandRelevancePacket
{
FPassDrawCommandArray        VisibleCachedDrawCommands[EMeshPass::Num];  // 已
构造好的 draw command
FPassDrawCommandBuildRequestArray DynamicBuildRequests[EMeshPass::Num];   //
待 PSO 编译
FPassDrawCommandBuildFlagsArray    DynamicBuildFlags[EMeshPass::Num];     //
对应 culling flags
int32  NumDynamicBuildRequestElements[EMeshPass::Num];
bool   bUseCachedMeshDrawCommands;
};

关键设计: EMeshPass::Num 个并列数组,EMeshPass::Type 枚举值直接当数组下标 →
MeshPass 即桶 ID。这意味着只要传入一个 EMeshPass::Type,就能 O(1) 定位到对应桶。

### 1.2 投递逻辑 (L1074-L1133)

void FDrawCommandRelevancePacket::AddCommandsForMesh(
int32 PrimitiveIndex, const FPrimitiveSceneInfo* InPrimitiveSceneInfo,
const FStaticMeshBatchRelevance& StaticMeshRelevance, const FStaticMeshBatch&
StaticMesh,
EMeshDrawCommandCullingPayloadFlags CullingPayloadFlags, const FScene& Scene,
bool bCanCache, EMeshPass::Type PassType)
{
// Nanite 早退: 不入本 packet
if (bIsNaniteMesh && Scene.PrimitivesAlwaysVisibleOffset != ~0u) return;

      // 决策: cached 还是 dynamic
      const bool bUseCachedMeshCommand =
             bUseCachedMeshDrawCommands
          && !!(FPassProcessorManager::GetPassFlags(ShadingPath, PassType) &
          EMeshPassFlags::CachedMeshCommands)
          && StaticMeshRelevance.bSupportsCachingMeshDrawCommands
          && bCanCache;

      if (bUseCachedMeshCommand)
      {
          // ===== Cached 路径 =====
          const FCachedMeshDrawCommandInfo& Cmd =
          InPrimitiveSceneInfo->StaticMeshCommandInfos[
              StaticMeshRelevance.GetStaticMeshCommandInfoIndex(PassType)];

          const FMeshDrawCommand* MeshDrawCommand = Cmd.StateBucketId >= 0
              ?
              &Scene.CachedMeshDrawCommandStateBuckets[PassType].GetByElementId(Cmd.
              StateBucketId).Key
              : &Scene.CachedDrawLists[PassType].MeshDrawCommands[Cmd.CommandIndex];

          VisibleCachedDrawCommands[(uint32)PassType].AddUninitialized();
          VisibleCachedDrawCommands[(uint32)PassType].Last().Setup(
              MeshDrawCommand, /*...*/, Cmd.SortKey, Cmd.CullingPayload,
              CullingPayloadFlags);
      }
      else
      {
          // ===== Dynamic 路径: 仅登记请求,留到 FilterStaticMeshes 阶段构造 PSO
          =====
          NumDynamicBuildRequestElements[PassType] +=
          StaticMeshRelevance.NumElements;
          DynamicBuildRequests[PassType].Add(&StaticMesh);
          DynamicBuildFlags[PassType].Add(CullingPayloadFlags);
      }
}

### 1.3 完整流程: ComputeRelevance → Finalize

ComputeRelevance (并行 packet)
└─► DrawCommandPacket.AddCommandsForMesh(..., EMeshPass::BasePass /
TranslucencyStandard / ...)
└─► 写 FDrawCommandRelevancePacket 的某个 EMeshPass::Num 下标

FilterStaticMeshes (后续阶段,处理 DynamicBuildRequests)

Finalize (L1196, 串行,单 packet 一次)
└─► for (PassIndex = 0; PassIndex < EMeshPass::Num; PassIndex++)
{
FPassDrawCommandArray& Src =
DrawCommandPacket.VisibleCachedDrawCommands[PassIndex];
FMeshCommandOneFrameArray& Dst =
WriteViewCommands.MeshCommands[PassIndex];
if (Src.Num() > 0) memcpy(&Dst[Dst.AddUninitialized(Src.Num())],
&Src[0], ...);

                // 同时把 dynamic build requests / flags 也 memcpy 到 view 的 per-
                pass 数组
                ...
            }

核心要点:

- ComputeRelevance 阶段只做轻量级分类 (按 ViewRelevance 标志决定该 mesh 走哪些
  pass,再按 bCanCache 决定是引用 cached 还是排队 build)。

- 真正的draw command 构造走两条路:
    - cached: 从 Scene.CachedDrawLists[PassType] /
      CachedMeshDrawCommandStateBuckets[PassType] 中查表,Setup 出一个
      FVisibleMeshDrawCommand。

    - dynamic: 只把 &StaticMesh 指针塞进 DynamicBuildRequests[PassType],等
      FilterStaticMeshes 阶段按 pass 跑 FMeshPassProcessor::AddMeshBatch() 构造
      PSO + draw command。

- 过滤/裁剪参数 (CullingPayloadFlags 中的 MinScreenSizeCull / MaxScreenSizeCull)
  是在 ComputeRelevance 里基于 LOD range 算出来的,直接附在每个 draw command 上,留
  给 GPU 端做 instance culling。

## 2. BasePass vs TranslucencyPass 的分流

两个 pass 的触发条件完全不同,分流发生在 ComputeRelevance 主循环的 static mesh 内部
(L1500-L1700)。

### 2.1 BasePass 路径 (L1546-L1600)

if (StaticMeshRelevance.bUseForMaterial
&& (ViewRelevance.bRenderInMainPass || ViewRelevance.bRenderCustomDepth))
{
if (ShadingPath == EShadingPath::Mobile)
{
if (!StaticMeshRelevance.bUseSkyMaterial)
{
DrawCommandPacket.AddCommandsForMesh(..., EMeshPass::BasePass);
if (!bMobileBasePassAlwaysUsesCSM)
DrawCommandPacket.AddCommandsForMesh(...,
EMeshPass::MobileBasePassCSM);
}
else
{
DrawCommandPacket.AddCommandsForMesh(..., EMeshPass::SkyPass);
}
MarkMask |= EMarkMaskBits::StaticMeshVisibilityMapMask;  // 计入 visible
static mesh
}
else // Deferred
{
DrawCommandPacket.AddCommandsForMesh(..., EMeshPass::BasePass);
MarkMask |= EMarkMaskBits::StaticMeshVisibilityMapMask;

          if (StaticMeshRelevance.bUseSkyMaterial)
              DrawCommandPacket.AddCommandsForMesh(..., EMeshPass::SkyPass);
          if (StaticMeshRelevance.bUseSingleLayerWaterMaterial)
          {
              DrawCommandPacket.AddCommandsForMesh(...,
              EMeshPass::SingleLayerWaterPass);
              DrawCommandPacket.AddCommandsForMesh(...,
              EMeshPass::SingleLayerWaterDepthPrepass);
          }
      }
      // 附加 pass: bUseAnisotropy / bRenderCustomDepth /
      bAddLightmapDensityCommands / DebugViewMode / HitProxy
      // ...
}

BasePass 触发条件:

1. Mesh 级别: bUseForMaterial (材质在当前 pass 类型下有效)。
2. View 级别: bRenderInMainPass (主 pass 渲染) 或 bRenderCustomDepth (CustomDepth
   pass 渲染)。

关键特性:

- 天空/水面在 Mobile 上不进 BasePass,Mobile 走 BasePass + 可选 MobileBasePassCSM;
  天空单独走 SkyPass。

- Deferred 路径: BasePass 必走;天空走 SkyPass;单层水面走 SingleLayerWaterPass +
  SingleLayerWaterDepthPrepass (因为水面需要独立的 depth prepass 做反射)。

- BasePass 注入的 mesh 一定会打标 StaticMeshVisibilityMapMask = 0x2 (L1556,
  L1566),这是 Finalize 阶段合成 FViewInfo::PrimitiveVisibilityMap 的关键位。

### 2.2 Translucency 路径 (L1620-L1700)

if (StaticMeshRelevance.bUseForMaterial
&& ViewRelevance.HasTranslucency()
&& !ViewRelevance.bEditorPrimitiveRelevance   // editor 选中 mesh 不进
translucent
&& ViewRelevance.bRenderInMainPass)
{
if (View.Family->AllowTranslucencyAfterDOF())
{
// ===== DOF 后分桶 =====
if (bNormalTranslucency
|| (View.AutoBeforeDOFTranslucencyBoundary > 0.f &&
bSeparateTranslucency))
DrawCommandPacket.AddCommandsForMesh(...,
EMeshPass::TranslucencyStandard);

          if ((bNormalTranslucency || ...) && bTranslucencyModulate
              && View.Family->AllowStandardTranslucencySeparated())
              DrawCommandPacket.AddCommandsForMesh(...,
              EMeshPass::TranslucencyStandardModulate);

          if (bSeparateTranslucency)
              DrawCommandPacket.AddCommandsForMesh(...,
              EMeshPass::TranslucencyAfterDOF);

          if (bSeparateTranslucency && bTranslucencyModulate)
              DrawCommandPacket.AddCommandsForMesh(...,
              EMeshPass::TranslucencyAfterDOFModulate);

          if (bPostMotionBlurTranslucency)
              DrawCommandPacket.AddCommandsForMesh(...,
              EMeshPass::TranslucencyAfterMotionBlur);
      }
      else
      {
          // ===== 单桶模式: 全部合并 =====
          DrawCommandPacket.AddCommandsForMesh(..., EMeshPass::TranslucencyAll);
      }

      // 附加 Lumen translucent radiance cache 标记
      if (ViewRelevance.bTranslucentSurfaceLighting)
      {
          DrawCommandPacket.AddCommandsForMesh(...,
          EMeshPass::LumenTranslucencyRadianceCacheMark);
          DrawCommandPacket.AddCommandsForMesh(...,
          EMeshPass::LumenFrontLayerTranslucencyGBuffer);
      }

      if (ViewRelevance.bDistortion)
          DrawCommandPacket.AddCommandsForMesh(..., EMeshPass::Distortion);
}

Translucency 触发条件:

1. Mesh 级别: bUseForMaterial。
2. View 级别: HasTranslucency() (材质有 translucent 域) 且 bRenderInMainPass。
3. 排除: bEditorPrimitiveRelevance (编辑器选中的特殊 mesh,只走 HitProxy 不进
   translucent)。

关键分流逻辑:

分流维度                             取值     目标桶                             
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━  ━━━━━━━  ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
View.Family->AllowTranslucencyAft    true     5 桶细分
erDOF()
───────────────────────────────────  ───────  ────────────────────────────────────
同上                                 false    合并到 TranslucencyAll 单桶
───────────────────────────────────  ───────  ────────────────────────────────────
bNormalTranslucency / (auto          true     TranslucencyStandard
boundary + bSeparateTranslucency)
───────────────────────────────────  ───────  ────────────────────────────────────
bTranslucencyModulate +              true     TranslucencyStandardModulate
AllowStandardTranslucencySeparate
d
───────────────────────────────────  ───────  ────────────────────────────────────
bSeparateTranslucency                true     TranslucencyAfterDOF (DOF 后绘制,
需 scene color copy)
───────────────────────────────────  ───────  ────────────────────────────────────
同上 + bTranslucencyModulate         true     TranslucencyAfterDOFModulate
───────────────────────────────────  ───────  ────────────────────────────────────
bPostMotionBlurTranslucency          true     TranslucencyAfterMotionBlur
───────────────────────────────────  ───────  ────────────────────────────────────
bTranslucentSurfaceLighting          true     LumenTranslucencyRadianceCacheMark
(Lumen)                                       +
LumenFrontLayerTranslucencyGBuffer
───────────────────────────────────  ───────  ────────────────────────────────────
bDistortion                          true     Distortion

注意: 与 BasePass 不同,Translucent 注入不打 StaticMeshVisibilityMapMask。因为
translucent 物体不能写入深度,不能被视作"遮挡物"。但同时,ComputeRelevance 会 调用
TranslucentPrimCount.Add(ETranslucencyPass::TPT_TranslucencyStandard,
bUsesSceneColorCopy) (L1847-L1878) 把 translucent primitive 计数累加到 view-level,
这是给后续 translucent pass 用来判断"是否有 translucent primitive 需要 scene color
copy"。

### 2.3 两类 pass 调用的位置差异

- BasePass 块 (L1546-L1620) 在 if (ViewRelevance.bDrawRelevance) 内部,与 depth
  pass / velocity / hit proxy 同一层级。

- Translucent 块 (L1620-L1700) 紧跟在 BasePass 之后,但嵌套在同一个 if
  (ViewRelevance.bDrawRelevance) 内。

- 关键差异: BasePass 块检查 bRenderInMainPass || bRenderCustomDepth;Translucent 块
  额外要求 HasTranslucency() 且 !bEditorPrimitiveRelevance。

简言之,BasePass 与 Translucency 是互斥的(一个 mesh 不会两个都进):如果材质不透明就
走 BasePass 系;如果材质带 Translucent 域就走 Translucency 系;天空/水面/编辑器选中
mesh 走各自专属 pass。

## 3. 一句话总结

- 分发机制: 靠 EMeshPass::Type 当数组下标,把 cached/dynamic 两种形式的 draw
  command 塞进 FDrawCommandRelevancePacket 的并列数组;Finalize() 阶段再 memcpy 到
  FViewCommands::MeshCommands[PassIndex]。

- BasePass 分流: 看 bUseForMaterial && (bRenderInMainPass || bRenderCustomDepth),
  并按 ShadingPath (Mobile/Deferred) 进一步细分天空/水面/CSM。

- Translucency 分流: 在 HasTranslucency() && bRenderInMainPass && !
  bEditorPrimitiveRelevance 前提下,按 bNormalTranslucency /
  bSeparateTranslucency / bTranslucencyModulate / bPostMotionBlurTranslucency 五个
  标志组合出 5 个 translucent 子桶 (或合并到 TranslucencyAll),并联动 Lumen 和
  Distortion 两个相邻 pass。
