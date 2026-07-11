# RenderAfterTranslucency 引擎修改方案分析

> 分析基准：当前工程源码（UE5.4 定制版，含 SecondStageDepthPass / WaterInfoTexturePass 等自定义 Pass）。
> 按要求只列出**存在问题的部分**和**需要继续修改的部分**，已验证无误的条目（枚举与断言数值 34+4/34、Component→Desc→Proxy 数据链路、SceneVisibility 静/动态分流插入点、InstanceCullingDrawParams 成员模式、Stat 声明/定义位置、SetupMeshPass 自动建 Pass 等）不再赘述。
> 所有结论均给出当前源码文件、行号与代码片段佐证。

---

## A. 必须修复的错误

### A1.（最严重）两个新 Processor 必须加 `EFlags::ForcePassDrawRenderState`，否则 Create 函数里设置的渲染状态会被 `Process()` 覆盖，深度 Pass 可能完全不写深度

方案中 `CreateMobileAfterTranslucencyDepthPassProcessor` / `CreateMobileAfterTranslucencyPassProcessor` 只传了 `CanUseDepthStencil`。但 `FMobileBasePassMeshProcessor::Process()` 在**没有** `ForcePassDrawRenderState` 时会重设 DepthStencilState：

`Engine/Source/Runtime/Renderer/Private/MobileBasePass.cpp:942-961`
```c++
	const bool bMaskedInEarlyPass = (MaterialResource.IsMasked() || MeshBatch.bDitheredLODTransition) && Scene && MaskedInEarlyPass(Scene->GetShaderPlatform());
	const bool bForcePassDrawRenderState = ((Flags & EFlags::ForcePassDrawRenderState) == EFlags::ForcePassDrawRenderState);

	FMeshPassProcessorRenderState DrawRenderState(PassDrawRenderState);
	if (!bForcePassDrawRenderState)
	{
		if (bTranslucentBasePass)
		{
			MobileBasePass::SetTranslucentRenderState(DrawRenderState, MaterialResource, ShadingModels);
		}
		else if((MeshBatch.bUseForDepthPass && Scene->EarlyZPassMode == DDM_AllOpaque) || bMaskedInEarlyPass)
		{
			DrawRenderState.SetDepthStencilState(TStaticDepthStencilState<false, CF_Equal>::GetRHI());
		}
		else
		{
			const bool bEnableReceiveDecalOutput = ((Flags & EFlags::CanUseDepthStencil) == EFlags::CanUseDepthStencil);
			MobileBasePass::SetOpaqueRenderState(DrawRenderState, PrimitiveSceneProxy, MaterialResource, ShadingModels, bEnableReceiveDecalOutput && IsMobileHDR(), bPassUsesDeferredShading);
		}
	}
```

具体后果：

1. **深度 Pass 不写深度**：若 `Scene->EarlyZPassMode == DDM_AllOpaque` 或材质为 Masked 且平台启用 MaskedInEarlyPass，命令状态被改成 `TStaticDepthStencilState<false, CF_Equal>`（第 954 行）——DepthWrite 关闭、比较函数 CF_Equal。此时 MobileAfterTranslucencyDepthPass 一个像素的深度都写不进去，整套机制失效。
2. **颜色 Pass 被打开深度写**：走到第 959 行 `SetOpaqueRenderState` 时，`MobileBasePass.cpp:547-556` 会强制换成**深度写开启**的状态：
```c++
	if (bEnableReceiveDecalOutput || bUsesDeferredShading)
	{
		DrawRenderState.SetDepthStencilState(TStaticDepthStencilState<
				true, CF_DepthNearOrEqual,
				true, CF_Always, SO_Keep, SO_Keep, SO_Replace,
				...
```
这与方案中给颜色 Pass 设置的 `FExclusiveDepthStencil::DepthRead_StencilRead` 矛盾；在多 Pass 路径（见 A3）第二个 RenderPass 的深度绑定是只读的，深度写状态的 PSO 会触发 RHI 校验失败或未定义行为。

