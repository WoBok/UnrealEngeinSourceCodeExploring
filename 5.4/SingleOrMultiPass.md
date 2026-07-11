```
void FMobileSceneRenderer::RenderForward(FRDGBuilder& GraphBuilder, FRDGTextureRef ViewFamilyTexture, FSceneTextures& SceneTextures, FDBufferTextures& DBufferTextures)
{
    const FViewInfo& MainView = Views[0];

    GVRSImageManager.PrepareImageBasedVRS(GraphBuilder, ViewFamily, SceneTextures);
    FRDGTextureRef NewShadingRateTarget = GVRSImageManager.GetVariableRateShadingImage(GraphBuilder, MainView, FVariableRateShadingImageManager::EVRSPassType::BasePass);

    FRenderTargetBindingSlots BasePassRenderTargets = InitRenderTargetBindings_Forward(ViewFamilyTexture, SceneTextures);
    BasePassRenderTargets.ShadingRateTexture = (!MainView.bIsSceneCapture && !MainView.bIsReflectionCapture && (NewShadingRateTarget != nullptr)) ? NewShadingRateTarget : nullptr;

    static const auto CVarMobileMultiView = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("vr.MobileMultiView"));
    const bool bIsMultiViewApplication = (CVarMobileMultiView && CVarMobileMultiView->GetValueOnAnyThread() != 0);

    //if the scenecolor isn't multiview but the app is, need to render as a single-view multiview due to shaders
    BasePassRenderTargets.MultiViewCount = MainView.bIsMobileMultiViewEnabled ? 2 : (bIsMultiViewApplication ? 1 : 0);

    const FRDGSystemTextures& SystemTextures = FRDGSystemTextures::Get(GraphBuilder);

    FRenderViewContextArray RenderViews;
    GetRenderViews(Views, RenderViews);

    for (FRenderViewContext& ViewContext : RenderViews)
    {
       FViewInfo& View = *ViewContext.ViewInfo;

       SCOPED_GPU_MASK(GraphBuilder.RHICmdList, !View.IsInstancedStereoPass() ? View.GPUMask : (View.GPUMask | View.GetInstancedView()->GPUMask));
       SCOPED_CONDITIONAL_DRAW_EVENTF(GraphBuilder.RHICmdList, EventView, RenderViews.Num() > 1, TEXT("View%d"), ViewContext.ViewIndex);

       if (!ViewContext.bIsFirstView)
       {
          BasePassRenderTargets[0].SetLoadAction(ERenderTargetLoadAction::ELoad);
          if (bRequiresSceneDepthAux)
          {
             BasePassRenderTargets[1].SetLoadAction(ERenderTargetLoadAction::ELoad);
          }
          BasePassRenderTargets.DepthStencil.SetDepthLoadAction(ERenderTargetLoadAction::ELoad);
          BasePassRenderTargets.DepthStencil.SetStencilLoadAction(ERenderTargetLoadAction::ELoad);
          BasePassRenderTargets.DepthStencil.SetDepthStencilAccess(bIsFullDepthPrepassEnabled ? FExclusiveDepthStencil::DepthRead_StencilWrite : FExclusiveDepthStencil::DepthWrite_StencilWrite);
       }

       View.BeginRenderView();

       UpdateDirectionalLightUniformBuffers(GraphBuilder, View);

       FMobileBasePassTextures MobileBasePassTextures{};
       MobileBasePassTextures.ScreenSpaceAO = bRequiresAmbientOcclusionPass ? SceneTextures.ScreenSpaceAO : SystemTextures.White;
       MobileBasePassTextures.DBufferTextures = DBufferTextures;

       EMobileSceneTextureSetupMode SetupMode = (bIsFullDepthPrepassEnabled ? EMobileSceneTextureSetupMode::SceneDepth : EMobileSceneTextureSetupMode::None) | EMobileSceneTextureSetupMode::CustomDepth;
       FMobileRenderPassParameters* PassParameters = GraphBuilder.AllocParameters<FMobileRenderPassParameters>();
       PassParameters->View = View.GetShaderParameters();
       PassParameters->MobileBasePass = CreateMobileBasePassUniformBuffer(GraphBuilder, View, EMobileBasePass::Opaque, SetupMode, MobileBasePassTextures);
       PassParameters->ReflectionCapture = View.MobileReflectionCaptureUniformBuffer;
       PassParameters->RenderTargets = BasePassRenderTargets;
       PassParameters->LocalFogVolumeInstances = View.LocalFogVolumeViewData.GPUInstanceDataBufferSRV;
       PassParameters->LocalFogVolumeTileDrawIndirectBuffer = View.LocalFogVolumeViewData.GPUTileDrawIndirectBuffer;
       PassParameters->LocalFogVolumeTileDataTexture = View.LocalFogVolumeViewData.TileDataTextureArraySRV;
       PassParameters->LocalFogVolumeTileDataBuffer = View.LocalFogVolumeViewData.GPUTileDataBufferSRV;
       PassParameters->HalfResLocalFogVolumeViewSRV = View.LocalFogVolumeViewData.HalfResLocalFogVolumeViewSRV;
       PassParameters->HalfResLocalFogVolumeDepthSRV = View.LocalFogVolumeViewData.HalfResLocalFogVolumeDepthSRV;
    
       BuildInstanceCullingDrawParams(GraphBuilder, View, PassParameters);

       // Split if we need to render translucency in a separate render pass
       if (bRequiresMultiPass)
       {
          RenderForwardMultiPass(GraphBuilder, PassParameters, ViewContext, SceneTextures);
       }
       else
       {
          RenderForwardSinglePass(GraphBuilder, PassParameters, ViewContext, SceneTextures);
       }
    }
}

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
       
#if UE_ENABLE_DEBUG_DRAWING
       if ((!IsMobileHDR() || bTonemapSubpass) && FSceneRenderer::ShouldCompositeDebugPrimitivesInPostProcess(View))
       {
          // Draw debug primitives after translucency for LDR as we do not have a post processing pass
          RenderMobileDebugPrimitives(RHICmdList, View);
       }
#endif

       if (bDoOcclusionQueries)
       {
          // Issue occlusion queries
          RHICmdList.SetCurrentStat(GET_STATID(STAT_CLMM_Occlusion));
          const bool bAdrenoOcclusionMode = (CVarMobileAdrenoOcclusionMode.GetValueOnRenderThread() != 0 && IsOpenGLPlatform(ShaderPlatform));
          if (bAdrenoOcclusionMode)
          {
             // flush
             RHICmdList.SubmitCommandsHint();
          }
          RenderOcclusion(RHICmdList);
       }

       // Pre-tonemap before MSAA resolve (iOS only)
       PreTonemapMSAA(RHICmdList, SceneTextures);
       if (bTonemapSubpassInline)
       {
          RHICmdList.NextSubpass();
          RenderMobileCustomResolve(RHICmdList, View, NumMSAASamples, SceneTextures);
       }
    });
    
    // resolve MSAA depth
    if (!bIsFullDepthPrepassEnabled)
    {
       AddResolveSceneDepthPass(GraphBuilder, *ViewContext.ViewInfo, SceneTextures.Depth);
    }
}

void FMobileSceneRenderer::RenderForwardMultiPass(FRDGBuilder& GraphBuilder, FMobileRenderPassParameters* PassParameters, FRenderViewContext& ViewContext, FSceneTextures& SceneTextures)
{
    GraphBuilder.AddPass(
       RDG_EVENT_NAME("SceneColorRendering"),
       PassParameters,
       ERDGPassFlags::Raster,
       [this, PassParameters, ViewContext, &SceneTextures](FRHICommandList& RHICmdList)
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
    });

    FViewInfo& View = *ViewContext.ViewInfo;

    // resolve MSAA depth
    if (!bIsFullDepthPrepassEnabled)
    {
       AddResolveSceneDepthPass(GraphBuilder, View, SceneTextures.Depth);
    }
    if (bRequiresSceneDepthAux)
    {
       AddResolveSceneColorPass(GraphBuilder, View, SceneTextures.DepthAux);
    }

    FExclusiveDepthStencil::Type ExclusiveDepthStencil = FExclusiveDepthStencil::DepthRead_StencilRead;
    if (bModulatedShadowsInUse)
    {
       // FIXME: modulated shadows write to stencil
       ExclusiveDepthStencil = FExclusiveDepthStencil::DepthRead_StencilWrite;
    }

    EMobileSceneTextureSetupMode SetupMode = EMobileSceneTextureSetupMode::SceneDepth | EMobileSceneTextureSetupMode::SceneDepthAux | EMobileSceneTextureSetupMode::CustomDepth;
    FMobileRenderPassParameters* SecondPassParameters = GraphBuilder.AllocParameters<FMobileRenderPassParameters>();
    *SecondPassParameters = *PassParameters;
    SecondPassParameters->MobileBasePass = CreateMobileBasePassUniformBuffer(GraphBuilder, View, EMobileBasePass::Translucent, SetupMode);
    SecondPassParameters->ReflectionCapture = View.MobileReflectionCaptureUniformBuffer;
    SecondPassParameters->RenderTargets[0].SetLoadAction(ERenderTargetLoadAction::ELoad);
    SecondPassParameters->RenderTargets[1] = FRenderTargetBinding();
    SecondPassParameters->RenderTargets.DepthStencil.SetDepthLoadAction(ERenderTargetLoadAction::ELoad);
    SecondPassParameters->RenderTargets.DepthStencil.SetStencilLoadAction(ERenderTargetLoadAction::ELoad);
    SecondPassParameters->RenderTargets.DepthStencil.SetDepthStencilAccess(ExclusiveDepthStencil);
    
    const bool bDoOcclusionQueries = (!bIsFullDepthPrepassEnabled && ViewContext.bIsLastView && DoOcclusionQueries());
    SecondPassParameters->RenderTargets.NumOcclusionQueries = bDoOcclusionQueries ? ComputeNumOcclusionQueriesToBatch() : 0u;

    GraphBuilder.AddPass(
       RDG_EVENT_NAME("DecalsAndTranslucency"),
       SecondPassParameters,
       ERDGPassFlags::Raster,
       [this, SecondPassParameters, ViewContext, bDoOcclusionQueries, &SceneTextures](FRHICommandList& RHICmdList)
    {
       FViewInfo& View = *ViewContext.ViewInfo;
          
       // scene depth is read only and can be fetched
       RHICmdList.SetCurrentStat(GET_STATID(STAT_CLMM_Translucency));
       RenderDecals(RHICmdList, View, &SecondPassParameters->InstanceCullingDrawParams);
       RenderModulatedShadowProjections(RHICmdList, ViewContext.ViewIndex, View);
       RenderFog(RHICmdList, View);
       // Draw translucency.
       RenderTranslucency(RHICmdList, View);

       if (bDoOcclusionQueries)
       {
          // Issue occlusion queries
          RHICmdList.SetCurrentStat(GET_STATID(STAT_CLMM_Occlusion));
          RenderOcclusion(RHICmdList);
       }

       // Pre-tonemap before MSAA resolve (iOS only)
       PreTonemapMSAA(RHICmdList, SceneTextures);
    });

    AddResolveSceneColorPass(GraphBuilder, View, SceneTextures.Color);
}
```

