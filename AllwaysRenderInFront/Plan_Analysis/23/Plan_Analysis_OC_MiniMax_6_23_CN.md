# 计划分析报告：UE 5.4 Mobile Forward 端 `bRenderAfterTranslucency` Mesh Pass

## 0. 总结

| 类别 | 结论 |
|---|---|
| **计划是否能实现核心目标？** | 部分可行 — 针对遮挡意图，Depth-Stencil 状态设置**有误** |
| **第 2-3 步（Primitive/SceneProxy）的 9 个行号声明** | **全部正确** |
| **EMeshPass 枚举与 `static_assert` 数字** | 计划的 `33 + 4` / `33` **正确**（当前源码为 32 / 32+4） |
| **关键遗漏** | 未更新 `SceneVisibility.cpp:2228-2232` 动态网格路径；未更新 `SceneRendering.cpp:4209` 移动端跳过列表；未处理 `RenderMobileEditorPrimitives`；未更新 `SetupPrecachePSOParams` |
| **设计问题** | `bAfterTranslucencyBasePass` 成员**冗余**；针对用户的遮挡目标，深度写入状态**反了** |
| **会静默失效的其他代理类型** | `Niagara`、`ParticleSystem`、`Landscape`、`GeometryCollection`、`HairStrands/Groom`、`Widget`、`TextRender`、`MRMesh` 等都不会用到新 Pass |

---

## 1. 第 1 步 — `EMeshPass` 枚举与 `GetMeshPassName`（计划第 1 节）

### 1.1 计划声明验证

| 计划声明 | 实际情况 | 结论 |
|---|---|---|
| `MeshPassProcessor.h:32` — `namespace EMeshPass` | 第 32 行 | ✓ |
| 编辑器块之前枚举有 33 个值 | **实际 32 个值**，不是 33（36–67 行） | ✗ 数量差一 |
| `NumBits = 6` | 第 77 行 | ✓ |
| 当前 `static_assert` = `32 + 4` / `32` | **实际**：`32 + 4`（第 128 行）/ `32`（第 130 行） | ✓ 计划正确说明当前是 `32` |
| 计划更新为 `33 + 4` / `33` | 数学：32 + 1 = 33 | ✓ **正确** |
| 列出的 32 个枚举值 | 都在 36–67 行 | ✓ 全部存在 |

### 1.2 没有其他静态断言对新数量敏感

唯一硬编码数量的断言位于 `MeshPassProcessor.h:128, 130`（计划已更新）。其他引用（`MeshPassProcessor.cpp:2258` 等）都是动态的，自动适配：

- `MeshPassProcessor.h:80` — `static_assert(EMeshPass::Num <= (1 << EMeshPass::NumBits))` — 33 ≤ 64 ✓
- `MeshPassProcessor.h:519` — `sizeof(FMeshPassMask::Data) * 8 >= EMeshPass::Num` — Data 是 `uint64` ✓
- `MeshPassProcessor.cpp:2258` — `static_assert(EMeshPass::Num <= FPSOCollectorCreateManager::MaxPSOCollectorCount)` — 必须 ≥ 33；UE 5.4 中 `MaxPSOCollectorCount` 远超 64，安全。

### 1.3 第 1 步结论：**OK** — 插入位置和 `static_assert` 数字正确。

---

## 2. 第 2-3 步 — `UPrimitiveComponent`、`FPrimitiveSceneProxy`、`FPrimitiveSceneProxyDesc`

### 2.1 计划声明验证（全部正确）

| 计划声明 | 实际位置 | 备注 |
|---|---|---|
| `PrimitiveComponent.h:407` UPROPERTY 块 | 第 407–408 行 | UPROPERTY 宏在 407，字段在 408 |
| `PrimitiveComponent.h:1917` `SetRenderInMainPass` UFUNCTION | 第 1917–1918 行 | |
| `PrimitiveComponent.cpp:4457` 实现 | 第 4457–4464 行 | |
| `PrimitiveComponent.cpp:333` 构造函数初始化 | 第 333 行 | |
| `PrimitiveSceneProxy.h:1200` 字段 | 第 1200 行 | |
| `PrimitiveSceneProxy.h:700` `ShouldRenderInMainPass()` | 第 700 行 | |
| `PrimitiveSceneProxy.cpp:277` `InitializeFrom` | 第 277 行 | |
| `PrimitiveSceneProxy.cpp:428` 构造函数 | 第 428 行 | |
| `PrimitiveSceneProxyDesc.h:93` 字段 | 第 93 行 | |
| `PrimitiveSceneProxyDesc.h:25` 初始化 | 第 25 行 | |

