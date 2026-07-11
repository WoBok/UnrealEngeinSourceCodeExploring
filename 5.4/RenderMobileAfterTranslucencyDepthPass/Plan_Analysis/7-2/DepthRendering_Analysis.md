# DepthRendering.cpp 分析

本文只基于 `Engine/Source/Runtime/Renderer/Private/DepthRendering.cpp` 本文件进行分析，不展开其他文件的实现。

## 主要作用

`DepthRendering.cpp` 实现 Renderer 的深度绘制逻辑，核心是 Depth PrePass / Early Z Pass。它负责把需要提前写入深度缓冲的 Mesh 转换成深度绘制命令，并在 Deferred、Mobile、编辑器辅助绘制、HMD hidden area mask、LOD dither stencil 等场景中调度这些命令。

从文件结构看，它主要做以下几类事情：

1. 配置 Depth Pass 行为。
   - `r.ParallelPrePass` 控制是否并行执行 prepass。
   - `r.EarlyZSortMasked` 控制 masked draw 是否排在 draw order 后面。
   - `r.StencilForLODDither` / `r.StencilLODMode` 控制 dithered LOD transition 是否使用 stencil，以及 stencil 填充走 raster、compute 或 async compute。
   - `GetDepthPassInfo` 从 `Scene` 读取 `EarlyZPassMode`、`bEarlyZPassMovable`，并决定 LOD dither stencil pass 的 flags。

2. 定义深度绘制使用的 shader 组合。
   - `TDepthOnlyVS<true>`：position-only depth vertex shader。
   - `TDepthOnlyVS<false>`：普通 depth vertex shader。
   - `FDepthOnlyPS`：depth-only pixel shader。
   - `GetDepthPassShaders` 根据是否 position-only、材质是否每个像素都写入、是否使用 PixelDepthOffset、是否写 custom depth 等条件选择 shader pipeline。

3. 设置 Depth Pass 的固定渲染状态。
   - `SetupDepthPassState` 禁止颜色写入，开启 depth write，并使用 `CF_DepthNearOrEqual` depth test。
   - dithered LOD、mobile deferred 等路径会在此基础上修改 stencil/depth stencil 状态。

4. 调度 Deferred Shading 的 PrePass。
   - `FDeferredShadingSceneRenderer::RenderPrePass` 是 deferred 路径的主入口。
   - 它先绘制 HMD hidden area mask，再处理 dither stencil fill，然后在 `DepthPass.EarlyZPassMode != DDM_None` 时绘制主 depth pass。
   - 每个 view 会通过 `View.ParallelMeshDrawCommandPasses[DepthMeshPass].BuildRenderingCommands(...)` 构建命令，再通过 `DispatchDraw(...)` 提交。
   - 如果任一 view 使用 second stage depth pass，会复制第一阶段 depth，再调度 `EMeshPass::SecondStageDepthPass`。
   - 编辑器 primitive 由 `RenderPrePassEditorPrimitives` 补充绘制，也会使用 `FDepthPassMeshProcessor` 处理 `ViewMeshElements` / `TopViewMeshElements`。

5. 调度 Mobile 的 PrePass。
   - `FMobileSceneRenderer::ShouldRenderPrePass` 只在 `DDM_MaskedOnly` 或 `DDM_AllOpaque` 时返回 true。
   - `FMobileSceneRenderer::RenderPrePass` 直接 dispatch `EMeshPass::DepthPass` 的 mesh draw command。

6. 注册 MeshPassProcessor。
   - `DepthPass` 和 `MobileDepthPass` 使用 `CreateDepthPassProcessor`。
   - `SecondStageDepthPass` 使用 `CreateSecondStageDepthPassProcessor`。
   - `DitheredLODFadingOutMaskPass` 使用 `CreateDitheredLODFadingOutMaskPassProcessor`。

## Mesh 是否会被绘制的判断链

实际决定一个 `FMeshBatch` 是否进入 Depth Pass 的核心函数是：

- `FDepthPassMeshProcessor::AddMeshBatch`，行 1021 起。
- `FDepthPassMeshProcessor::TryAddMeshBatch`，行 974 起。
- `FDepthPassMeshProcessor::ShouldRender`，行 939 起。
- `FDepthPassMeshProcessor::Process`，行 787 起，最终构建 draw command。

