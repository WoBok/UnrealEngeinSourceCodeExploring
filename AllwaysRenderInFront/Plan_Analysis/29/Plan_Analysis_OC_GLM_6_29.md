# Plan.md 问题与缺口分析

## 1. MeshPass 数量断言写错

`MeshPassProcessor.h` 当前无 Editor 的 `EMeshPass::Num` 是 32，带 Editor 是 `32 + 4`。方案新增 2 个 Pass 后，应改为：

- `#if WITH_EDITOR`：`EMeshPass::Num == 34 + 4`
- `#else`：`EMeshPass::Num == 34`

方案文字中“并更新底部断言 `EMeshPass::Num == 34 + 4` 与 `EMeshPass::Num == 34`”是正确方向，但示例代码里若从当前源码直接套用，需要确认不是按旧文档里错误基数重复加。

## 2. `FMobileBasePassMeshProcessor::AddMeshBatch` 有空指针风险

方案新增代码：

```cpp
const bool bShouldRenderAfterTranslucency = PrimitiveSceneProxy->ShouldRenderAfterTranslucency();
```

但原函数允许 `PrimitiveSceneProxy == nullptr`，例如部分动态绘制路径会传空 Proxy。这里会直接崩溃。

应改为带空指针判断，例如：

```cpp
const bool bShouldRenderAfterTranslucency = PrimitiveSceneProxy && PrimitiveSceneProxy->ShouldRenderAfterTranslucency();
```

或在 after translucency pass 中显式拒绝空 Proxy。

## 3. `MobileAfterTranslucencyDepthPass` 不应直接复用 BasePass 像素着色流程

方案用 `FMobileBasePassMeshProcessor` 创建深度写入 Pass，并只设置 `CW_NONE`。这确实不会写颜色，也不会影响后续 Pass 的颜色写入状态，因为每个 MeshDrawCommand/PSO 有自己的 BlendState。

但它仍会走 Mobile BasePass 的 VS/PS、LightMapPolicy、材质 base pass shader 选择、局部光/天光逻辑，而不是纯 DepthOnly shader。潜在问题：

- 性能明显高于真正 depth-only pass，尤其 Android VR 不划算。
- 对完全不需要像素阶段的非 Masked opaque 也会跑 BasePass 像素 shader。
- 与真正 depth pass 的 position-only/default-material 优化不一致。
- 若材质依赖 BasePass 输出或特殊移动端 base pass permutation，容易引入无意义 PSO 与 shader 变体。

更稳妥做法是新增/复用 `FDepthPassMeshProcessor` 风格的专用深度 Pass，或至少在 depth pass processor 中强制 `ForcePassDrawRenderState` 并确认不会被 `SetOpaqueRenderState` 改写深度/模板状态。

## 4. `MobileAfterTranslucencyDepthPass` 的深度/模板状态会被 `SetOpaqueRenderState` 覆盖

方案中 Depth Pass Processor 设置了：

```cpp
PassDrawRenderState.SetDepthStencilAccess(FExclusiveDepthStencil::DepthWrite_StencilWrite);
PassDrawRenderState.SetDepthStencilState(TStaticDepthStencilState<true, CF_DepthNearOrEqual>::GetRHI());
```

但 `FMobileBasePassMeshProcessor::Process` 默认会进入：

```cpp
MobileBasePass::SetOpaqueRenderState(...)
```

在 Mobile HDR 或 Mobile Deferred 等情况下，`SetOpaqueRenderState` 会改写 depth/stencil state，并写 receive decal / shading model / lighting channel stencil。这对“只写深度，不写颜色”的 pass 不是纯深度行为，可能污染模板，也可能与后续 translucency/lighting 逻辑产生冲突。

如继续复用 `FMobileBasePassMeshProcessor`，Depth Pass 至少应加 `EFlags::ForcePassDrawRenderState`，并慎重考虑 `DepthWrite_StencilWrite` 是否应改成只写深度、保留 stencil：例如 `DepthWrite_StencilNop` 或相应只读/不写模板访问。

## 5. 在 BasePass 后立刻写 AfterTranslucencyDepth 会导致被透明物体深度测试挡掉

核心需求是“透明物体之后再绘制标记物体，以遮挡透明物体”。方案把深度 Pass 放在 BasePass 后、Translucency 前：