计划中镜像 `bRenderInMainPass` 添加 `bRenderAfterTranslucency` 的模式在结构上**正确**，这 5 个文件对于基础字段传递已经足够。

### 2.2 第 2-3 步结论：基础字段传递**OK**。

### 2.3 计划未更新的其他位置（潜在问题）

#### 2.3.1 `SetupPrecachePSOParams` — `PrimitiveComponent.cpp:4620-4632`

```cpp
// PrimitiveComponent.cpp:4620
void UPrimitiveComponent::SetupPrecachePSOParams(FPSOPrecacheParams& Params)
{
    Params.bRenderInMainPass = bRenderInMainPass;       // <-- 新字段是否需要镜像？
    Params.bRenderInDepthPass = bRenderInDepthPass;
    ...
}
```

如果 PSO 预缓存需要知道新 Pass，这里需要 `Params.bRenderAfterTranslucency = bRenderAfterTranslucency;`。**计划未更新。** 新 `FPSOPrecacheParams` 位域（`Engine/Public/PSOPrecache.h:110`）是 `uint64 : 1`，加 1 位安全。

#### 2.3.2 `FPrimitiveSceneInfo` 中缓存的 `bShouldRenderInMainPass`

`Engine/Source/Runtime/Renderer/Public/PrimitiveSceneInfo.h:675` 与 `Private/PrimitiveSceneInfo.cpp:305` 从代理缓存 `bShouldRenderInMainPass`。如果新字段需要在 SceneInfo 层面被读取，需镜像。**计划未更新。**

#### 2.3.3 构造链

计划正确处理了 `PrimitiveSceneProxy.cpp` 中两参数构造函数（第 428 行）和 `InitializeFrom`（第 277 行）。计划也覆盖了 `FPrimitiveSceneProxyDesc` 的默认构造函数。**无问题。**

#### 2.3.4 序列化

`UPROPERTY` 自动处理存读档。**无需额外代码。** ✓

---

## 3. 第 4 步 — `FMobileBasePassMeshProcessor` 和 Pass 注册

### 3.1 计划声明验证（行号全部正确）

| 计划声明 | 实际情况 |
|---|---|
| `MobileBasePassRendering.h:480` 构造函数声明 | 第 480–487 行 |
| `MobileBasePassRendering.h:533` `bPassUsesDeferredShading` 字段 | 第 533 行 |
| `MobileBasePass.cpp:810` 构造函数实现 | 第 810–826 行 |
| `MobileBasePass.cpp:867` `AddMeshBatch` | 第 867–890 行 |
| `MobileBasePass.cpp:1151` `CreateMobileBasePassProcessor` | 第 1151–1163 行 |
| `MobileBasePass.cpp:1223` REGISTER 宏 | 第 1223 行是 `// Skipping EMeshPass::TranslucencyAfterDOFModulate...` 注释；注册块在 1222 行结束。插入位置在 `MobileTranslucencyAfterDOFPass` 注册之后的下一行 |

### 3.2 ⚠ 关键设计问题 — 深度模板状态反了

计划写的是：

```c++
PassDrawRenderState.SetDepthStencilAccess(FExclusiveDepthStencil::DepthRead_StencilRead);
PassDrawRenderState.SetDepthStencilState(TStaticDepthStencilState<false, CF_DepthNearOrEqual>::GetRHI());
```

`TStaticDepthStencilState` 的第一个模板参数是 `bEnableDepthWrite`：
- `true` → 深度**写入启用**
- `false` → 深度**写入禁用**

用户的目标：*"透明物体不写入深度，所以我可以把透明物体遮挡住"* — 覆盖前面的半透明像素。

要让 after-translucency 不透明 Pass 正确遮挡其后的物体 **并** 覆盖前面的半透明颜色，必须：
1. **测试深度** 与现有不透明深度对比（不会穿墙）— `CF_DepthNearOrEqual` 部分正确。
2. **写入深度** 让新不透明物体的深度被后续 Pass 正确读取。
3. **写入颜色**（通过 `SetBlendState(TStaticBlendStateWriteMask<CW_RGBA>::GetRHI())` — 计划已包含；好的）。

**计划的 `TStaticDepthStencilState<false, ...>` 对于所述目标是错误的。** 应该是：

