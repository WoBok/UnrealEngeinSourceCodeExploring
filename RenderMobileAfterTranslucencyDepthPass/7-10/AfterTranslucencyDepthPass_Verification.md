# RenderMobileAfterTranslucencyDepthPass 深度写入影响因素验证

验证问题：`bIsFullDepthPrepassEnabled` 代表 EarlyZ（全量深度预 Pass）是否开启，方案用 `!bIsFullDepthPrepassEnabled` 决定是否执行自定义深度写入。**除它之外，还有哪些因素会影响 RenderMobileAfterTranslucencyDepthPass 的深度写入？**

## 结论摘要

1. `bIsFullDepthPrepassEnabled` **不只由 `r.Mobile.EarlyZPass` 决定**——它背后共有 **5 个触发源**（其中 4 个是"隐性"的项目设置：Mobile AO、DBuffer 贴花、移动端距离场阴影、Forward 局部光+可动聚光灯阴影）。任何一个开启都会把它翻转为 true，你的深度 Pass 会被静默跳过。
2. 但这个守卫是**自洽**的：同一个标志同时控制"深度附件是否可写"、"引擎自己的 DepthPass 是否构建"和"你的 Pass 是否执行"。它翻转为 true 时，标记物体的深度改由 Full Prepass 写入（有完整证据链，见 §3），**核心功能（遮挡半透明）在所有组合下都成立**，变化的只是深度写入的时机和整帧性能特征。
3. 在深度 Pass 确实执行的前提下（`bIsFullDepthPrepassEnabled == false`），还存在 **PSO 状态层、命令生成层、渲染路径层**共约 10 个可能影响"某个网格是否真的写入深度"的因素，逐条验证见 §5——其中需要特别注意的只有 `MeshBatch.bUseForDepthPass` 和半透明材质 Section 两条，其余要么被 Create 参数排除，要么行为与设计一致。
4. 用户提到的"移动端延迟渲染的 EarlyZ 判断逻辑复杂"——延迟路径确实经由 `MobileUsesShadowMaskTexture`/`IsMobileDeferredShadingEnabled` 参与这套判断，但方案已在可见性分流处对 Mobile Deferred 做了回落保护（标记物体走 BasePass），你的两个 Pass 在延迟路径根本不会被 dispatch，因此延迟侧的复杂逻辑**不影响**本方案（详见 §5.5）。

---

## 1. bIsFullDepthPrepassEnabled 的完整决定链

### 1.1 直接来源

`Engine/Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp:302`
```c++
	bIsFullDepthPrepassEnabled = Scene->EarlyZPassMode == DDM_AllOpaque;
	bIsMaskedOnlyDepthPrepassEnabled = Scene->EarlyZPassMode == DDM_MaskedOnly;
```

### 1.2 Scene->EarlyZPassMode 在移动端的赋值

`Engine/Source/Runtime/Renderer/Private/RendererScene.cpp:4694-4708`（`FScene::GetEarlyZPassMode`）
```c++
	else if (GetFeatureLevelShadingPath(InFeatureLevel) == EShadingPath::Mobile)
	{
		OutZPassMode = DDM_None;
				 
		const bool bMaskedOnlyPrePass = FReadOnlyCVARCache::MobileEarlyZPass(ShaderPlatform) == 2;
		if (bMaskedOnlyPrePass)
		{
			OutZPassMode = DDM_MaskedOnly;
		}

		if (MobileUsesFullDepthPrepass(ShaderPlatform))
		{
			OutZPassMode = DDM_AllOpaque;
		}
	}
```
移动端只有三种取值：`DDM_None`（默认）/ `DDM_MaskedOnly`（r.Mobile.EarlyZPass=2）/ `DDM_AllOpaque`（`MobileUsesFullDepthPrepass` 为 true）。**桌面端的 DDM_NonMaskedOnly / DDM_AllOccluders / DDM_AllOpaqueNoVelocity（含 velocity 复杂逻辑）在移动端不会出现**——你看到的"复杂判断"（:4671-4692 的 Deferred 分支，含 `CVarEarlyZPass`、`FVelocityRendering::DepthPassCanOutputVelocity` 等）只作用于桌面 Deferred 路径。

### 1.3 MobileUsesFullDepthPrepass 的 5 个触发源（关键！）

