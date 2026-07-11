# UE5.4 移动端 "After Translucency" 渲染方案分析

> 分析基于当前工程源码（UE 5.4）中以下原始文件：
> - `Engine/Source/Runtime/Renderer/Public/MeshPassProcessor.h`
> - `Engine/Source/Runtime/Engine/Classes/Components/PrimitiveComponent.h`
> - `Engine/Source/Runtime/Engine/Private/Components/PrimitiveComponent.cpp`
> - `Engine/Source/Runtime/Engine/Public/PrimitiveSceneProxy.h`
> - `Engine/Source/Runtime/Engine/Private/PrimitiveSceneProxy.cpp`
> - `Engine/Source/Runtime/Engine/Public/PrimitiveSceneProxyDesc.h`
> - `Engine/Source/Runtime/Renderer/Private/MobileBasePassRendering.h`
> - `Engine/Source/Runtime/Renderer/Private/MobileBasePass.cpp`
> - `Engine/Source/Runtime/Renderer/Private/MobileBasePassRendering.cpp`
> - `Engine/Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp`
> - `Engine/Source/Runtime/Renderer/Private/SceneRendering.h`
> - `Engine/Source/Runtime/Renderer/Private/SceneRendering.cpp`
> - `Engine/Source/Runtime/Renderer/Private/SceneVisibility.cpp`
> - `Engine/Source/Runtime/Engine/Public/PrimitiveViewRelevance.h`
> - `Engine/Source/Runtime/Engine/Private/StaticMeshRender.cpp`
> - `Engine/Source/Runtime/Engine/Private/SkeletalMesh.cpp`
> - `Engine/Source/Runtime/RenderCore/Public/RenderCore.h`
> - `Engine/Source/Runtime/RenderCore/Private/RenderCore.cpp`
> - `Engine/Source/Runtime/Renderer/Private/BasePassRendering.h`
> - `Engine/Source/Runtime/Renderer/Private/BasePassRendering.cpp`

---

## 1. 需求与方案核心思路

**目标**：在移动端 Forward 渲染路径下，让标记的 opaque/masked 物体跳过普通 BasePass，在 Translucency 渲染完成后再执行一次 opaque 风格的渲染，从而利用已有的深度缓冲遮挡透明物体。

**方案核心**：
1. 新增 `EMeshPass::MobileAfterTranslucencyPass`。
2. 新增 `UPrimitiveComponent::bRenderAfterTranslucency` 开关，并一路同步到 `FPrimitiveSceneProxy`、`FPrimitiveViewRelevance`。
3. 复用 `FMobileBasePassMeshProcessor`，通过构造函数/AddMeshBatch 中的分支，把标记物体从 BasePass 过滤掉并加入新 Pass。
4. 在 `RenderForwardSinglePass` / `RenderForwardMultiPass` 的 `RenderTranslucency` 之后调用新的 `RenderMobileAfterTranslucencyPass`。
5. 在 `SceneVisibility` 中对静态/动态 Mesh 进行 Pass 分流。

---

## 2. 源码核对结论

当前工程源码**尚未应用**方案中的任何修改，处于 UE 5.4 原始状态：
- `EMeshPass::Num` 在非编辑器配置下为 `32`，编辑器配置下为 `32 + 4`。
- `FPrimitiveSceneProxy` 中没有 `bRenderAfterTranslucency` 字段。
- `FMobileBasePassMeshProcessor` 构造函数没有 `bAfterTranslucencyBasePass` 参数。
- `FMobileSceneRenderer` 中没有 `RenderMobileAfterTranslucencyPass` 函数和 `AfterTranslucencyInstanceCullingDrawParams`。

因此方案是**从零开始的新增功能**，不存在与已有改动的冲突。

---

## 3. 方案中正确的部分