```cpp
RenderMobileBasePass(...);
RenderMobileAfterTranslucencyDepthPass(...);
...
RenderTranslucency(...);
RenderMobileAfterTranslucencyPass(...);
```

这会让透明物体渲染时看到标记物体的深度。移动端透明 Pass 使用 `CF_DepthNearOrEqual` 深度测试且不写深度，因此被标记物体遮挡的透明像素会在透明 Pass 阶段直接被剔除。

如果目标是“标记物体最终盖住透明物体”，该结果是合理的；但如果还希望透明物体先完整完成颜色混合，再由标记物体覆盖，那么深度预写不能放在透明前。需要明确取舍。

同时，这个深度预写会影响 decals、fog、modulated shadow、部分 scene depth fetch 逻辑，因为它们在同一段 translucency 阶段前后使用 scene depth。

## 6. SinglePass 里写深度的位置破坏 `DepthReadSubpass` 假设

`RenderForwardSinglePass` 在 BasePass 后调用：

```cpp
RHICmdList.NextSubpass();
```

注释说明之后 scene depth 是 read-only/fetchable。方案把 `MobileAfterTranslucencyDepthPass` 放在 BasePass 后、`NextSubpass()` 前，这在单 Pass 路径中还处于深度可写阶段，技术上可能可行。

但如果后续把该深度写移动到 `NextSubpass()` 后，就会违反当前 render pass 的 `DepthReadSubpass` 深度只读假设，必须拆 RDG pass 或调整 RenderTargets.DepthStencilAccess，不能直接 DispatchDraw。

## 7. MultiPass 路径完全漏改

方案只在 `RenderForwardSinglePass` 中添加 after translucency pass。当前移动端还存在 `RenderForwardMultiPass`，透明在第二个 RDG pass `DecalsAndTranslucency` 中绘制。

如果项目触发 `bRequiresMultiPass`，新功能会失效：

- BasePass 后没有调用 `RenderMobileAfterTranslucencyDepthPass`。
- 第二个透明 pass 后没有调用 `RenderMobileAfterTranslucencyPass`。
- 第二个 pass 的 `DepthStencilAccess` 当前多为 `DepthRead_StencilRead` 或 `DepthRead_StencilWrite`，不允许写 after depth。

需要在 MultiPass 中单独设计：要么在第一个 BasePass RDG pass 末尾写 after-depth，要么新增第三个 RDG raster pass 在 translucency 后绘制 after-color，并正确设置 color/depth load/store 与 MSAA resolve。

## 8. Mobile Deferred 路径未处理

需求说只要 Mobile Forward，但源码中 `MobileShadingRenderer.cpp` 还包含 mobile deferred render path。若项目设置可能切到 mobile deferred，方案不会完整生效，且新增 MeshPass/可见性逻辑仍会生成命令。

建议加条件限制：只在 `!IsMobileDeferredShadingEnabled(ShaderPlatform)` 或 renderer forward 路径中启用该功能，避免 deferred 路径下生成但不绘制或绘制状态不匹配。

## 9. SceneVisibility 的静态网格 CSM 命令数量可能不匹配

方案在静态 mobile 分支中对 `bRenderAfterTranslucency` 的 mesh 不添加 BasePass，只添加两个新 Pass，但仍保留：

```cpp
if (!bMobileBasePassAlwaysUsesCSM)
{
    DrawCommandPacket.AddCommandsForMesh(..., EMeshPass::MobileBasePassCSM);
}
```

这会导致该 mesh 进入 `MobileBasePassCSM` 但不进入 `BasePass`。移动端后续有逻辑假设 BasePass 与 MobileBasePassCSM 的 visible mesh commands 数量/顺序匹配，`MeshDrawCommands.cpp` 中有 `checkf(MeshCommands.Num() == MeshCommandsCSM.Num())`。

因此 after translucency mesh 不应加入 `MobileBasePassCSM`，或者必须同步调整 mobile base pass CSM merge 逻辑。

## 10. 动态网格仍会加入 `MobileBasePassCSM`

方案修改 `ComputeDynamicMeshRelevance` 时只替换了 BasePass 为两个新 Pass，但原代码后面仍会执行：

```cpp
if (ShadingPath == EShadingPath::Mobile)
{
    PassMask.Set(EMeshPass::MobileBasePassCSM);
    View.NumVisibleDynamicMeshElements[EMeshPass::MobileBasePassCSM] += NumElements;
}
```