`Engine/Source/Runtime/RenderCore/Private/RenderUtils.cpp:616-619`
```c++
RENDERCORE_API bool MobileUsesFullDepthPrepass(const FStaticShaderPlatform Platform)
{
	return MobileUsesShadowMaskTexture(Platform) || IsMobileAmbientOcclusionEnabled(Platform) || IsUsingDBuffers(Platform) || FReadOnlyCVARCache::MobileEarlyZPass(Platform) == 1;
}
```

其中 `MobileUsesShadowMaskTexture`（:577-582）：
```c++
RENDERCORE_API bool MobileUsesShadowMaskTexture(const FStaticShaderPlatform Platform)
{
	// Only distance field shadow needs to render shadow mask texture on mobile deferred, normal shadows need to be rendered separately because of handling lighting channels.
	// Besides distance field shadow, with clustered lighting and shadow of local light enabled, shadows will render to shadow mask texture on mobile forward, lighting channels are handled in base pass shader.
	return IsMobileDistanceFieldEnabled(Platform) || (!IsMobileDeferredShadingEnabled(Platform) && IsMobileMovableSpotlightShadowsEnabled(Platform) && MobileForwardEnableLocalLights(Platform));
}
```

展开后，**以下任意一项开启都会使 `bIsFullDepthPrepassEnabled = true`，从而跳过你的深度 Pass**：

| # | 触发源 | 对应设置 | 佐证 |
|---|---|---|---|
| 1 | 显式 EarlyZ | `r.Mobile.EarlyZPass = 1`（ECVF_ReadOnly，RendererScene.cpp:126-134） | RenderUtils.cpp:618 |
| 2 | 移动端 AO | `r.Mobile.AmbientOcclusion`（平台掩码 `GMobileAmbientOcclusionPlatformMask`） | RenderUtils.cpp:556-559 |
| 3 | DBuffer 贴花 | `r.DBuffer`（项目设置 "DBuffer Decals"） | RenderUtils.cpp:618 `IsUsingDBuffers` |
| 4 | 移动端距离场（距离场阴影） | `r.DistanceFields` + 平台 `bSupportsDistanceFields` | RenderUtils.cpp:561-564 `IsMobileDistanceFieldEnabled` |
| 5 | Forward 局部光 + 可动聚光灯阴影 | `r.Mobile.Forward.EnableLocalLights` **且** `r.Mobile.EnableMovableSpotlightsShadow` | RenderUtils.cpp:581 |

> **对 VR 项目的现实提醒**：第 5 条最容易被忽视——如果项目后续开启"Forward 局部光 + 可动聚光灯阴影"组合，Full Prepass 会静默开启，整帧多一遍全量深度绘制，且你的深度 Pass 从此不再执行（功能仍正确，见 §3）。这些开关全部是 ini/启动期决定（`r.Mobile.EarlyZPass` 为只读 CVar；其余的通过 CVar Sink `UpdateEarlyZPassModeCVarSinkFunction`，RendererScene.cpp:1370-1406，在变化时刷新 `EarlyZPassMode` 并重建静态绘制列表），**不会在一帧内中途改变**。

### 1.4 附带影响：这些开关同时改变 Shader 编译环境

`Engine/Source/Runtime/RenderCore/Private/Shader.cpp:1761`
```c++
		KeyString += MobileUsesFullDepthPrepass(Platform) ? TEXT("_MobFDP") : TEXT("");
```
`MobileUsesFullDepthPrepass` 参与 Shader Key（DDC Key），并被 `MobileRequiresSceneDepthAux`（RenderUtils.cpp:481-499）、贴花/雾/阴影的深度读取方式（`bMobileForceDepthRead`，MobileFogRendering.cpp:113、DecalRenderingCommon.cpp:928 等）引用。切换这些设置等价于切换渲染管线形态，需要重编 Shader——这也解释了"为什么守卫必须跟引擎用同一个标志而不是自己发明一个"。

---

## 2. 守卫为什么必须用（且只用）bIsFullDepthPrepassEnabled

三处消费方使用**同一个标志**，翻转时三者同步变化，不存在中间状态：

1. **深度附件是否可写**——`MobileShadingRenderer.cpp:1494-1496`（`InitRenderTargetBindings_Forward`）：
```c++
	BasePassRenderTargets.DepthStencil = bIsFullDepthPrepassEnabled ? 
		FDepthStencilBinding(SceneDepth, ERenderTargetLoadAction::ELoad, ERenderTargetLoadAction::ELoad, FExclusiveDepthStencil::DepthRead_StencilWrite) : 
		FDepthStencilBinding(SceneDepth, ERenderTargetLoadAction::EClear, ERenderTargetLoadAction::EClear, FExclusiveDepthStencil::DepthWrite_StencilWrite);
```
（第二个及后续 View 同样按此切换，:1540）

