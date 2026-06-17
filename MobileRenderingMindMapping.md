0. Runtime/Engine/Private/GameViewportClient.cpp：1369 Draw中:1847中调用GetRendererModule().BeginRenderingViewFamily(SceneCanvas, &ViewFamily);
1. Runtime/Renderer/Private/SceneRendering.cpp:4965,BeginRenderingViewFamily调用BeginRenderingViewFamilies，然后调用5087行FSceneRenderer::CreateSceneRenderers(ViewFamiliesConst, Canvas->GetHitProxyConsumer(), SceneRenderers);
2. Runtime/Renderer/Private/SceneRendering.cpp:4296，CreateSceneRenderers中判断ShadingPath == EShadingPath::Deferred，:4304创建OutSceneRenderers.Add(new FMobileSceneRenderer(InViewFamily, HitProxyConsumer));
3. Runtime/Renderer/Private/SceneRendering.cpp:5113 ENQUEUE_RENDER_COMMAND循环调用:5119，BeginRenderingViewFamilies中调用RenderViewFamilies_RenderThread
4. Runtime/Renderer/Private/SceneRendering.cpp:4829，RenderViewFamilies_RenderThread中调用SceneRenderer->Render(GraphBuilder);
5. Runtime/Renderer/Private/MobileShadingRenderer.cpp:910，Render中:1311判断bDeferredShading，:1317走RenderForward(GraphBuilder, ViewFamilyTexture, SceneTextures, DBufferTextures);
6. Runtime/Renderer/Private/MobileShadingRenderer.cpp:1503中:1567判断bRequiresMultiPass，走:1573RenderForwardSinglePass(GraphBuilder, PassParameters, ViewContext, SceneTextures);
7. Runtime/Renderer/Private/MobileShadingRenderer.cpp:1578中调用:1609RenderMobileBasePass(RHICmdList, View, &PassParameters->InstanceCullingDrawParams);
8. Runtime/Renderer/Private/MobileBasePassRendering.cpp:470 RenderMobileBasePass中478调用View.ParallelMeshDrawCommandPasses[EMeshPass::BasePass].DispatchDraw(nullptr, RHICmdList, InstanceCullingDrawParams);
9. Runtime/Renderer/Private/MeshDrawCommands.cpp :1640 DispatchDraw中:1697调用		
if (TaskContext.bUseGPUScene)
		{
			if (TaskContext.MeshDrawCommands.Num() > 0)
			{
				TaskContext.InstanceCullingContext.SubmitDrawCommands(
					TaskContext.MeshDrawCommands,
					TaskContext.MinimalPipelineStatePassSet,
					OverrideArgs,
					0,
					TaskContext.MeshDrawCommands.Num(),
					TaskContext.InstanceFactor,
					RHICmdList);
			}
		}