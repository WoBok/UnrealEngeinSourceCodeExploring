# Plan 方案分析（CX / MiniMax）

> 目标：UE5.4 移动端 VR，让指定物体跳过不透明阶段、在透明物体之后复用 BasePass 的渲染逻辑再绘制一次，从而覆盖在透明物体之上，但仍会被不透明物体遮挡。  
> 已逐文件核对：`MeshPassProcessor.h`、`PrimitiveComponent.h/.cpp`、`PrimitiveSceneProxy.h/.cpp`、`MobileBasePassRendering.h`、`MobileBasePass.cpp`、`MobileShadingRenderer.cpp`、`SceneVisibility.cpp`、`RendererStats`、`MobileBasePassRendering.cpp`。

## 总体评价

整体思路成立：在移动端透明之后插入一个新的 EMeshPass，复用 `FMobileBasePassMeshProcessor` + BasePass 渲染状态、深度测试，可以正确得到"被不透明遮挡、覆盖透明"的合成效果，深度共用即可保证不透明仍然挡得住。但**当前方案在编译、Pass 注册、调用点、状态/统计、跨平台路径、可见性裁剪等多个层面都有遗漏或错误**，按原文直接落代码无法通过编译或运行后 pass 内是空的。

下面按"必须先修的硬错"→"会直接导致 Pass 跑不出来的注册/调用遗漏"→"会导致画面/性能/阴影等异常但仍能编过"→"建议优化"逐项列出。

---

## 一、编译期硬错（不改直接编不过）

### 1.1 `FMobileBasePassMeshProcessor` 构造函数初始化列表少一个右括号
文件：`Source/Runtime/Renderer/Private/MobileBasePass.cpp:810` 附近。
原文（方案里写的）：
```cpp
, bDeferredShading(IsMobileDeferredShadingEnabled(GetFeatureLevelShaderPlatform(ERHIFeatureLevel::ES3_1)))
, bPassUsesDeferredShading(bDeferredShading && !bTranslucentBasePass
, bAfterTranslucencyBasePass(IsAfterTranslucencyBasePass))
```
`bPassUsesDeferredShading` 的右括号丢掉了。正确写法：
```cpp
, bPassUsesDeferredShading(bDeferredShading && !bTranslucentBasePass)
, bAfterTranslucencyBasePass(IsAfterTranslucencyBasePass)
```

### 1.2 `CreateMobileAfterTranslucencyPassProcessor` 调用错位
`FMobileBasePassMeshProcessor` 构造函数签名是：
```cpp
FMobileBasePassMeshProcessor(
    EMeshPass::Type, const FScene*, const FSceneView*,
    const FMeshPassProcessorRenderState&, FMeshPassDrawListContext*,
    EFlags,
    ETranslucencyPass::Type InTranslucencyPassType = TPT_MAX,
    bool bAfterTranslucencyBasePass = false);
```
方案里写的是 `new FMobileBasePassMeshProcessor(..., Flags, true);`，编译器会把 `true` 隐式收窄到 `ETranslucencyPass::Type`，既绕过了 translucency 默认参数，又让 bAfterTranslucencyBasePass 走默认 false，整个分流逻辑直接失效。  
应改成显式写法：
```cpp
return new FMobileBasePassMeshProcessor(
    EMeshPass::MobileAfterTranslucencyPass, Scene, InViewIfDynamicMeshCommand,
    PassDrawRenderState, InDrawListContext, Flags,
    ETranslucencyPass::TPT_MAX, /*bAfterTranslucencyBasePass=*/true);
```

### 1.3 `static_assert(EMeshPass::Num == ...)` 没有真正更新
方案说"更新底部断言 EMeshPass::Num == 33 + 4 与 EMeshPass::Num == 33"，但贴出的代码里这两个值仍然没变。  
加一个 `MobileAfterTranslucencyPass` 后：
- 非 Editor：`Num` 从 33 → 34
- Editor：33 + 4 = 37 → 34 + 4 = 38

应改成 `Num == 34` / `Num == 34 + 4`，否则在打开 `WITH_EDITOR` 时会直接断言失败（不打开也会因为"switch 漏 case"触发 `checkf(0, ...)`）。