### 1. MeshBatch 初始开关

`AddMeshBatch` 首先读取：

```cpp
bool bDraw = MeshBatch.bUseForDepthPass;
```

如果 `MeshBatch.bUseForDepthPass` 为 false，这个 mesh 不会继续进入后续材质和 shader 判断。

### 2. Occluder 过滤

当满足以下条件时，会进入 occluder 过滤：

```cpp
bDraw
&& bRespectUseAsOccluderFlag
&& !MeshBatch.bUseAsOccluder
&& EarlyZPassMode < DDM_AllOpaque
```

过滤规则如下：

- 如果没有 `PrimitiveSceneProxy`，直接 `bDraw = false`。
- 如果有 `PrimitiveSceneProxy`，必须满足：
  - `PrimitiveSceneProxy->ShouldUseAsOccluder()` 为 true。
  - primitive 不是 movable，或者 `bEarlyZPassMovable` 允许 movable 进入 Early Z。
- 如果当前是 dynamic mesh command，还会按屏幕尺寸过滤：
  - primitive 的 bounds 半径换算到屏幕空间后，必须大于 `GMinScreenRadiusForDepthPrepass`。

注意：当 `EarlyZPassMode >= DDM_AllOpaque` 时，这段 occluder 过滤不会执行，因此 `DDM_AllOpaque` / `DDM_AllOpaqueNoVelocity` 不再要求 mesh 被标为 occluder。

### 3. `DDM_AllOpaqueNoVelocity` 额外跳过 velocity primitive

当 `EarlyZPassMode == DDM_AllOpaqueNoVelocity` 且存在 `PrimitiveSceneProxy` 时，代码会检查这个 primitive 是否会在后续 velocity pass 写 depth + velocity。

如果满足：

- `FOpaqueVelocityMeshProcessor::PrimitiveCanHaveVelocity(...)`
- dynamic view 存在
- `PrimitiveHasVelocityForFrame(...)`
- `PrimitiveHasVelocityForView(...)`

则 `bDraw = false`。也就是说，该模式会避免在 prepass 中绘制后续 velocity pass 会处理的物体。

### 4. 材质代理和 shader map 必须有效

如果 `bDraw` 仍为 true，`AddMeshBatch` 会沿着 `MaterialRenderProxy` fallback 链查找可用材质：

- `MaterialRenderProxy->GetMaterialNoFallback(FeatureLevel)` 必须返回材质。
- `Material->GetRenderingThreadShaderMap()` 必须有效。
- 找到后调用 `TryAddMeshBatch`。
- 如果当前 proxy 不可用，则继续 `GetFallback(FeatureLevel)`。

### 5. TryAddMeshBatch 的材质级过滤

`TryAddMeshBatch` 会先排除不适合默认 opaque/depth pass 的材质：

```cpp
!IsTranslucentBlendMode(Material)
&& (!PrimitiveSceneProxy || PrimitiveSceneProxy->ShouldRenderInDepthPass())
&& ShouldIncludeDomainInMeshPass(Material.GetMaterialDomain())
&& ShouldIncludeMaterialInDefaultOpaquePass(Material)
```

因此，能继续进入 `ShouldRender` 的 mesh 必须满足：

- 材质不是 translucent blend mode。
- 没有 proxy，或 proxy 明确允许 `ShouldRenderInDepthPass()`。
- 材质 domain 能进入 mesh pass。
- 材质能进入默认 opaque pass。

之后它还会计算：

- vertex factory 是否支持 position-only stream。
- vertex factory 是否支持 null pixel shader。
- 材质是否真正需要评估 World Position Offset。

这些值会传给 `ShouldRender`。

### 6. ShouldRender 按 EarlyZPassMode 和材质属性决策

`ShouldRender` 会输出三个结果：

- `bShouldRender`：是否绘制。
- `bUseDefaultMaterial`：是否用默认材质替换原材质。
- `bPositionOnly`：是否走 position-only depth shader。

第一条快速路径是 position-only：