**修复**：两个 Create 函数中都改为
```c++
const FMobileBasePassMeshProcessor::EFlags Flags =
	FMobileBasePassMeshProcessor::EFlags::CanUseDepthStencil |
	FMobileBasePassMeshProcessor::EFlags::ForcePassDrawRenderState;
```
`ForcePassDrawRenderState` 的语义见 `Engine/Source/Runtime/Renderer/Private/MobileBasePassRendering.h:473-474`：
```c++
	// Informs the processor to use PassDrawRenderState for all mesh commands
	ForcePassDrawRenderState = (1 << 2),
```
副作用提示：加了 Force 后不再输出 ReceiveDecal 的 stencil 位（`SetOpaqueRenderState` 内的 `GET_STENCIL_BIT_MASK(RECEIVE_DECAL, ...)`，MobileBasePass.cpp:533-537 不会执行），标记物体将不参与贴花 stencil 剔除——对本需求通常无影响，但需知晓。

---

### A2. AddMeshBatch 的 Pass 分流会误杀标记物体的半透明材质 Section

`FMobileBasePassMeshProcessor` 不只服务 BasePass，还被注册给全部移动端半透明 Pass：

`Engine/Source/Runtime/Renderer/Private/MobileBasePass.cpp:1218-1222`
```c++
REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR(MobileBasePass, 			CreateMobileBasePassProcessor, 			EShadingPath::Mobile, EMeshPass::BasePass, 		...);
REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR(MobileBasePassCSM,			CreateMobileBasePassCSMProcessor,		EShadingPath::Mobile, EMeshPass::MobileBasePassCSM, 	...);
REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR(MobileTranslucencyAllPass,		CreateMobileTranslucencyAllPassProcessor,	EShadingPath::Mobile, EMeshPass::TranslucencyAll, 	...);
REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR(MobileTranslucencyStandardPass,	CreateMobileTranslucencyStandardPassProcessor,	EShadingPath::Mobile, EMeshPass::TranslucencyStandard, 	...);
REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR(MobileTranslucencyAfterDOFPass,	CreateMobileTranslucencyAfterDOFProcessor,	EShadingPath::Mobile, EMeshPass::TranslucencyAfterDOF, 	...);
```

方案里的 else 分支：
```c++
    else
    {
        if (bShouldRenderAfterTranslucency)
            return;
    }
```
当 `MeshPassType == TranslucencyStandard`（或 AfterDOF/All）且该 Primitive 标记了 `bRenderAfterTranslucency` 时也会 return——**标记物体上带半透明材质的 Section 会从半透明 Pass 中被剔除而彻底消失**（SceneVisibility 的半透明入队逻辑与本方案的分流无关，见 SceneVisibility.cpp:1634-1639，物体仍会进 TranslucencyStandard，但命令生成阶段被这里拦掉）。

**修复**：else 分支只拦不透明 Pass（利用现成的 `bTranslucentBasePass` 成员，构造于 MobileBasePass.cpp:822）：
```c++
    else if (!bTranslucentBasePass) // 仅 BasePass / MobileBasePassCSM 需要剔除
    {
        if (bShouldRenderAfterTranslucency)
            return;
    }
```

---

### A3. 未处理 `bIsFullDepthPrepassEnabled`（EarlyZPass = DDM_AllOpaque）路径：深度附件只读，AfterTranslucencyDepthPass 的 DepthWrite 非法

该标志的来源：

`Engine/Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp:302`
```c++
	bIsFullDepthPrepassEnabled = Scene->EarlyZPassMode == DDM_AllOpaque;
```

启用后，整个 SceneColor RenderPass 的深度绑定是**只读**的：