对 `bRenderAfterTranslucency` 的动态 SkeletalMesh 来说，这会继续构建 MobileBasePassCSM 动态命令，可能带来错误绘制、浪费，或与 BasePass/CSM 合并假设冲突。

需要在 `ViewRelevance.bRenderAfterTranslucency` 时跳过 `MobileBasePassCSM`。

## 11. Dynamic mesh pass setup 可能未覆盖新 Pass

`FSceneRenderer::SetupMeshPass` 会遍历所有 `MainView` pass 并 dispatch setup，但 mobile 特判跳过 BasePass 和 MobileBasePassCSM。新 Pass 注册为 `MainView` 后理论上会被通用 `SetupMeshPass` 设置。

但需确认 mobile `InitViews` 调用 `SetupMeshPass` 时机早于 `BuildInstanceCullingDrawParams`，且新 Pass 没有被 DebugViewPS/mobile base pass after shadow init 逻辑跳过。否则动态 SkeletalMesh 的 after pass 只设置了 `PassMask`，但没有生成 `ParallelMeshDrawCommandPass`。

这是需要实测/断点确认的部分，尤其 VR mobile multiview/ISR 下。

## 12. `BuildInstanceCullingDrawParams` 对无 GPUScene 路径不影响，但 Dispatch 参数要安全

方案新增：

```cpp
View.ParallelMeshDrawCommandPasses[EMeshPass::MobileAfterTranslucencyDepthPass].BuildRenderingCommands(...);
View.ParallelMeshDrawCommandPasses[EMeshPass::MobileAfterTranslucencyPass].BuildRenderingCommands(...);
```

只在 `Scene->GPUScene.IsEnabled()` 下执行。若 GPUScene 未启用，传入未构建的 `FInstanceCullingDrawParams` 通常应无效但可空安全；不过方案传的是成员地址。需要确认目标 Android VR 配置 GPUScene 状态，以及 DispatchDraw 对未初始化参数是否安全。

保守做法：与其他 pass 一样保持同路径，但不要依赖新成员默认状态；必要时在非 GPUScene 路径传 `nullptr` 或确保成员初始化。

## 13. PSO 预缓存直接 `return` 会导致运行时 hitch

方案在 `CollectPSOInitializers` 开头对两个新 Pass 直接 `return`，这能避免复杂 PSO 预缓存错误，但在 Android/Vulkan 上可能导致第一次显示标记物体时现场创建 PSO，产生卡顿。

如果功能用于 VR，建议不要长期跳过。应为两个新 Pass 正确收集 PSO：

- after color pass 可接近 BasePass PSO，但 render target/depth state 不同。
- after depth pass 若改为 DepthOnly，应复用 DepthPass PSO 收集逻辑。

同时 `FPSOPrecacheParams` 没有 `bRenderAfterTranslucency` 字段，若要按组件属性区分预缓存，也需要扩展参数并在 `UPrimitiveComponent::SetupPrecachePSOParams` 写入。

## 14. `bRenderAfterTranslucency` 只写入 StaticMesh/SkeletalMesh 的 ViewRelevance 不覆盖所有 Mesh/Skeletal 变体

方案只改：

- `FStaticMeshSceneProxy::GetViewRelevance`
- `FSkeletalMeshSceneProxy::GetViewRelevance`

普通 StaticMesh、InstancedStaticMesh/HISM 因继承或调用 `FStaticMeshSceneProxy::GetViewRelevance` 大概率能覆盖。但 Nanite static mesh、GeometryCollection、Poseable/特殊 skeletal proxy、编辑器/调试代理等不一定覆盖。

如果“只 Mesh 和 Skeletal Mesh”严格指 `UStaticMeshComponent` 与 `USkeletalMeshComponent`，还需要确认 Nanite 是否应禁用或额外处理。当前方案没有处理 Nanite 专用渲染路径，Nanite mesh 不会按这两个 mobile after pass 正常绘制。

## 15. `bRenderAfterTranslucency` 与 `bRenderInMainPass` 的语义需要约束

方案的 `AddMeshBatch` 保留原始过滤：

```cpp
PrimitiveSceneProxy && !PrimitiveSceneProxy->ShouldRenderInMainPass()
```