### 1.4 头文件没有 `RenderMobileAfterTranslucencyPass` 声明
`MobileShadingRenderer.cpp` 里的 `void FMobileSceneRenderer::RenderMobileAfterTranslucencyPass(...)` 没有在任何 `.h` 出现，`MobileShadingRenderer.cpp` 自己的类体里也没声明，编译报"no member named RenderMobileAfterTranslucencyPass"。  
需要在 `FMobileSceneRenderer` 类内（`MobileShadingRenderer.h` / `SceneRendering.h` 中 `class FMobileSceneRenderer` 声明区）补一个：
```cpp
void RenderMobileAfterTranslucencyPass(FRHICommandList& RHICmdList, const FViewInfo& View, const FInstanceCullingDrawParams* InstanceCullingDrawParams);
```

---

## 二、Pass 跑不出来的注册/调用遗漏（编译能过，运行起来 Pass 列表为空）

### 2.1 SceneVisibility 里的 PassMask 没加新 pass
`Source/Runtime/Renderer/Private/SceneVisibility.cpp:2213` 附近，针对 `EMeshPass::BasePass` 的 `PassMask.Set(EMeshPass::BasePass)` 之后，必须再加一行：
```cpp
PassMask.Set(EMeshPass::MobileAfterTranslucencyPass);
```
否则可见性阶段不会给新 pass 收集 draw commands，`View.ParallelMeshDrawCommandPasses[EMeshPass::MobileAfterTranslucencyPass]` 是空的，`DispatchDraw` 出来什么都没有——这是方案最致命的一处遗漏。  
注意：这里通常还会有一处 `View.NumVisibleDynamicMeshElements[EMeshPass::MobileAfterTranslucencyPass] += NumElements;`，参考同段 `BasePass` 写法补上即可。

### 2.2 还要同步复制一份 `AddCommandsForMesh` 的入口
`SceneVisibility.cpp` 约 1564 / 1580 行有两处对 BasePass 的 `DrawCommandPacket.AddCommandsForMesh(..., EMeshPass::BasePass)` 调用（按 `bIsValidGPUScene` 等条件分支），方案里只改了 PassMask，没有处理 AddCommandsForMesh 这条路。`MobileAfterTranslucencyPass` 也要按相同的条件被加进 `AddCommandsForMesh`，否则 `CachedMeshCommands` 模式（PSOPrecache 的关键路径）下也不会缓存 shader 命令。

### 2.3 `BuildInstanceCullingDrawParams` 没有为新 Pass 生成 InstanceCulling 参数
`Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp:1433` 的 `FMobileSceneRenderer::BuildInstanceCullingDrawParams` 目前只为 BasePass / SkyPass / DepthPass / Translucency / DebugViewMode 准备 `InstanceCullingDrawParams`。  
`RenderMobileAfterTranslucencyPass` 里 `&PassParameters->InstanceCullingDrawParams` 实际就是 BasePass 那份 culling 结果——能用，但仅限 GPUScene 共享同一组 culling 工作的情形；如果之后想让 after-translucency 单独走 culling（例如更紧凑的视锥/遮挡），需要：
- 在 `FMobileRenderPassParameters` 里加一个 `FInstanceCullingDrawParams InstanceCullingDrawParams_AfterTranslucency;` 字段；
- 在 `BuildInstanceCullingDrawParams` 里补一行 `View.ParallelMeshDrawCommandPasses[EMeshPass::MobileAfterTranslucencyPass].BuildRenderingCommands(GraphBuilder, Scene->GPUScene, PassParameters->InstanceCullingDrawParams_AfterTranslucency);`
- 在 `RenderMobileAfterTranslucencyPass` 调用处传入新的字段。

最小可用版可以先共用 culling 参数，但要清楚这是一个耦合点。

### 2.4 `EMeshPass::MobileAfterTranslucencyPass` 的 `EMeshPassFlags` 选择
`REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR(..., EMeshPass::MobileAfterTranslucencyPass, EMeshPassFlags::CachedMeshCommands | EMeshPassFlags::MainView)` 与 BasePass 保持一致是对的。但**注意 CustomRenderPass/SceneCapture 路径**下走的是另一套 visibility，需要确认这个新 pass 在 `CustomRenderPass` 场景下也被正确触发，否则 SceneCapture/平面反射抓不到这些物体（见 3.2）。

---

## 三、调用点/路径遗漏（编译能过、Forward 能跑，但别的模式不生效或行为异常）