`MobileShadingRenderer.cpp:1494-1496`（`InitRenderTargetBindings_Forward`）
```c++
	BasePassRenderTargets.DepthStencil = bIsFullDepthPrepassEnabled ? 
		FDepthStencilBinding(SceneDepth, ERenderTargetLoadAction::ELoad, ERenderTargetLoadAction::ELoad, FExclusiveDepthStencil::DepthRead_StencilWrite) : 
		FDepthStencilBinding(SceneDepth, ERenderTargetLoadAction::EClear, ERenderTargetLoadAction::EClear, FExclusiveDepthStencil::DepthWrite_StencilWrite);
```
（第二个及以后的 View 同样，见 :1540）

此时在 :1609 后 dispatch 一个 `DepthWrite_StencilWrite` 的 Pass 属于越权访问，RDG/RHI 校验会报错。不过在这个配置下**深度其实不需要你再写**：标记物体照常进 `EMeshPass::DepthPass`（本方案没有改这里）：

`Engine/Source/Runtime/Renderer/Private/SceneVisibility.cpp:1530-1542`
```c++
	// Add depth commands.
	if (StaticMeshRelevance.bUseForDepthPass && (bDrawDepthOnly || (bMobileMaskedInEarlyPass && ViewRelevance.bMasked)))
	{
		...
		DrawCommandPacket.AddCommandsForMesh(..., EMeshPass::DepthPass);
	}
```
而 Full Prepass 在 SceneColor Pass 之前独立执行（MobileShadingRenderer.cpp:1248 `RenderFullDepthPrepass(GraphBuilder, Views, SceneTextures);`），标记物体的深度已经在那里写入，半透明依旧会被正确遮挡。

**修复**：dispatch 处加条件：
```c++
if (!bIsFullDepthPrepassEnabled)
{
	RenderMobileAfterTranslucencyDepthPass(RHICmdList, View, &AfterTranslucencyDepthInstanceCullingDrawParams);
}
```
颜色 Pass 不受影响（DepthRead 与只读绑定兼容）。
同理，`BuildInstanceCullingDrawParams` 中对 DepthPass 也是这样按条件构建的（MobileShadingRenderer.cpp:1437-1440），可参照给新深度 Pass 加同样条件，避免无用的 BuildRenderingCommands。

---

### A4. 单 Pass 路径插入点受 Subpass 约束——深度 Pass 必须在 `NextSubpass()` 之前

`RenderForwardSinglePass` 使用 `ESubpassHint::DepthReadSubpass`（MobileShadingRenderer.cpp:1586），在第 1614 行切换 Subpass 之后深度即为只读 fetch：

`MobileShadingRenderer.cpp:1608-1623`
```c++
		RHICmdList.SetCurrentStat(GET_STATID(STAT_CLMM_Opaque));
		RenderMobileBasePass(RHICmdList, View, &PassParameters->InstanceCullingDrawParams);
		RenderMobileDebugView(RHICmdList, View);
		RHICmdList.PollOcclusionQueries();
		PostRenderBasePass(RHICmdList, View);
		// scene depth is read only and can be fetched
		RHICmdList.NextSubpass();
		RHICmdList.SetCurrentStat(GET_STATID(STAT_CLMM_Translucency));
		RenderDecals(RHICmdList, View, &PassParameters->InstanceCullingDrawParams);
		RenderModulatedShadowProjections(RHICmdList, ViewContext.ViewIndex, View);
		...
		// Draw translucency.
		RenderTranslucency(RHICmdList, View);
```

方案把深度 Pass 加在 :1609（`RenderMobileBasePass` 之后）**恰好合法**，但这是硬约束而不是随意位置：绝不能放到 `NextSubpass()`（:1614）之后，否则在 Vulkan 上深度写会违反 Subpass 声明。同样，颜色 Pass 必须在 `RenderTranslucency`（:1623）之后、且在 `bTonemapSubpassInline` 的第二次 `NextSubpass()`（:1650）之前——方案位置满足，但建议在代码中用注释固定这两个锚点。

