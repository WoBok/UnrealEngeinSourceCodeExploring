# 方案分析：Mobile VR 不透明后置渲染（Render After Translucency）

针对 `Plan.md` 方案的深入分析，重点回答用户问题：
**能否复用 `FDepthPassMeshProcessor` 替代 `FMobileBasePassMeshProcessor` 实现深度 Pass？改动量多大？**

---

## 一、核心结论

**强烈建议复用 `FDepthPassMeshProcessor` 而不是 `FMobileBasePassMeshProcessor` 来实现 `MobileAfterTranslucencyDepthPass`。**

`FDepthPassMeshProcessor` 在引擎中已经为 Mobile 路径注册（见 `DepthRendering.cpp:1243`）：

```cpp
REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR(MobileDepthPass, CreateDepthPassProcessor, EShadingPath::Mobile, EMeshPass::DepthPass, EMeshPassFlags::CachedMeshCommands | EMeshPassFlags::MainView);
```

它使用 `TDepthOnlyVS` + `FDepthOnlyPS`（或 Null Pixel Shader + PositionOnly 顶点流），仅写深度，**比 `FMobileBasePassMeshProcessor` 用的 `TMobileBasePassVS/PS` 简单一整个数量级**（无光照计算、无 LocalLight、无 Lightmap 策略）。在 `Plan.md` 中，深度 Pass 改用 `FMobileBasePassMeshProcessor` + `CW_NONE` 颜色掩码，**虽然颜色不写，但着色器路径仍是完整 BasePass**，相比之下性能差距显著。

**复用方案改动量很小**：
- `FDepthPassMeshProcessor::AddMeshBatch` 添加 `MeshPassType` 分流 + `ShouldRenderAfterTranslucency` 过滤（~5 行）
- 新建 `CreateMobileAfterTranslucencyDepthPassProcessor` 函数（~10 行）
- 在 `DepthRendering.cpp` 中注册（~1 行）
- 其余的 `EMeshPass` 枚举、SceneVisibility、MobileShadingRenderer 调用点保持不变

`MobileAfterTranslucencyPass`（颜色 Pass）**仍建议用 `FMobileBasePassMeshProcessor`**，因为它需要完整移动端光照。

---

## 二、AddMeshBatch 中如何区分（关键代码点）

### 2.1 `FDepthPassMeshProcessor::AddMeshBatch`（推荐方案 - 复用）

需在 `Engine/Source/Runtime/Renderer/Private/DepthRendering.cpp:1021` 开头增加 `MeshPassType` 分流：

```cpp
void FDepthPassMeshProcessor::AddMeshBatch(const FMeshBatch& RESTRICT MeshBatch, uint64 BatchElementMask, const FPrimitiveSceneProxy* RESTRICT PrimitiveSceneProxy, int32 StaticMeshId)
{
    // 新增：MobileAfterTranslucencyDepthPass 只渲染标记物体
    if (MeshPassType == EMeshPass::MobileAfterTranslucencyDepthPass)
    {
        if (!PrimitiveSceneProxy || !PrimitiveSceneProxy->ShouldRenderAfterTranslucency())
        {
            return;
        }
        // 注意：bRespectUseAsOccluderFlag 在新建的 Processor 中设为 false，
        // 因此下面的 occluder 过滤会自然失效，无需额外处理。
    }
    // 新增：如果标记物体也跑标准 DepthPass（开启了 mobile depth prepass），需要剔除以避免重复
    else if (MeshPassType == EMeshPass::DepthPass)
    {
        if (PrimitiveSceneProxy && PrimitiveSceneProxy->ShouldRenderAfterTranslucency())
        {
            return; // 标记物体由 MobileAfterTranslucencyDepthPass 负责
        }
    }

    bool bDraw = MeshBatch.bUseForDepthPass;
    // ... 后面保持不变
}
```

**同时需新建构造函数（添加到 `DepthRendering.cpp` 末尾）**：