### 3.1 Mobile Deferred 路径没改
`MobileShadingRenderer.cpp:1947` 的 `RenderDeferredSinglePass`、`:1996` 的 `RenderDeferredMultiPass` 也调用了 `RenderMobileBasePass` 和 `RenderTranslucency`。Quest 2/3、Pico 等主流 VR 设备默认就开 Mobile Deferred Shading（`r.Mobile.ShadingPath`），不补这两处：  
- 走 Deferred 时你的物体不会出现"覆盖在透明上"的效果；
- HDR + Deferred + Subpass 模式下还可能与 nextSubpass 的 LoadAction 冲突（见 3.5）。

需要在这两处 `RenderTranslucency(RHICmdList, View);` 之后按对应的 `PassParameters`（注意 `DeferredMultiPass` 用的是 `ThirdPassParameters->InstanceCullingDrawParams`，不是 `PassParameters`）调用 `RenderMobileAfterTranslucencyPass`，并且 depth-stencil 的 `DepthStencilAccess` 要与所在 subpass 保持一致（DeferredMultiPass 的最后一个 subpass 已经是 `DepthRead_StencilWrite`，按这个设就行）。

### 3.2 CustomRenderPass / SceneCapture 路径没改
`MobileShadingRenderer.cpp:857` 的 `RenderCustomRenderPassBasePass`（SceneCapture、平面反射、CustomRenderPass）也会调 `RenderMobileBasePass`，方案没有对应处理。  
如果你的标记物体需要出现在反射捕获、SceneCapture 的输出里，需要在这里也补一次"先 RenderMobileBasePass 走常规路径，再以同 pass 处理标记物体"——具体方式可以：  
- 仿照 Forward/Deferred 的思路，在该函数末尾追加一次 `RenderMobileAfterTranslucencyPass`；或  
- 把"after-translucency"做成 cvar 关闭，免得和 SceneCapture 语义打架。

### 3.3 RenderMobileBasePass 自身的 `RenderMobileEditorPrimitives` 顺序
`MobileBasePassRendering.cpp:472` 起 `RenderMobileBasePass` 在 `BasePass` 之后还会调一次 `RenderMobileEditorPrimitives`（编辑器选择高亮等）。  
方案里把 `RenderMobileAfterTranslucencyPass` 插在 `RenderTranslucency` 之后意味着：编辑器选择高亮（`HitProxy` / `EditorSelection`）不会出现在新 pass 之上——通常不是问题，但要在 PR 描述里写清楚，避免美术提"为啥选中框被透明挡了"。

### 3.4 DBuffer 贴花（Deferred Decals）
Forward 路径里 `RenderDecals` 在 `RenderTranslucency` 之前发生。你的物体在 DBuffer 贴花之后才画，结果是**贴花不会投到这个新 pass 的物体上**。  
如果业务要求物体要被贴花影响，需要把 `RenderMobileAfterTranslucencyPass` 放在 `RenderDecals` 之后（位置在 `RenderTranslucency` 之前）——但这又会让"覆盖透明"的目标失败。  
正确顺序取决于业务：
- 物体只受场景 DBuffer 影响 → 移到 decals 之后、translucency 之前；  
- 物体完全无视贴花 → 当前方案即可。

需要你在文档里点明这一选择。

### 3.5 Subpass 边界与 `ELoad` 行为
`RenderForwardSinglePass` / `RenderDeferredSinglePass` 是带 subpass 的，subpass 间 color 是 tile-local 的；`RenderTranslucency` 之后 subpass 已经结束，**进入 nextSubpass / 切到下一个 Raster pass**。  
`RenderMobileAfterTranslucencyPass` 内部：
- `SetLoadAction(ELoad)`（颜色）、depth `ELoad` 是隐含的（RDG 默认），这没问题；  
- 但要确认 `PassDrawRenderState.SetDepthStencilAccess(DefaultBasePassDepthStencilAccess)` 在 after-translucency 这个时刻仍然合法。  
  - FullDepthPrepass 开启时，BasePass 阶段 depth 是 `DepthRead_StencilWrite`，到 translucent 时降为 `DepthRead_StencilRead`。  
  - 如果 after-translucency 仍用 `DefaultBasePassDepthStencilAccess`（默认是 `DepthWrite_StencilWrite`），又会把 depth 写回去，**会影响之后 PostProcess 的 SceneDepth、HZB、Motion Blur 等**。  
  - 推荐强制设成 `DepthRead_StencilWrite`（不写 depth），并把 `bAfterTranslucencyBasePass` 路径上 depth-stencil state 显式切到 `TStaticDepthStencilState<false, CF_DepthNearOrEqual>::GetRHI()`，写关闭。否则深度"二次写入"会破坏不透明深度"被新 pass 看到的语义"，且会出现 depth fight（虽然 depth test `NearOrEqual` 不会"自覆盖"自己，但 transparent 之后再覆盖可能会被某些 platform 的 early-Z / HiZ 优化搞坏）。

