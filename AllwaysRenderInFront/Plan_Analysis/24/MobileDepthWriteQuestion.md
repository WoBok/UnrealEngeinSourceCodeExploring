我现在使用UE移动端，安卓端的Forward渲染路径
Engine/Source/Runtime/Renderer/Private/MobileBasePass.cpp:1151 CreateMobileBasePassProcessor中
const FExclusiveDepthStencil::Type DefaultBasePassDepthStencilAccess = FScene::GetDefaultBasePassDepthStencilAccess(FeatureLevel);
PassDrawRenderState.SetDepthStencilAccess(DefaultBasePassDepthStencilAccess);
根据
FExclusiveDepthStencil::Type FScene::GetDefaultBasePassDepthStencilAccess(ERHIFeatureLevel::Type InFeatureLevel)
{
FExclusiveDepthStencil::Type BasePassDepthStencilAccess = FExclusiveDepthStencil::DepthWrite_StencilWrite;

	if (GetFeatureLevelShadingPath(InFeatureLevel) == EShadingPath::Deferred)
	{
		const EShaderPlatform ShaderPlatform = GetFeatureLevelShaderPlatform(InFeatureLevel);
		if (ShouldForceFullDepthPass(ShaderPlatform)
			&& CVarBasePassWriteDepthEvenWithFullPrepass.GetValueOnAnyThread() == 0)
		{
			BasePassDepthStencilAccess = FExclusiveDepthStencil::DepthRead_StencilWrite;
		}
	}

	return BasePassDepthStencilAccess;
}
这里来判断是开启DepthWrite_StencilWrite还是DepthRead_StencilWrite，如果开了EarlyZPass就DepthRead_StencilWrite
正常情况下并没有开启EarlyZPass，所以深度会和BasePass一起写入，验证移动端Forward渲染路径下只会和MobileBasePass一起渲染对吧？
Engine/Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp:1578 RenderForwardSinglePass中
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
RenderMobileBasePass(RHICmdList, View, &PassParameters->InstanceCullingDrawParams);     
这里RenderMobileBasePass(RHICmdList, View, &PassParameters->InstanceCullingDrawParams);会渲染MobileBasePass的深度
我现在需要你找到移动端ForwardPass中这里是怎么写入深度的？写入深度的实际操作代码在哪里？调用路径，调用链路是什么？
