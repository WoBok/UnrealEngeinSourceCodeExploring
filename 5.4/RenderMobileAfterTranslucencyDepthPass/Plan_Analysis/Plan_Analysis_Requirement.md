void FMobileSceneRenderer::RenderForwardSinglePass(FRDGBuilder& GraphBuilder, FMobileRenderPassParameters* PassParameters, FRenderViewContext& ViewContext, FSceneTextures& SceneTextures)
{
if (bTonemapSubpassInline)
{
// tonemapping LUT pass before we start main render pass. The texture is needed by the custom resolve pass which does tonemapping
PassParameters->ColorGradingLUT = AddCombineLUTPass(GraphBuilder, *ViewContext.ViewInfo);
}

	PassParameters->RenderTargets.SubpassHint = bTonemapSubpassInline ? ESubpassHint::CustomResolveSubpass : ESubpassHint::DepthReadSubpass;
	const bool bDoOcclusionQueries = (!bIsFullDepthPrepassEnabled && ViewContext.bIsLastView && DoOcclusionQueries());
	PassParameters->RenderTargets.NumOcclusionQueries = bDoOcclusionQueries ? ComputeNumOcclusionQueriesToBatch() : 0u;
	
	GraphBuilder.AddPass(
		RDG_EVENT_NAME("SceneColorRendering"),
		PassParameters,
		// the second view pass should not be merged with the first view pass on mobile since the subpass would not work properly.
		ERDGPassFlags::Raster | ERDGPassFlags::NeverMerge,
		[this, PassParameters, ViewContext, bDoOcclusionQueries, &SceneTextures](FRHICommandList& RHICmdList)
	{
		FViewInfo& View = *ViewContext.ViewInfo;
			
		if (GIsEditor && !View.bIsSceneCapture && ViewContext.bIsFirstView)
		{
			DrawClearQuad(RHICmdList, View.BackgroundColor);
		}

		// Depth pre-pass
		RHICmdList.SetCurrentStat(GET_STATID(STAT_CLM_MobilePrePass));
		RenderMaskedPrePass(RHICmdList, View);
		// Opaque and masked
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
		if (GMaxRHIShaderPlatform != SP_METAL_SIM)
		{
			RenderFog(RHICmdList, View);
		}
		// Draw translucency.
		RenderTranslucency(RHICmdList, View);
        RenderMobileAfterTranslucencyPass(RHICmdList, View, &AfterTranslucencyInstanceCullingDrawParams);//RenderAfterTranslucency Added

// scene depth is read only and can be fetched
这里为什么depth read only, 是因为RHICmdList.SetCurrentStat(GET_STATID(STAT_CLMM_Translucency));这一句的原因吗？
RenderMobileAfterTranslucencyPass(RHICmdList, View, &AfterTranslucencyInstanceCullingDrawParams);//RenderAfterTranslucency Added
这是我准备放在RenderTranslucency(RHICmdList, View);之后，这样会影响我读写深度吗？
FMeshPassProcessor* CreateMobileAfterTranslucencyPassProcessor(ERHIFeatureLevel::Type FeatureLevel, const FScene* Scene, const FSceneView* InViewIfDynamicMeshCommand, FMeshPassDrawListContext* InDrawListContext)
{
FMeshPassProcessorRenderState PassDrawRenderState;
PassDrawRenderState.SetBlendState(TStaticBlendStateWriteMask<CW_RGBA>::GetRHI());
const FExclusiveDepthStencil::Type DefaultBasePassDepthStencilAccess = FScene::GetDefaultBasePassDepthStencilAccess(FeatureLevel);
PassDrawRenderState.SetDepthStencilAccess(DefaultBasePassDepthStencilAccess);
PassDrawRenderState.SetDepthStencilState(TStaticDepthStencilState<true, CF_DepthNearOrEqual>::GetRHI());

	const FMobileBasePassMeshProcessor::EFlags Flags = FMobileBasePassMeshProcessor::EFlags::CanUseDepthStencil
		| (MobileBasePassAlwaysUsesCSM(GShaderPlatformForFeatureLevel[FeatureLevel]) ? FMobileBasePassMeshProcessor::EFlags::CanReceiveCSM : FMobileBasePassMeshProcessor::EFlags::None);

	return new FMobileBasePassMeshProcessor(EMeshPass::MobileAfterTranslucencyPass, Scene, InViewIfDynamicMeshCommand, PassDrawRenderState, InDrawListContext, Flags, true);
}
这是我PassProcessor的创建逻辑，这里会对读写产生影响吗？查找先后执行顺序，谁会覆盖谁？
void FMobileSceneRenderer::RenderMobileAfterTranslucencyPass(FRHICommandList& RHICmdList, const FViewInfo& View, const FInstanceCullingDrawParams* InstanceCullingDrawParams)
{
CSV_SCOPED_TIMING_STAT_EXCLUSIVE(RenderAfterTranslucency);
SCOPED_DRAW_EVENT(RHICmdList, MobileAfterTranslucencyPass);
SCOPE_CYCLE_COUNTER(STAT_AfterTranslucencyDrawTime);
SCOPED_GPU_STAT(RHICmdList, AfterTranslucency);

	RHICmdList.SetViewport(View.ViewRect.Min.X, View.ViewRect.Min.Y, 0, View.ViewRect.Max.X, View.ViewRect.Max.Y, 1);
	View.ParallelMeshDrawCommandPasses[EMeshPass::MobileAfterTranslucencyPass].DispatchDraw(nullptr, RHICmdList, InstanceCullingDrawParams);
}
这是我RenderMobileAfterTranslucencyPass的定义

---
深度渲染是在什么地方？这里和深度渲染应该没有关系了吧？深度应该在这之前就已经渲染完成了吧？RenderMobileBasePass这里为什么还需要写入深度？
找出完整的移动端的深度渲染链路，Forward路径下的，整理成Mermaid，对于函数调用链要完整，不可省略其中的步骤，要让我可以完整自顶向下追溯
分析深度渲染和这里的关系，RenderMobileAfterTranslucencyPass中渲染的如果都是不透明物体，这些物体的深度应该会和BasePass中的一起渲染深度吧？
所以我在这里RenderMobileAfterTranslucencyPass是可以读到这个Pass中物体的深度的吧？我也不需要在这里写入深度吧？

---
void FMobileBasePassMeshProcessor::AddMeshBatch(const FMeshBatch &RESTRICT MeshBatch, uint64 BatchElementMask, const FPrimitiveSceneProxy *RESTRICT PrimitiveSceneProxy, int32 StaticMeshId)
{
if (!MeshBatch.bUseForMaterial ||
(Flags & FMobileBasePassMeshProcessor::EFlags::DoNotCache) == FMobileBasePassMeshProcessor::EFlags::DoNotCache ||
(PrimitiveSceneProxy && !PrimitiveSceneProxy->ShouldRenderInMainPass()))
{
return;
}
第一个问题，这里的判断，如果PrimitiveSceneProxy为空，不就直接return了吗？


---

【次要·VR 性能】 Tile-Based GPU 上的 tile memory 反复加载
位置:MobileShadingRenderer.cpp 的两个调用点。

问题:Adreno / Mali / Apple GPU 都是 Tile-Based,理想情况下整帧 color + depth 都保存在 on-chip tile memory。RenderTranslucency 结束后通常会做一次 store(因为后续 post-process 需要解析 color 与 depth)。在 store 之后再插入一个读 depth / 写 color 的 pass,会强制把 depth 从 main memory 重新 load 到 tile,破坏 tile 局部性。在 Adreno 6xx / Mali-G78 等较新 GPU 上,驱动会插入额外的 load/store,带来性能损失。

建议:

在真机上用 Snapdragon Profiler / RenderDoc 抓帧,确认 VR 性能影响是否可接受。
如果性能不可接受,考虑使用 r.Mobile.UseHWsRGBEncoding / r.Mobile.SupportGPUScene 等相关 cvar,或与 Epic 团队的 LateSubpass / OnChipDepthResolve 选项对齐。
长远看,若 Epic 后续把 EMobileSubpassHint 扩展成更细粒度,可考虑把 after-translucency 纳入第三个 subpass。???