### 3.6 VR Multi-View / `vr.MobileMultiView` 行为
`RenderForward` 里根据 `MainView.bIsMobileMultiViewEnabled` 决定 `BasePassRenderTargets.MultiViewCount = 2`，新 pass 的 RenderTargetBinding 必须继承同一 MultiViewCount，否则左右眼像素位置会错位。  
方案里 `RenderMobileAfterTranslucencyPass` 用的 viewport 是 `View.ViewRect`，但没有显式带 MultiViewCount，需要在调用处传入**与 subpass 一致的 `BasePassRenderTargets` 副本**（或一份新的 `FRenderTargetBindingSlots`），并设置相同的 `MultiViewCount`。**这是 VR + Multi-View 场景下不补就会黑边/错位的关键点。**

### 3.7 InstancedStereo / `IsInstancedStereoPass`
Forward/Deferred 都对 `IsInstancedStereoPass` 做了 `SCOPED_GPU_MASK` 处理；新 pass 也要在 `GraphBuilder.AddPass` 中包同样的 GPU mask，否则 multi-GPU（MGPU）VR 工程会出现只画一只眼。

---

## 四、行为/视觉/性能隐患（编过且能跑出东西，但效果/性能/正确性有问题）

### 4.1 其它 Pass 仍然会处理这个物体
`FMobileBasePassMeshProcessor::AddMeshBatch` 的过滤只控制**新 BasePass 类的 Processor**，但**不控制其他 Pass 的 Processor**。  
因此带 `bRenderAfterTranslucency = true` 的物体还会被：
- `DepthPass`（prepass）写深度——意味着 prepass 阶段会画一次，浪费；
- `CSMShadowDepth` / `VSMShadowDepth` 投影——一般不想让这些物体投影；  
- `CustomDepth` 参与 custom depth mask；  
- `Velocity` 写 motion vector，破坏之后 motion blur；  
- `LightmapDensity`、`DebugViewMode`（如果开了 `UseDebugViewPS`）会复现这个物体。

最简方案：在 `FPrimitiveSceneProxy` 暴露 `ShouldRenderAfterTranslucency()` 后，**在每个相关 MeshPassProcessor 的 `AddMeshBatch` 顶部加 early-return**（参考 `ShouldRenderInMainPass()` 的写法）：
```cpp
if (PrimitiveSceneProxy && PrimitiveSceneProxy->ShouldRenderAfterTranslucency() && !IsAfterTranslucencyAcceptingPass(MeshPassType))
    return;
```
把 `IsAfterTranslucencyAcceptingPass` 写成"在白名单内（MobileAfterTranslucencyPass 自身、以及 Reflection Capture / TranslucencyAll 等少数场景）才画"。  
否则你打开 `r.Mobile.AllowDeferredShadingOnOpenGL`、`r.Mobile.AmbientOcclusion`、CSM 等任意一项，画面就会同时出现"两次"的物体（一次 prepass 写深度的，再一次 after-translucency 覆盖），出现自影、闪烁。

### 4.2 CSM 阴影策略
按你"不写入深度、被不透明挡"的语义，这些物体**不应该投影**。需要：
- 在 `ShadowSetupMobile.cpp` 跳过，或在 `FPrimitiveSceneProxy::GetLightRelevance` / `GetStaticShadowDepthMapCastingReceivers` 中通过 `bRenderAfterTranslucency` 短路（参考 `bRenderInMainPass` 的处理）；  
- 否则你开了 CSM 的 VR 场景里会出现"after-translucency 物体投影到不透明物体上"，且它的 shadow caster 又参与了 shadow occlusion pass，可能与 after-translucency pass 的深度测试相互干扰。