这段代码是虚幻引擎（Unreal Engine）基于RDG（渲染依赖图）的移动端前向渲染管线（Mobile Forward Renderer）的核心调度逻辑。其核心在于根据硬件支持和渲染特性，将场景划分为 **单通道（Single Pass）** 或 **多通道（Multi Pass）** 进行渲染。

以下是对这两个核心函数的深度分析：

### 1. RenderForwardSinglePass (单通道渲染)

**核心思想：极致利用移动端 GPU 的 TBDR（基于图块的延迟渲染）架构特性。**

* **运行机制**：将不透明物体（Opaque）、半透明物体（Translucency）、贴花（Decals）、甚至色调映射（Tonemapping）全部打包进一个巨大的 RDG Pass (`SceneColorRendering`) 中。
* **关键代码**：`RHICmdList.NextSubpass();`
* 这是该函数的最核心标志。它利用了 Vulkan / Metal 的 Subpass 特性。
* 在绘制完不透明物体后，调用 `NextSubpass()`，将深度缓冲区（Depth Buffer）转换为只读状态。


* **性能优势**：**零显存带宽开销**。场景的颜色和深度数据始终保留在 GPU 的极速片上内存（On-chip Memory / Tile Memory）中，没有发生与系统显存（VRAM）的来回读写（Store/Load），极大地节省了移动端的功耗和发热。

