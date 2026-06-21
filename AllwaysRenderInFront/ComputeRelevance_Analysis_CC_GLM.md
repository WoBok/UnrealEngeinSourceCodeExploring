# `FRelevancePacket::ComputeRelevance` 分析

> 源文件：`Engine/Source/Runtime/Renderer/Private/SceneVisibility.cpp`
> 函数位置：约第 1299–1958 行（UE 5.4 分支）
> 调用入口：`FRelevancePacket::LaunchComputeRelevanceTask`（1161） / `FComputeAndMarkRelevance::Finalize`（2068）

## 一、作用概述

`ComputeRelevance` 是 Unreal 可见性系统的**核心阶段之一**。它在视锥剔除（FrustumCull）之后执行，针对当前 View 已通过剔除、被判定为「可能可见」的一组 Primitive（保存在 `Input.Prims` 中），逐个计算其**对当前视图的相关性（View Relevance）**，并据此：

1. 调用每个 Primitive 的 `GetViewRelevance(&View)`，得到该 Primitive 在本视图下的渲染相关性标志（静态/动态/阴影/半透明/编辑器…）。
2. 对**静态相关性**的 Primitive 进行 LOD 选择、HLOD 淡入淡出、距离剔除判定，并把对应的静态网格的 MeshDrawCommand 按 Pass 注册到 `DrawCommandPacket`（BasePass / DepthPass / Velocity / SkyPass / 半透明各类桶 / CustomDepth / 光线追踪等）。
3. 对**动态相关性**的 Primitive 收集索引，供后续动态渲染路径处理。
4. 累计若干视图级别的统计量与全局标志（着色模型掩码、Substrate、天空材质、单层水、GDF、CustomDepth 等）。
5. 更新 Primitive 的 `LastRenderTime`、组件最后渲染时间，触发反射捕获/间接光照缓存的按需更新。

简言之：**把“可能可见”转化为“具体要画哪些 Pass 的哪些 MeshDrawCommand”**，是连接剔除与实际绘制命令生成/收集的桥梁。

## 二、并行与调度上下文

- `ComputeRelevance` 运行在 **ParallelRenderingThread** 任务标签下（`ETaskTag::EParallelRenderingThread`），可被多个 `FRelevancePacket` 并行执行。
- 视图的可见 Primitive 被切分成多个 Packet（每包 `NumPrimitivesPerPacket` 个），由 `FComputeAndMarkRelevance` 管理：
  - `EVisibilityTaskSchedule::Parallel`：每装满一个包就 `LaunchComputeRelevanceTask` 立即派发任务。
  - 否则在 `Finalize()` 中用 `ParallelFor` 统一执行（见 2062–2069）。
- 每个任务依赖 `Scene.GetCacheMeshDrawCommandsTask()`（缓存 MeshDrawCommand 的任务），因为相关性判定会引用已缓存的命令。
- 动态 Primitive 索引列表 `DynamicPrimitiveIndexList` 是每任务本地对象；任务结束后通过 `CommandPipe` 入队或就地释放计数，避免跨线程共享可变状态。

## 三、关键执行步骤

### 1. 初始化视图级累加器（1304–1326）
重置一组视图级标志为 0/false：`CombinedShadingModelMask`、Substrate 相关、`bSceneHasSkyMaterial`、`bHasSingleLayerWaterMaterial`、`bUsesGlobalDistanceField` 等。预计算若干路径相关常量：
- `ShadingPath`（Mobile / Deferred 等）。
- `bMobileMaskedInEarlyPass`、`bMobileBasePassAlwaysUsesCSM`、`bVelocityPassWritesDepth`。
- HLOD 状态（`bHLODActive`、`HLODState`）。
- `MaxDrawDistanceScale = ViewDistanceScale * FOV 距离缩放`，用于后续距离剔除。

### 2. 内联 lambda：登记动态/编辑器 Primitive（1328–1359）
- `AddEditorDynamicPrimitive`：编辑器下计入 `NumVisibleDynamicEditorPrimitives`，并原子 OR 写入 `DynamicPrimitiveViewMasks`（若启用）或追加到列表。
- `AddDynamicPrimitive`：同理登记常规动态 Primitive。
- 使用 `FPlatformAtomics::InterlockedOr` 保证多包并发写掩码安全。

### 3. 主循环：遍历 `Input.Prims`（1361–1954）
对每个 Primitive 索引 `BitIndex`：