### 3.1 EMeshPass 枚举扩展
- 新增 `MobileAfterTranslucencyPass` 合理。
- `NumBits = 6` 足够容纳（最大 64），当前从 32 增加到 33 后仍远小于 64。
- `static_assert` 从 `32 / 32+4` 更新为 `33 / 33+4` 正确。
- `FMeshPassMask`（`uint64 Data`）和 `FViewInfo::NumVisibleDynamicMeshElements[EMeshPass::Num]`、`ParallelMeshDrawCommandPasses[EMeshPass::Num]` 都会因 `EMeshPass::Num` 自增而自动扩展，**无需手动扩容**。

### 3.2 PrimitiveComponent / Proxy / Desc 数据链
- 在 `UPrimitiveComponent` 新增 `bRenderAfterTranslucency` 位域、`SetRenderAfterTranslucency` Setter、构造函数初始化，思路正确。
- 同步到 `FPrimitiveSceneProxyDesc::InitializeFrom`、`FPrimitiveSceneProxy` 的位域与构造函数初始化列表，数据链路完整。

### 3.3 PrimitiveViewRelevance 分流标记
- 在 `FPrimitiveViewRelevance` 新增 `bRenderAfterTranslucency` 并在 `StaticMeshRender.cpp` / `SkeletalMesh.cpp` 的 `GetViewRelevance` 中赋值，方向正确。

### 3.4 Mesh Processor 复用
- 复用 `FMobileBasePassMeshProcessor` 是合理选择，可以沿用 opaque 材质判定、LightmapPolicy、PSO 缓存等逻辑。

### 3.5 渲染时机
- 在 `RenderForwardSinglePass` 和 `RenderForwardMultiPass` 中，把 `RenderMobileAfterTranslucencyPass` 放在 `RenderTranslucency` 之后，符合"在透明之后渲染"的需求。

### 3.6 Instance Culling
- 在 `BuildInstanceCullingDrawParams` 中为 `MobileAfterTranslucencyPass` 调用 `BuildRenderingCommands`，并把 `AfterTranslucencyInstanceCullingDrawParams` 作为 `FMobileSceneRenderer` 成员保存，做法与 `TranslucencyInstanceCullingDrawParams` 一致，正确。

---

## 4. 错误 / 必须修正

### 4.1 缺少 `DEFINE_GPU_DRAWCALL_STAT(AfterTranslucency)`

**问题**：方案在 `BasePassRendering.h` 中声明了 `DECLARE_GPU_DRAWCALL_STAT_EXTERN(AfterTranslucency)`，但没有在任何 `.cpp` 中定义它。

**后果**：链接阶段会报未定义引用。

**修正**：在 `Engine/Source/Runtime/Renderer/Private/BasePassRendering.cpp` 中（与 `DEFINE_GPU_DRAWCALL_STAT(Basepass);` 同文件）添加：

```cpp
DEFINE_GPU_DRAWCALL_STAT(AfterTranslucency);
```

### 4.2 `CreateMobileAfterTranslucencyPassProcessor` 不应注释掉 BlendState

**问题**：方案中注释了 `PassDrawRenderState.SetBlendState(TStaticBlendStateWriteMask<CW_RGBA>::GetRHI());`。

**后果**：
- `FMobileBasePassMeshProcessor::Process` 对 opaque 材质调用 `MobileBasePass::SetOpaqueRenderState` 时，**不会为普通 opaque 材质设置 BlendState**（只在 masked + alpha-to-coverage 时设置）。
- 如果 `FMeshPassProcessorRenderState` 的默认 BlendState 不是写入 RGBA，AfterTranslucency Pass 的颜色将不会覆盖透明层，导致无法实现"遮挡透明物体"的核心需求。

**修正**：保留与 BasePass 一致的 BlendState 设置：

```cpp
PassDrawRenderState.SetBlendState(TStaticBlendStateWriteMask<CW_RGBA>::GetRHI());
```

### 4.3 `bAfterTranslucencyBasePass` 成员冗余

**问题**：方案在 `FMobileBasePassMeshProcessor` 中新增了 `const bool bAfterTranslucencyBasePass` 并在构造函数中传入。文档末尾也意识到了这一点。

**后果**：增加不必要的成员变量和构造函数复杂度；AddMeshBatch 中的判断逻辑可以更简洁。

