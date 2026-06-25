以下问题的验证均严格按照UE5.4移动安卓端Forward路径进行
Engine/Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp:1578RenderForwardSinglePass中的相关线索代码
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
给我找到相关问题的答案，要有引擎中代码的验证，代码片段要给我在什么文件第几行中
1. Translucency深度测试的方式什么？LessEqual吗？
2. Translucency Sort Priority原理？
3. 怎么修改深度测试的方式的？解释下方的enum，在移动渲染Forward渲染管线中哪里用到了这些地方
   enum ECompareFunction
   {
   CF_Less,
   CF_LessEqual,
   CF_Greater,
   CF_GreaterEqual,
   CF_Equal,
   CF_NotEqual,
   CF_Never,
   CF_Always,

   ECompareFunction_Num,
   ECompareFunction_NumBits = 3,

   // Utility enumerations
   CF_DepthNearOrEqual		= (((int32)ERHIZBuffer::IsInverted != 0) ? CF_GreaterEqual : CF_LessEqual),
   CF_DepthNear			= (((int32)ERHIZBuffer::IsInverted != 0) ? CF_Greater : CF_Less),
   CF_DepthFartherOrEqual	= (((int32)ERHIZBuffer::IsInverted != 0) ? CF_LessEqual : CF_GreaterEqual),
   CF_DepthFarther			= (((int32)ERHIZBuffer::IsInverted != 0) ? CF_Less : CF_Greater),
   };
4. RHICmdList.NextSubpass();这里SubPass在初始化的时候关闭了深度写入
   FMeshPassProcessor* CreateMobileBasePassProcessor(ERHIFeatureLevel::Type FeatureLevel, const FScene* Scene, const FSceneView* InViewIfDynamicMeshCommand, FMeshPassDrawListContext* InDrawListContext)
   {
   FMeshPassProcessorRenderState PassDrawRenderState;
   PassDrawRenderState.SetBlendState(TStaticBlendStateWriteMask<CW_RGBA>::GetRHI());
   const FExclusiveDepthStencil::Type DefaultBasePassDepthStencilAccess = FScene::GetDefaultBasePassDepthStencilAccess(FeatureLevel);
   PassDrawRenderState.SetDepthStencilAccess(DefaultBasePassDepthStencilAccess);
   PassDrawRenderState.SetDepthStencilState(TStaticDepthStencilState<true, CF_DepthNearOrEqual>::GetRHI());

   const FMobileBasePassMeshProcessor::EFlags Flags = FMobileBasePassMeshProcessor::EFlags::CanUseDepthStencil
   | (MobileBasePassAlwaysUsesCSM(GShaderPlatformForFeatureLevel[FeatureLevel]) ? FMobileBasePassMeshProcessor::EFlags::CanReceiveCSM : FMobileBasePassMeshProcessor::EFlags::None);

   return new FMobileBasePassMeshProcessor(EMeshPass::BasePass, Scene, InViewIfDynamicMeshCommand, PassDrawRenderState, InDrawListContext, Flags);
   }
   这是MobileBasePassProcessor初始化的时候相关设置，PassDrawRenderState.SetDepthStencilState(TStaticDepthStencilState<true, CF_DepthNearOrEqual>::GetRHI());这里开启了深度写入，
   PassDrawRenderState.SetBlendState(TStaticBlendStateWriteMask<CW_RGBA>::GetRHI());这里开启写入颜色对吧？怎么将这里状态设置为不写入颜色？我只想开启深度写入，如果这里关闭了颜色写入，模型在渲染的时候还会调用颜色渲染的Vertex Shader和
   Fragment Shader吗？还是只会走深度渲染的相关Shader？

将分析放到Engine\Docs中，md格式，以引用的格式把我的问题放在文档开头

以下问题均需要找到验证代码
1. PassDrawRenderState.SetDepthStencilState(TStaticDepthStencilState<false, CF_DepthNearOrEqual>::GetRHI());这句代码中会设置深度测试模式为CF_DepthNearOrEqual吗？
2. 修改半透明深度测试主要改 Mobile translucency mesh pass processor 的 TStaticDepthStencilState<false, CF_DepthNearOrEqual> 中第二个模板参数；如果材质勾选 Disable Depth Test，会被改成 CF_Always。如果材质勾选 Disable Depth Test，会被改成 CF_Always。是在哪里设置的，找到代码位置
3. TStaticBlendStateWriteMask<CW_RGBA> 是开启 RT0 的 RGBA 颜色写入；改成 TStaticBlendStateWriteMask<CW_NONE> 或 TStaticBlendState<CW_NONE> 可以关闭颜色写入。但如果仍走 BasePass，仍会构建并绑定 Mobile BasePass 的 VS/PS，颜色写入只是被 blend/color-write-mask 屏蔽；不会自动切换成 DepthPass 的 depth-only shader。我想只在BassPass中写入深度，但不绑定颜色的VS/PS，可以吗？移动端Forward BasePass中写入深度具体怎么完成的，在不开启EarlyZPass的模式下，相关的深度写入Shader在哪？TStaticBlendState<CW_NONE> 可以关闭颜色写入。但如果仍走 BasePass，仍会构建并绑定 Mobile BasePass 的 VS/PS包含在你说的这个VS/PS当中吗？给我找到验证代码
4. 