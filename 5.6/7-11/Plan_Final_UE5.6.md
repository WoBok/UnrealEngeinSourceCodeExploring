# RenderAfterTranslucency UE5.6 完整移植方案

核验基线：当前工作区 UE 5.6 源码（2026-07-11）。参考文件：Docs/Plan_Final.md（UE 5.4 已验证方案）。

目标：仅移动端 Forward。标记的 Static Mesh / Skeletal Mesh 在不透明阶段不写颜色、只写 Scene Depth，透明物体绘制后再写颜色，从而遮挡透明物体，并保留可读取的硬件深度。不使用 CustomDepth。

## 0. 核验结论

UE 5.4 的核心设计在 UE 5.6 仍成立，但旧文档不能直接照抄：

1. EMeshPass 在 5.6 已增至非编辑器 39 项；加两个 Pass 后为 41，编辑器为 45，NumBits=6 仍足够。
2. SceneProxy 实现文件已变为 StaticMeshSceneProxy.cpp、SkeletalMeshSceneProxy.cpp。
3. Mobile Forward 已改为 RDG。旧版成员级 BuildInstanceCullingDrawParams() 和 DispatchDraw() 不再适用；必须在 Single/Multi Pass 局部 parameter collection 中增加 culling 参数，调用 BuildMeshRenderingCommands()，绘制使用 Pass->Draw()。
4. Full Prepass 包括 DDM_AllOpaque 与 DDM_AllOpaqueNoVelocity；现有 bIsFullDepthPrepassEnabled 已覆盖二者。
5. 5.6 新增 Mobile Custom Render Pass 路径。若不覆盖，标记物体会被 SceneVisibility 从 BasePass 分流，却没有新 Pass 绘制，部分 Capture/Custom Pass 会缺颜色。
6. ForcePassDrawRenderState、FDepthPassMeshProcessor 以及 Forward 的 depth-write/depth-read subpass 约束仍在，5.4 的状态选择仍正确。

需修改 19 个现有文件；Shader 文件无需修改。

## 1. 正确顺序与状态

非 Full Prepass：

~~~text
MaskedPrePass（如启用）
→ Mobile BasePass（普通不透明物体）
→ MobileAfterTranslucencyDepthPass（标记物体，只写硬件深度）
→ NextSubpass，深度变只读
→ Decal / Modulated Shadow / Fog
→ Translucency
→ MobileAfterTranslucencyPass（标记物体，写颜色、只读深度）
→ Debug / Occlusion / PreTonemap / Resolve
~~~

Full Prepass 时深度已经写入，必须跳过新增深度 Pass，但透明后仍绘制新增颜色 Pass。

| Pass | Color | Depth write | Depth test | Processor |
|---|---:|---:|---|---|
| MobileAfterTranslucencyDepthPass | CW_NONE | 是 | CF_DepthNearOrEqual | FDepthPassMeshProcessor |
| MobileAfterTranslucencyPass | CW_RGBA | 否 | CF_DepthNearOrEqual | FMobileBasePassMeshProcessor |

## 2. Mesh Pass 枚举

文件：Source/Runtime/Renderer/Public/MeshPassProcessor.h

在 MaterialCacheProjection 后、Editor Pass 前加入：

~~~cpp
MobileAfterTranslucencyDepthPass,
MobileAfterTranslucencyPass,
~~~

GetMeshPassName() 加入：

~~~cpp
case EMeshPass::MobileAfterTranslucencyDepthPass: return TEXT("MobileAfterTranslucencyDepthPass");
case EMeshPass::MobileAfterTranslucencyPass: return TEXT("MobileAfterTranslucencyPass");
~~~

断言改为：

~~~cpp
#if WITH_EDITOR
static_assert(EMeshPass::Num == 41 + 4, "Need to update switch(MeshPass) after changing EMeshPass");
#else
static_assert(EMeshPass::Num == 41, "Need to update switch(MeshPass) after changing EMeshPass");
#endif
~~~

NumBits=6 不改。

## 3. Component → Desc → Proxy → Relevance 链路

### 3.1 PrimitiveComponent.h

文件：Source/Runtime/Engine/Classes/Components/PrimitiveComponent.h