因此用户若关闭 `Render In Main Pass`，即使开启 `RenderAfterTranslucency` 也不会进入新 Pass。UI 上这两个选项可能让使用者误解。

建议：

- 明确要求 `bRenderAfterTranslucency` 依赖 `bRenderInMainPass=true`。
- 或在 setter/编辑器属性上用 `EditCondition="bRenderInMainPass"`。
- 或让 after translucency 独立于 main pass，修改过滤逻辑和 ViewRelevance 条件。

## 16. 需要处理 `bRenderInDepthPass` 对原 DepthPass 的影响

当前被标记物体仍然可能进入普通 DepthPass：

- `ViewRelevance.bRenderInDepthPass = ShouldRenderInDepthPass()` 仍为 true。
- SceneVisibility 的 depth commands 在 base pass 分流前已执行。
- 动态 mesh relevance 一开始也会设置 `EMeshPass::DepthPass`。

如果项目 EarlyZ/MaskedPrePass/full depth prepass 开启，标记物体可能仍会在透明前写入普通 scene depth，透明依然被提前遮挡，且与“在不透明 BasePass 阶段不渲染标记物体”目标冲突。

需要在静态和动态的 DepthPass 添加逻辑：当 `bRenderAfterTranslucency` 时，不加入普通 `DepthPass`，只加入 `MobileAfterTranslucencyDepthPass`。或者要求组件关闭 `bRenderInDepthPass`，但这不适合作为引擎功能默认行为。

## 17. After color pass 的深度测试函数可能不符合目标

方案的 after color pass 使用：

```cpp
TStaticDepthStencilState<false, CF_DepthNearOrEqual>
```

在 Reversed-Z 下 `NearOrEqual` 通常表示更近或相等。由于前面的 after-depth 已写入同一物体深度，第二次绘制应主要靠 Equal 匹配自身深度；使用 NearOrEqual 可能允许更近的片元通过，在重叠标记物体、多 pass 精度差、WPO/dither 变化时产生非预期覆盖。

更稳妥选择通常是：

- depth prewrite 后 color pass 用 `CF_Equal`；
- 或不做单独 depth prewrite，after color pass 直接 `DepthWrite=true`/`DepthTest=NearOrEqual` 绘制一次。

具体取决于是否必须让透明 pass 先被 after-depth 遮挡。

## 18. 统计宏声明位置不完整

方案在 `BasePassRendering.h` 添加：

```cpp
DECLARE_GPU_DRAWCALL_STAT_EXTERN(AfterTranslucency);
DECLARE_GPU_DRAWCALL_STAT_EXTERN(AfterTranslucencyDepth);
```

但当前 `Basepass` 的定义在 `BasePassRendering.cpp`：

```cpp
DEFINE_GPU_DRAWCALL_STAT(Basepass);
```

新 GPU stat 也需要对应 `DEFINE_GPU_DRAWCALL_STAT(...)`，否则链接失败。

另外 CPU cycle stat 的 `DECLARE_CYCLE_STAT_EXTERN` 在 `RenderCore.h`、`DEFINE_STAT` 在 `RenderCore.cpp` 是可行的，但要确认 `MobileBasePassRendering.cpp` 已包含 `RenderCore.h`，当前是包含的。

## 19. 新增函数声明/定义还需要检查 include 与链接

`RenderMobileAfterTranslucencyPass` / `RenderMobileAfterTranslucencyDepthPass` 定义放在 `MobileBasePassRendering.cpp` 可行，但其中使用新 GPU stat、cycle stat、mesh pass enum 后，需要确保：

- `SceneRendering.h` 中 `FMobileSceneRenderer` 声明与定义签名完全一致。
- `BasePassRendering.h` 的 GPU stat extern 被 `MobileBasePassRendering.cpp` 间接/直接包含。
- `RenderCore.h` 中的 stat 声明对 Renderer 模块可见。

若只声明不定义 GPU drawcall stat，会链接失败。

## 20. 不需要在 `FPrimitiveViewRelevance` 构造函数显式设 false

方案在构造函数 memzero 后又写：

```cpp
bRenderAfterTranslucency = false;
```

这不是错误，但无必要。真正需要注意的是 `operator|=` 会按字节 OR，新增 bit 后多 primitive/material relevance 合并时只会从 false 变 true，这符合预期。