#### 3.1 预取优化（1365–1382）
对下一个 Primitive 的 `Scene.Primitives` / `PrimitiveSceneProxies` 以及当前 Primitive 的 `StaticMeshRelevances` / `SceneData` 做 `Prefetch`，隐藏内存延迟——这是一个对缓存极度敏感的热循环。

#### 3.2 获取 ViewRelevance（1383–1394）
```cpp
ViewRelevance = PrimitiveSceneProxy->GetViewRelevance(&View);
```
解出 `bStaticRelevance / bDrawRelevance / bDynamicRelevance / bShadowRelevance / bEditorRelevance / bTranslucentRelevance / bHairStrandsRelevance` 等标志。`ViewRelevance` 是该 Primitive 所有材质相关性的“汇总”。

#### 3.3 反射捕获可见性早退（1396–1400）
若视图是反射捕获且 Primitive `!IsVisibleInReflectionCaptures()`，归入 `NotDrawRelevant` 并跳过。

#### 3.4 静态相关性处理（1402–1751）—— 核心路径
当 `bStaticRelevance && (bDrawRelevance || bShadowRelevance)`：

- **LOD 选择**：`ComputeLODForMeshes(...)` 计算应渲染的 `FLODMask`，并记录到 `PrimitivesLODMask`。
- **HLOD 淡入淡出判定**：`bIsHLODFading / bIsHLODFadingOut`、`bIsLODDithered`、`bIsLODRange`。
- **距离/屏幕尺寸阈值**：
  - `bDrawShadowDepth`：球体半径相对 CSM 最小屏幕半径是否足够大。
  - `bDrawDepthOnly`：基于 `GMinScreenRadiusForDepthPrepass` 或全 EarlyZPass。
- **逐静态网格遍历**（`StaticMeshRelevances`）：
  - Overlay 材质独立的更短剔除距离。
  - 仅当 `LODToRender.ContainsLOD(StaticMeshLODIndex)` 时处理该 mesh。
  - **MarkMask 计算**：根据 HLOD/LOD dither 状态设置 `StaticMeshFadeOutDitheredLODMapMask` / `FadeIn...`；被 HLOD 替代的 LOD 会被 `bHiddenByHLODFade` 隐藏。
  - **可缓存性** `bCanCache = !bIsPrimitiveDistanceCullFading && !bIsMeshDitheringLOD`——dither 或距离淡出中的命令不能复用缓存。
  - **GPU LOD 区间剔除标志** `CullingPayloadFlags`：MinScreenSizeCull / MaxScreenSizeCull，确保区间两端只在单方向按屏幕尺寸剔除。
  - **按 Pass 注册 MeshDrawCommand**（`DrawCommandPacket.AddCommandsForMesh`）：
    - Velocity（不透明/半透明，受 `bVelocityPassWritesDepth` 影响是否复用）。
    - DepthPass / SecondStageDepthPass（移动端 Masked 早通道）。
    - BasePass（移动端区分 Skydome 与 CSM）；非移动端追加 SkyPass、SingleLayerWater（含深度预通道）、AnisotropyPass、CustomDepth、LightmapDensity/DebugViewMode、HitProxy（编辑器）。
    - 半透明分桶：Standard / StandardModulate / AfterDOF / AfterDOFModulate / AfterMotionBlur；不支持 DOF 后半透明时统一进 `TranslucencyAll`。
    - Lumen 半透明表面光照、Distortion。
    - 编辑器 LevelInstance 可视化、EditorSelection。
    - 体积材质域（Volume / HeterogeneousVolumes）、天空材质、需排序三角形的半透明、MeshDecal 等收集到对应 batch 列表。
  - 记录 `MarkMasks[StaticMeshRelevance.Id]`，用于后续 visibility 位图转置。
  - 统计 `NumVisibleStaticMeshElements` 与三角形数。

#### 3.5 非绘制相关性早退（1753–1757）
`!bDrawRelevance` → `NotDrawRelevant.AddPrim`，跳过剩余处理。

#### 3.6 编辑器 Nanite 实例选取（1759–1840，`WITH_EDITOR`）
`CollectSelectedNaniteInstanceDraws` lambda：对 Nanite 网格收集选中实例的 `FInstanceDraw` 与 HitProxyID，支持 LevelInstance 可视化与选择高亮。