**连带的视觉副作用（需知晓）**：标记物体的深度在 Subpass 0 末尾就进入了深度缓冲，Subpass 1 里的 `RenderDecals` / `RenderModulatedShadowProjections` / `RenderFog`（:1616-1621）都会把它当作"已存在的不透明表面"来处理——贴花会投影到一个此时还看不见的物体区域、雾会按其深度计算。颜色 Pass 之后覆盖绘制时这些效果不会出现在标记物体上（尤其高度雾在逐像素 Pass 中不会作用于它，需要材质端或后续 Pass 自行补偿）。

---

### A5. `DEFINE_GPU_DRAWCALL_STAT` 必须写在 BasePassRendering.cpp，不能放进 .h

方案第 5 条把 DECLARE 与 DEFINE 都写在 "BasePassRendering.h" 小节下（"在:184附近添加 DEFINE_GPU_DRAWCALL_STAT(...)"）。当前引擎的组织方式：

```
Source\Runtime\Renderer\Private\BasePassRendering.h:144:  DECLARE_GPU_DRAWCALL_STAT_EXTERN(Basepass);
Source\Runtime\Renderer\Private\BasePassRendering.cpp:184: DEFINE_GPU_DRAWCALL_STAT(Basepass);
```

`DEFINE_GPU_DRAWCALL_STAT` 展开为带外部链接的变量定义，放在被多个翻译单元包含的头文件里会产生**重复定义链接错误**。请明确放到 `BasePassRendering.cpp:184` 附近：
```c++
DEFINE_GPU_DRAWCALL_STAT(Basepass);
DEFINE_GPU_DRAWCALL_STAT(AfterTranslucency);      //RenderAfterTranslucency Added
DEFINE_GPU_DRAWCALL_STAT(AfterTranslucencyDepth); //RenderAfterTranslucency Added
```
（`SCOPED_GPU_STAT` 在 MobileBasePassRendering.cpp 可用性没有问题：该文件已经使用 `SCOPED_GPU_STAT(RHICmdList, Basepass)`，见 MobileBasePassRendering.cpp:475，说明 BasePassRendering.h 已被包含。）

---

## B. 需要补充/决策的部分

### B1. 移动端延迟渲染路径下标记物体会直接消失（若无意支持，建议加保护）

SceneVisibility 的分流条件是 `ShadingPath == EShadingPath::Mobile`，这**包含 Mobile Deferred**。但方案只在 `RenderForwardSinglePass` / `RenderForwardMultiPass` 中 dispatch 新 Pass；延迟路径入口在：

`Engine/Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp:1885 / 1947`
```c++
void FMobileSceneRenderer::RenderDeferred(FRDGBuilder& GraphBuilder, const FSortedLightSetSceneInfo& SortedLightSet, ...)
void FMobileSceneRenderer::RenderDeferredSinglePass(FRDGBuilder& GraphBuilder, ...)
```
这两个函数没有任何新 Pass 的调用。一旦项目（或某个平台配置）切到 `r.Mobile.ShadingPath=1`，标记物体被移出 BasePass 却永远不会被绘制。

**建议**：既然只需要 Forward，就在分流处挡住延迟路径，例如在 `ComputeDynamicMeshRelevance` / 静态路径的判断中加上 `!IsMobileDeferredShadingEnabled(ShaderPlatform)`，或至少在文档/断言里明确此限制。（`FMobileBasePassMeshProcessor` 已缓存 `bDeferredShading`，见 MobileBasePass.cpp:823。）

### B2. 静态路径中 MobileBasePassCSM 的入队最好一起移进 else 分支

方案修改后的静态路径仍无条件执行（对应现引擎 SceneVisibility.cpp:1565-1568）：
```c++
	if (!bMobileBasePassAlwaysUsesCSM)
	{
		DrawCommandPacket.AddCommandsForMesh(..., EMeshPass::MobileBasePassCSM);
	}
```
标记物体会照常向 MobileBasePassCSM 提交命令，最终虽会被 AddMeshBatch 的分流（A2 修正后）丢弃，但白白付出命令生成/缓存的 CPU 成本。建议把这段也放进 `else`（即仅非标记物体入队 CSM Pass）。

