UE5.4 移动端延迟渲染方案分析

  需求概述
     
  在 Android VR 移动端 Forward 渲染路径中，实现标记物体在透明物体之后渲染，从而遮挡透明物
  体（利用透明物体不写入深度的特性）。

  ---
  方案整体评估

  ✅  核心逻辑正确

  方案的整体思路合理：
  - 新增 EMeshPass::MobileAfterTranslucencyPass 枚举
  - 通过 bRenderAfterTranslucency 标志进行物体标记
  - 在透明物体渲染后调用新的 Pass
  - 复用不透明物体的渲染逻辑和深度测试

  ---
  ⚠️ 发现的问题与建议

  1. 枚举位置冲突（严重）

  问题：
  // MeshPassProcessor.h:32
  WaterInfoTexturePass,
  MobileAfterTranslucencyPass,  // ← 新增位置

  #if WITH_EDITOR
  HitProxy,

  MobileAfterTranslucencyPass 放在条件编译 #if WITH_EDITOR 之前，会导致：
  - Editor 模式下 EMeshPass::Num 值不一致
  - 断言检查会失败

  修复建议：
  WaterInfoTexturePass,

  #if WITH_EDITOR
  HitProxy,
  HitProxyOpaqueOnly,
  EditorLevelInstance,
  EditorSelection,
  #endif

  MobileAfterTranslucencyPass,  // ← 移到这里

  Num,
  NumBits = 6,

  或者保持当前位置，但需要修改断言逻辑：
  #if WITH_EDITOR
  static_assert(EMeshPass::Num == 34 + 4, "...");  // 33→34
  #else
  static_assert(EMeshPass::Num == 34, "...");      // 33→34
  #endif

  ---
  2. CSM Pass 处理缺失（中等）

  问题：
  在 SceneVisibility.cpp:1564 的修改中：
  if(ViewRelevance.bRenderAfterTranslucency) {
      DrawCommandPacket.AddCommandsForMesh(..., EMeshPass::MobileAfterTranslucencyPass);
  } else {
      DrawCommandPacket.AddCommandsForMesh(..., EMeshPass::BasePass);
  }
  if (!bMobileBasePassAlwaysUsesCSM) {
      DrawCommandPacket.AddCommandsForMesh(..., EMeshPass::MobileBasePassCSM);  // ← 问题
  }

  MobileBasePassCSM 逻辑仍然执行，但标记为 bRenderAfterTranslucency 的物体已经被排除在
  BasePass 之外。

  修复建议：
  if(ViewRelevance.bRenderAfterTranslucency) {
      DrawCommandPacket.AddCommandsForMesh(..., EMeshPass::MobileAfterTranslucencyPass);
  } else {
      DrawCommandPacket.AddCommandsForMesh(..., EMeshPass::BasePass);
      if (!bMobileBasePassAlwaysUsesCSM) {
          DrawCommandPacket.AddCommandsForMesh(..., EMeshPass::MobileBasePassCSM);
      }
  }

  影响评估：
  - 如果 bRenderAfterTranslucency 物体不需要 CSM 阴影，当前方案可行
  - 如果需要接收 CSM 阴影，需要额外创建 MobileAfterTranslucencyPassCSM 或在新 Pass 中处理

  ---
  3. 深度模板状态疑问（需确认）

  问题：
  // CreateMobileAfterTranslucencyPassProcessor
  PassDrawRenderState.SetDepthStencilState(
      TStaticDepthStencilState<false, CF_DepthNearOrEqual>::GetRHI()
  );  // ← 第一个参数为 false（不写入深度）

  代码注释问："这里使用 false 就可以了吧？"

  分析：
  - false = 不写入深度缓冲
  - true = 写入深度缓冲

  根据需求"沿用不透明物体渲染深度测试等逻辑"：
  - 深度测试：CF_DepthNearOrEqual ✅  正确
  - 深度写入：取决于需求
    - 如果标记物体之间需要正确遮挡 → 应该写入深度（true）
    - 如果只需要被透明物体遮挡 → 不写入也可以（false）

  建议：
  改为 true 以保持与不透明物体一致的行为：
  PassDrawRenderState.SetDepthStencilState(
      TStaticDepthStencilState<true, CF_DepthNearOrEqual>::GetRHI()
  );

  ---
  4. 混合状态注释疑问（轻微）

  问题：
  // CreateMobileAfterTranslucencyPassProcessor
  //PassDrawRenderState.SetBlendState(TStaticBlendStateWriteMask<CW_RGBA>::GetRHI()); //
  ← 注释掉

  代码注释问："是否还需要？"

  分析：
  - 不透明物体需要写入所有颜色通道
  - 注释掉会使用默认混合状态（通常是 Opaque）

  建议：
  保持与 CreateMobileBasePassProcessor 一致，取消注释：
  PassDrawRenderState.SetBlendState(TStaticBlendStateWriteMask<CW_RGBA>::GetRHI());

  ---
  5. Flags 配置简化（轻微）

  问题：
  const FMobileBasePassMeshProcessor::EFlags Flags =
      FMobileBasePassMeshProcessor::EFlags::CanUseDepthStencil;  // ← 简化版

  代码注释问："这里直接赋值 CanUseDepthStencil 就可以了吧？"

  分析：
  原始 CreateMobileBasePassProcessor 包含：
  Flags = CanUseDepthStencil
        | (MobileBasePassAlwaysUsesCSM(...) ? CanReceiveCSM : None);

  如果标记物体不需要接收 CSM，当前简化版可行。但为了与 BasePass
  行为一致，建议保留完整逻辑：
  const FMobileBasePassMeshProcessor::EFlags Flags =
      FMobileBasePassMeshProcessor::EFlags::CanUseDepthStencil
      | (MobileBasePassAlwaysUsesCSM(GShaderPlatformForFeatureLevel[FeatureLevel])
          ? FMobileBasePassMeshProcessor::EFlags::CanReceiveCSM
          : FMobileBasePassMeshProcessor::EFlags::None);

  ---
  6. 构造函数参数传递错误（严重）

  问题：
  // CreateMobileAfterTranslucencyPassProcessor
  return new FMobileBasePassMeshProcessor(
      EMeshPass::MobileAfterTranslucencyPass,
      Scene,
      InViewIfDynamicMeshCommand,
      PassDrawRenderState,
      InDrawListContext,
      Flags,
      true  // ← 这里传递 bool，但期望的是 ETranslucencyPass::Type
  );

  修复：
  构造函数签名：
  FMobileBasePassMeshProcessor(
      EMeshPass::Type,
      const FScene*,
      const FSceneView*,
      const FMeshPassProcessorRenderState&,
      FMeshPassDrawListContext*,
      EFlags,
      ETranslucencyPass::Type = ETranslucencyPass::TPT_MAX,  // ← 第7个参数
      bool bAfterTranslucencyBasePass = false                // ← 第8个参数
  );

  正确调用：
  return new FMobileBasePassMeshProcessor(
      EMeshPass::MobileAfterTranslucencyPass,
      Scene,
      InViewIfDynamicMeshCommand,
      PassDrawRenderState,
      InDrawListContext,
      Flags,
      ETranslucencyPass::TPT_MAX,  // ← 添加这个参数
      true  // ← bAfterTranslucencyBasePass
  );

  ---
  7. 统计数据声明位置（轻微）

  问题：
  // BasePassRendering.h:144
  DECLARE_GPU_DRAWCALL_STAT_EXTERN(Basepass);
  DECLARE_GPU_DRAWCALL_STAT_EXTERN(AfterTranslucency);

  DECLARE_GPU_DRAWCALL_STAT_EXTERN 通常不在 BasePassRendering.h 中声明移动端专用统计。

  建议：
  检查是否应该在 MobileBasePassRendering.h 或 MobileBasePassRendering.cpp 中声明：
  // MobileBasePassRendering.cpp
  DEFINE_GPU_DRAWCALL_STAT(AfterTranslucency);

  ---
  8. 动态Mesh处理完整性（中等）

  问题：
  SceneVisibility.cpp:2211 只处理了 PassMask 和
  NumVisibleDynamicMeshElements，但需要确认后续流程是否完整。

  需要检查：
  - FMobileBasePassMeshProcessor::AddMeshBatch 中的分流逻辑是否覆盖动态Mesh
  - 动态Mesh是否需要在其他地方（如剔除、排序）进行额外处理

  当前代码已覆盖：
  - ✅  Static Mesh (SceneVisibility.cpp:1564)
  - ✅  Dynamic Mesh (SceneVisibility.cpp:2211)
  - ✅  AddMeshBatch 分流逻辑 (MobileBasePass.cpp:867)

  ---
  9. 注册宏标志疑问（需确认）

  问题：
  REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR(
      MobileAfterTranslucencyPass,
      CreateMobileAfterTranslucencyPassProcessor,
      EShadingPath::Mobile,
      EMeshPass::MobileAfterTranslucencyPass,
      EMeshPassFlags::CachedMeshCommands | EMeshPassFlags::MainView  // ← 是否正确？
  );

  分析：
  - EMeshPassFlags::CachedMeshCommands：支持静态网格命令缓存 ✅
  - EMeshPassFlags::MainView：仅在主视图中执行 ✅

  看起来合理，但需要确认是否需要添加其他标志（如 EMeshPassFlags::DepthTest）。

  ---
  🔍 潜在遗漏检查

  1. 实例剔除上下文清理

  AfterTranslucencyInstanceCullingDrawParams 添加后，是否需要在帧结束时清理？检查其他
  Pass 的生命周期管理。

  2. 多视图支持

  VR 有双眼渲染，确认 RenderMobileAfterTranslucencyPass
  是否在正确的位置被调用（每个视图一次）。

  3. 材质兼容性

  标记物体使用的材质是否需要特殊处理？例如：
  - Masked 材质
  - Dithered LOD
  - Decals

  当前代码通过 TryAddMeshBatch 处理材质，应该已经覆盖。

  4. 性能统计完整性

  已添加：
  - STAT_AfterTranslucencyDrawTime
  - SCOPED_GPU_STAT(AfterTranslucency)

  需要确认是否需要在 Profiler 中注册新的统计类别。

  ---
  ✅  正确的部分

  1. Pass 分流逻辑：通过 bAfterTranslucencyBasePass 和 ShouldRenderAfterTranslucency()
  正确分离
  2. 深度访问权限：FExclusiveDepthStencil::DepthRead_StencilRead 适合后置渲染
  3. 视口设置：正确复制到 RenderMobileAfterTranslucencyPass
  4. 实例剔除：正确添加到 BuildInstanceCullingDrawParams
  5. 调用时机：在 RenderTranslucency 之后调用 ✅
  6. Blueprint 接口：提供了 SetRenderAfterTranslucency 蓝图函数

  ---
  📋 修改优先级

  🔴 必须修复

  1. 枚举位置冲突或断言值更新
  2. 构造函数参数传递错误

  🟡 强烈建议

  3. CSM Pass 处理逻辑
  4. 深度写入状态改为 true
  5. 恢复混合状态设置

  🟢 可选优化

  6. Flags 配置完整性
  7. 统计声明位置调整

  ---
  总结

  方案整体可行且逻辑正确，但存在几处必须修复的编译/运行时错误。修复后应能实现预期功能。建
  议按优先级逐步验证，特别是深度写入行为和 CSM 阴影需求。