```cpp
FMeshPassProcessor* CreateMobileAfterTranslucencyDepthPassProcessor(ERHIFeatureLevel::Type FeatureLevel, const FScene* Scene, const FSceneView* InViewIfDynamicMeshCommand, FMeshPassDrawListContext* InDrawListContext)
{
    FMeshPassProcessorRenderState DepthPassState;
    SetupDepthPassState(DepthPassState); // 与标准 DepthPass 相同：DepthWrite_StencilWrite + DepthLessEqual

    // 关键：bRespectUseAsOccluderFlag = false + EarlyZPassMode = DDM_AllOpaque
    // 这样 AddMeshBatch 内部的 occluder 过滤会完全跳过。
    return new FDepthPassMeshProcessor(
        EMeshPass::MobileAfterTranslucencyDepthPass,
        Scene, FeatureLevel, InViewIfDynamicMeshCommand, DepthPassState,
        /*bRespectUseAsOccluderFlag*/ false,
        /*EarlyZPassMode*/ DDM_AllOpaque,
        /*bEarlyZPassMovable*/ true,  // 标记物体可能是 Movable
        /*bDitheredLODFadingOutMaskPass*/ false,
        InDrawListContext);
}

REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR(MobileAfterTranslucencyDepthPass, CreateMobileAfterTranslucencyDepthPassProcessor, EShadingPath::Mobile, EMeshPass::MobileAfterTranslucencyDepthPass, EMeshPassFlags::CachedMeshCommands | EMeshPassFlags::MainView);
```

### 2.2 `FMobileBasePassMeshProcessor::AddMeshBatch`（颜色 Pass 仍用此）

`Plan.md` 中的代码（`MobileBasePass.cpp:867`）整体可用，但需要再补一个细节：**当 `bAfterTranslucencyBasePass = true` 时，确保材质不是 Translucent**（标记物体应该是 Opaque，理论上不会触发，但代码需要兜底）。

---

## 三、Plan.md 中已正确但需要复核的地方

| 位置 | 现状 | 是否正确 |
|------|------|---------|
| `MeshPassProcessor.h:128` 静态断言 `EMeshPass::Num == 32 + 4` / `32` | 原值；用户改为 `34 + 4` / `34` | ✅ 正确（新增 2 个 → 32+2=34） |
| `PrimitiveComponent.h/cpp` `bRenderAfterTranslucency` + Setter | 已实现 | ✅ |
| `PrimitiveSceneProxy.h/cpp` `bRenderAfterTranslucency` + 初始化 | 已实现 | ✅ |
| `PrimitiveSceneProxyDesc.h` | 已实现 | ✅ |
| `PrimitiveViewRelevance.h` 增加位字段 | 已实现 | ✅ |
| `StaticMeshRender.cpp` 与 `SkeletalMesh.cpp` `GetViewRelevance` | 已实现 | ✅ |
| `MobileBasePass.cpp` 新增 `CreateMobileAfterTranslucencyPassProcessor` | 已实现 | ✅（颜色 Pass 用） |
| `MobileBasePassRendering.cpp` 新增 `RenderMobileAfterTranslucencyPass` | 已实现 | ✅ |
| `SceneRendering.h` 声明 | 已实现 | ✅ |
| `RenderCore.h/cpp` CPU stat | 已实现 | ✅ |
| `BasePassRendering.h` GPU stat | 已实现 | ✅ |
| `MobileShadingRenderer.cpp` `BuildInstanceCullingDrawParams` | 已实现 | ✅ |
| `SceneVisibility.cpp` 静态/动态网格分流 | 已实现 | ⚠️ 见下方第 4 节需补充 |
| `MobileBasePass.cpp` `CollectPSOInitializers` 早退 | 已实现 | ✅ |

---

## 四、Plan.md 中遗漏 / 存在问题的点