### 4.3 反射捕获 / 平面反射 / 镜面材质
`EMeshPass::TranslucencyAll` 是反射捕获走的 Pass；你的物体在反射里要出现吗？  
- 如果要：要在 `MobileBasePass.cpp` 那个 `CreateMobileTranslucencyAllPassProcessor`（或对应的反射路径）里**不**排除 `bRenderAfterTranslucency`，或者在反射捕获路径里也加一份 `RenderMobileAfterTranslucencyPass` 调用；  
- 如果不要：保持"反射里也不画"则 OK，但要意识到这一点。

### 4.4 Local Fog Volume / SkyAtmosphere
`FMobileBasePassUniformParameters` 里包含了 LocalFogVolume、Substrate、PlanarReflection、AO 等。新 pass 直接复用了 `CreateMobileBasePassUniformBuffer(..., EMobileBasePass::Opaque, ...)` 的 uniform buffer，**Fog/Sky 参数是 BasePass 阶段的快照**。  
如果你的物体在透明之后需要看到新一帧的 fog（通常 fog 不会跨 pass 变化）问题不大；但**Mobile HDR / 自定义 PostProcess** 等都需要检查输出 view 的色彩空间一致。

### 4.5 移动端 PSO 数量 / 编译时间
`REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR` 走的是 `CachedMeshCommands` 路径，每多一个 pass，PSO 缓存会按材质 × permutation 翻倍。  
- `r.Mobile.ShadingPath` 选了 Deferred，会再多一遍；  
- 移动端 VR 项目 PSO 数量是 D3D12/Vulkan 的卡点，**强烈建议**为 `MobileAfterTranslucencyPass` 单独开一个 shader permutation（如 `OUTPUT_SCENE_COLOR_AFTER_TRANSLUCENCY=1`）而不是全量复用，避免不同 backbuffer 格式 / MultiView / 阴影组合编译出冗余 PSO。  
- 同时在 `ShouldCacheShaderByPlatformAndOutputFormat` / `MobileBasePassModifyCompilationEnvironment` 检查是否需要加 define 跳过冗余编译。

### 4.6 性能：VR + Mobile 带宽
`RenderMobileAfterTranslucencyPass` 会再触发一次对全屏 color 的 load + draw，会增加 tile-resolve / store 带宽：  
- Forward MultiPass：color 已经是 ELoad 状态，多一次绘制仅增加 vertex/index 提交和 fragment 着色开销；  
- Forward SinglePass：subpass 切到 nextSubpass 后会触发 tile store → load 跨 subpass，多一次 color resolve。  
**对移动端 VR（左右眼 × tile 1~2 × MSAA 2/4）来说这是带宽热点**，建议：
- 只对带 `bRenderAfterTranslucency` 的物体所在的 sub-view 做分支；  
- 用一个 cvar 开关（如 `r.Mobile.RenderAfterTranslucency 0/1`），默认开，但允许低端机关闭。

### 4.7 Substrate / Strata / Mobile 材质路径
UE5.4 移动端开始用 Substrate 简化材质的复杂光照。你的新 pass 走的是同一个 `FMobileBasePassMeshProcessor`，对 Substrate 应该兼容，但**`SubstrateMobileForwardPassUniformParameters` 在 BasePass 与 Translucent 之间可能存在差异**，需要核对 `SetupMobileBasePassUniformParameters` 的 `EMobileBasePass` 是否影响 Substrate 参数——若不影响则 OK，否则需要再加一个 `EMobileBasePass::AfterTranslucency` 分支。

### 4.8 Editor 选择高亮 / HitProxy
同 3.3。`HitProxy` 是独立 EMeshPass，方案没改，不会出问题；但 `EditorSelection` 高亮（`bIsEditorSelectedActor` 路径）也是独立 pass，不会跟着 after-translucency 一起画。需要在美术侧知会。

---

## 五、统计 / Profile / Debug 工具