### 2. RenderForwardMultiPass (多通道渲染)

**核心思想：为了兼容性或特殊渲染需求，进行传统的分步渲染。**

* **运行机制**：将渲染强行拆分为两个独立的 RDG Pass：
1. `SceneColorRendering`：只渲染不透明物体和深度预计算。
2. `DecalsAndTranslucency`：渲染贴花、雾效和半透明物体。


* **关键代码**：在两个 Pass 之间调用了 `AddResolveSceneDepthPass` 和 `AddResolveSceneColorPass`。
* 第二个 Pass 的参数被显式设置为 `ERenderTargetLoadAction::ELoad`。


* **性能劣势**：**高带宽开销**。第一个 Pass 结束后，必须将图块内存中的颜色和深度数据写入（Resolve/Store）到主显存；第二个 Pass 开始时，又必须将这些数据从主显存重新读取（Load）到芯片上。
* **触发条件**：当开启了某些无法在 Subpass 中完成的特性（如需要读取全屏幕深度进行复杂计算），或者硬件/图形 API 不支持 Subpass 时，管线会退化为这种模式。

---

### 核心差异总结 (The "Why")

两者的本质区别在于**对 GPU 片上内存（Tile Memory）的管理方式**。Single Pass 让数据始终驻留在片上，性能极佳；Multi Pass 则在中间打断了这一过程，强行进行了一次昂贵的显存 I/O 操作以保证渲染逻辑的正确性或兼容性。