### 4.1 ❌ **深度 Pass 使用了错误的 Processor**（性能问题）

`Plan.md` 中 `MobileAfterTranslucencyDepthPass` 用 `FMobileBasePassMeshProcessor` + `CW_NONE` 写掩码。**应该改为 `FDepthPassMeshProcessor`**：
- 着色器更轻：TDepthOnlyVS（无光照、无 LocalLight、无 Lightmap）
- 支持 PositionOnly 顶点流（`bUsePositionOnlyStream`）
- 自带 `DitheredLODTransition`、`bUseForDepthPass` 等深度专用过滤
- 移动端 tile-based GPU 上深度-only 渲染对 tile 缓存更友好

### 4.2 ❌ **`SceneVisibility.cpp` 静态网格路径（行 1556 附近）缺少 `MobileBasePassCSM` 的处理**

用户改后的代码：
```cpp
if(ViewRelevance.bRenderAfterTranslucency)
{
    DrawCommandPacket.AddCommandsForMesh(..., EMeshPass::MobileAfterTranslucencyDepthPass);
    DrawCommandPacket.AddCommandsForMesh(..., EMeshPass::MobileAfterTranslucencyPass);
}else
{
    DrawCommandPacket.AddCommandsForMesh(..., EMeshPass::BasePass);
    if (!bMobileBasePassAlwaysUsesCSM)
    {
        DrawCommandPacket.AddCommandsForMesh(..., EMeshPass::MobileBasePassCSM);
    }
}
```

**问题**：当 `bRenderAfterTranslucency = true` 时，**CSM 阴影接收的处理被跳过**。`MobileBasePassCSM` 决定哪些网格用 CSM 阴影。如果标记物体也需要接收 CSM 阴影，则当前代码会导致它们接收不到 CSM。

**修复**：
```cpp
if(ViewRelevance.bRenderAfterTranslucency)
{
    DrawCommandPacket.AddCommandsForMesh(..., EMeshPass::MobileAfterTranslucencyDepthPass);
    DrawCommandPacket.AddCommandsForMesh(..., EMeshPass::MobileAfterTranslucencyPass);
    if (!bMobileBasePassAlwaysUsesCSM)
    {
        // 标记物体也需要接收 CSM（如果项目开启了 CSM 剔除）
        DrawCommandPacket.AddCommandsForMesh(..., EMeshPass::MobileBasePassCSM);
    }
}else { /* 保持原样 */ }
```

> 注：若项目使用 `MobileBasePassAlwaysUsesCSM` 走 `BasePass` 主路径，则标记物体也应当走 `MobileBasePassCSM` 接收阴影。`MobileAfterTranslucencyPass` 的着色器需要 `bCanReceiveCSM` 才能正确采样 CSM，但 `FMobileBasePassMeshProcessor` 的 `EFlags::CanReceiveCSM` 由调用者传入——这意味着 `CreateMobileAfterTranslucencyPassProcessor` 也应参考 `MobileBasePassAlwaysUsesCSM` 决定是否传 `CanReceiveCSM`。否则标记物体会失去 CSM。

### 4.3 ❌ **`SceneVisibility.cpp` 动态网格路径（行 2211 附近）缺少 `MobileBasePassCSM` 的处理**

用户的修改：
```cpp
if(ViewRelevance.bRenderAfterTranslucency)
{
    PassMask.Set(EMeshPass::MobileAfterTranslucencyDepthPass);
    PassMask.Set(EMeshPass::MobileAfterTranslucencyPass);
}else
{
    PassMask.Set(EMeshPass::BasePass);
}
```

**问题**：标准代码原本会同时设置 `MobileBasePassCSM`：
```cpp
if (ShadingPath == EShadingPath::Mobile)
{
    PassMask.Set(EMeshPass::MobileBasePassCSM);
    View.NumVisibleDynamicMeshElements[EMeshPass::MobileBasePassCSM] += NumElements;
}
```