```c++
PassDrawRenderState.SetDepthStencilAccess(FExclusiveDepthStencil::DepthWrite_StencilNop);
PassDrawRenderState.SetDepthStencilState(TStaticDepthStencilState<true, CF_DepthNearOrEqual>::GetRHI());
```

如果用户确实不要深度写入，需要明白这意味着后续 Pass（`CustomDepth` Pass、遮挡等）将看不到 after-translucency 物体的深度 — 这很可能与用户的目标相悖。

**回答用户的问题 "SetDepthStencilState 和 TStaticDepthStencilState 的作用是什么？"：**
- `SetDepthStencilState` 配置该 Draw 的 GPU 深度模板状态。
- `TStaticDepthStencilState<bDepthWrite, DepthTestFunc>` 提供静态类型的 GPU 状态：`<true, CF_DepthNearOrEqual>` = 标准不透明（测试 + 写入，LessOrEqual），`<false, CF_DepthNearOrEqual>` = 半透明风格（测试、不写）。对于 after-translucency 不透明，第一个才正确。

### 3.3 设计问题 — `bAfterTranslucencyBasePass` 成员冗余

计划添加了 `bool bAfterTranslucencyBasePass` 成员，但同样的信息可通过 `InMeshPassType == EMeshPass::MobileAfterTranslucencyPass` 获得。在 `AddMeshBatch` 中可简化为：

```c++
// 在 AddMeshBatch 中，用 MeshPassType 替换 bAfterTranslucencyBasePass 分支：
const bool bAfterTranslucencyPass = (MeshPassType == EMeshPass::MobileAfterTranslucencyPass);
if (bAfterTranslucencyPass != PrimitiveSceneProxy->ShouldRenderAfterTranslucency())
{
    return;
}
```

或者直接比较 `MeshPassType` 以完全去掉这个成员。

### 3.4 设计问题 — `EFlags` 选择

计划对新 Pass 仅使用 `FMobileBasePassMeshProcessor::EFlags::CanUseDepthStencil`。这意味着：
- `CanReceiveCSM` **禁用** — 新 Pass 不接收级联阴影。对于 VR 移动端游戏通常可接受，但用户应确认。
- `ForcePassDrawRenderState` 不需要。
- `DoNotCache` 不需要。

可接受。无需更改。

### 3.5 第 4 步结论：**基本 OK**，但**深度模板状态必须修正**，`bAfterTranslucencyBasePass` 成员**冗余**。

---

## 4. 第 5 步 — `RenderMobileAfterTranslucencyPass` 和 Mobile Scene Renderer 接线

### 4.1 计划声明验证（行号全部正确）

| 计划声明 | 实际情况 |
|---|---|
| `SceneRendering.h:2695` `RenderMobileBasePass` 声明 | 第 2695 行 |
| `SceneRendering.h:2796` `TranslucencyInstanceCullingDrawParams` | 第 2796 行 |
| `MobileShadingRenderer.cpp:1433` `BuildInstanceCullingDrawParams` | 第 1433–1446 行 |
| `MobileShadingRenderer.cpp:1624` `RenderForwardSinglePass` | 实际从 1578 开始；`RenderTranslucency` 在第 1623 行（差 1） |
| `MobileShadingRenderer.cpp:1736` `RenderForwardMultiPass` | 实际从 1662 开始；`RenderTranslucency` 在第 1735 行（差 1） |
| `MobileBasePassRendering.cpp:492` `RenderMobileBasePass` | 第 470–491 行（计划说"492附近"— 在范围内） |

### 4.2 ⚠ 关键遗漏 — `FSceneRenderer::SetupMeshPass` 移动端跳过列表

`SceneRendering.cpp:4208-4212` 显式跳过 `EMeshPass::BasePass` 和 `EMeshPass::MobileBasePassCSM`（因为它们在 `SetupMobileBasePassAfterShadowInit` 中稍后合并并排序）。如果新 Pass 遵循同样模式（在 `BuildInstanceCullingDrawParams` 中构建），必须在此处添加：

```c++
// SceneRendering.cpp:4209 — 当前：
if (ShadingPath == EShadingPath::Mobile && (PassType == EMeshPass::BasePass || PassType == EMeshPass::MobileBasePassCSM))
// 应改为：
if (ShadingPath == EShadingPath::Mobile && (PassType == EMeshPass::BasePass || PassType == EMeshPass::MobileBasePassCSM || PassType == EMeshPass::MobileAfterTranslucencyPass))
```