### B3. PSO Precache 被整体跳过——接受首帧 PSO 编译卡顿，或补全收集逻辑

方案第 7 条在 `CollectPSOInitializers` 开头直接 return（插入点上下文确认无误，MobileBasePass.cpp:1056-1060）。后果有二：

1. 新 Pass 的 PSO 全部走运行时首帧编译，Android/Vulkan 上可能造成标记物体首次出现时卡顿；
2. `UPrimitiveComponent::SetupPrecachePSOParams` 不感知新标志（`Engine/Source/Runtime/Engine/Private/Components/PrimitiveComponent.cpp:4620-4622`）：
```c++
void UPrimitiveComponent::SetupPrecachePSOParams(FPSOPrecacheParams& Params)
{
	Params.bRenderInMainPass = bRenderInMainPass;
```
标记物体仍按 BasePass 状态预热 PSO（深度/混合状态与新 Pass 不一致，预热结果用不上）。

**建议**：如果标记物体数量少、材质固定，可接受现状；否则为 `MobileAfterTranslucencyPass` 按其真实 RenderState 收集 PSO（颜色 Pass 与 BasePass 的 RT 布局一致，只有 DepthStencil 状态不同，改造成本很低）。若深度 Pass 按 C2 换成 `FDepthPassMeshProcessor`，它自带完整的 CollectPSOInitializers。

### B4. 说明性确认：`InitializeFrom` 的归属

方案第 3 条写"PrimitiveSceneProxy.cpp 的 InitializeFrom 中 :277 附近"。该处实际是 **`FPrimitiveSceneProxyDesc::InitializeFrom`**（Component→Desc 的拷贝），而非 Proxy 的成员函数：

`Engine/Source/Runtime/Engine/Private/PrimitiveSceneProxy.cpp:265-277`
```c++
void FPrimitiveSceneProxyDesc::InitializeFrom(const UPrimitiveComponent* InComponent)
{
	...
	bRenderInDepthPass = InComponent->bRenderInDepthPass;
	bRenderInMainPass = InComponent->bRenderInMainPass;
```
Proxy 本体只从 Desc 初始化（:398/:428），组件构造路径通过 :393-396 的委托统一走 Desc：
```c++
FPrimitiveSceneProxy::FPrimitiveSceneProxy(const UPrimitiveComponent* InComponent, FName InResourceName)
	: FPrimitiveSceneProxy(FPrimitiveSceneProxyDesc(InComponent), InResourceName)
```
所以只要把拷贝加在 `FPrimitiveSceneProxyDesc::InitializeFrom`（:277 附近）+ Desc 初始化列表（:428 附近）+ `PrimitiveSceneProxyDesc.h:25` 默认值，这条链路就是完整的——方案代码落点正确，但注意作用域是 Desc，不要真的写进 Proxy 的某个 InitializeFrom。
另外 `FStaticMeshSceneProxyDesc::InitializeFrom`（StaticMeshRender.cpp:2608-2610）会调用基类版本，静态网格的 Desc 路径自动覆盖，无需额外修改。

---

## C. 方案中两个问题的解答

### C1. 深度 Pass 的 `CW_NONE` 会不会影响后续半透明 Pass 的颜色写入？——不会

渲染状态不是全局状态机，而是**每条 MeshDrawCommand 各自烘焙进 PSO**。每次 `Process()` 都从本 Processor 的 `PassDrawRenderState` 拷贝一份再修改：

