  方案概述
     
  这个方案的核心目标是：在透明物体渲染完成后，渲染标记的不透明物体，从而实现"遮挡透明物体
  "的效果。

  深度分析
  
  ✅  正确的部分

  1. 整体架构合理：通过添加新的 MobileAfterTranslucencyPass 来实现延迟渲染特定物体
  2. 数据流完整：从 Component → SceneProxy → ViewRelevance → MeshProcessor
  的数据传递链路完整
  3. Pass 注册正确：正确使用了 REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR 宏
  4. 分流逻辑清晰：在 AddMeshBatch 和 SceneVisibility.cpp 中正确实现了物体分流

  ⚠️ 潜在问题和需要修正的地方

  1. 关键错误：深度状态配置不当

  在 CreateMobileAfterTranslucencyPassProcessor 中：

  PassDrawRenderState.SetDepthStencilState(TStaticDepthStencilState<false,
  CF_DepthNearOrEqual>::GetRHI());

  问题：第一个参数 false
  表示不写入深度，但您的需求是"沿用不透明物体渲染深度测试等逻辑"，应该写入深度。

  正确配置：
  PassDrawRenderState.SetDepthStencilState(TStaticDepthStencilState<true,
  CF_DepthNearOrEqual>::GetRHI());

  理由：
  - 您希望这些物体表现得像不透明物体
  - 需要写入深度以便后续渲染正确遮挡
  - 使用 CF_DepthNearOrEqual 进行深度测试是正确的

  2. BlendState 注释掉可能导致问题

  //PassDrawRenderState.SetBlendState(TStaticBlendStateWriteMask<CW_RGBA>::GetRHI());//是
  否还需要？

  建议保留：
  - 不透明物体应该写入所有颜色通道
  - 注释掉可能导致使用默认混合状态，可能不符合预期
  - 应该取消注释

  3. CSM（Cascaded Shadow Maps）支持缺失

  在 CreateMobileAfterTranslucencyPassProcessor 中：

  const FMobileBasePassMeshProcessor::EFlags Flags =
  FMobileBasePassMeshProcessor::EFlags::CanUseDepthStencil;

  问题：如果场景使用 CSM，这些延迟渲染的物体可能无法正确接收阴影。

  建议修改：
  const FMobileBasePassMeshProcessor::EFlags Flags =
  FMobileBasePassMeshProcessor::EFlags::CanUseDepthStencil
      | (MobileBasePassAlwaysUsesCSM(GShaderPlatformForFeatureLevel[FeatureLevel]) ?
  FMobileBasePassMeshProcessor::EFlags::CanReceiveCSM :
  FMobileBasePassMeshProcessor::EFlags::None);

  4. 构造函数参数传递错误

  在 CreateMobileAfterTranslucencyPassProcessor 的最后一行：

  return new FMobileBasePassMeshProcessor(EMeshPass::MobileAfterTranslucencyPass, Scene,
  InViewIfDynamicMeshCommand, PassDrawRenderState, InDrawListContext, Flags, true);

  问题：最后一个参数应该是 bool IsAfterTranslucencyBasePass，但传递的是
  true，而构造函数声明中参数顺序是：

  FMobileBasePassMeshProcessor(..., ETranslucencyPass::Type InTranslucencyPassType =
  ETranslucencyPass::TPT_MAX, bool bAfterTranslucencyBasePass = false);

  正确调用：
  return new FMobileBasePassMeshProcessor(EMeshPass::MobileAfterTranslucencyPass, Scene,
  InViewIfDynamicMeshCommand, PassDrawRenderState, InDrawListContext, Flags,
  ETranslucencyPass::TPT_MAX, true);

  5. SceneVisibility.cpp 中的 CSM Pass 逻辑问题

  在静态网格处理中：

  if(ViewRelevance.bRenderAfterTranslucency)
  {
      DrawCommandPacket.AddCommandsForMesh(..., EMeshPass::MobileAfterTranslucencyPass);
  }else
  {
      DrawCommandPacket.AddCommandsForMesh(..., EMeshPass::BasePass);
  }
  if (!bMobileBasePassAlwaysUsesCSM)
  {
      DrawCommandPacket.AddCommandsForMesh(..., EMeshPass::MobileBasePassCSM);
  }

  问题：bRenderAfterTranslucency 的物体也可能需要 CSM Pass，但当前逻辑只处理了 BasePass
  的情况。

  建议修改：
  if(ViewRelevance.bRenderAfterTranslucency)
  {
      DrawCommandPacket.AddCommandsForMesh(..., EMeshPass::MobileAfterTranslucencyPass);
  }else
  {
      DrawCommandPacket.AddCommandsForMesh(..., EMeshPass::BasePass);
      if (!bMobileBasePassAlwaysUsesCSM)
      {
          DrawCommandPacket.AddCommandsForMesh(..., EMeshPass::MobileBasePassCSM);
      }
  }

  或者如果 AfterTranslucency 物体也需要 CSM：
  if(ViewRelevance.bRenderAfterTranslucency)
  {
      DrawCommandPacket.AddCommandsForMesh(..., EMeshPass::MobileAfterTranslucencyPass);
  }else
  {
      DrawCommandPacket.AddCommandsForMesh(..., EMeshPass::BasePass);
  }
  if (!bMobileBasePassAlwaysUsesCSM)
  {
      DrawCommandPacket.AddCommandsForMesh(..., EMeshPass::MobileBasePassCSM);
  }

  6. GPU Stat 声明缺失

  在 BasePassRendering.h 中声明了：
  DECLARE_GPU_DRAWCALL_STAT_EXTERN(AfterTranslucency);

  但需要在对应的 .cpp 文件中定义：
  DEFINE_GPU_DRAWCALL_STAT(AfterTranslucency);

  建议：在 MobileBasePassRendering.cpp 或 BasePassRendering.cpp 中添加定义。

  7. EMeshPass::Num 更新不完整

  在 MeshPassProcessor.h 中更新了断言：
  static_assert(EMeshPass::Num == 33 + 4, ...);
  #else
  static_assert(EMeshPass::Num == 33, ...);

  问题：添加了 MobileAfterTranslucencyPass 后，非编辑器版本的数量应该是 34，不是 33。

  正确修改：
  #if WITH_EDITOR
  static_assert(EMeshPass::Num == 34 + 4, ...);
  #else
  static_assert(EMeshPass::Num == 34, ...);
  #endif

  🤔 需要考虑的边缘情况

  1. 材质兼容性：某些特殊材质（如天空材质、水材质）可能需要特殊处理
  2. 后处理影响：DOF、Motion Blur 等后处理是否会影响这些延迟渲染的物体
  3. 性能影响：额外的 Draw Call 和状态切换开销
  4. 动态阴影：这些物体是否需要投射和接收动态阴影
  5. Instance Culling：确保 GPU Scene 和 Instance Culling 正确处理新的 Pass
  6. Mobile Deferred Shading：如果启用了移动端延迟渲染，是否需要额外处理

  📋 缺失的功能

  1. Editor 支持：没有考虑编辑器内的预览和调试
  2. Console Variables：建议添加 CVar 来控制该功能的启用/禁用
  3. 材质域限制：可能需要在材质编译时检查兼容性
  4. Nanite 支持：如果使用 Nanite，需要额外处理

  修正建议总结

  // 在 CreateMobileAfterTranslucencyPassProcessor 中：
  FMeshPassProcessorRenderState PassDrawRenderState;
  PassDrawRenderState.SetBlendState(TStaticBlendStateWriteMask<CW_RGBA>::GetRHI()); //
  取消注释
  PassDrawRenderState.SetDepthStencilAccess(FExclusiveDepthStencil::DepthWrite_StencilRea
  d); // 改为写入深度
  PassDrawRenderState.SetDepthStencilState(TStaticDepthStencilState<true,
  CF_DepthNearOrEqual>::GetRHI()); // 第一个参数改为 true

  const FMobileBasePassMeshProcessor::EFlags Flags =
  FMobileBasePassMeshProcessor::EFlags::CanUseDepthStencil
      | (MobileBasePassAlwaysUsesCSM(GShaderPlatformForFeatureLevel[FeatureLevel]) ?
  FMobileBasePassMeshProcessor::EFlags::CanReceiveCSM :
  FMobileBasePassMeshProcessor::EFlags::None);

  return new FMobileBasePassMeshProcessor(EMeshPass::MobileAfterTranslucencyPass, Scene,
  InViewIfDynamicMeshCommand, PassDrawRenderState, InDrawListContext, Flags,
  ETranslucencyPass::TPT_MAX, true);

  这个方案整体设计合理，主要问题集中在渲染状态配置上。修正这些问题后，应该能够实现您期望
  的"透明物体后渲染不透明物体以遮挡透明物体"的效果。