2. **引擎自己的 DepthPass 构建**——`MobileShadingRenderer.cpp:1437-1440`：
```c++
		if (!bIsFullDepthPrepassEnabled)
		{
			View.ParallelMeshDrawCommandPasses[EMeshPass::DepthPass].BuildRenderingCommands(GraphBuilder, Scene->GPUScene, DepthPassInstanceCullingDrawParams);
		}
```

3. **你的深度 Pass dispatch**（方案 15.1/15.2/15.3 的守卫）。

`bIsFullDepthPrepassEnabled` 为 true 时深度附件是 `DepthRead`，任何深度写入都会触发 RHI/RDG 校验失败——所以"是否有其他因素让引擎把深度附件改成只读"这个问题的答案是：**没有，Forward 路径下只有这一个标志控制它**（Mobile Deferred 的 `InitRenderTargetBindings_Deferred` 是另一套，但那条路径不会执行你的 Pass）。

---

## 3. 守卫翻转为 true 时，标记物体深度由谁写入（证据链）

此时你的深度 Pass 跳过，但标记物体**照常进入常规 DepthPass**（方案未从 DepthPass 剔除标记物体，正是为了这条路径）：

1. Full Prepass 强制所有静态网格进 DepthPass——`SceneVisibility.cpp:1059`：
```c++
	bFullEarlyZPass = ShouldForceFullDepthPass(View.GetShaderPlatform());
```
`ShouldForceFullDepthPass`（RenderUtils.cpp:621-626）在移动端就是 `MobileUsesFullDepthPrepass`。

2. `SceneVisibility.cpp:1423`（静态网格深度命令的入队条件）：
```c++
			const bool bDrawDepthOnly = ViewData.bFullEarlyZPass || ((ShadingPath != EShadingPath::Mobile) && (...屏幕尺寸过滤...));
```
注意移动端**没有**桌面那个"小物体按屏幕半径跳过 Prepass"的过滤（`GMinScreenRadiusForDepthPrepass` 仅在 `ShadingPath != Mobile` 时生效）——即移动端 Full Prepass 下标记物体不会因为太小而漏写深度。

3. `SceneVisibility.cpp:1530-1542`：满足 `bDrawDepthOnly` 即 `AddCommandsForMesh(..., EMeshPass::DepthPass)`；动态网格（骨骼网格）在 `ComputeDynamicMeshRelevance` :2198-2209 无条件设置 DepthPass 掩码。

4. Prepass 在 SceneColor Pass 之前独立执行——`MobileShadingRenderer.cpp:1248`：
```c++
	RenderFullDepthPrepass(GraphBuilder, Views, SceneTextures);
```

结论：Full Prepass 开启时，标记物体深度在**更早**的时机写入（Prepass 里、BasePass 之前），半透明照样被遮挡，之后的颜色 Pass（DepthRead + CF_DepthNearOrEqual）照常通过。**功能不变，只是深度写入时机前移、整帧多一遍全量 Prepass 的开销。**

---

## 4. DDM_MaskedOnly（r.Mobile.EarlyZPass = 2）的情况

此时 `bIsFullDepthPrepassEnabled == false`，你的深度 Pass **正常执行**。区别在于 Masked 材质的标记物体深度会被写两遍：

1. `RenderMaskedPrePass` 在 Subpass 0 开头执行——`MobileShadingRenderer.cpp:849-855`：
```c++
void FMobileSceneRenderer::RenderMaskedPrePass(FRHICommandList& RHICmdList, const FViewInfo& View)
{
	if (bIsMaskedOnlyDepthPrepassEnabled)
	{
		RenderPrePass(RHICmdList, View, &DepthPassInstanceCullingDrawParams);
	}
}
```
Masked 标记物体经 `SceneVisibility.cpp:1531` 的 `(bMobileMaskedInEarlyPass && ViewRelevance.bMasked)` 分支进入 DepthPass，深度提前写入。

2. 之后你的深度 Pass 以 `CF_DepthNearOrEqual` 等值重写一遍——冗余但无害，不产生错误。

---

## 5. 深度 Pass 执行时，影响"某个网格是否真的写入深度"的其余因素（逐条验证）

### 5.1 PSO 状态层：Process() 中的两处状态修改——均保留深度写