> **💡 记忆总结：**
> 单通道利用 Subpass 锁死片上内存实现性能狂飙，多通道被迫切断流程做显存搬运以保全兼容。



在虚幻引擎的移动端渲染管线中，将渲染逻辑拆分为 **Single Pass** 和 **Multi Pass**，本质上是对**移动端特殊的硬件架构**在“极致性能”与“功能兼容”之间做出的双轨制妥协。

以下是必须分开处理的核心原因：

### 1. 移动端独特的硬件生命线：TBDR 架构

桌面端和移动端的 GPU 架构截然不同，移动端采用的是 **TBDR（基于图块的延迟渲染）** 架构，它有两个核心的存储层级：

* **片上内存（On-chip Memory / Tile Memory）：** 镶嵌在 GPU 内部，容量极小，但速度极快、功耗极低。
* **系统显存（System RAM）：** 容量大，但速度慢。GPU 与显存之间的数据搬运（Load/Store）是移动端**发热、掉电卡顿的万恶之源**。

移动端渲染的最高指导原则就是：**尽可能把所有计算都在“片上内存”里做完，避免与“系统显存”通信。**

### 2. Single Pass（单通道）：性能的理想国

当场景的光影需求比较常规时，引擎会走 `RenderForwardSinglePass`。

* **运作方式：** 借助现代图形 API（如 Vulkan, Metal）的 **Subpass（子通道）** 技术，引擎将不透明物体、半透明、贴花甚至后处理打包成一个任务。GPU 针对屏幕上的每一个小图块（Tile），在片上内存里一口气算完所有步骤，最后只把最终的颜色输出到显存。
* **为什么快：** 彻底砍掉了中间步骤的显存读写，极大地节省了带宽，发热降到最低，性能拉满。

### 3. Multi Pass（多通道）：现实的妥协线

单通道虽快，但它有一个致命弱点：**无法全局回读数据**。当你的游戏开启了以下特性或遇到以下环境时，单通道就会破产，必须走 `RenderForwardMultiPass`：

* **复杂的渲染特性：** 如果场景中需要高级的软粒子、复杂的半透明折射、或者特殊的屏幕空间特效（这些通常需要读取当前屏幕**完整**的深度缓冲或颜色缓冲）。单通道下，其他图块还在计算中，拿不到全局数据；此时必须强制打断（End Pass），把第一阶段的结果写回主显存（Resolve），再开一个新的 Pass 把它读取进来继续算。
* **API 兼容性灾难：** 很多老旧设备或低版本的 OpenGL ES 根本不支持 Subpass 这种高级特性，引擎必须提供一条传统的、分步渲染的后备路径。

---

💡 **一句话总结：分开处理是为了在支持复杂渲染特性或兼容老旧设备的同时（多通道保底），依然能让现代设备榨干片上内存的极限性能（单通道冲刺）。**