紧跟 bRenderInMainPass：

~~~cpp
UPROPERTY(EditAnywhere, AdvancedDisplay, BlueprintReadOnly, Category = Rendering,
    meta = (DisplayName = "Render Opaque After Translucency (Mobile)",
            EditCondition = "bRenderInMainPass"))
uint8 bRenderAfterTranslucency : 1;
~~~

紧跟 SetRenderInMainPass：

~~~cpp
UFUNCTION(BlueprintCallable, Category = "Rendering")
ENGINE_API void SetRenderAfterTranslucency(bool bValue);
~~~

### 3.2 PrimitiveComponent.cpp

构造函数加入 bRenderAfterTranslucency=false。实现：

~~~cpp
void UPrimitiveComponent::SetRenderAfterTranslucency(bool bValue)
{
    if (bRenderAfterTranslucency != bValue)
    {
        bRenderAfterTranslucency = bValue;
        MarkRenderStateDirty();
    }
}
~~~

不加入 SetupPrecachePSOParams()；本方案先禁用两个新 Pass 的不准确 PSO precache。

### 3.3 PrimitiveSceneProxyDesc.h

默认构造加入 bRenderAfterTranslucency=false，字段紧跟 bRenderInMainPass：

~~~cpp
uint32 bRenderAfterTranslucency : 1;
~~~

### 3.4 PrimitiveSceneProxy.cpp

5.6 的函数名是 InitializeFromPrimitiveComponent。加入：

~~~cpp
bRenderAfterTranslucency = InComponent->bRenderAfterTranslucency;
~~~

Proxy 初始化列表中按字段顺序加入：

~~~cpp
, bRenderInMainPass(InProxyDesc.bRenderInMainPass)
, bRenderAfterTranslucency(InProxyDesc.bRenderAfterTranslucency)
, bForceHidden(false)
~~~

### 3.5 PrimitiveSceneProxy.h

加入 getter 和字段：

~~~cpp
inline bool ShouldRenderAfterTranslucency() const { return bRenderAfterTranslucency; }
uint8 bRenderAfterTranslucency : 1;
~~~

字段紧跟 bRenderInMainPass，避免初始化顺序告警。

### 3.6 PrimitiveViewRelevance.h

紧跟 bRenderInMainPass 加入：

~~~cpp
uint32 bRenderAfterTranslucency : 1;
~~~

该结构体构造函数逐字节清零，operator|= 逐字节 OR，无需额外代码。

## 4. 只让 Static Mesh / Skeletal Mesh 生效

文件 Source/Runtime/Engine/Private/StaticMeshSceneProxy.cpp：在 FStaticMeshSceneProxy::GetViewRelevance() 加入：

~~~cpp
Result.bRenderAfterTranslucency = ShouldRenderAfterTranslucency();
~~~

5.4 的 StaticMeshRender.cpp 在 5.6 对应此文件。常规 ISM/HISM 也会覆盖。

文件 Source/Runtime/Engine/Private/SkeletalMeshSceneProxy.cpp：在 FSkeletalMeshSceneProxy::GetViewRelevance() 加入同一行。

5.4 的 SkeletalMesh.cpp 对应实现已移到此文件。5.6 Skeletal Mesh 可走 cached MDC 或 dynamic relevance，因此下一节静态、动态两条命令生成路径都必须修改。

## 5. SceneVisibility 静态/动态分流

文件：Source/Runtime/Renderer/Private/SceneVisibility.cpp

### 5.1 静态 Mesh Draw Command

在 “Specific logic for mobile packets” 的非 Sky 分支计算：

~~~cpp
const bool bMobileRenderAfterTranslucency =
    ViewRelevance.bRenderAfterTranslucency &&
    !IsMobileDeferredShadingEnabled(Scene.GetShaderPlatform());
~~~

true 时分别把原 DrawCommandPacket.AddCommandsForMesh 的最后参数设为两个新 Pass；false 时保留原 BasePass 和条件 MobileBasePassCSM。原调用全部参数保持：

~~~cpp
PrimitiveIndex, PrimitiveSceneInfo, StaticMeshRelevance, StaticMesh,
CullingPayloadFlags, Scene, bCanCache, <MeshPass>
~~~