`FDepthPassMeshProcessor::Process`（DepthRendering.cpp:817-828）在拷贝 `PassDrawRenderState` 后有两处修改：

**(a) `SetMobileDepthPassRenderState`（ES3.1 无条件执行，:825-828）**——深度写**保持开启**，只附加 stencil：

`DepthRendering.cpp:753-760`
```c++
void SetMobileDepthPassRenderState(const FPrimitiveSceneProxy* RESTRICT PrimitiveSceneProxy, FMeshPassProcessorRenderState& DrawRenderState, const FMeshBatch& RESTRICT MeshBatch, bool bUsesDeferredShading)
{
	DrawRenderState.SetDepthStencilState(TStaticDepthStencilState<
		true, CF_DepthNearOrEqual,
		true, CF_Always, SO_Keep, SO_Keep, SO_Replace,
		false, CF_Always, SO_Keep, SO_Keep, SO_Keep,
		// don't use masking as it has significant performance hit on Mali GPUs (T860MP2)
		0x00, 0xff >::GetRHI());
```
第一个模板参数 `true` = 深度写开启。后续写入的 stencil 位（Forward 下为 RECEIVE_DECAL + MOBILE_CAST_CONTACT_SHADOW，:762-781）与引擎自己的移动端 Prepass 完全一致——**这反而是好事**：标记物体获得正确的"是否接收贴花/接触阴影"stencil 标记。
注意：这也意味着方案 12.3 中 Create 函数设置的 `DepthWrite_StencilWrite` 访问声明是必须的（stencil 确实会被写）。

**(b) `SetDepthPassDitheredLODTransitionState`（:819-822）**——仅当 `ViewInfo->bAllowStencilDither` 且网格处于 LOD 抖动过渡时才改状态（DepthRendering.cpp:246-266）。而：

`SceneVisibility.cpp:5443`
```c++
		View.bAllowStencilDither = DepthPass.bDitheredLODTransitionsUseStencil;
```
`DepthRendering.cpp:102`
```c++
	Info.bDitheredLODTransitionsUseStencil = CVarStencilForLODDither.GetValueOnAnyThread() > 0;
```
`r.StencilForLODDither` 默认 0（且为只读 CVar），移动端项目通常不开——**默认不触发**。

### 5.2 命令生成层：会让网格默默不进深度 Pass 的条件

| 条件 | 代码位置 | 对标记物体的影响 |
|---|---|---|
| `MeshBatch.bUseForDepthPass == false` | AddMeshBatch 入口 DepthRendering.cpp:1023 | **需注意**。静态网格代理在"仅阴影 LOD"或按 Section 合批的特殊分支会置 false（StaticMeshRender.cpp:596、:1171、:1316）。常规不透明材质的正常 LOD 均为 true。如标记物体使用了"Shadow LOD"之类配置需实测 |
| 材质为半透明 BlendMode | TryAddMeshBatch DepthRendering.cpp:976-980 | 标记物体上的半透明 Section 不写深度（**符合设计**：半透明 Section 走正常半透明 Pass） |
| 材质域非 Surface / 贴花材质 | DepthRendering.cpp:982-983 `ShouldIncludeDomainInMeshPass` / `ShouldIncludeMaterialInDefaultOpaquePass` | 正常物体不受影响 |
| `ShouldRenderInDepthPass() == false` | DepthRendering.cpp:977；PrimitiveSceneProxy.h:701 `return bRenderInMainPass \|\| bRenderInDepthPass` | 标记物体 `bRenderInMainPass=true` 恒通过，**不可能触发** |
| Occluder/Movable/屏幕尺寸过滤 | DepthRendering.cpp:1026-1046 | 已被 Create 参数 `bRespectUseAsOccluderFlag=false` 排除 |
| `ShouldRender` 按 Masked/NonMasked 过滤 | DepthRendering.cpp:939-969（DDM_MaskedOnly/DDM_NonMaskedOnly 分支） | 已被 Create 参数固定 `EarlyZPassMode=DDM_AllOpaque` 排除（不跟随 Scene 的模式） |
| DDM_AllOpaqueNoVelocity 的速度剔除 | DepthRendering.cpp:1050-1074 | 我们的模式是 DDM_AllOpaque，不触发；且该模式移动端本就不出现（§1.2） |
| PDO（Pixel Depth Offset）材质 | DepthRendering.cpp:805-812 `GetDepthPassShaders(..., MaterialUsesPixelDepthOffset, ...)` | 自动绑定 `FDepthOnlyPS` 输出偏移深度，行为正确；另注意移动端 PDO 受 `r.Mobile.AllowPixelDepthOffset` 总开关控制（RenderUtils.cpp:522-530） |