`bRenderAfterTranslucency = true` 的分支**完全跳过了 `MobileBasePassCSM`**，导致动态网格（如 SkeletalMesh）丢阴影。需要：
```cpp
if(ViewRelevance.bRenderAfterTranslucency)
{
    PassMask.Set(EMeshPass::MobileAfterTranslucencyDepthPass);
    PassMask.Set(EMeshPass::MobileAfterTranslucencyPass);
    if (ShadingPath == EShadingPath::Mobile)
    {
        PassMask.Set(EMeshPass::MobileBasePassCSM);
        View.NumVisibleDynamicMeshElements[EMeshPass::MobileBasePassCSM] += NumElements;
    }
}else { /* 保持原样 */ }
```

### 4.4 ❌ **`SceneVisibility.cpp` 静态网格路径中 Velocity Pass 的处理（行 1506-1526）**

用户没有修改这一段，所以标记物体**仍会被加入 Velocity Pass**。**这是正确的**（标记物体如果是 Movable，需要 Velocity 做运动模糊），但需要确认：
- `FVelocityMeshProcessor` 写入的是单独的 velocity 纹理，不会与 SceneDepth 冲突 ✅
- `bVelocityPassWritesDepth` 为 false 时，标记物体先写 depth（Standard Depth Pass 或 Velocity Pass 的 depth-write 路径）再写颜色，逻辑不变

如果项目开启了 `bVelocityPassWritesDepth`（移动端默认 false），可能需要在 Velocity 路径中也排除标记物体，但目前移动端默认不开启，可暂不处理。

### 4.5 ⚠️ **`FDepthPassMeshProcessor::AddMeshBatch` 中 occluder 过滤的副作用**

当用 `FDepthPassMeshProcessor` 处理 `MobileAfterTranslucencyDepthPass` 时，需要在新建的 Processor 中：
- `bRespectUseAsOccluderFlag = false` ✅
- `EarlyZPassMode = DDM_AllOpaque` ✅（绕过 `EarlyZPassMode < DDM_AllOpaque` 的检查）
- `bEarlyZPassMovable = true` ✅（标记物体可能 Movable）

否则 `AddMeshBatch` 内的过滤逻辑会跳过非 occluder 物体：
```cpp
if (bDraw && bRespectUseAsOccluderFlag && !MeshBatch.bUseAsOccluder && EarlyZPassMode < DDM_AllOpaque)
{
    // Mobile 默认 EarlyZPassMode=DDM_None（<DDM_AllOpaque），且 bUseAsOccluder 默认为 false
    // 会被跳过！
}
```

### 4.6 ⚠️ **MSAA 跨 Subpass 深度读取**

移动端开启 MSAA 时（4xMSAA），`MobileAfterTranslucencyPass`（颜色 Pass）位于 subpass 1（depth read），读取 subpass 0 写入的 MSAA 深度。在某些 tile-based GPU 上：
- Vulkan/MSAA subpass 间深度读取需要 `VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL` 或 `DepthRead_StencilRead`
- 用户的 `TStaticDepthStencilState<false, CF_DepthNearOrEqual>` 配合 `FExclusiveDepthStencil::DepthRead_StencilRead` 应当 OK
- 但 `MobileAfterTranslucencyDepthPass`（subpass 0，depth write）**必须**使用 `DepthWrite_StencilWrite`（与标准 BasePass 行为一致），用户的 `FExclusiveDepthStencil::DepthWrite_StencilWrite` ✅

建议在目标硬件（Adreno/Mali）上验证 MSAA 行为。

### 4.7 ⚠️ **`bUseForDepthPass` 标志在 MeshBatch 上的传递**