必须满足：

- Mobile Deferred 回落 BasePass，否则物体会消失。
- 标记物体不能生成 MobileBasePassCSM。
- 原 DepthPass 入队逻辑不删除；Full/Masked prepass 仍需要它。
- Sky/Water/Anisotropy/CustomDepth 原逻辑不变。

### 5.2 动态 Mesh

在 ComputeDynamicMeshRelevance() 中计算：

~~~cpp
const bool bMobileRenderAfterTranslucency =
    ShadingPath == EShadingPath::Mobile &&
    ViewRelevance.bRenderAfterTranslucency &&
    !IsMobileDeferredShadingEnabled(View.GetShaderPlatform());
~~~

true 时设置并累计：

~~~cpp
PassMask.Set(EMeshPass::MobileAfterTranslucencyDepthPass);
View.NumVisibleDynamicMeshElements[EMeshPass::MobileAfterTranslucencyDepthPass] += NumElements;
PassMask.Set(EMeshPass::MobileAfterTranslucencyPass);
View.NumVisibleDynamicMeshElements[EMeshPass::MobileAfterTranslucencyPass] += NumElements;
~~~

false 时保留 BasePass。Mobile CSM 条件改为：

~~~cpp
if (ShadingPath == EShadingPath::Mobile && !bMobileRenderAfterTranslucency)
~~~

其余 Sky/Anisotropy/CustomDepth/Debug/Velocity/Water 逻辑保留。

## 6. 颜色 Pass Processor

文件：Source/Runtime/Renderer/Private/MobileBasePass.cpp

### 6.1 AddMeshBatch()

保留 5.6 当前开头检查，然后加入：

~~~cpp
const bool bAfterTranslucencyColorPass =
    MeshPassType == EMeshPass::MobileAfterTranslucencyPass;
const bool bShouldRenderAfterTranslucency =
    !bDeferredShading && PrimitiveSceneProxy &&
    PrimitiveSceneProxy->ShouldRenderAfterTranslucency();

if (bAfterTranslucencyColorPass)
{
    if (!bShouldRenderAfterTranslucency)
    {
        return;
    }
}
else if (!bTranslucentBasePass && bShouldRenderAfterTranslucency)
{
    return;
}
~~~

!bTranslucentBasePass 不能省略，否则会误删同一 Primitive 上的透明材质 section。

### 6.2 PSO precache

CollectPSOInitializers() 开头对 MobileAfterTranslucencyPass 直接 return。

### 6.3 Create 与注册

~~~cpp
FMeshPassProcessor* CreateMobileAfterTranslucencyPassProcessor(
    ERHIFeatureLevel::Type FeatureLevel,
    const FScene* Scene,
    const FSceneView* View,
    FMeshPassDrawListContext* Context)
{
    FMeshPassProcessorRenderState State;
    State.SetBlendState(TStaticBlendStateWriteMask<CW_RGBA>::GetRHI());
    State.SetDepthStencilAccess(FExclusiveDepthStencil::DepthRead_StencilRead);
    State.SetDepthStencilState(
        TStaticDepthStencilState<false, CF_DepthNearOrEqual>::GetRHI());

    const auto Flags =
        FMobileBasePassMeshProcessor::EFlags::CanUseDepthStencil |
        FMobileBasePassMeshProcessor::EFlags::ForcePassDrawRenderState;

    return new FMobileBasePassMeshProcessor(
        EMeshPass::MobileAfterTranslucencyPass,
        Scene, View, State, Context, Flags);
}

REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR(
    MobileAfterTranslucencyPass,
    CreateMobileAfterTranslucencyPassProcessor,
    EShadingPath::Mobile,
    EMeshPass::MobileAfterTranslucencyPass,
    EMeshPassFlags::CachedMeshCommands | EMeshPassFlags::MainView);
~~~

ForcePassDrawRenderState 必须保留。5.6 Process() 否则会根据 Full/Masked 状态改成 CF_Equal，或调用 SetOpaqueRenderState() 重新开启深度写。

## 7. 深度 Pass Processor

文件：Source/Runtime/Renderer/Private/DepthRendering.cpp

AddMeshBatch() 最前加入：