`MobileBasePass.cpp:945`
```c++
	FMeshPassProcessorRenderState DrawRenderState(PassDrawRenderState);
```
而每个半透明 Pass 有自己独立的 Processor 实例与状态，且半透明命令一律走 `SetTranslucentRenderState` 重设混合（`MobileBasePass.cpp:948-951`）。`TranslucencyStandard` 等 Processor 由各自的 Create 函数构造（注册表见 MobileBasePass.cpp:1220-1222），与你的深度 Pass Processor 互不相干。

### C2. 深度 Pass 能否绑定移动端 DepthPass 的专用 Shader？——可以，且是推荐做法

你的担心是对的：当前方案的深度 Pass 仍然走 `FMobileBasePassMeshProcessor::Process`，绑定完整的 BasePass VS/PS：

`MobileBasePass.cpp:906-908, 930-940`
```c++
	TMeshProcessorShaders<
		TMobileBasePassVSPolicyParamType<FUniformLightMapPolicy>,
		TMobileBasePassPSPolicyParamType<FUniformLightMapPolicy>> BasePassShaders;
	...
	if (!MobileBasePass::GetShaders(
		LightMapPolicyType, LocalLightSetting, MaterialResource, ...
```
即便 `CW_NONE` 屏蔽了颜色输出，**PS 仍会被执行**（在 TBDR 上只是 ROP 阶段丢弃写入），光照/贴图采样开销一分不少。

引擎在移动端本来就注册了深度专用 Processor，可直接复用其全部基础设施：

`Engine/Source/Runtime/Renderer/Private/DepthRendering.cpp:1243`
```c++
REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR(MobileDepthPass, CreateDepthPassProcessor, EShadingPath::Mobile, EMeshPass::DepthPass, EMeshPassFlags::CachedMeshCommands | EMeshPassFlags::MainView);
```

它的优势（DepthRendering.cpp 佐证）：

- 状态即为"只写深度"：`SetupDepthPassState`（:486-491）
```c++
	// Disable color writes, enable depth tests and writes.
	DrawRenderState.SetBlendState(TStaticBlendState<CW_NONE>::GetRHI());
	DrawRenderState.SetDepthStencilState(TStaticDepthStencilState<true, CF_DepthNearOrEqual>::GetRHI());
```
- 不透明、非 WPO、支持 PositionOnly 流的材质自动替换为默认材质 + 仅位置 VS + **空 PS**（`ShouldRender`，:945-954；Shader 实现 :158-165）：
```c++
	if (IsOpaqueBlendMode(Material)
		&& EarlyZPassMode != DDM_MaskedOnly
		&& bSupportPositionOnlyStream
		&& !bMaterialModifiesMeshPosition
		&& Material.WritesEveryPixel(...))
	{
		bShouldRender = true;
		bUseDefaultMaterial = true;
		bPositionOnly = true;
	}
```
```c++
IMPLEMENT_MATERIAL_SHADER_TYPE(template<>,TDepthOnlyVS<true>,TEXT("/Engine/Private/PositionOnlyDepthVertexShader.usf"),TEXT("Main"),SF_Vertex);
IMPLEMENT_SHADERPIPELINE_TYPE_VS(DepthPosOnlyNoPixelPipeline, TDepthOnlyVS<true>, true);
```
- Skeletal Mesh（GPUSkin 不支持 PositionOnly）会走 `Process<false>`：完整 VF 的 DepthOnly VS + 非 Masked 时空 PS，仍远比 BasePass 便宜。