### 5.1 `STAT_AfterTranslucencyDrawTime` / `STAT_AfterTranslucency` 都没声明
`SCOPE_CYCLE_COUNTER(STAT_AfterTranslucencyDrawTime)` 用的标识符在 `RendererStats` / `SceneRendering` 范围都没声明。  
至少要在 `MobileShadingRenderer.cpp` 顶部补：
```cpp
DECLARE_CYCLE_STAT(TEXT("MobileAfterTranslucency Pass"), STAT_AfterTranslucencyDrawTime, STATGROUP_SceneRendering);
```
或改用 `QUICK_SCOPE_CYCLE_COUNTER(STAT_AfterTranslucencyDrawTime)`，省一个全局声明。

### 5.2 `SCOPED_GPU_STAT(RHICmdList, AfterTranslucency)` 用的 GPU stat
需要在 `RHIStats`/`GpuProfiler` 注册一个新项（参考 `SCOPED_GPU_STAT(RHICmdList, Basepass)` 在 `MobileBasePassRendering.cpp:477` 那里怎么声明的）。  
否则在 `RenderDoc` / Insights 里看不到这个 GPU pass。

### 5.3 `CSV_SCOPED_TIMING_STAT_EXCLUSIVE(RenderAfterTranslucency)` 的 CSV 自定义统计
需要把 `RenderAfterTranslucency` 加到 `Stats::FStatsThreadStateOverlay` 或对应 CSV profile 的 `Stats2.cpp` 注册表里；当前 `RenderBasePass` 是怎么注册的就照样加一份。  
否则 CSV 视图下"RenderAfterTranslucency"会报"unknown stat"。

### 5.4 RDG 事件 / Insights 时间线
`SCOPED_DRAW_EVENT(RHICmdList, MobileAfterTranslucencyPass)` 是好的，但 `RenderMobileBasePass` 那里是 `RDG_EVENT_NAME("RenderMobileBasePass")` 在 `GraphBuilder.AddPass` 那一层——新 pass 最好也在调用 `RenderMobileAfterTranslucencyPass` 之前补一个 `RDG_EVENT_NAME("RenderMobileAfterTranslucencyPass")`，否则 Insights 时间线里这条 pass 名字不直观。

---

## 六、API / 蓝图 / 工程侧注意

### 6.1 `bRenderAfterTranslucency` UPROPERTY meta
`meta = (DisplayName = "Render Opaque After Translucency (Mobile)")` 没问题，但建议再加：
- `meta = (EditCondition = "bRenderInMainPass == true")`——和 `bRenderInMainPass` 互斥（既然要走 after-translucency 路线，就不该再走主 base pass）；  
- `Category = Rendering` 之外，可以考虑加 `meta = (DisplayPriority = 100)` 让它排在更显眼位置。  
  另外 `bRenderInMainPass` 已经被 `FPostProcessSettings::bMobileEnable` 之类的 Setting 改写过，**有些 Material 在 `ShouldRenderInMainPass() == false` 的 path 上会被裁掉**——你加的 `ShouldRenderAfterTranslucency()` 检查要放在 `AddMeshBatch` 的最前面，并**跳过** `bRenderInMainPass` 那个早返：
```cpp
if (PrimitiveSceneProxy && !PrimitiveSceneProxy->ShouldRenderInMainPass() && !PrimitiveSceneProxy->ShouldRenderAfterTranslucency())
    return;
```
否则既不画 main pass 也不画 after-translucency。

### 6.2 蓝图与序列化兼容
新加 `uint8 bRenderAfterTranslucency : 1` 在位域里位置在 `bRenderInMainPass` 之后是 OK 的，但要：
- 确认 `bRenderInMainPass` 周围的 `UPROPERTY` 注释里没写过"do not reorder"——如果有，移动到结构末尾以免破坏 Cooked Asset；  
- 同步在 `FPrimitiveSceneProxy::FPrimitiveSceneProxy(...)` 拷贝构造、`FStaticPrimitiveDrawInterface` 等里加新的标志位（你列出的 4 处 `bRenderInMainPass` 拷贝点都要补 `bRenderAfterTranslucency`）。

### 6.3 `SetRenderAfterTranslucency` 的脏标记
`MarkRenderStateDirty()` 已经够用。但对**每帧切换**的物体（例如 VR 玩家手上的武器一直切换状态）会触发大量 `RecreateRenderState_Concurrent`，注意可能掉帧。  
如果仅在 BeginPlay 一次性设置，则问题不大。