## 21. 不透明标记物体的材质类型需要限制

方案目标是“不透明物体 after translucency”。但分流条件只看组件标记，不看材质是否 opaque/masked。

`FMobileBasePassMeshProcessor::ShouldDraw` 对非 translucent base pass 会 `return !bIsTranslucent`，所以透明材质不会被 after color/depth pass 绘制。但 SceneVisibility 仍可能把透明标记组件加入新 pass 的 command/relevance 统计，造成无绘制但有开销。

建议在文档/实现中明确仅支持 opaque/masked material；必要时在 GetViewRelevance 或 SceneVisibility 中只对 `Result.bOpaque || Result.bMasked` 分流。

## 22. 透明 Pass 是否存在时不应决定 after pass 是否执行

当前方案在 `RenderTranslucency()` 后直接调用 after color pass，这很好；不要把 after pass 包进 `ShouldRenderTranslucency()` 条件里。因为即使场景没有透明物体，标记物体也仍需要在 BasePass 之后绘制，否则会消失。

后续实现时需要保持这一点。

## 23. 文档中“透明物体渲染完成后再写入标记物体深度”的表述与实际方案不一致

需求开头说“渲染完成透明物体后再渲染标记物体”，但方案实际是：

1. BasePass 后写标记物体深度；
2. Translucency；
3. 标记物体写颜色。

即深度并不是透明完成后才写，而是在透明前写。这个设计会让透明阶段就被标记物体深度裁掉。若这是期望，应在方案中明确；若不是，则需要重新安排 pass。

---

# 追加分析（针对问题 3 与问题 13）

## A. 问题 3 深入：移动端 Forward 是否可以复用 `FDepthPassMeshProcessor`

### A.1 `FDepthPassMeshProcessor` 在哪里、如何被使用

`FDepthPassMeshProcessor` 定义于 `DepthRendering.h:176`，实现于 `DepthRendering.cpp`。它的职责是只写深度（DepthOnly），不写颜色。

使用方式分两类：

1. 作为已注册的 MeshPass，按 `EMeshPass::DepthPass` 缓存/构建 MeshDrawCommand。注册点：
   - `DepthRendering.cpp:1242` Deferred 路径。
   - `DepthRendering.cpp:1243` Mobile 路径：
     ```cpp
     REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR(MobileDepthPass, CreateDepthPassProcessor, EShadingPath::Mobile, EMeshPass::DepthPass, EMeshPassFlags::CachedMeshCommands | EMeshPassFlags::MainView);
     ```
   - 创建函数 `CreateDepthPassProcessor`（`DepthRendering.cpp:1230`）调用 `SetupDepthPassState`：
     ```cpp
     // DepthRendering.cpp:486
     void SetupDepthPassState(FMeshPassProcessorRenderState& DrawRenderState)
     {
         DrawRenderState.SetBlendState(TStaticBlendState<CW_NONE>::GetRHI());           // 不写颜色
         DrawRenderState.SetDepthStencilState(TStaticDepthStencilState<true, CF_DepthNearOrEqual>::GetRHI()); // 写深度
     }
     ```

2. 作为临时栈对象用于阴影/编辑器图元等（`ShadowSetup.cpp:2584`、`DepthRendering.cpp:385/433/459`）。

移动端实际的 DepthPass 执行入口在 `MobileSceneRenderer`：
- `FMobileSceneRenderer::ShouldRenderPrePass()`（`DepthRendering.cpp:660`）：只有 `EarlyZPassMode == DDM_MaskedOnly` 或 `DDM_AllOpaque` 才会跑。
- `FMobileSceneRenderer::RenderPrePass()`（`DepthRendering.cpp:666`）内部：
  ```cpp
  View.ParallelMeshDrawCommandPasses[EMeshPass::DepthPass].DispatchDraw(nullptr, RHICmdList, InstanceCullingDrawParams);
  ```
- 在 Forward SinglePass 中通过 `RenderMaskedPrePass`（`MobileShadingRenderer.cpp:1606`）在 BasePass 之前、同一个 render pass（Subpass0）内调用。

结论：移动端本来就在用 `FDepthPassMeshProcessor` 写 `EMeshPass::DepthPass`，证明它在移动端 Forward、Subpass0 中可用。

### A.2 Shader 绑定能否完成