### 5.3 RenderPass/Subpass 层（方案已固定，此处仅列全）

- 深度附件绑定：见 §2，唯一开关即 `bIsFullDepthPrepassEnabled`。
- Subpass 顺序：深度 Pass 必须在 `RHICmdList.NextSubpass()`（MobileShadingRenderer.cpp:1614）之前——Subpass 切换后深度只读（`ESubpassHint::DepthReadSubpass`，:1586）。方案插入点在 :1609 `RenderMobileBasePass` 之后，满足。
- MultiPass（GLES）第二个 RenderPass 深度只读（:1700 `DepthRead_StencilRead`）——深度必须在第一个 Pass 写，方案已如此。Android Vulkan 恒走 SinglePass（`RequiresMultiPass` 对 Vulkan 直接返回 false，:2131-2137）。

### 5.4 可见性层

视锥剔除、硬件遮挡查询、`bDrawRelevance`、HLOD fade 对本 Pass 的作用与对 BasePass 完全相同（`FRelevancePacket` 统一处理），没有额外的深度 Pass 专属剔除（移动端的 `GMinScreenRadiusForDepthPrepass` 过滤不生效，见 §3.2）。唯一注意：物体被遮挡剔除后深度自然不写——这与"它本来也看不见"一致，非异常。

### 5.5 渲染路径层：Mobile Deferred 为什么不用担心

你观察到的"移动端延迟渲染 EarlyZ 判断复杂"主要是两点，均不影响本方案：

1. Mobile Deferred 会经 `MobileUsesShadowMaskTexture` → `IsMobileDistanceFieldEnabled` 等参与 `MobileUsesFullDepthPrepass` 判断（RenderUtils.cpp:577-582 注释即在说明这套关系）；`GetDefaultBasePassDepthStencilAccess`（RendererScene.cpp:4648-4663）里的 `DepthRead` 切换也只作用于**桌面 Deferred**（:4652 判断 `EShadingPath::Deferred`）。
2. 方案在可见性分流处已用 `!IsMobileDeferredShadingEnabled(...)` 保护（Plan_Final.md §10.1/§10.2）：Mobile Deferred 下标记物体回落 BasePass 正常渲染，两个新 Pass 的命令即便被缓存也永远不会 dispatch（`RenderDeferred`/`RenderDeferredSinglePass`，MobileShadingRenderer.cpp:1885/:1947 中没有调用点）。**延迟路径的深度附件绑定方式与本方案无交集。**

### 5.6 GPUScene 开关

`BuildInstanceCullingDrawParams` 仅在 `Scene->GPUScene.IsEnabled()` 时构建参数（MobileShadingRenderer.cpp:1435）；关闭时 `DispatchDraw` 以默认参数工作——与现有 SkyPass/Translucency 的成员参数模式完全相同，无深度写入影响。

---

## 6. 项目侧自查清单

部署前用以下命令确认当前设备/配置实际处于哪条路径（可在设备上跑 `r.Mobile.EarlyZPass` 等只读变量查询，或直接看 RenderDoc 帧结构）：

- [ ] `r.Mobile.EarlyZPass` 当前值（0 = 你的深度 Pass 生效；1 = 静默走 Full Prepass；2 = MaskedOnly，你的 Pass 生效 + Masked 物体深度双写）
- [ ] `r.Mobile.AmbientOcclusion`（开 → Full Prepass）
- [ ] `r.DBuffer`（开 → Full Prepass）
- [ ] `r.DistanceFields` + 项目"Generate Mesh Distance Fields"（开 → Full Prepass）
- [ ] `r.Mobile.Forward.EnableLocalLights` 与 `r.Mobile.EnableMovableSpotlightsShadow` 是否**同时**开启（是 → Full Prepass）
- [ ] `r.StencilForLODDither` = 0（默认；非 0 时标记物体 LOD 抖动过渡会附加 stencil 状态，深度写仍保留）
- [ ] RenderDoc 验证：Full Prepass 关闭时帧内应出现 `MobileAfterTranslucencyDepthPass` 事件（位于 MobileBasePass 之后、NextSubpass 之前）；开启时应看到 `RenderFullDepthPrepass` 中包含标记物体、且 `MobileAfterTranslucencyDepthPass` 不出现——两种情况下半透明都应被标记物体遮挡。