~~~cpp
if (MeshPassType == EMeshPass::MobileAfterTranslucencyDepthPass &&
    (!PrimitiveSceneProxy || !PrimitiveSceneProxy->ShouldRenderAfterTranslucency()))
{
    return;
}
~~~

只过滤新增 Pass，普通 DepthPass 不过滤。CollectPSOInitializers() 对新 Pass 直接 return，因为其实际 SceneColor RenderPass RT/Subpass 布局与普通 Depth collector 不同。

在 MobileDepthPass 注册后加入：

~~~cpp
FMeshPassProcessor* CreateMobileAfterTranslucencyDepthPassProcessor(
    ERHIFeatureLevel::Type FeatureLevel,
    const FScene* Scene,
    const FSceneView* View,
    FMeshPassDrawListContext* Context)
{
    FMeshPassProcessorRenderState State;
    SetupDepthPassState(State);
    State.SetDepthStencilAccess(FExclusiveDepthStencil::DepthWrite_StencilWrite);

    return new FDepthPassMeshProcessor(
        EMeshPass::MobileAfterTranslucencyDepthPass,
        Scene, FeatureLevel, View, State,
        false, DDM_AllOpaque, true, false, Context);
}

REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR(
    MobileAfterTranslucencyDepthPass,
    CreateMobileAfterTranslucencyDepthPassProcessor,
    EShadingPath::Mobile,
    EMeshPass::MobileAfterTranslucencyDepthPass,
    EMeshPassFlags::CachedMeshCommands | EMeshPassFlags::MainView);
~~~

参数含义：不按 Occluder 过滤；DDM_AllOpaque 接纳 opaque+masked；允许 Movable；不是 dither fading mask pass。SetupDepthPassState 已提供 CW_NONE、depth write 与 NearOrEqual。

## 8. 5.6 绘制函数与声明

文件：Source/Runtime/Renderer/Private/MobileBasePassRendering.cpp

在 RenderMobileBasePass() 后增加深度、颜色两个函数。均设置 Viewport；核心绘制必须使用 5.6 写法：

~~~cpp
if (auto* Pass =
    View.ParallelMeshDrawCommandPasses[EMeshPass::MobileAfterTranslucencyDepthPass])
{
    Pass->Draw(RHICmdList, InstanceCullingDrawParams);
}
~~~

颜色函数索引改成 MobileAfterTranslucencyPass。使用 SCOPE_CYCLE_COUNTER、RHI_BREADCRUMB_EVENT_STAT、SCOPED_GPU_STAT；不要照抄 5.4 的 DispatchDraw()/SCOPED_DRAW_EVENT。

文件：Source/Runtime/Renderer/Private/SceneRendering.h

只加两个函数声明：

~~~cpp
void RenderMobileAfterTranslucencyDepthPass(
    FRHICommandList&, const FViewInfo&, const FInstanceCullingDrawParams*);
void RenderMobileAfterTranslucencyPass(
    FRHICommandList&, const FViewInfo&, const FInstanceCullingDrawParams*);
~~~

5.4 文档中的 renderer 成员级 culling 参数在 5.6 已不存在，不要添加。

## 9. 5.6 主 Mobile Forward RDG 调度

文件：Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp

### 9.1 RenderForwardSinglePass()

局部 FForwardSinglePassParameterCollection 增加两个独立 culling 参数。

在 Scene->GPUScene.IsEnabled() 内：非 Full 时为 after-depth 调用 BuildMeshRenderingCommands；始终为 after-color 调用。不能复用 Translucency 参数。

Pass lambda 中：

1. RenderMobileBasePass() 后、RenderMobileDebugView() 和第一次 NextSubpass() 前，非 Full 时绘制新增深度。
2. RenderTranslucency() 后、debug primitives / occlusion / PreTonemapMSAA / inline-tonemap 的第二次 NextSubpass() 前绘制新增颜色。

深度必须在第一次 NextSubpass 前，因为非 Full 的 Subpass 0 才是 DepthWrite_StencilWrite；颜色位于 depth-read subpass。

### 9.2 RenderForwardMultiPass()