**具体做法**：
1. 仿照 `CreateDepthPassProcessor`（:1230-1240）新写：
```c++
FMeshPassProcessor* CreateMobileAfterTranslucencyDepthPassProcessor(ERHIFeatureLevel::Type FeatureLevel, const FScene* Scene, const FSceneView* InViewIfDynamicMeshCommand, FMeshPassDrawListContext* InDrawListContext)
{
	FMeshPassProcessorRenderState DepthPassState;
	SetupDepthPassState(DepthPassState);
	// EarlyZPassMode 传 DDM_AllOpaque：保证 Masked 材质也被绘制（带 FDepthOnlyPS 做 clip）
	// bRespectUseAsOccluderFlag 传 false：否则 AddMeshBatch(:1026-1046) 会按 Occluder/Movable/屏幕尺寸过滤掉动态小物体
	return new FDepthPassMeshProcessor(EMeshPass::MobileAfterTranslucencyDepthPass, Scene, FeatureLevel, InViewIfDynamicMeshCommand, DepthPassState, /*bRespectUseAsOccluderFlag*/false, DDM_AllOpaque, /*bEarlyZPassMovable*/true, false, InDrawListContext);
}
```
2. 在 `FDepthPassMeshProcessor::AddMeshBatch`（DepthRendering.cpp:1021）加与 MobileBasePass 相同的分流：`MeshPassType == EMeshPass::MobileAfterTranslucencyDepthPass` 时要求 `ShouldRenderAfterTranslucency()`，其余 Pass 反之剔除标记物体（DepthPass 本身可不剔除，见 A3 的讨论）。
3. 注意其入口要求 `MeshBatch.bUseForDepthPass`（:1023）——静态网格与骨骼网格的常规材质均满足。
4. 这样 A1 中"深度 Pass 状态被覆盖"的问题也随之消失（FDepthPassMeshProcessor 没有 SetOpaqueRenderState 那套覆盖逻辑），且自带 PSO Precache 支持（缓解 B3）。

颜色 Pass 保持 `FMobileBasePassMeshProcessor` 不变（需要完整光照着色），只需按 A1 加 `ForcePassDrawRenderState`。

---

## D. 方案总体评价与替代方案

**总体评价**：方向正确。"两个新 MeshPass + ViewRelevance 分流 + 在 Forward 渲染循环两处 dispatch"与引擎现有自定义 Pass（如本工程已有的 WaterInfoTexturePass）的做法一致；VR 关心的 Instanced Stereo/MultiView 也不需要额外处理——`FSceneRenderer::SetupMeshPass` 对所有注册为 `MainView` 的 Pass 统一走 Stereo Instance Culling（SceneRendering.cpp:4202-4252，`EInstanceCullingMode::Stereo` 分支），新 Pass 自动获得双目实例化。修完 A1-A5 后此方案可用。

**每帧成本**：标记物体的几何被绘制两遍（深度 + 颜色）。采用 C2 的 DepthOnly Processor 后，深度遍成本约等于一次 Prepass（PositionOnly VS/空 PS），是移动端可接受的开销；命令生成侧因两个新 Pass 都支持 CachedMeshCommands，静态物体几乎无每帧 CPU 增量。

**替代方案对比**：

1. **（备选，改动最小）强制 `r.EarlyZPass = DDM_AllOpaque`（Full Depth Prepass）+ 只加一个颜色 Pass**：
   如 A3 所述，Full Prepass 开启时标记物体深度天然写入（SceneVisibility.cpp:1541 + MobileShadingRenderer.cpp:1248），半透明自动被遮挡，只需 `MobileAfterTranslucencyPass` 一个颜色 Pass。
   代价：整个场景多一遍全量深度绘制，对 Mobile VR（本就顶点带宽敏感、TBDR 下 Prepass 收益有限）通常不划算，除非项目已因其他原因启用 Full Prepass。当前方案（按需写深度）对 VR 更合适。
2. **Stencil 标记 + 半透明材质端剔除**：让标记物体写 stencil，半透明材质读 stencil 丢弃像素。移动端 Forward 下需要材质端配合、且 stencil 位已被 ReceiveDecal/ShadingModel 占用（MobileBasePass.cpp:533-544），侵入性更高，不推荐。
3. **CustomDepth**：你已排除，且它需要独立 RT，在移动端多一次 RenderPass 切换，确实不如本方案。

**修复优先级**：A1（ForcePassDrawRenderState 或 C2 换 Processor）> A3（Full Prepass 保护）> A2（半透明误杀）> A5（链接错误）> B1/B2/B3。