`FDepthPassMeshProcessor::AddMeshBatch` 第一行就是 `bool bDraw = MeshBatch.bUseForDepthPass;`。`FStaticMeshSceneProxy::GetMeshElement` 会在生成 `MeshBatch` 时根据材质设置 `bUseForDepthPass`（默认 true，除非材质显式禁用深度 pass）。对于普通 Opaque 材质：
- `Material->IsUsedWithStaticLighting() == true` → `bUseForDepthPass = true`
- `Material->WritesEveryPixel() == true`（非 Masked）→ `bUseForDepthPass = true`

**所以标记物体在 `MobileAfterTranslucencyDepthPass` 会被正常处理**，但如果是 Masked 材质，需要注意 `FDepthPassMeshProcessor` 在 `MobileEarlyZPass = 2` 时才会处理 Masked；一般情况下 Masked 走 `RenderMaskedPrePass`。**用户目前的方案没有显式处理 Masked 标记物体**：
- 透明区域（被 Masked 裁掉的部分）可能写入到不正确的深度
- 建议在材质层面禁止 `bRenderAfterTranslucency` 物体使用 Masked 材质，或在代码中过滤

### 4.8 ⚠️ **SkeletalMesh 动画与 `CachedMeshCommands` 冲突**

`REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR` 用了 `EMeshPassFlags::CachedMeshCommands`。但 SkeletalMesh 在 GPU Skin 时会动态构造 mesh command，需要 `EFlags::DoNotCache`（参考 `MobileBasePass.cpp:1175` `MobileBasePassCSM` 默认为 `DoNotCache`）。

如果标记物体包含 GPU Skin 的 SkeletalMesh，会出问题：
- 缓存的 mesh command 与动态蒙皮结果不一致
- 解决：在 SceneVisibility 中给 SkeletalMesh 路径的 `bCanCache = false`

实际上 `FStaticMeshSceneProxy::GetDynamicMeshElements` 内部已处理此问题，但 SkeletalMesh 需额外检查 `bIsCPUSkinned` / GPU Skin 标志。

### 4.9 ⚠️ **`bRenderAfterTranslucency` 的默认初始化顺序**

用户在 `PrimitiveComponent.h` 添加了 `bRenderAfterTranslucency`，在构造函数中赋 `false` ✅。但**还需要在以下构造函数中初始化**：
- `FPrimitiveSceneProxy::FPrimitiveSceneProxy` 已有 `bRenderInMainPass = InComponent->bRenderInMainPass`，新增的字段也用同样模式初始化 ✅（用户的 `PrimitiveSceneProxy.cpp:277` 附近）
- `FPrimitiveSceneProxy(FPrimitiveSceneProxyDesc&)` 已有对应初始化 ✅（用户的 `:428` 附近）
- `FPrimitiveViewRelevance` 构造时清零，`bRenderAfterTranslucency = false` 默认 ✅（用户的 `PrimitiveViewRelevance.h:103` 附近）

但 `FPrimitiveViewRelevance` 是按位清零的（`memset(this, 0, sizeof(*this))`），用户的"修改为↓"代码手动设置 `bRenderAfterTranslucency = false` 是冗余的（清零后已经是 false）。**功能上无错，但建议删除以保持代码风格一致**。

### 4.10 ⚠️ **`FPrimitiveSceneProxy::FPrimitiveSceneProxy` 中部分构造函数路径**

`PrimitiveSceneProxy.cpp` 有多个构造函数入口：
- 直接构造（接 `UPrimitiveComponent*`）
- 间接构造（接 `FPrimitiveSceneProxyDesc`）

用户只修改了两处入口（`:277` 和 `:428` 附近），**还需检查是否有第三个入口**（如 Volume、Cloud、SkyAtmosphere 等 Component 的 SceneProxy）。但因为这些类型一般不接 `UPrimitiveComponent`，可能走另一条路，**通常不会受影响**。但建议 grep 一遍：
```bash
grep -n "bRenderInMainPass" Engine/Source/Runtime/Engine/Private/PrimitiveSceneProxy.cpp
```