```cpp
IsOpaqueBlendMode(Material)
&& EarlyZPassMode != DDM_MaskedOnly
&& bSupportPositionOnlyStream
&& !bMaterialModifiesMeshPosition
&& Material.WritesEveryPixel(...)
```

满足时：

- 绘制。
- 使用默认材质。
- 使用 position-only 路径。

否则进入普通路径。代码用以下条件判断材质是否需要 masked/depth pixel 处理：

```cpp
const bool bMaterialMasked =
    !Material.WritesEveryPixel(...)
    || Material.IsTranslucencyWritingCustomDepth();
```

普通路径的绘制规则是：

- 非 masked 材质：`EarlyZPassMode != DDM_MaskedOnly` 时绘制。
- masked 材质：`EarlyZPassMode != DDM_NonMaskedOnly` 时绘制。

如果是非 masked 且材质不修改 mesh position，则仍可使用默认材质，但不是 position-only：

```cpp
bUseDefaultMaterial = true;
bPositionOnly = false;
```

如果是 masked，或者材质会修改顶点位置，则必须保留原材质相关 shader，以保证 mask / WPO / depth 行为正确。

### 7. Process 是最后一道实际生成命令的门

当 `ShouldRender` 返回 true 后，`TryAddMeshBatch` 会根据 `bPositionOnly` 调用：

- `Process<true>`：position-only。
- `Process<false>`：普通 depth pass。

`Process` 内还必须成功取得 depth pass shader：

```cpp
GetDepthPassShaders(...)
```

如果 shader 获取失败，返回 false，不会构建 draw command。

成功后会：

- 应用 dithered LOD transition depth/stencil 状态。
- mobile 路径设置 stencil 信息。
- 计算 masked draw 的 sort key。
- 调用 `BuildMeshDrawCommands(...)` 生成实际绘制命令。

因此，一个 mesh 最终被绘制的必要链路可以概括为：

```text
MeshBatch.bUseForDepthPass
-> 通过 occluder / movable / 屏幕尺寸过滤
-> 在 DDM_AllOpaqueNoVelocity 下未被 velocity pass 排除
-> 找到有效 MaterialRenderProxy / Material / ShaderMap
-> 非 translucent
-> PrimitiveSceneProxy 允许 ShouldRenderInDepthPass
-> 材质 domain 和 opaque pass 允许
-> EarlyZPassMode 与 masked/non-masked 规则匹配
-> 能取得 DepthPass shader
-> BuildMeshDrawCommands
```

## EarlyZPassMode 对 Mesh 类型的影响

只根据本文件中的判断，可整理为：

| EarlyZPassMode | 本文件中的效果 |
| --- | --- |
| `DDM_None` | `RenderPrePass` 不绘制 depth pass。 |
| `DDM_MaskedOnly` | `ShouldRender` 排除普通 opaque / non-masked，只允许 masked 类路径继续。Mobile prepass 会在此模式下启用。 |
| `DDM_NonMaskedOnly` | `ShouldRender` 允许 non-masked，排除 masked。 |
| `DDM_AllOccluders` | 仍会执行 occluder 过滤；材质侧可允许 masked 和 non-masked。 |
| `DDM_AllOpaque` | 不执行 `!MeshBatch.bUseAsOccluder` 引发的 occluder 过滤；材质侧可允许 masked 和 non-masked，translucent 仍被排除。Mobile prepass 会在此模式下启用。 |
| `DDM_AllOpaqueNoVelocity` | 类似 all opaque，但会跳过本帧/本 view 需要 velocity pass 的 primitive。 |

## 结论

`DepthRendering.cpp` 的核心职责是组织 Early Z / Depth PrePass 的渲染状态、shader、pass 调度和 mesh draw command 构建。Mesh 是否绘制并不是单点判断，而是由 `MeshBatch` 标记、occluder 策略、primitive proxy、EarlyZPassMode、材质透明/遮罩/WPO 属性、vertex factory 能力、shader 可用性共同决定。最关键的排查入口是 `FDepthPassMeshProcessor::AddMeshBatch -> TryAddMeshBatch -> ShouldRender -> Process`。