**建议修正**：直接比较 `MeshPassType`：

```cpp
void FMobileBasePassMeshProcessor::AddMeshBatch(...)
{
    // ... 现有 ShouldRenderInMainPass 检查 ...

    const bool bIsAfterTranslucencyPass = (MeshPassType == EMeshPass::MobileAfterTranslucencyPass);
    if (bIsAfterTranslucencyPass != PrimitiveSceneProxy->ShouldRenderAfterTranslucency())
    {
        return;
    }
    // ...
}
```

这样可以移除 `bAfterTranslucencyBasePass` 成员和构造函数参数。

### 4.4 `FMobileBasePassMeshProcessor::Process` 会覆盖 AfterTranslucency Pass 的深度状态

**问题**：方案在 `CreateMobileAfterTranslucencyPassProcessor` 中设置：

```cpp
PassDrawRenderState.SetDepthStencilAccess(FExclusiveDepthStencil::DepthRead_StencilRead);
PassDrawRenderState.SetDepthStencilState(TStaticDepthStencilState<false, CF_DepthNearOrEqual>::GetRHI());
```

但 `FMobileBasePassMeshProcessor::Process` 对 opaque 材质会调用 `MobileBasePass::SetOpaqueRenderState`：

```cpp
if (bEnableReceiveDecalOutput || bUsesDeferredShading)
{
    DrawRenderState.SetDepthStencilState(TStaticDepthStencilState<
            true, CF_DepthNearOrEqual,    // <-- 深度写入变为 true
            true, CF_Always, SO_Keep, SO_Keep, SO_Replace,
            ...>::GetRHI());
    DrawRenderState.SetStencilRef(StencilValue);
}
```

在移动端开启 `MobileHDR` 时，`bEnableReceiveDecalOutput = (Flags & CanUseDepthStencil) && IsMobileHDR()` 通常为 `true`，会把深度写入重新打开，并写入 stencil。

**后果**：
- 单 Pass 模式下，AfterTranslucency 处于 `DepthReadSubpass`，深度附件是 read-only，尝试写入深度可能触发 validation error 或被驱动忽略。
- 多 Pass 模式下，`SecondPassParameters` 的 `DepthStencilAccess` 是 `DepthRead_StencilRead`，深度写入同样不合法。
- 即使不报错，也可能意外修改 stencil 状态。

**建议修正（任选其一）**：

**方案 A（推荐）**：给 `CreateMobileAfterTranslucencyPassProcessor` 传入 `EFlags::ForcePassDrawRenderState`，让 `Process` 不再调用 `SetOpaqueRenderState` 覆盖深度/模板状态：

```cpp
const FMobileBasePassMeshProcessor::EFlags Flags =
    FMobileBasePassMeshProcessor::EFlags::CanUseDepthStencil |
    FMobileBasePassMeshProcessor::EFlags::ForcePassDrawRenderState;
```

代价：AfterTranslucency 物体不会接收 decals（ stencil 不写入 decal 接收标记），对大多数"遮挡透明 UI/特效"的场景可接受。

**方案 B**：修改 `Process` 或 `SetOpaqueRenderState`，在 `MeshPassType == EMeshPass::MobileAfterTranslucencyPass` 时强制使用只读深度。侵入性更大，但能保留 decal stencil 标记。

### 4.5 AfterTranslucency Pass 缺少正确的 PSO Precache 深度访问

**问题**：`FMobileBasePassMeshProcessor::CollectPSOInitializers` 对 opaque 材质的 `ExclusiveDepthStencil` 计算为：

```cpp
FExclusiveDepthStencil ExclusiveDepthStencil = (bTranslucentBasePass || bMaskedInEarlyPass)
    ? FExclusiveDepthStencil::DepthRead_StencilRead
    : FExclusiveDepthStencil::DepthWrite_StencilWrite;
```

对于 `MobileAfterTranslucencyPass`，`bTranslucentBasePass == false`，所以 PSO 预缓存会生成 `DepthWrite_StencilWrite` 的 PSO。