FForwardFirstPassParameterCollection 增加 after-depth params，非 Full 时 build；第一 RDG pass 的 RenderMobileBasePass() 后立即画深度。第一 Pass 后会 resolve depth，第二 Pass 是只读。

FForwardSecondPassParameterCollection 增加 after-color params并 build；第二 RDG pass 的 RenderTranslucency() 后画颜色。

### 9.3 Mobile Deferred

不修改 RenderDeferredSinglePass/MultiPass。SceneVisibility 的 !IsMobileDeferredShadingEnabled 与 Processor 的 !bDeferredShading 保证 Deferred 回落普通 BasePass。

## 10. UE5.6 新增：Mobile Custom Render Pass

RenderCustomRenderPassBasePass() 单独构建 BasePass 与可选 TranslucencyAll。完整覆盖建议仅在 !bDeferredShading 时启用：

1. FCustomPassParameterCollection 增加 after-depth/after-color culling 参数。
2. 非 Full 时 build after-depth；始终 build after-color。
3. Base pass lambda 中，在 RenderMobileBasePass() 后、循环 NextSubpass() 前绘制深度。
4. bIncludeTranslucent=true：把 Translucency 参数包装成含 after-color params 的局部 collection；RenderTranslucency() 后绘制颜色。
5. bIncludeTranslucent=false：没有透明阶段；Forward base pass lambda 完成一次 NextSubpass 后绘制颜色。
6. Deferred 不构建、不绘制新增 Pass。

透明命令与 after-color 不可共用 culling 参数。若项目确认永不使用 Custom Render Pass，本节可选；“全面不遗漏”的版本建议实现。

## 11. CPU/GPU 统计

- RenderCore/Public/RenderCore.h：声明 STAT_AfterTranslucencyDepthDrawTime、STAT_AfterTranslucencyDrawTime。
- RenderCore/Private/RenderCore.cpp：DEFINE_STAT 两项。
- Renderer/Private/BasePassRendering.h：DECLARE_GPU_DRAWCALL_STAT_EXTERN 两项。
- Renderer/Private/BasePassRendering.cpp：DEFINE_GPU_DRAWCALL_STAT 两项。

GPU Stat 定义只能放 .cpp，避免重复定义。

## 12. Shader 无需修改

颜色 Pass 复用 Mobile BasePass VS/PS 与 LightMapPolicy；permutation key 不包含 EMeshPass。

深度 Pass 复用 TDepthOnlyVS、FDepthOnlyPS：Static Mesh 简单 opaque 可走 position-only/null PS；Skeletal GPUSkin VF 走完整 DepthOnly VS；Masked 走 FDepthOnlyPS opacity clip。

## 13. 已知限制与风险

1. 新深度只写硬件 depth，不写 Mobile SceneDepthAux。固定管线遮挡正确，但 DepthFade、软粒子、水深淡出可能看不到标记物体。
2. 标记物体深度在 decal/fog 前存在，但颜色随后覆盖，因此不会正确接收这段屏幕空间 decal/fog；与 5.4 一致。
3. 背后 opaque 可能先画、最后被 after-color 覆盖，视觉正确但增加 overdraw。
4. Masked + r.EarlyZPassOnlyMaterialMasking=1 必须实机验证，避免颜色 Pass 重填镂空像素。
5. 两个新增 Pass 暂不 precache，首次可能创建 PSO。VR 若不能接受，应按 Single/Multi/Custom 的真实 RT/Subpass 实现 collector。
6. 只延后 opaque/masked section；同组件 translucent section 仍走正常透明 Pass。
7. Mobile Deferred 回落普通 BasePass，不具备本效果但物体不能消失。
8. 支持传统 Static/Skeletal Mesh；Nanite 独立 Pass 不在范围内。

## 14. 完整修改文件清单