否则新 Pass 会被设置两次：一次由 `FSceneRenderer::SetupMeshPass`（同文件第 4233 行），一次由 `BuildInstanceCullingDrawParams`。这会导致 mesh-draw-command 重复创建和错误的渲染。

### 4.3 ⚠ 关键遗漏 — `SetupMobileBasePassAfterShadowInit` 接线

`MobileShadingRenderer.cpp:377-427`（`SetupMobileBasePassAfterShadowInit`）将 `EMeshPass::BasePass` 和 `EMeshPass::MobileBasePassCSM` 合并到单个 `FParallelMeshDrawCommandPass::DispatchPassSetup` 调用（第 410-425 行）。新 Pass **未**在此注册，因此：

- 新 Pass 不会接收"与 CSM 合并"的特殊处理。
- 新 Pass 使用**默认**的 `FSceneRenderer::SetupMeshPass` 路径（根据上面的跳过列表），即新 Pass 在标准时间被设置为常规缓存的 mesh command pass。

只要新 Pass 不需要与 CSM 合并，这是 OK 的。对于典型的"after-translucency 不透明"用例，这是可以接受的。**但**用户必须从 `FSceneRenderer::SetupMeshPass` 跳过列表中移除新 Pass（如果希望它绕过移动端设置），或者保留跳过并接受标准移动端设置。如计划所写需要添加跳过列表。

### 4.4 ⚠ 关键遗漏 — `ComputeDynamicMeshRelevance` 中的动态网格路径

`SceneVisibility.cpp:2228-2232` — 动态网格路径具有**同样**的移动端特殊情况，添加 `EMeshPass::MobileBasePassCSM`。计划的更新是结构性的，用户可能意识到了也可能没意识到动态网格方面镜像了静态网格方面。**这也必须更新**，加上相同的 `if (ViewRelevance.bRenderAfterTranslucency) { EMeshPass::MobileAfterTranslucencyPass } else { EMeshPass::BasePass }` 逻辑。

### 4.5 ⚠ SceneVisibility.cpp 修改中的关键结构性 Bug

计划的静态网格分支（如 1564-1568 行所写）将 `MobileBasePassCSM` 添加放在 `if/else` **之外**：

```c++
if(ViewRelevance.bRenderAfterTranslucency)
{
    DrawCommandPacket.AddCommandsForMesh(..., EMeshPass::MobileAfterTranslucencyPass);
}else
{
    DrawCommandPacket.AddCommandsForMesh(..., EMeshPass::BasePass);
}
if (!bMobileBasePassAlwaysUsesCSM)
{
    DrawCommandPacket.AddCommandsForMesh(..., EMeshPass::MobileBasePassCSM);  // <-- 两个分支都会运行！
}
```

这意味着当 `bRenderAfterTranslucency=true` 时，原始体会被同时添加到新 Pass 和 `MobileBasePassCSM`，导致它被绘制两次（在带 CSM 的 base pass 中一次，在 after-translucency pass 中又一次）。**这是一个 Bug。** `MobileBasePassCSM` 的添加必须在 `else` 分支内，或者整个 `else` 分支也应被包装。

正确的代码应为：