### 6.4 PrimitiveSceneProxy 的拷贝构造 / Move
`PrimitiveSceneProxy.cpp:428` 你已经标了 `InProxyDesc.bRenderAfterTranslucency`，但 `FPrimitiveSceneProxy` 在 `UPrimitiveComponent` -> `FPrimitiveSceneInfo` 之间的拷贝还有 `UpdateSceneInfoFlags`、`FStaticPrimitiveDrawInterface` 等路径，搜索全工程确认 `bRenderInMainPass` 的复制点都已同步：
```
PrimitiveSceneProxy.h/.cpp
PrimitiveSceneInfo.h/.cpp
StaticPrimitiveDrawInterface.h
MeshPassProcessorRenderState if any
```

### 6.5 移动端 VR 后期（Tonemap / PostProcess）顺序
`bTonemapSubpassInline = true` 模式下，`RenderMobileCustomResolve` 在最后做 tonemap。如果 `RenderMobileAfterTranslucencyPass` 画在 tonemap subpass 内部、但 color 已经写入到 subpass 外部的 resolve target，**有可能**被 tonemap 当作"已经 tonemap 过的颜色"再处理一次。  
方案需要明确把 after-translucency 放在哪个 subpass 之后。  
- 建议放在 `RenderMobileCustomResolve` **之前**（subpass 内），这样新绘制的颜色会和主场景一起被 tonemap；  
- 不要放到 `RenderMobileCustomResolve` 之后，否则 LDR 路径下颜色会"亮"出 tonemap 范围，看起来很怪。

### 6.6 `r.Mobile.SupportGPUScene` / `Nanite`
Nanite 在移动端没开，不影响。GPUScene 开启时（UE5 移动 VR 默认开），`AddCommandsForMesh` 那条路要正确处理（见 2.2）。

---

## 七、推荐调整（按优先级）

| 优先级 | 改动 | 目的 |
| --- | --- | --- |
| P0 | 修 1.1 / 1.2 / 1.3 / 1.4 编译错 | 编过 |
| P0 | 补 `SceneVisibility` 的 `PassMask.Set(MobileAfterTranslucencyPass)` 与 `AddCommandsForMesh` 入口 | pass 跑得起来 |
| P0 | 补 `RenderMobileAfterTranslucencyPass` 头文件声明 | 编过 |
| P0 | 处理 3.6 VR Multi-View + 3.5 depth write 关闭 | VR + 移动正确性 |
| P1 | 补 Mobile Deferred 路径（3.1）、CustomRenderPass（3.2） | 完整覆盖所有 shading path |
| P1 | 在所有非新 pass 的 MeshProcessor 中跳过 `ShouldRenderAfterTranslucency()` 物体（4.1） | 不闪烁 / 不重复绘制 |
| P1 | 加 `STAT_*` / `SCOPED_GPU_STAT` / `CSV_*` 声明（5.x） | Insights / Profile 可观测 |
| P2 | 处理 DBuffer 顺序（3.4）、Subpass 与 tonemap 顺序（6.5） | 业务相关，需要决策 |
| P2 | 关闭 CSM 投影（4.2）、规划反射捕获策略（4.3） | 视觉正确 |
| P3 | 性能开关 cvar（4.6）、Substrate 检查（4.7）、Property UI（6.1） | 工程化 |

---

## 八、结论

方案**方向对、可以走通**，但当前补丁集合并不足以让它在 UE5.4 + 移动端 VR 上正确跑起来。最小可用集需要补齐：

1. 4 处编译错（1.1~1.4）；  
2. `SceneVisibility` 里的 PassMask + `AddCommandsForMesh`（2.1、2.2）；  
3. Mobile Deferred 调用点（3.1）；  
4. 其他非 BasePass 的 MeshProcessor 跳过这种物体（4.1）；  
5. VR MultiView + depth-stencil 状态正确（3.5、3.6）。

在这些补齐之前，建议先**单独切一个分支验证：仅在 Forward SinglePass + 关闭 Deferred + 关闭 CSM + 关闭 Prepass** 的最小集里把新 pass 跑通，再按上面优先级表逐项回滚到默认设置。

若希望我直接按这份分析把改后的代码草稿落到对应文件里，告诉我，我可以分两批给你：
- 第一批：先修编译错 + SceneVisibility + 头文件声明，让 Forward SinglePass 真出画面；  
- 第二批：补 Deferred、CSM、VR Multi-View、Stats、Property UI 等"工程化"补丁。