- [ ] Source/Runtime/Renderer/Public/MeshPassProcessor.h
- [ ] Source/Runtime/Engine/Classes/Components/PrimitiveComponent.h
- [ ] Source/Runtime/Engine/Private/Components/PrimitiveComponent.cpp
- [ ] Source/Runtime/Engine/Public/PrimitiveSceneProxy.h
- [ ] Source/Runtime/Engine/Private/PrimitiveSceneProxy.cpp
- [ ] Source/Runtime/Engine/Public/PrimitiveSceneProxyDesc.h
- [ ] Source/Runtime/Engine/Public/PrimitiveViewRelevance.h
- [ ] Source/Runtime/Engine/Private/StaticMeshSceneProxy.cpp
- [ ] Source/Runtime/Engine/Private/SkeletalMeshSceneProxy.cpp
- [ ] Source/Runtime/Renderer/Private/SceneVisibility.cpp
- [ ] Source/Runtime/Renderer/Private/MobileBasePass.cpp
- [ ] Source/Runtime/Renderer/Private/DepthRendering.cpp
- [ ] Source/Runtime/Renderer/Private/MobileBasePassRendering.cpp
- [ ] Source/Runtime/Renderer/Private/SceneRendering.h
- [ ] Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp
- [ ] Source/Runtime/RenderCore/Public/RenderCore.h
- [ ] Source/Runtime/RenderCore/Private/RenderCore.cpp
- [ ] Source/Runtime/Renderer/Private/BasePassRendering.h
- [ ] Source/Runtime/Renderer/Private/BasePassRendering.cpp

Shader 文件：0 个。

## 15. 静态检查与验证矩阵

- [ ] 非编辑器 EMeshPass::Num=41、编辑器 45，NumBits=6。
- [ ] 两个新 Pass 均有 name、processor 注册、静态入队、动态 PassMask、RDG build、RHI draw。
- [ ] Component→Desc→Proxy→Static/Skeletal Relevance 链路完整。
- [ ] bitfield 声明与 Proxy 初始化列表顺序一致。
- [ ] 标记物体不进 BasePass/CSM；透明 section 未被误删。
- [ ] SinglePass 深度在第一次 NextSubpass 前，颜色在透明后/tonemap subpass 前。
- [ ] MultiPass 深度在第一 pass，颜色在第二 pass。
- [ ] Full Prepass 不 build/draw 新深度，包括 DDM_AllOpaqueNoVelocity。
- [ ] Mobile Deferred 回落；Custom Render Pass 已覆盖或明确限制。
- [ ] Win64 Editor 与 Android arm64 Development/Shipping 编译。
- [ ] Static、Skeletal、cached/dynamic skeletal MDC、ISM/HISM。
- [ ] Opaque、Masked、WPO、同组件 opaque+translucent section。
- [ ] 运行时 SetRenderAfterTranslucency(true/false)。
- [ ] Vulkan SinglePass + Mobile MultiView；GLES/MultiPass（若支持）。
- [ ] MSAA 1x/2x/4x；Mobile HDR/inline tonemap 开关。
- [ ] DDM_None、MaskedOnly、AllOpaque、AllOpaqueNoVelocity。
- [ ] SceneCapture2D、Custom Render Pass 包含/不包含 translucency。
- [ ] RenderDoc/AGI 顺序为 BasePass → after-depth → Translucency → after-color。
- [ ] Full Prepass 无重复 after-depth；profiler 可见新增统计。

## 16. UE5.4 → UE5.6 迁移映射

| 5.4 原方案 | 5.6 正确做法 |
|---|---|
| StaticMeshRender.cpp | StaticMeshSceneProxy.cpp |
| SkeletalMesh.cpp relevance | SkeletalMeshSceneProxy.cpp |
| renderer 成员 culling params | Single/Multi/Custom 的 RDG 局部 collection |
| BuildInstanceCullingDrawParams() | 各路径内 BuildMeshRenderingCommands() |
| DispatchDraw() | 可空 Pass 指针的 Draw() |
| 三参数 RenderMobileBasePass | 5.6 四参数版本（额外 Sky params） |
| SCOPED_DRAW_EVENT | RHI_BREADCRUMB_EVENT_STAT |
| 仅 DDM_AllOpaque | 同时覆盖 DDM_AllOpaqueNoVelocity |
| 无 Custom Render Pass 补丁 | 覆盖 RenderCustomRenderPassBasePass() |

以上方案保留 UE5.4 已验证行为，并适配 UE5.6 当前 Mesh Pass、SceneProxy 文件拆分、RDG instance culling 与 Mobile RenderPass/Subpass 调度。