**后果**：PSO 的 depth/stencil access 与运行时 render pass 的 `DepthRead_StencilRead` 不一致。在某些 RHI（如 Vulkan）可能触发 validation warning，或在 PSO 创建时无法命中正确的 pipeline layout。

**建议**：在 `CollectPSOInitializers` 中特殊处理 `MobileAfterTranslucencyPass`，使其 `ExclusiveDepthStencil` 与运行时的 `DepthRead_StencilRead` 一致：

```cpp
FExclusiveDepthStencil ExclusiveDepthStencil;
if (MeshPassType == EMeshPass::MobileAfterTranslucencyPass || bTranslucentBasePass || bMaskedInEarlyPass)
{
    ExclusiveDepthStencil = FExclusiveDepthStencil::DepthRead_StencilRead;
}
else
{
    ExclusiveDepthStencil = FExclusiveDepthStencil::DepthWrite_StencilWrite;
}
```

---

## 5. 潜在问题

### 5.1 `bRenderAfterTranslucency` 与 `bRenderInMainPass` 的隐式耦合

当前方案中，`bRenderAfterTranslucency` 只是分流标记，物体仍然必须满足 `ViewRelevance.bRenderInMainPass == true` 才会被加入 `MobileAfterTranslucencyPass`。

**潜在行为**：
- 如果用户只勾选 `bRenderAfterTranslucency = true`，保持 `bRenderInMainPass = true`：正常工作。
- 如果用户同时关闭 `bRenderInMainPass = false`：
  - `SceneVisibility.cpp` 中静态网格进入分支的条件 `(bRenderInMainPass || bRenderCustomDepth)` 不满足，不会加入 `MobileAfterTranslucencyPass`。
  - `ComputeDynamicMeshRelevance` 中动态网格同理。
  - 结果：物体完全不渲染。

**建议**：
- 要么在文档/属性提示中明确说明："必须保持 RenderInMainPass 为 true，否则物体消失"。
- 要么修改分流条件为 `(bRenderInMainPass || bRenderAfterTranslucency || bRenderCustomDepth)`，但这会扩大分支范围，需要谨慎。

### 5.2 `MobileBasePassCSM` 仍会为 AfterTranslucency 物体生成命令

在 `SceneVisibility.cpp` 的静态网格分流中：

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
    DrawCommandPacket.AddCommandsForMesh(..., EMeshPass::MobileBasePassCSM);
}
```

以及 `ComputeDynamicMeshRelevance` 中：

```cpp
if (ShadingPath == EShadingPath::Mobile && ViewRelevance.bRenderAfterTranslucency)
{
    PassMask.Set(EMeshPass::MobileAfterTranslucencyPass);
}
else
{
    PassMask.Set(EMeshPass::BasePass);
}
// ...
if (ShadingPath == EShadingPath::Mobile)
{
    PassMask.Set(EMeshPass::MobileBasePassCSM);
}
```

**影响**：`bRenderAfterTranslucency` 的物体仍然会渲染到 `MobileBasePassCSM` Pass。该 Pass 用于移动端 CSM 阴影接收（shadow setup 阶段使用），**不是颜色渲染**，因此不会破坏"透明之后渲染颜色"的需求。但如果用户希望这些物体完全不参与不透明阶段的任何绘制，需要注意这一点。

### 5.3 多 Pass 模式下 Uniform Buffer 类型不匹配

在 `RenderForwardMultiPass` 中：

```cpp
SecondPassParameters->MobileBasePass = CreateMobileBasePassUniformBuffer(
    GraphBuilder, View, EMobileBasePass::Translucent, SetupMode, ...);