```c++
if (ViewRelevance.bRenderAfterTranslucency)
{
    DrawCommandPacket.AddCommandsForMesh(..., EMeshPass::MobileAfterTranslucencyPass);
    // 注意：故意不添加 MobileBasePassCSM；新 Pass 意在作为独立的 after-translucency 重新绘制，
    // 而不是 CSM 增强的 Pass。
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

**同样的 Bug 存在于动态网格路径**（第 2211-2232 行）— 也必须修复。

### 4.6 ⚠ 关键遗漏 — `RenderMobileEditorPrimitives`

`MobileBasePassRendering.cpp:490` 在 `RenderMobileBasePass` 内部调用 `RenderMobileEditorPrimitives`。这是用于编辑器 viewmesh/topmesh、simple elements 等。计划的新 `RenderMobileAfterTranslucencyPass` **未**调用此函数。**需要决定**：编辑器原始体也应在新 Pass 中绘制吗？对于移动端 VR 游戏，发售版本答案可能是否（编辑器与发售无关），但对于编辑器中的迭代，应该考虑。

### 4.7 ⚠ `RenderForwardSinglePass`（MobileShadingRenderer.cpp:1578-1660）中的 Subpass 约束

单 Pass 路径使用两个 RDG subpasses，由第 1614 行的 `RHICmdList.NextSubpass()` 分隔。在此 subpass 切换之后，深度状态变为只读。计划将 `RenderMobileAfterTranslucencyPass` 插入到 `RenderTranslucency`（第 1623 行）**之后**，即在 subpass 切换（第 1614 行）**之后**。这意味着：

- 如果新 Pass 使用 `TStaticDepthStencilState<true, ...>`（深度写入），**subpass 状态的只读深度附件将导致验证错误**或强制渲染 Pass 边界。
- 因此正确插入应在第二个 subpass 的 lambda **内部**，但采用深度写入状态 — 这就是冲突。

**对用户来说有三种选择**：
1. **保持计划原样，使用深度只读**（`false, CF_DepthNearOrEqual`）— 在第二个 subpass 中工作，但不写入深度。（这是计划目前的选项，但与"遮挡半透明"的目标相冲突，因为遮挡需要为任何后续 Pass 写入深度。）
2. **在 subpass 切换之前插入新 Pass**（在 `RenderMobileBasePass` 第 1609 行之后，`PostRenderBasePass` 第 1612 行之前）— 会变成"在 base pass 和 decals/translucency 之间"，**不是**"在半透明之后"。语义错误。
3. **使用在现有 Pass 之后调度的新 RDG AddPass**（不在 lambda 内）— 破坏 subpass 合并但允许任何深度状态。如果 subpass 对性能不关键，这是最干净的方案。

计划未涉及此问题。**用户必须决定 subpass 策略。** 对于 subpass 是有意义的性能优化的移动端 VR 游戏，选项 1 是安全选择，但深度模板状态必须修订以匹配实际目标。

### 4.8 需要决定 — 哪些路径使用新 Pass？

计划只提到 `RenderForwardSinglePass`（1623）和 `RenderForwardMultiPass`（1735）。其他需要考虑的路径：
- `RenderCustomRenderPassBasePass`（MobileShadingRenderer.cpp:857-908）— scene capture。**发售版本可能跳过**。
- `RenderDeferredSinglePass`（第 1947 行）和 `RenderDeferredMultiPass`（第 1996 行）— 移动端延迟着色。**除非用户希望在 mobile-deferred 上也启用此功能，否则可能跳过**。
- `RenderHitProxies`（SceneHitProxyRendering.cpp）— 仅编辑器。

计划未提到这些。**需要决定。**

### 4.9 EMobileBasePass Uniform Buffer 模式

移动端 scene renderer 在第一个 subpass/first pass 使用 `EMobileBasePass::Opaque`，在第二个 subpass/second pass 使用 `EMobileBasePass::Translucent`。新 Pass 在第二个 subpass/second pass 上下文中运行，所以 `MobileBasePass` UB 被设置为 `Translucent` 模式。新 Pass 可能需要 `Opaque` 模式。这是一个 shader-UB 问题，用户应验证新 Pass 是否需要 `Opaque` uniform buffer 状态。可能 OK，因为新 Pass 使用与常规 base pass 相同的 shader。

### 4.10 第 5 步结论：存在**若干关键遗漏和一个关键结构性 Bug**。

---

## 5. 第 6 步 — `FPrimitiveViewRelevance` 和 `GetViewRelevance` 重写

### 5.1 计划声明验证（全部正确）

| 计划声明 | 实际情况 |
|---|---|
| `PrimitiveViewRelevance.h:54` `bRenderInMainPass` | 第 54 行 |
| `PrimitiveViewRelevance.h:103` 构造函数 | 第 89–104 行；第 103 行 = `bRenderInMainPass = true;` |
| `StaticMeshRender.cpp:2055` `GetViewRelevance` | 第 2055 行 |
| `SkeletalMesh.cpp:7107` `GetViewRelevance` | 第 7107 行 |

构造函数中的 `bRenderAfterTranslucency = false` **冗余**（结构已由 93–97 行的循环零初始化），但无害。

### 5.2 ⚠ 关键遗漏 — 许多其他代理类也设置 `bRenderInMainPass`

计划只更新 StaticMesh 和 SkeletalMesh，但以下代理也设置了 `bRenderInMainPass`，如果用户希望它们使用新 Pass，也需要设置 `bRenderAfterTranslucency`：

**高优先级（发售中常用的主要原始体类型）：**

| 文件 | 行号 | 类 |
|---|---|---|
| `Source/Runtime/Engine/Private/Particles/ParticleSystemRender.cpp` | 6856 | `FParticleSystemSceneProxy` |
| `Plugins/FX/Niagara/Source/Niagara/Private/NiagaraComponent.cpp` | 286 | `FNiagaraSceneProxy` |
| `Source/Runtime/Experimental/GeometryCollectionEngine/Private/GeometryCollection/GeometryCollectionSceneProxy.cpp` | 936 | `FGeometryCollectionSceneProxy` |
| `Source/Runtime/Experimental/GeometryCollectionEngine/Private/GeometryCollection/GeometryCollectionSceneProxy.cpp` | 1171 | `FNaniteGeometryCollectionSceneProxy` |
| `Source/Runtime/Landscape/Private/LandscapeRender.cpp` | 1987 | `FLandscapeComponentSceneProxy` |
| `Plugins/Runtime/HairStrands/Source/HairStrandsCore/Private/GroomComponent.cpp` | 1091 | `FHairStrandsSceneProxy`（Groom） |
| `Source/Runtime/UMG/Private/Components/WidgetComponent.cpp` | 559 | `FWidgetSceneProxy` |
| `Source/Runtime/Engine/Private/Components/HeterogeneousVolumeComponent.cpp` | 171 | `FHeterogeneousVolumeSceneProxy` |
| `Source/Runtime/Engine/Private/Components/TextRenderComponent.cpp` | 857 | `FTextRenderSceneProxy` |
| `Source/Runtime/MRMesh/Private/MRMeshComponent.cpp` | 492 | `FMRMeshSceneProxy` |
| `Source/Runtime/Engine/Private/Rendering/NaniteResources.cpp` | 1031 | `Nanite::FSceneProxy`（硬编码为 true） |

**中等优先级：** ProceduralMesh、CustomMesh、Cable、DynamicMesh、VirtualHeightfieldMesh、WaterMesh、Dataflow、PaperRender、SparseVolumeTexture、USD、ImagePlate、GeometryCache、LidarPointCloud。

**注意：** 多个代理委托给 `FStaticMeshSceneProxy::GetViewRelevance`（HISM、ISM、SplineMesh、Nanite-SplineMesh、WaterBodyInfoMesh、StereoStaticMesh）— 更新 `FStaticMeshSceneProxy` 时这些会自动继承新字段。

**需要决定：** 用户是否会在这些代理类型中的任何一个上使用新 Pass？如果只使用 StaticMesh 和 SkeletalMesh，计划 OK。如果更多，需要额外更新。

### 5.3 第 6 步结论：**如果范围仅为 StaticMesh+SkeletalMesh 则 OK**；**否则不完整**。

---

## 6. 第 7 步 — `RenderCore` 统计（计划第 5 节之后）

### 6.1 计划声明验证（全部正确）

| 计划声明 | 实际情况 |
|---|---|
| `RenderCore.cpp:65` `DEFINE_STAT(STAT_BasePassDrawTime)` | 第 65 行 |
| `RenderCore.h:44` `DECLARE_CYCLE_STAT_EXTERN(...)` | 第 44 行 |
| `BasePassRendering.h:144` `DECLARE_GPU_DRAWCALL_STAT_EXTERN(Basepass)` | 第 144 行 |

### 6.2 第 7 步结论：**OK** — 所有行号正确。

---

## 7. 计划未涉及的横切问题

### 7.1 不需要 `bAfterTranslucencyBasePass`
同样的信息可通过 `MeshPassType == EMeshPass::MobileAfterTranslucencyPass` 获得。建议删除冗余成员。

### 7.2 NumBits 约束
`EMeshPass::NumBits = 6` → 最多 64 个 Pass。当前 32；加完后 33。安全。

### 7.3 `FPrimitiveSceneInfo` 缓存
如果新字段需要在 scene-info 层面被查询，在 `FPrimitiveSceneInfo::bShouldRenderAfterTranslucency`（PrimitiveSceneInfo.h:675，.cpp:305）中镜像。否则并非严格必要，因为 `SceneVisibility.cpp` 通过 view-relevance 链检查代理。

### 7.4 无需特殊 "dithered LOD" 或 "Lumen" pass 集成
移动端不使用 Nanite base pass，Lumen 仅延迟。不需要在那里集成。

### 7.5 `EMarkMaskBits::StaticMeshVisibilityMapMask` 不需要更新
可见性 map 位用于静态网格可见性跟踪。新 Pass 使用同一位。

### 7.6 Subpass 约束总结
对于 after-translucency Pass 在 `RenderForwardSinglePass` 的第二个 subpass 中写入深度，要么：
- Pass 被重新调度到新的 RDG add-pass 中（失去 subpass 合并），或者
- 深度写入被禁用（计划的当前选择）— 但与用户的遮挡目标不符。

用户必须决定此权衡。

---

## 8. 最终结论与建议

### 8.1 计划中可正常工作的部分
1. **第 1 步**（EMeshPass 枚举）— 行号正确，`static_assert` 数学正确，插入位置正确。
2. **第 2-3 步**（PrimitiveComponent + SceneProxy + Desc）— 正确，对于代理级字段完整。
3. **第 6 步**（PrimitiveViewRelevance）— 构造函数正确，但若更多代理类型使用此功能则需扩展。
4. **第 7 步**（Stats）— 行号正确。

### 8.2 需要修复的部分
1. **深度模板状态**（第 4 步）：如果用户的遮挡目标正确，将 `<false, ...>` 改为 `<true, ...`，并将 `DepthRead_StencilRead` 改为 `DepthWrite_StencilNop`。
2. **`bAfterTranslucencyBasePass` 成员**（第 4 步）：冗余；改用 `MeshPassType`。
3. **SceneVisibility.cpp 静态网格分支**（第 6 步）：`MobileBasePassCSM` 的添加必须移入 `else` 分支。
4. **SceneVisibility.cpp 动态网格路径**：根本未涉及；镜像静态网格修复。
5. **`FSceneRenderer::SetupMeshPass` 跳过列表**（SceneRendering.cpp:4209）：必须将 `EMeshPass::MobileAfterTranslucencyPass` 添加到移动端跳过列表。
6. **`RenderForwardSinglePass` 的 Subpass 策略**：必须在深度写入（失去 subpass）和深度读取（匹配计划但不遮挡）之间做出决定。
7. **需要对 `RenderCustomRenderPassBasePass`、`RenderDeferredSinglePass`、`RenderDeferredMultiPass` 做出决定**。
8. **需要对新 Pass 中的 `RenderMobileEditorPrimitives`、`RenderMobileDebugView` 调用做出决定**。
9. **`SetupPrecachePSOParams`**（PrimitiveComponent.cpp:4622）：添加 `Params.bRenderAfterTranslucency = bRenderAfterTranslucency;`。
10. **`FPrimitiveSceneInfo` 缓存**（PrimitiveSceneInfo.h:675，.cpp:305）：可选，取决于是否需要 scene-info 级别查询。
11. **其他代理类**（第 6 步）：如果有 Niagara、Particle、Landscape、Groom 等需要新 Pass，它们的 `GetViewRelevance` 也必须更新。

### 8.3 风险评估
- **编译风险**：低。所有行号和字段位置正确。如果遗漏 `static_assert` 会触发，但计划已处理。
- **功能风险**：高。计划有 4 个关键问题（深度状态、动态网格路径、跳过列表、scene-vis 中的结构性 Bug），会导致功能不工作或工作不正确。
- **可维护性风险**：低。字段传递模式已经成熟。

### 8.4 推荐修改摘要
1. **修正深度模板状态** in `CreateMobileAfterTranslucencyPassProcessor`（第 4 步）。
2. **删除 `bAfterTranslucencyBasePass` 成员**；改用 `InMeshPassType == EMeshPass::MobileAfterTranslucencyPass`（第 4 步）。
3. **修复 SceneVisibility.cpp 静态网格 if/else**，使 `MobileBasePassCSM` 在 `bRenderAfterTranslucency` 为 true 时不被添加（第 6 步）。
4. **在 `ComputeDynamicMeshRelevance` 中添加动态网格路径更新**（第 6 步 — 缺失）。
5. **将 `EMeshPass::MobileAfterTranslucencyPass` 添加到移动端跳过列表**，在 `FSceneRenderer::SetupMeshPass` 中（SceneRendering.cpp:4209 — 缺失）。
6. **做出 subpass 策略决定**并记录。
7. **决定延迟/自定义渲染路径**并记录。
8. **更新 `SetupPrecachePSOParams`** 在 PrimitiveComponent.cpp:4622。
9. **决定哪些其他代理类**（如果有）需要 `GetViewRelevance` 更新。