可以。`FDepthPassMeshProcessor::Process`（`DepthRendering.cpp:787`）通过 `GetDepthPassShaders<bPositionOnly>`（`DepthRendering.cpp:175`）获取 `TDepthOnlyVS` / `FDepthOnlyPS`，再走标准 `BuildMeshDrawCommands` 完成绑定。这套 DepthOnly shader 在移动端是被编译和使用的（移动端 MaskedPrePass 就用它），所以 Mesh 与 Skeletal Mesh 的顶点工厂都能正确绑定。

关键点：
- 不透明且 `WritesEveryPixel` 的材质会走 position-only / 默认材质，PS 可能为空（`bNeedsPixelShader=false`），这正是最省的纯深度。
- Masked 材质会带 `FDepthOnlyPS` 以执行 clip，行为正确。
- WPO 材质也会被正确处理（`MaterialUsesPixelDepthOffset`、`MaterialModifiesMeshPosition`）。

### A.3 直接复用 `EMeshPass::DepthPass` 的障碍

不能直接把标记物体塞进现成的 `EMeshPass::DepthPass`，原因：

1. 时机不对。移动端 `DepthPass` 在 BasePass 之前（prepass）执行，而需求要求“在透明之后”。同一个 `EMeshPass::DepthPass` 的 ParallelMeshDrawCommandPass 在一帧里只 DispatchDraw 在 prepass 位置。
2. `ShouldRenderPrePass()` 受 `EarlyZPassMode` 限制，项目若不开 prepass 则根本不执行。
3. 普通 DepthPass 会把标记物体的深度写进 prepass，导致透明阶段被提前遮挡（与问题 16 同源）。

因此正确做法是：复用 `FDepthPassMeshProcessor` 的“处理器类型与 shader 流程”，但注册到**新的 MeshPass**（`MobileAfterTranslucencyDepthPass`），并在透明后单独 DispatchDraw。

### A.4 推荐实现步骤（仅移动端 Forward，非 HDR）

1. 仍然新增 `EMeshPass::MobileAfterTranslucencyDepthPass`（深度）与 `EMeshPass::MobileAfterTranslucencyPass`（颜色）两个枚举。

2. 深度 Pass 的 Processor 改为复用 `FDepthPassMeshProcessor`，新增创建函数（放在 `DepthRendering.cpp`，与 `CreateDepthPassProcessor` 并列）：
   ```cpp
   FMeshPassProcessor* CreateMobileAfterTranslucencyDepthPassProcessor(
       ERHIFeatureLevel::Type FeatureLevel, const FScene* Scene,
       const FSceneView* InViewIfDynamicMeshCommand, FMeshPassDrawListContext* InDrawListContext)
   {
       FMeshPassProcessorRenderState DepthPassState;
       SetupDepthPassState(DepthPassState); // CW_NONE + 写深度

       // 关键：用 DDM_AllOpaque，保证不依赖项目 EarlyZ 设置也能收集 opaque+masked
       const EDepthDrawingMode EarlyZPassMode = DDM_AllOpaque;
       const bool bEarlyZPassMovable = true;

       return new FDepthPassMeshProcessor(
           EMeshPass::MobileAfterTranslucencyDepthPass,
           Scene, FeatureLevel, InViewIfDynamicMeshCommand,
           DepthPassState,
           /*bRespectUseAsOccluderFlag*/ false,  // 不要按 occluder 过滤，标记物体不一定是 occluder
           EarlyZPassMode, bEarlyZPassMovable,
           /*bDitheredLODFadingOutMaskPass*/ false,
           InDrawListContext);
   }
   ```
   说明：
   - `FDepthPassMeshProcessor` 的 `MeshPassType` 仅用于 PSO 收集分支判断，传入新枚举不会破坏其逻辑（它只对 `DitheredLODFadingOutMaskPass` 做特判）。
   - `bRespectUseAsOccluderFlag=false` 避免标记物体因非 occluder 被 `AddMeshBatch` 过滤（见 `DepthRendering.cpp:1026`）。
   - `EarlyZPassMode=DDM_AllOpaque` 让 opaque 和 masked 都能写深度，不受项目 prepass 配置影响。

3. 颜色 Pass 仍用 `FMobileBasePassMeshProcessor`（保留方案原设计），用 `CF_Equal`（见问题 17）测试自身深度、不写深度。