```

`SecondPassParameters` 用于 decals/translucency，其 `MobileBasePass` uniform buffer 是 `Translucent` 类型。

当 `RenderMobileAfterTranslucencyPass` 在同一个 lambda 中调用时，当前绑定的是 `Translucent` 类型的 MobileBasePass UB。虽然 `FMobileBasePassMeshProcessor::Process` 在 `BuildMeshDrawCommands` 时已经把 uniform buffer 绑定到每个 mesh draw command 中（通过 `ShaderElementData.InitializeMeshMaterialData`），但实际的 uniform buffer 内容仍然是 build mesh command 时传入的值。

**潜在影响**：如果 `Translucent` 与 `Opaque` 类型的 MobileBasePass uniform buffer 布局或内容不同，AfterTranslucency 的 opaque 物体可能拿到错误的参数（例如没有正确的 scene texture 引用）。

**建议**：在 `RenderMobileAfterTranslucencyPass` 调用前，确保当前 binding 的是 `EMobileBasePass::Opaque` 类型的 uniform buffer。更安全的做法是：把 AfterTranslucency 绘制拆分为一个独立的 `FRDGPass` 并绑定正确的参数。但这会改变现有的单/多 Pass 架构，需要更多改动。

### 5.4 单 Pass 模式与 `bTonemapSubpassInline` 的时序

在 `RenderForwardSinglePass` 中：

```cpp
RenderTranslucency(RHICmdList, View);
RenderMobileAfterTranslucencyPass(RHICmdList, View, &AfterTranslucencyInstanceCullingDrawParams); // 用户插入位置
// ...
PreTonemapMSAA(RHICmdList, SceneTextures);
if (bTonemapSubpassInline)
{
    RHICmdList.NextSubpass();
    RenderMobileCustomResolve(RHICmdList, View, NumMSAASamples, SceneTextures);
}
```

当前插入位置在 `PreTonemapMSAA` / `NextSubpass` 之前，处于 `DepthReadSubpass`，可以正常写入 SceneColor。但需注意：
- AfterTranslucency 写入的颜色会经过后续的 CustomResolve / Tonemapping，符合预期。
- 如果后续把 `RenderMobileAfterTranslucencyPass` 移动到 `NextSubpass` 之后，颜色可能写入 resolve target 而不是 MSAA SceneColor，导致结果错误。

**建议**：保持当前位置，不要移到 `NextSubpass` 之后。

### 5.5 Masked 材质在 AfterTranslucency Pass 中的 Early-Z 行为

`Process` 函数中：

```cpp
else if((MeshBatch.bUseForDepthPass && Scene->EarlyZPassMode == DDM_AllOpaque) || bMaskedInEarlyPass)
{
    DrawRenderState.SetDepthStencilState(TStaticDepthStencilState<false, CF_Equal>::GetRHI());
}
```

如果 masked 物体在 EarlyZPass 中写入了深度，AfterTranslucency Pass 中会对这些像素使用 `CF_Equal` 测试，这是正确的。但如果 EarlyZPass 模式不是 `DDM_AllOpaque`，masked 物体使用 `CF_DepthNearOrEqual`，也能正确渲染。该逻辑无需修改。

### 5.6 统计与事件命名

方案中使用了：

```cpp
CSV_SCOPED_TIMING_STAT_EXCLUSIVE(RenderAfterTranslucency);
SCOPED_DRAW_EVENT(RHICmdList, MobileAfterTranslucencyPass);
SCOPE_CYCLE_COUNTER(STAT_AfterTranslucencyDrawTime);
SCOPED_GPU_STAT(RHICmdList, AfterTranslucency);
```

需要确认 `CSV_STAT_FNAME` 或 `CSV_DEFINE_STAT` 中是否已注册 `RenderAfterTranslucency`。如果没有，可能需要在 `RenderCore.cpp` 或相关 CSV 统计定义处补充。不过 UE 的 CSV 统计通常允许首次使用时自动创建，具体取决于项目配置。

---

## 6. 遗漏项

### 6.1 其他 `FPrimitiveSceneProxy` 子类未设置 `bRenderAfterTranslucency`

用户声明只支持 Static Mesh 和 Skeletal Mesh，但以下子类也实现了 `GetViewRelevance` 并设置 `bRenderInMainPass`：
- `FInstancedStaticMeshSceneProxy`（继承自 `FStaticMeshSceneProxy`，会继承修改）
- `FHLODMeshSceneProxy`
- `FLandscapeSceneProxy`
- `FSplineMeshSceneProxy`
- `FGroomSceneProxy`
- `FGeometryCollectionSceneProxy`
- 各类粒子/条带代理

**结论**：按当前方案，只有 `FStaticMeshSceneProxy` 和 `FSkeletalMeshSceneProxy` 及其直接子类会生效。其他类型的物体即使勾选了 `bRenderAfterTranslucency` 也不会被分流，仍会走 BasePass。

**建议**：在文档中明确说明支持范围；如需扩展，需在对应 `GetViewRelevance` 中补充 `Result.bRenderAfterTranslucency = ShouldRenderAfterTranslucency();`。

### 6.2 没有处理编辑器专用 Pass

`EMeshPass` 的编辑器部分（`HitProxy`、`HitProxyOpaqueOnly`、`EditorSelection` 等）仍然会把物体加入普通 BasePass。如果用户希望在编辑器选中/拾取时也能看到 AfterTranslucency 效果，需要额外处理。但对运行时游戏无影响。

### 6.3 没有处理 `DebugViewMode` / `LightmapDensity`

当开启 `UseDebugViewPS` 或 `bAddLightmapDensityCommands` 时，物体仍会被加入 `DebugViewMode` / `LightmapDensity` Pass。这些 Pass 与 BasePass 并行，不影响核心功能。

### 6.4 没有处理移动端 Deferred Shading

方案只在 `RenderForwardSinglePass` / `RenderForwardMultiPass` 中调用 `RenderMobileAfterTranslucencyPass`。如果项目启用移动端延迟渲染（`r.Mobile.ShadingPath = 1`），`RenderDeferred*` 路径不会调用，因此 AfterTranslucency 功能在延迟路径下不生效。

**注意**：`REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR` 注册的是 `EShadingPath::Mobile`，会同时注册到 Forward 和 Deferred 的 processor 表中，只是 Deferred 路径没有实际 DispatchDraw，因此无害。

### 6.5 没有处理 `r.GPUScene` 关闭的情况

`BuildInstanceCullingDrawParams` 只在 `Scene->GPUScene.IsEnabled()` 为 true 时构建 `AfterTranslucencyInstanceCullingDrawParams`。如果 GPUScene 关闭，`RenderMobileAfterTranslucencyPass` 传入的 `InstanceCullingDrawParams` 将为空指针，需要确认 `DispatchDraw` 在 nullptr 时的行为。

查看 `FParallelMeshDrawCommandPass::DispatchDraw` 的实现：当 GPUScene 关闭时，BasePass 也是传入 nullptr 或 fallback params。因此与现有行为一致，通常不会有问题，但需在真机上验证。

---

## 7. 建议的改进实现

### 7.1 推荐的最小修正清单

1. **BasePassRendering.cpp** 添加：
   ```cpp
   DEFINE_GPU_DRAWCALL_STAT(AfterTranslucency);
   ```

2. **MobileBasePass.cpp `CreateMobileAfterTranslucencyPassProcessor`**：
   - 保留 `SetBlendState(TStaticBlendStateWriteMask<CW_RGBA>::GetRHI());`
   - Flags 增加 `ForcePassDrawRenderState`：
     ```cpp
     const FMobileBasePassMeshProcessor::EFlags Flags =
         FMobileBasePassMeshProcessor::EFlags::CanUseDepthStencil |
         FMobileBasePassMeshProcessor::EFlags::ForcePassDrawRenderState;
     ```
   - 移除构造函数中的 `bAfterTranslucencyBasePass` 参数，改为在 `AddMeshBatch` 中比较 `MeshPassType`。

3. **MobileBasePass.cpp `CollectPSOInitializers`**：
   - 为 `MobileAfterTranslucencyPass` 使用 `DepthRead_StencilRead` 生成 PSO。

4. **验证 `bRenderAfterTranslucency` 与 `bRenderInMainPass` 的交互**：
   - 要么在 UI 上添加 `EditCondition` 提示，要么修改分流条件。

5. **多 Pass 模式下的 Uniform Buffer**：
   - 至少进行真机验证；如果发现问题，需要为 AfterTranslucency Pass 单独绑定 `EMobileBasePass::Opaque` uniform buffer。

### 7.2 关于 `bAfterTranslucencyBasePass` 的简化

按文档末尾的思路，建议移除该成员：

```cpp
// MobileBasePassRendering.h：删除 bAfterTranslucencyBasePass 字段和构造函数参数