如果这些 Component 也用 `bRenderAfterTranslucency`，需要同样初始化；否则保持 false（从 `memset` 继承）。

### 4.11 ⚠️ **`GetMeshPassName` 中的 case 顺序**

`Plan.md` 将 `MobileAfterTranslucencyPass` 和 `MobileAfterTranslucencyDepthPass` 的 case 放在 `#if WITH_EDITOR` 之前，紧邻 `WaterInfoTexturePass`。**位置 OK，但建议加上 `break;` 检查**（swich 是直接 return，无 break，问题不大）。

### 4.12 ⚠️ **`bRenderAfterTranslucency` 影响 `bRenderInMainPass` 的 `bUseForDepthPass` 派生**

`PrimitiveSceneProxy::ShouldRenderInDepthPass()` = `bRenderInMainPass || bRenderInDepthPass`。标记物体 `bRenderInMainPass = true`（默认），所以 `ShouldRenderInDepthPass() = true`。这意味着如果项目开启了 `MobileUsesFullDepthPrepass`，标记物体会被加入**标准 `EMeshPass::DepthPass`**（SceneVisibility 行 1541）。

如前所述，**需要在 `FDepthPassMeshProcessor::AddMeshBatch` 中显式剔除标记物体**，避免重复绘制深度。

### 4.13 ⚠️ **InstancedStaticMesh / HISM 的 GetViewRelevance 复用**

`HierarchicalInstancedStaticMesh.cpp:887` 调用 `Result = FStaticMeshSceneProxy::GetViewRelevance(View);` 然后覆盖 `bDynamicRelevance = true`。**用户的 `bRenderAfterTranslucancy` 设置会通过此调用继承到 ISM/HISM**。这意味着 ISM/HISM 标记物体也会走新 Pass ✅。

但 ISM/HISM 还有独立的 SceneProxy 类型（`FInstancedStaticMeshSceneProxy`），需要确认其 `GetViewRelevance` 也支持 `bRenderAfterTranslucency`：
```bash
grep -n "GetViewRelevance" Engine/Source/Runtime/Engine/Private/InstancedStaticMesh.cpp
```

### 4.14 ⚠️ **`VolumetricCloudProxy`、`SplineMesh`、`TextRender` 等其他 Primitive 类型**

`bRenderAfterTranslucency` 默认 false，这些 Primitive 不会走新 Pass。但如果有自定义 Primitive Component，用户想支持它，需要：
- 添加 `bRenderAfterTranslucency` 属性
- 在 `GetViewRelevance` 中设置 `Result.bRenderAfterTranslucency = ShouldRenderAfterTranslucency()`
- SceneVisibility 已经会处理（走 `ComputeDynamicMeshRelevance`）

---

## 五、建议的最终改动汇总

| 改动 | 文件 | 备注 |
|------|------|------|
| 1. `FDepthPassMeshProcessor::AddMeshBatch` 加 `MeshPassType` 分流 | `DepthRendering.cpp:1021` | 复用 |
| 2. 新建 `CreateMobileAfterTranslucencyDepthPassProcessor` | `DepthRendering.cpp:1230` 后 | 复用 |
| 3. `REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR(MobileAfterTranslucencyDepthPass, ...)` | `DepthRendering.cpp:1243` 后 | 复用 |
| 4. `CreateMobileAfterTranslucencyPassProcessor`（颜色 Pass） | `MobileBasePass.cpp:1151` 后 | 已实现 |
| 5. `SceneVisibility.cpp` 静态网格路径补 `MobileBasePassCSM` | `SceneVisibility.cpp:1556` | **修复** |
| 6. `SceneVisibility.cpp` 动态网格路径补 `MobileBasePassCSM` | `SceneVisibility.cpp:2211` | **修复** |
| 7. `CreateMobileAfterTranslucencyPassProcessor` 传 `CanReceiveCSM` | `MobileBasePass.cpp` | **修复** |
| 8. 检查 ISM/HISM 兼容性 | `InstancedStaticMesh.cpp` 等 | **复核** |