4. 注册（`DepthRendering.cpp` 末尾或 `MobileBasePass.cpp`）：
   ```cpp
   REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR(MobileAfterTranslucencyDepthPass,
       CreateMobileAfterTranslucencyDepthPassProcessor,
       EShadingPath::Mobile, EMeshPass::MobileAfterTranslucencyDepthPass,
       EMeshPassFlags::CachedMeshCommands | EMeshPassFlags::MainView);
   ```
   注意：用 `REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR` 会自动注册一个 PSO Collector（见 A.5 与问题 13），DepthPass 处理器自带 `CollectPSOInitializers`，可正确预缓存纯深度 PSO，无需像方案那样直接 `return`。

5. 过滤标记物体：`FDepthPassMeshProcessor` 没有 `ShouldRenderAfterTranslucency` 分流逻辑。两种做法：
   - 做法一（推荐，零侵入）：依赖可见性阶段（`SceneVisibility.cpp`）把标记物体加入 `MobileAfterTranslucencyDepthPass`、不加入 `DepthPass`/`BasePass`；非标记物体不加入新 Pass。这样 Processor 内不需要判断标记。
   - 做法二：在 `FDepthPassMeshProcessor::AddMeshBatch` 内对 `MeshPassType == MobileAfterTranslucencyDepthPass` 增加 `PrimitiveSceneProxy && PrimitiveSceneProxy->ShouldRenderAfterTranslucency()` 判断（注意空指针，见问题 2）。

6. 渲染时机（`MobileShadingRenderer.cpp` SinglePass，Subpass0 内）：
   - 必须保证写深度发生在 Subpass0 深度仍可写阶段。当前 `RenderForwardSinglePass` 在 BasePass 后、`NextSubpass()` 前 depth 可写；但透明在 `NextSubpass()` 之后、depth 只读阶段。
   - 由于需求是“透明之后写深度+颜色”，而透明阶段 depth 已是只读（`ESubpassHint::DepthReadSubpass`），**在 SinglePass 同一 render pass 内无法在透明后再写深度**。这是该方案的根本限制。

   两个可行路线：
   - 路线 1（贴近需求但需要拆 pass）：标记物体的 depth+color 都放到透明之后，且需要一个 depth 可写的新阶段。SinglePass 子通道模型不允许，因此要么走 MultiPass（见问题 7），把标记物体放到 translucency RDG pass 之后、新增一个 `DepthWrite` 的 RDG raster pass；要么关闭该 view 的 tonemap/subpass 合并。
   - 路线 2（方案当前结构）：深度写在 BasePass 之后、`NextSubpass()` 之前（仍 depth 可写），颜色写在透明之后。此时 depth 在透明前已写入，透明会被标记物体遮挡。若可接受“透明被标记物体遮挡”，路线 2 可行且改动最小。

   结论：`FDepthPassMeshProcessor` 完全能在移动端 Forward Subpass0 写深度，shader 绑定没问题；但“透明之后再写深度”受 Subpass0 深度只读限制，必须用路线 2 的时机（透明前写深度）或改造 MultiPass。

### A.5 复用 `FDepthPassMeshProcessor` 相比复用 `FMobileBasePassMeshProcessor` 的优势

- 纯 DepthOnly shader，移动端开销最小，符合 Android VR。
- 不会被 `MobileBasePass::SetOpaqueRenderState` 改写 depth/stencil（解决问题 4）。
- 不写 receive-decal / shading model / lighting channel stencil，深度阶段干净。
- 自带 position-only / 默认材质优化与正确的 PSO 收集。

唯一代价：需要保证可见性阶段把标记物体正确分流到新 Pass（与方案已有的 `SceneVisibility` 改动一致）。

---

## B. 问题 13 确认：方案的深度 Pass 在 `CollectPSOInitializers` 直接 return，是否真的不收集 PSO？BasePass 是否已经收集过？

### B.1 PSO 预缓存的调用链

`UPrimitiveComponent::PrecachePSOs`（`PrimitiveComponent.cpp:4649`）→ 材质 `PrecachePSOs` → `FMaterial::CollectPSOs`（`MaterialShared.cpp:3132`）→ `FMaterialShaderMap::CollectPSOPrecacheData`（`MaterialShader.cpp:2715`）。