// MobileBasePass.cpp：
void FMobileBasePassMeshProcessor::AddMeshBatch(...)
{
    if (!MeshBatch.bUseForMaterial || ...)
    {
        return;
    }

    const bool bIsAfterTranslucencyPass = (MeshPassType == EMeshPass::MobileAfterTranslucencyPass);
    const bool bShouldRenderAfterTranslucency = PrimitiveSceneProxy && PrimitiveSceneProxy->ShouldRenderAfterTranslucency();
    if (bIsAfterTranslucencyPass != bShouldRenderAfterTranslucency)
    {
        return;
    }

    // ... 原有逻辑
}
```

### 7.3 可选：让 AfterTranslucency 物体跳过 `MobileBasePassCSM`

如果希望这些物体完全不参与不透明阶段的颜色/CSM 绘制，可以修改 `SceneVisibility.cpp`：

```cpp
if (!bMobileBasePassAlwaysUsesCSM && !ViewRelevance.bRenderAfterTranslucency)
{
    DrawCommandPacket.AddCommandsForMesh(..., EMeshPass::MobileBasePassCSM);
}
```

动态网格同理。但这会影响 CSM 阴影接收，通常不建议，除非明确需求。

---

## 8. 综合评估

| 维度 | 评估 |
|------|------|
| 功能正确性 | **基本可行**，但需修正 BlendState、GPU stat 定义、深度写入覆盖问题。 |
| 代码侵入性 | 中等，主要集中在 `MobileBasePass.cpp`、`SceneVisibility.cpp`、引擎组件层。 |
| 性能影响 | 新增一个 Pass 会增加 draw call batch 和 PSO 缓存；AfterTranslucency 物体不参与 EarlyZ/Prepass 后的 opaque pass，但能遮挡透明层，符合需求。 |
| 可维护性 | 建议移除 `bAfterTranslucencyBasePass` 冗余成员，用 `MeshPassType` 判断。 |
| 移动端兼容性 | 单 Pass 和多 Pass 路径都需真机验证；特别注意 Mali/Adreno 对 read-only depth 与 stencil 写入组合的容忍度。 |
| 遗漏范围 | 仅支持 Static/Skeletal Mesh；其他 primitive 类型、移动端 Deferred、编辑器专用 Pass 未覆盖。 |

---

## 9. 实施前检查清单

- [ ] 确认 `MobileAfterTranslucencyPass` 的 `static_assert` 数值正确（33 / 33+4）。
- [ ] 确认 `DEFINE_GPU_DRAWCALL_STAT(AfterTranslucency)` 已添加。
- [ ] 确认 `CreateMobileAfterTranslucencyPassProcessor` 保留 BlendState 并添加 `ForcePassDrawRenderState`。
- [ ] 确认 `AddMeshBatch` 中的分流逻辑不依赖冗余的 `bAfterTranslucencyBasePass`。
- [ ] 确认 `CollectPSOInitializers` 中 AfterTranslucency Pass 的 `ExclusiveDepthStencil` 为 `DepthRead_StencilRead`。
- [ ] 确认 `RenderForwardSinglePass` / `RenderForwardMultiPass` 中调用位置在 `RenderTranslucency` 之后、`PreTonemapMSAA`/`NextSubpass` 之前。
- [ ] 确认 `BuildInstanceCullingDrawParams` 中已为 `MobileAfterTranslucencyPass` 构建命令。
- [ ] 在 Mali/Adreno 真机上验证：透明物体被正确遮挡、无 validation error、无 stencil/depth 写入异常。
- [ ] 验证 `bRenderAfterTranslucency` 在关闭 `bRenderInMainPass` 时的行为（建议至少给出明确文档说明）。