#### 3.7 动态/编辑器/毛发 Primitive 登记（1842–1858）
- `bEditorRelevance` → `AddEditorDynamicPrimitive`。
- `bDynamicRelevance` → `AddDynamicPrimitive`；若有简单光源则加入 `VisibleDynamicPrimitivesWithSimpleLights`。
- `bHairStrandsRelevance` → `AddDynamicPrimitive`。

#### 3.8 半透明计数与 Distortion（1860–1899）
对半透明 Primitive 按 DOF 分桶策略计入 `TranslucentPrimCount`，并设置 `bHasDistortionPrimitives`。

#### 3.9 视图级标志累加（1901–1912）
逐 Primitive OR/MAX 累加：着色模型掩码、Substrate、复杂特殊渲染路径、GDF、光照通道、半透明表面光照、CustomDepth/Stencil、天空、单层水、第二阶段深度通道。

#### 3.10 CustomDepth / Nanite 实例范围（1914–1930）
`bRenderCustomDepth` 时收集 stencil 值；Nanite 网格记录 `FPrimitiveInstanceRange` 到 `NaniteCustomDepthInstances`。

#### 3.11 半透明自阴影（1932–1936）
`GUseTranslucencyShadowDepths && bTranslucentSelfShadow` → 登记到 `TranslucentSelfShadowPrimitives`。

#### 3.12 时间戳与缓存更新（1938–1953）
- 更新 `PrimitiveSceneInfo->LastRenderTime` 与组件最后渲染时间（`bUpdateLastRenderTimeOnScreen = true`）。
- 按需 `CacheReflectionCaptures()`。
- 需要更新间接光照缓存缓冲的 Primitive 登记到 `DirtyIndirectLightingCacheBufferPrimitives`。

### 4. 提交静态网格元素计数（1956–1957）
循环结束后，原子地把本包累计的 `NumVisibleStaticMeshElements` 加到 `View.NumVisibleStaticMeshElements`（多包并发安全）。

## 四、关键设计要点总结

1. **职责定位**：剔除（谁可能可见）→ **ComputeRelevance（决定画哪些 Pass 的哪些 mesh）** → Finalize 合并与转置 visibility 位图 → 实际提交绘制。它不直接画，而是“填表”。

2. **相关性驱动一切**：以 `FPrimitiveViewRelevance` 为输入开关，决定静态/动态/阴影/半透明/编辑器等分支；静态路径进一步用 LOD + HLOD + 距离阈值细化到每个 mesh 的每个 Pass。

3. **MeshDrawCommand 缓存复用**：`bCanCache` 控制能否复用已缓存的命令；dither 过渡、距离淡出这类“逐视图逐 mesh 状态”会禁用缓存，强制动态重建。

4. **多 Pass 注册**：单个 mesh 可能同时进 Velocity/Depth/BasePass/Translucency/CustomDepth 等多个 Pass 桶，每个 Pass 是独立缓存的 mesh command 列表。

5. **并行安全**：包内状态私有，跨包共享状态（`DynamicPrimitiveViewMasks`、`View.NumVisibleStaticMeshElements`）用原子操作；任务通过 `FTaskEvent` 依赖与 `CommandPipe` 串行化动态列表。

6. **性能意识**：大量 `Prefetch`、`SCOPE_CYCLE_COUNTER(STAT_ComputeViewRelevance)`、按包切分并行，体现这是渲染热路径。

7. **特性门控密集**：移动端/光线追踪/Substrate/Lumen/HairStrands/Nanite/HLOD/编辑器各有独立分支，函数是平台与渲染特性差异的汇聚点。

8. **副作用不止于绘制**：还承担 LastRenderTime 维护、反射捕获/间接光照缓存触发等“被看见”后的状态更新，影响流送、LOD、光照更新等多子系统。

## 五、与其他阶段的衔接

- **上游**：`FrustumCull` 任务产出 `Input.Prims`（可见 Primitive 索引）；任务依赖 `Tasks.ComputeRelevance.AddPrerequisites(Tasks.FrustumCull)`（3388）。
- **下游**：`FComputeAndMarkRelevance::Finalize` 汇总各包的 `DrawCommandPacket`、`MarkMasks`，做 visibility 位图转置与命令预留；`CacheMeshDrawCommandsTask` 为其提供可复用缓存命令。
- **任务图**：`Tasks.ComputeRelevance` 是一个 `FTaskEvent`，所有包的 `ComputeRelevanceTask` 作为前置，`Trigger()` 后下游（如 mesh command 收集）方可继续。