关键在 `MaterialShader.cpp:2746`：
```cpp
for (int32 Index = 0; Index < FPSOCollectorCreateManager::GetPSOCollectorCount(ShadingPath); ++Index)
{
    PSOCollectorCreateFunction CreateFunction = FPSOCollectorCreateManager::GetCreateFunction(ShadingPath, Index);
    if (CreateFunction)
    {
        IPSOCollector* PSOCollector = CreateFunction(PrecacheParams.FeatureLevel);
        PSOCollector->CollectPSOInitializers(SceneTexturesConfig, *Material, VertexFactoryData, PrecachePSOParams, PSOInitializers);
    }
}
```

即：**每个通过 `REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR` 注册的 Pass 都会有独立的 PSO Collector，按各自的 `CollectPSOInitializers` 收集。** 不同 Pass 之间不共享收集结果。

### B.2 方案的写法会发生什么

方案把两个新 Pass 用 `REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR` 注册（Plan.md 第 311–312 行），因此它们各自的 PSO Collector 会被调用。

但方案又在 `FMobileBasePassMeshProcessor::CollectPSOInitializers` 开头加入：
```cpp
if (MeshPassType == EMeshPass::MobileAfterTranslucencyPass || MeshPassType == EMeshPass::MobileAfterTranslucencyDepthPass)
{
    return;
}
```

由于两个新 Pass 都用 `FMobileBasePassMeshProcessor` 作为 Collector，这个 early return 会让**两个新 Pass 都不收集任何 PSO**。

### B.3 BasePass 是否已经替它们收集过？——没有

- BasePass 的 PSO Collector 是同一个 `FMobileBasePassMeshProcessor`，但它创建时 `MeshPassType == EMeshPass::BasePass`，收集的 PSO 使用 BasePass 自己的 RenderTargets/DepthStencil 状态与 blend（`CW_RGBA`、`DepthWrite`）。
- 两个新 Pass 的目标 PSO 状态不同：
  - 深度 Pass：`CW_NONE` + 仅写深度（或 DepthOnly shader）。
  - 颜色 Pass：`CW_RGBA` + `DepthRead` + `CF_Equal`（建议）/`NearOrEqual`、不写深度。
- PSO 的 key 包含 blend/depth-stencil/render target 等状态，BasePass 收集的 PSO 与新 Pass 实际使用的 PSO **不是同一个**。

因此结论：

> 方案确实导致两个新 Pass 完全不收集 PSO；BasePass 也没有替它们收集到匹配的 PSO。运行时首次绘制标记物体时会触发 PSO 的运行时创建，在 Android/Vulkan VR 上会产生明显卡顿（hitch）。

### B.4 修正建议

- 深度 Pass：若按问题 3 改为复用 `FDepthPassMeshProcessor`，其 `CollectPSOInitializers`（`DepthRendering.cpp:1096`）本就能正确收集纯深度 PSO，无需 return。它内部依据 `PreCacheParams.bRenderInDepthPass` 决定是否收集——需要确保标记物体的组件 `bRenderInDepthPass` 为 true，或扩展参数（见下）。
- 颜色 Pass：保留 `FMobileBasePassMeshProcessor`，**不要 early return**，而是让它按新 Pass 的 RenderState 收集。但要注意当前 `CollectPSOInitializers` 内部用 `bTranslucentBasePass`/`bMaskedInEarlyPass` 推导 depth-stencil（`MobileBasePass.cpp:1093`），新颜色 Pass 是 opaque-after-translucency，需要让其走 `DepthRead` 且 `CF_Equal` 的状态，可能需要在该函数内对新 MeshPassType 增加分支，使收集到的 PSO 与运行时 `CreateMobileAfterTranslucencyPassProcessor` 设置一致。
- 若要按组件标记精确预缓存，`FPSOPrecacheParams`（`PSOPrecache.h:101` 的位域，`Unused : 21`）没有 `bRenderAfterTranslucency`，可新增一位并在 `UPrimitiveComponent::SetupPrecachePSOParams`（`PrimitiveComponent.cpp:4620`）写入，使 Collector 能据此判断是否收集。
- 退一步：若短期内仍想 early return 规避复杂度，应明确这是“已知会有首帧 PSO 卡顿”的临时方案，并在非 VR 或可接受 hitch 的场景下使用。