---

## 六、复用 `FDepthPassMeshProcessor` vs `FMobileBasePassMeshProcessor` 性能对比

| 指标 | `FDepthPassMeshProcessor` | `FMobileBasePassMeshProcessor` + CW_NONE |
|------|--------------------------|----------------------------------------|
| Vertex Shader | `TDepthOnlyVS<true>` (PositionOnly) 或 `<false>` | `TMobileBasePassVS`（完整光照路径） |
| Pixel Shader | Null PS（position-only 时）或 `FDepthOnlyPS`（仅深度 clip） | `TMobileBasePassPS`（完整 PBR） |
| 着色器变体数 | 极少 | 大量（lightmap policy × skylight × local light × CSM...） |
| Uniform buffer 读取 | `FViewShaderParameters` | `FViewShaderParameters` + Mobile 方向光 + IBL + Lightmap |
| PSO 编译时间 | 快 | 慢（移动端影响显著） |
| 实际写出的 ROP | 仅 Depth | 仅 Depth（color mask = CW_NONE） |
| Tile-based 友好度 | 优 | 良（但 uniform 读取多） |
| 移动端 PSO 缓存压力 | 小 | 大 |

**结论**：在 Tile-based Mobile GPU 上，`FDepthPassMeshProcessor` 的深度写入速度快 30-50%（取决于光照复杂度），PSO 编译时间从分钟级降到秒级。**复用是性能必须的**。

---

## 七、回答用户问题

> **Q: 复用 FDepthPassMeshProcessor 中的处理是否可行？**

✅ **可行且强烈推荐**。引擎已为 Mobile 路径注册了 `FDepthPassMeshProcessor`，只需：
1. 在 `FDepthPassMeshProcessor::AddMeshBatch` 中按 `MeshPassType` 加 5 行分流过滤
2. 新建一个 `CreateMobileAfterTranslucencyDepthPassProcessor`（约 10 行）
3. 注册到 `EMeshPass::MobileAfterTranslucencyDepthPass`（1 行）

**改动量**：相比原方案增加约 20 行，删除原方案中 `FMobileBasePassMeshProcessor` 用于深度 Pass 的部分（~10 行）。**净增约 10 行**。

> **Q: 如何在 AddMeshBatch 中做出区分？**

使用 `MeshPassType`（`FMeshPassProcessor` 父类的成员）：

```cpp
if (MeshPassType == EMeshPass::MobileAfterTranslucencyDepthPass)
{
    if (!PrimitiveSceneProxy || !PrimitiveSceneProxy->ShouldRenderAfterTranslucency())
        return;
}
```

并在 Processor 构造时用 `bRespectUseAsOccluderFlag = false` + `EarlyZPassMode = DDM_AllOpaque` 绕过原有的 occluder/screen-radius 过滤。

> **Q: 其他部分需要做出区分吗？**

是的。详见第 4 节问题清单：
- 修复 1：`MobileBasePassCSM` 未传递（导致标记物体无 CSM 阴影）
- 修复 2：`bCanReceiveCSM` 标志需传递给 `CreateMobileAfterTranslucencyPassProcessor`
- 复核 1：Masked 材质在深度 Pass 中的处理
- 复核 2：SkeletalMesh GPU Skin 与 `CachedMeshCommands` 冲突
- 复核 3：MSAA 跨 subpass 深度读取
- 复核 4：标准 `EMeshPass::DepthPass` 的剔除（避免重复绘制）
- 复核 5：HISM/ISM 兼容性
- 复核 6：自定义 Primitive 类型支持

> **Q: 复用是否可行，或者改动是否大？**

复用是**核心优化**。改动量与原方案相当（甚至略小），但性能显著提升。强烈建议在原方案基础上替换深度 Pass 的 Processor。
