# UE5.4 Mobile Forward Translucency Depth / Sort / Subpass 分析

> 以下问题的验证均严格按照UE5.4移动安卓端Forward路径进行
> Engine/Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp:1578RenderForwardSinglePass中的相关线索代码
> ```cpp
> RHICmdList.SetCurrentStat(GET_STATID(STAT_CLM_MobilePrePass));
> RenderMaskedPrePass(RHICmdList, View);
> // Opaque and masked
> RHICmdList.SetCurrentStat(GET_STATID(STAT_CLMM_Opaque));
> RenderMobileBasePass(RHICmdList, View, &PassParameters->InstanceCullingDrawParams);
> RenderMobileDebugView(RHICmdList, View);
> RHICmdList.PollOcclusionQueries();
> PostRenderBasePass(RHICmdList, View);
> // scene depth is read only and can be fetched
> RHICmdList.NextSubpass();
> RHICmdList.SetCurrentStat(GET_STATID(STAT_CLMM_Translucency));
> RenderDecals(RHICmdList, View, &PassParameters->InstanceCullingDrawParams);
> RenderModulatedShadowProjections(RHICmdList, ViewContext.ViewIndex, View);
> if (GMaxRHIShaderPlatform != SP_METAL_SIM)
> {
> RenderFog(RHICmdList, View);
> }
> // Draw translucency.
> RenderTranslucency(RHICmdList, View);
> ```
> 给我找到相关问题的答案，要有引擎中代码的验证，代码片段要给我在什么文件第几行中
> 1. Translucency深度测试的方式什么？LessEqual吗？
> 2. Translucency Sort Priority原理？
> 3. 怎么修改深度测试的方式的？解释下方的enum，在移动渲染Forward渲染管线中哪里用到了这些地方
> ```cpp
> enum ECompareFunction
> {
> CF_Less,
> CF_LessEqual,
> CF_Greater,
> CF_GreaterEqual,
> CF_Equal,
> CF_NotEqual,
> CF_Never,
> CF_Always,
>
> ECompareFunction_Num,
> ECompareFunction_NumBits = 3,
>
> // Utility enumerations
> CF_DepthNearOrEqual		= (((int32)ERHIZBuffer::IsInverted != 0) ? CF_GreaterEqual : CF_LessEqual),
> CF_DepthNear			= (((int32)ERHIZBuffer::IsInverted != 0) ? CF_Greater : CF_Less),
> CF_DepthFartherOrEqual	= (((int32)ERHIZBuffer::IsInverted != 0) ? CF_LessEqual : CF_GreaterEqual),
> CF_DepthFarther			= (((int32)ERHIZBuffer::IsInverted != 0) ? CF_Less : CF_Greater),
> };
> ```
> 4. RHICmdList.NextSubpass();这里SubPass在初始化的时候关闭了深度写入
> ```cpp
> FMeshPassProcessor* CreateMobileBasePassProcessor(ERHIFeatureLevel::Type FeatureLevel, const FScene* Scene, const FSceneView* InViewIfDynamicMeshCommand, FMeshPassDrawListContext* InDrawListContext)
> {
> FMeshPassProcessorRenderState PassDrawRenderState;
> PassDrawRenderState.SetBlendState(TStaticBlendStateWriteMask<CW_RGBA>::GetRHI());
> const FExclusiveDepthStencil::Type DefaultBasePassDepthStencilAccess = FScene::GetDefaultBasePassDepthStencilAccess(FeatureLevel);
> PassDrawRenderState.SetDepthStencilAccess(DefaultBasePassDepthStencilAccess);
> PassDrawRenderState.SetDepthStencilState(TStaticDepthStencilState<true, CF_DepthNearOrEqual>::GetRHI());
>
> const FMobileBasePassMeshProcessor::EFlags Flags = FMobileBasePassMeshProcessor::EFlags::CanUseDepthStencil
> | (MobileBasePassAlwaysUsesCSM(GShaderPlatformForFeatureLevel[FeatureLevel]) ? FMobileBasePassMeshProcessor::EFlags::CanReceiveCSM : FMobileBasePassMeshProcessor::EFlags::None);
>
> return new FMobileBasePassMeshProcessor(EMeshPass::BasePass, Scene, InViewIfDynamicMeshCommand, PassDrawRenderState, InDrawListContext, Flags);
> }
> ```
> 这是MobileBasePassProcessor初始化的时候相关设置，PassDrawRenderState.SetDepthStencilState(TStaticDepthStencilState<true, CF_DepthNearOrEqual>::GetRHI());这里开启了深度写入，
> PassDrawRenderState.SetBlendState(TStaticBlendStateWriteMask<CW_RGBA>::GetRHI());这里开启写入颜色对吧？怎么将这里状态设置为不写入颜色？我只想开启深度写入，如果这里关闭了颜色写入，模型在渲染的时候还会调用颜色渲染的Vertex Shader和
> Fragment Shader吗？还是只会走深度渲染的相关Shader？
>
> 将分析放到Engine\Docs中，md格式，以引用的格式把我的问题放在文档开头

## 结论摘要

1. UE5.4 Mobile Forward 的普通 Translucency 默认使用 `CF_DepthNearOrEqual`，并且关闭深度写入；不是直接写死 `CF_LessEqual`。因为 UE 默认使用 inverted Z，`CF_DepthNearOrEqual` 实际展开为 `CF_GreaterEqual`，Android Vulkan 上对应 `VK_COMPARE_OP_GREATER_OR_EQUAL`，OpenGL ES 上对应 `GL_GEQUAL`。
2. `Translucency Sort Priority` 是半透明 draw command 的最高优先级排序字段：低 priority 先画，高 priority 后画；同 priority 再按距离从远到近画，最后按同 primitive 内 mesh id 稳定排序。
3. 修改半透明深度测试主要改 Mobile translucency mesh pass processor 的 `TStaticDepthStencilState<false, CF_DepthNearOrEqual>` 中第二个模板参数；如果材质勾选 Disable Depth Test，会被改成 `CF_Always`。
4. `TStaticBlendStateWriteMask<CW_RGBA>` 是开启 RT0 的 RGBA 颜色写入；改成 `TStaticBlendStateWriteMask<CW_NONE>` 或 `TStaticBlendState<CW_NONE>` 可以关闭颜色写入。但如果仍走 BasePass，仍会构建并绑定 Mobile BasePass 的 VS/PS，颜色写入只是被 blend/color-write-mask 屏蔽；不会自动切换成 DepthPass 的 depth-only shader。

## 1. Translucency 深度测试方式

### 1.1 Mobile Forward 中 Translucency 被渲染的位置

`RenderForwardSinglePass` 在 BasePass 后调用 `NextSubpass()`，然后渲染 Decals / Fog / Translucency：

```cpp
// Engine/Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp:1604
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
```

```cpp
// Engine/Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp:1622
// Draw translucency.
RenderTranslucency(RHICmdList, View);
```

`RenderTranslucency` 本身只是 dispatch 对应 mesh pass：

```cpp
// Engine/Source/Runtime/Renderer/Private/MobileTranslucentRendering.cpp:7
void FMobileSceneRenderer::RenderTranslucency(FRHICommandList& RHICmdList, const FViewInfo& View)
{
	const bool bShouldRenderTranslucency = ShouldRenderTranslucency(StandardTranslucencyPass) && ViewFamily.EngineShowFlags.Translucency;
	if (bShouldRenderTranslucency)
	{
		CSV_SCOPED_TIMING_STAT_EXCLUSIVE(RenderTranslucency);
		SCOPE_CYCLE_COUNTER(STAT_TranslucencyDrawTime);
		SCOPED_DRAW_EVENT(RHICmdList, Translucency);
		SCOPED_GPU_STAT(RHICmdList, Translucency);

		RHICmdList.SetViewport(View.ViewRect.Min.X, View.ViewRect.Min.Y, 0.0f, View.ViewRect.Max.X, View.ViewRect.Max.Y, 1.0f);
		View.ParallelMeshDrawCommandPasses[StandardTranslucencyMeshPass].DispatchDraw(nullptr, RHICmdList, &TranslucencyInstanceCullingDrawParams);
	}
}
```

Mobile Forward 中 `StandardTranslucencyMeshPass` 来自 `StandardTranslucencyPass`：

```cpp
// Engine/Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp:307
StandardTranslucencyPass = ViewFamily.AllowTranslucencyAfterDOF() ? ETranslucencyPass::TPT_TranslucencyStandard : ETranslucencyPass::TPT_AllTranslucency;
StandardTranslucencyMeshPass = TranslucencyPassToMeshPass(StandardTranslucencyPass);
```

并且在 render pass 前构建对应 draw commands：

```cpp
// Engine/Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp:1437
if (!bIsFullDepthPrepassEnabled)
{
	View.ParallelMeshDrawCommandPasses[EMeshPass::DepthPass].BuildRenderingCommands(GraphBuilder, Scene->GPUScene, DepthPassInstanceCullingDrawParams);
}
View.ParallelMeshDrawCommandPasses[EMeshPass::BasePass].BuildRenderingCommands(GraphBuilder, Scene->GPUScene, PassParameters->InstanceCullingDrawParams);
View.ParallelMeshDrawCommandPasses[EMeshPass::SkyPass].BuildRenderingCommands(GraphBuilder, Scene->GPUScene, SkyPassInstanceCullingDrawParams);
View.ParallelMeshDrawCommandPasses[StandardTranslucencyMeshPass].BuildRenderingCommands(GraphBuilder, Scene->GPUScene, TranslucencyInstanceCullingDrawParams);
```

### 1.2 Mobile Translucency Pass 的默认 DepthStencilState

Mobile translucency processor 初始化如下：

```cpp
// Engine/Source/Runtime/Renderer/Private/MobileBasePass.cpp:1184
FMeshPassProcessor* CreateMobileTranslucencyStandardPassProcessor(ERHIFeatureLevel::Type FeatureLevel, const FScene* Scene, const FSceneView* InViewIfDynamicMeshCommand, FMeshPassDrawListContext* InDrawListContext)
{
	FMeshPassProcessorRenderState PassDrawRenderState;
	PassDrawRenderState.SetDepthStencilState(TStaticDepthStencilState<false, CF_DepthNearOrEqual>::GetRHI());
	PassDrawRenderState.SetDepthStencilAccess(FExclusiveDepthStencil::DepthRead_StencilRead);
```

```cpp
// Engine/Source/Runtime/Renderer/Private/MobileBasePass.cpp:1196
FMeshPassProcessor* CreateMobileTranslucencyAfterDOFProcessor(ERHIFeatureLevel::Type FeatureLevel, const FScene* Scene, const FSceneView* InViewIfDynamicMeshCommand, FMeshPassDrawListContext* InDrawListContext)
{
	FMeshPassProcessorRenderState PassDrawRenderState;
	PassDrawRenderState.SetDepthStencilState(TStaticDepthStencilState<false, CF_DepthNearOrEqual>::GetRHI());
	PassDrawRenderState.SetDepthStencilAccess(FExclusiveDepthStencil::DepthRead_StencilRead);
```

```cpp
// Engine/Source/Runtime/Renderer/Private/MobileBasePass.cpp:1207
FMeshPassProcessor* CreateMobileTranslucencyAllPassProcessor(ERHIFeatureLevel::Type FeatureLevel, const FScene* Scene, const FSceneView* InViewIfDynamicMeshCommand, FMeshPassDrawListContext* InDrawListContext)
{
	FMeshPassProcessorRenderState PassDrawRenderState;
	PassDrawRenderState.SetDepthStencilState(TStaticDepthStencilState<false, CF_DepthNearOrEqual>::GetRHI());
	PassDrawRenderState.SetDepthStencilAccess(FExclusiveDepthStencil::DepthRead_StencilRead);
```

这里 `TStaticDepthStencilState<false, CF_DepthNearOrEqual>` 的第一个模板参数是 `bEnableDepthWrite`，不是 depth test enable。所以含义是：

- 深度写入：关闭。
- 深度比较：`CF_DepthNearOrEqual`。
- RenderPass depth/stencil 访问：`DepthRead_StencilRead`。

`TStaticDepthStencilState` 模板定义如下：

```cpp
// Engine/Source/Runtime/RenderCore/Public/RHIStaticStates.h:197
template<
	bool bEnableDepthWrite = true,
	ECompareFunction DepthTest = CF_DepthNearOrEqual,
	bool bEnableFrontFaceStencil = false,
```

```cpp
// Engine/Source/Runtime/RenderCore/Public/RHIStaticStates.h:235
static FDepthStencilStateRHIRef CreateRHI()
{
	FDepthStencilStateInitializerRHI Initializer(
		bEnableDepthWrite,
		DepthTest,
```

### 1.3 `CF_DepthNearOrEqual` 在 UE 默认 inverted Z 下不是 LessEqual

`ERHIZBuffer` 定义：

```cpp
// Engine/Source/Runtime/RHI/Public/RHIDefinitions.h:180
enum class ERHIZBuffer
{
	// Before changing this, make sure all math & shader assumptions are correct! Also wrap your C++ assumptions with
	//		static_assert(ERHIZBuffer::IsInvertedZBuffer(), ...);
	// Shader-wise, make sure to update Definitions.usf, HAS_INVERTED_Z_BUFFER
	FarPlane = 0,
	NearPlane = 1,

	// 'bool' for knowing if the API is using Inverted Z buffer
	IsInverted = (int32)((int32)ERHIZBuffer::FarPlane < (int32)ERHIZBuffer::NearPlane),
};
```

`ECompareFunction` 中的 utility enum：

```cpp
// Engine/Source/Runtime/RHI/Public/RHIDefinitions.h:287
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
```

因此在 UE 默认设置下：

- `ERHIZBuffer::FarPlane = 0`
- `ERHIZBuffer::NearPlane = 1`
- `IsInverted = true`
- `CF_DepthNearOrEqual = CF_GreaterEqual`
- `CF_DepthNear = CF_Greater`
- `CF_DepthFartherOrEqual = CF_LessEqual`
- `CF_DepthFarther = CF_Less`

Android Vulkan 映射：

```cpp
// Engine/Source/Runtime/VulkanRHI/Private/VulkanState.cpp:123
static inline VkCompareOp CompareOpToVulkan(ECompareFunction InOp)
{
	switch (InOp)
	{
		case CF_Less:			return VK_COMPARE_OP_LESS;
		case CF_LessEqual:		return VK_COMPARE_OP_LESS_OR_EQUAL;
		case CF_Greater:		return VK_COMPARE_OP_GREATER;
		case CF_GreaterEqual:	return VK_COMPARE_OP_GREATER_OR_EQUAL;
```

Android OpenGL ES 映射：

```cpp
// Engine/Source/Runtime/OpenGLDrv/Private/OpenGLState.cpp:102
static GLenum TranslateCompareFunction(ECompareFunction CompareFunction)
{
	switch(CompareFunction)
	{
	case CF_Less: return GL_LESS;
	case CF_LessEqual: return GL_LEQUAL;
	case CF_Greater: return GL_GREATER;
	case CF_GreaterEqual: return GL_GEQUAL;
```

所以问题 1 的答案是：Mobile Forward Translucency 默认语义是“near or equal”，源码写的是 `CF_DepthNearOrEqual`；在 UE 默认 inverted Z 下实际是 `GreaterEqual` / `GEQUAL`，不是 `LessEqual`。只有非 inverted Z 时 `CF_DepthNearOrEqual` 才会展开成 `CF_LessEqual`。

### 1.4 材质 Disable Depth Test 会覆盖成 Always

Mobile 半透明 render state 中，如果材质 `ShouldDisableDepthTest()` 为 true，会改成 `CF_Always`：

```cpp
// Engine/Source/Runtime/Renderer/Private/MobileBasePass.cpp:740
if (Material.ShouldDisableDepthTest())
{
	DrawRenderState.SetDepthStencilState(TStaticDepthStencilState<false, CF_Always>::GetRHI());
}
```

Deferred 路径也有同类逻辑，可作为对照：

```cpp
// Engine/Source/Runtime/Renderer/Private/BasePassRendering.cpp:382
else if (bDisableDepthTest)
{
	DrawRenderState.SetDepthStencilState(TStaticDepthStencilState<false, CF_Always>::GetRHI());
}
```

## 2. Translucency Sort Priority 原理

### 2.1 组件属性语义

`UPrimitiveComponent::TranslucencySortPriority` 的注释直接说明了语义：

```cpp
// Engine/Source/Runtime/Engine/Classes/Components/PrimitiveComponent.h:769
/**
 * Translucent objects with a lower sort priority draw behind objects with a higher priority.
 * Translucent objects with the same priority are rendered from back-to-front based on their bounds origin.
 * This setting is also used to sort objects being drawn into a runtime virtual texture.
 *
 * Ignored if the object is not translucent.  The default priority is zero.
```

```cpp
// Engine/Source/Runtime/Engine/Classes/Components/PrimitiveComponent.h:778
UPROPERTY(EditAnywhere, BlueprintReadOnly, AdvancedDisplay, Category=Rendering)
int32 TranslucencySortPriority;
```

同文件还有距离偏移：

```cpp
// Engine/Source/Runtime/Engine/Classes/Components/PrimitiveComponent.h:781
/**
 * Modified sort distance offset for translucent objects in world units.
 * A positive number will move the sort distance further and a negative number will move the distance closer.
```

```cpp
// Engine/Source/Runtime/Engine/Classes/Components/PrimitiveComponent.h:788
UPROPERTY(EditAnywhere, BlueprintReadOnly, AdvancedDisplay, Category = Rendering)
float TranslucencySortDistanceOffset = 0.0f;
```

运行时修改接口会 `MarkRenderStateDirty()`，使渲染代理更新：

```cpp
// Engine/Source/Runtime/Engine/Private/Components/PrimitiveComponent.cpp:1814
void UPrimitiveComponent::SetTranslucentSortPriority(int32 NewTranslucentSortPriority)
{
	if (NewTranslucentSortPriority != TranslucencySortPriority)
	{
		TranslucencySortPriority = NewTranslucentSortPriority;
		MarkRenderStateDirty();
	}
}
```

```cpp
// Engine/Source/Runtime/Engine/Private/Components/PrimitiveComponent.cpp:1832
void UPrimitiveComponent::SetTranslucencySortDistanceOffset(float NewTranslucencySortDistanceOffset)
{
	if ( !FMath::IsNearlyEqual(NewTranslucencySortDistanceOffset, TranslucencySortDistanceOffset) )
	{
		TranslucencySortDistanceOffset = NewTranslucencySortDistanceOffset;
		MarkRenderStateDirty();
	}
}
```

### 2.2 Component 值进入 PrimitiveSceneProxy

组件属性复制到 scene proxy：

```cpp
// Engine/Source/Runtime/Engine/Private/PrimitiveSceneProxy.cpp:336
Mobility = InComponent->Mobility;;
TranslucencySortPriority = InComponent->TranslucencySortPriority;
TranslucencySortDistanceOffset = InComponent->TranslucencySortDistanceOffset;
```

proxy 提供 getter：

```cpp
// Engine/Source/Runtime/Engine/Public/PrimitiveSceneProxy.h:656
inline int16 GetTranslucencySortPriority() const { return TranslucencySortPriority; }
inline float GetTranslucencySortDistanceOffset() const { return TranslucencySortDistanceOffset; }
```

### 2.3 SortKey 的字段布局

半透明 draw command 的 sort key 使用 64 bit：

```cpp
// Engine/Source/Runtime/Renderer/Public/MeshPassProcessor.h:1356
struct
{
	uint64 MeshIdInPrimitive	: 16; // Order meshes belonging to the same primitive by a stable id.
	uint64 Distance				: 32; // Order by distance.
	uint64 Priority				: 16; // First order by priority.
} Translucent;
```

比较直接比较 packed 64 bit：

```cpp
// Engine/Source/Runtime/Renderer/Public/MeshPassProcessor.h:1375
FORCEINLINE bool operator<(FMeshDrawCommandSortKey B) const
{
	return PackedData < B.PackedData;
}
```

可见 `Priority` 位于高 16 bit，所以先按 priority 排；然后按 `Distance`；最后按 `MeshIdInPrimitive`。

### 2.4 Priority 如何写入 SortKey

通用半透明 sort key 生成：

```cpp
// Engine/Source/Runtime/Renderer/Private/BasePassRendering.cpp:388
FMeshDrawCommandSortKey CalculateTranslucentMeshStaticSortKey(const FPrimitiveSceneProxy* RESTRICT PrimitiveSceneProxy, uint16 MeshIdInPrimitive)
{
	uint16 SortKeyPriority = 0;
	float DistanceOffset = 0.0f;

	if (PrimitiveSceneProxy)
	{
		const FPrimitiveSceneInfo* PrimitiveSceneInfo = PrimitiveSceneProxy->GetPrimitiveSceneInfo();
		SortKeyPriority = (uint16)((int32)PrimitiveSceneInfo->Proxy->GetTranslucencySortPriority() - (int32)SHRT_MIN);
		DistanceOffset = PrimitiveSceneInfo->Proxy->GetTranslucencySortDistanceOffset();
	}

	FMeshDrawCommandSortKey SortKey;
	SortKey.Translucent.MeshIdInPrimitive = MeshIdInPrimitive;
	SortKey.Translucent.Priority = SortKeyPriority;
	SortKey.Translucent.Distance = *(uint32*)(&DistanceOffset); // View specific, so will be filled later inside VisibleMeshCommands.

	return SortKey;
}
```

这里 `(Priority - SHRT_MIN)` 把 signed priority 映射到 `uint16` 排序空间：

- `-1 -> 32767`
- `0 -> 32768`
- `1 -> 32769`

排序是升序，所以低 priority 先画，高 priority 后画。半透明混合中“后画”会覆盖/混合到前面，因此注释说 lower priority draw behind higher priority。

Mobile BasePass 对半透明还有特殊调整：

```cpp
// Engine/Source/Runtime/Renderer/Private/MobileBasePass.cpp:963
FMeshDrawCommandSortKey SortKey; 
if (bTranslucentBasePass)
{
	const bool bIsUsingMobilePixelProjectedReflection = MaterialResource.IsUsingPlanarForwardReflections() 
													&& IsUsingMobilePixelProjectedReflection(GetFeatureLevelShaderPlatform(FeatureLevel));

	SortKey = CalculateTranslucentMeshStaticSortKey(PrimitiveSceneProxy, MeshBatch.MeshIdInPrimitive);
	// We always want water to be rendered first on mobile in order to mimic other renderers where it is opaque. We shift the other priorities by 1.
	// And we also want to render the meshes used for mobile pixel projected reflection first if it is opaque.
	SortKey.Translucent.Priority = ShadingModels.HasShadingModel(MSM_SingleLayerWater) || (!bIsTranslucent && bIsUsingMobilePixelProjectedReflection) ? uint16(0) : uint16(FMath::Clamp(uint32(SortKey.Translucent.Priority) + 1, 0u, uint32(USHRT_MAX)));
}
```

也就是说在 Mobile 中：

- SingleLayerWater 或 mobile pixel projected reflection 相关 opaque mesh 会被 priority 置 0，最先画。
- 其他半透明 priority 整体加 1，避免和这些特殊对象冲突。

### 2.5 同 priority 时按距离排序

构建 draw commands 后，会更新半透明距离 sort key：

```cpp
// Engine/Source/Runtime/Renderer/Private/MeshDrawCommands.cpp:167
float Distance = 0.0f;
if (TranslucentSortPolicy == ETranslucentSortPolicy::SortByDistance)
{
	//sort based on distance to the view position, view rotation is not a factor
	Distance = (BoundsOrigin - ViewOrigin).Size();
}
else if (TranslucentSortPolicy == ETranslucentSortPolicy::SortAlongAxis)
{
	// Sort based on enforced orthogonal distance
	const FVector CameraToObject = BoundsOrigin - ViewOrigin;
	Distance = FVector::DotProduct(CameraToObject, TranslucentSortAxis);
}
else
{
	// Sort based on projected Z distance
	check(TranslucentSortPolicy == ETranslucentSortPolicy::SortByProjectedZ);
	Distance = ViewMatrix.TransformPosition(BoundsOrigin).Z;
}
```

应用 `TranslucencySortDistanceOffset`：

```cpp
// Engine/Source/Runtime/Renderer/Private/MeshDrawCommands.cpp:186
// Apply distance offset from the primitive
const uint32 PackedOffset = VisibleCommand.SortKey.Translucent.Distance;
const float DistanceOffset = *((float*)&PackedOffset);
Distance += DistanceOffset;
```

将距离编码回 sort key：

```cpp
// Engine/Source/Runtime/Renderer/Private/MeshDrawCommands.cpp:198
// Patch distance inside translucent mesh sort key.
FMeshDrawCommandSortKey SortKey;
SortKey.PackedData = VisibleCommand.SortKey.PackedData;
SortKey.Translucent.Distance = (uint32)~BitInvertIfNegativeFloat(*((uint32*)&Distance));
VisibleCommand.SortKey.PackedData = SortKey.PackedData;
```

`BitInvertIfNegativeFloat`：

```cpp
// Engine/Source/Runtime/Renderer/Private/MeshDrawCommands.cpp:138
uint32 BitInvertIfNegativeFloat(uint32 f)
{
	unsigned mask = -int32(f >> 31) | 0x80000000;
	return f ^ mask;
}
```

最后排序：

```cpp
// Engine/Source/Runtime/Renderer/Private/MeshDrawCommands.cpp:1094
else if (Context.TranslucencyPass != ETranslucencyPass::TPT_MAX)
{
	// When per-pixel OIT is enabled, sort primitive from front to back ensure avoid 
	// constantly resorting front-to-back samples list.
	bool bInverseSorting = OIT::IsSortedPixelsEnabled(*Context.View);
```

```cpp
// Engine/Source/Runtime/Renderer/Private/MeshDrawCommands.cpp:1110
UpdateTranslucentMeshSortKeys(
	Context.TranslucentSortPolicy,
	Context.TranslucentSortAxis,
	Context.ViewOrigin,
	Context.ViewMatrix,
	*Context.PrimitiveBounds,
	Context.TranslucencyPass,
	bInverseSorting,
	Context.MeshDrawCommands
);
```

```cpp
// Engine/Source/Runtime/Renderer/Private/MeshDrawCommands.cpp:1122
{
	QUICK_SCOPE_CYCLE_COUNTER(STAT_SortVisibleMeshDrawCommands);
	Context.MeshDrawCommands.Sort(FCompareFMeshDrawCommands());
}
```

`FCompareFMeshDrawCommands` 先比 sort key，再比 state bucket：

```cpp
// Engine/Source/Runtime/Renderer/Public/MeshPassProcessor.h:1623
struct FCompareFMeshDrawCommands
{
	FORCEINLINE bool operator() (const FVisibleMeshDrawCommand& A, const FVisibleMeshDrawCommand& B) const
	{
		// First order by a sort key.
		if (A.SortKey != B.SortKey)
		{
			return A.SortKey < B.SortKey;
		}

		// Next order by instancing bucket.
		if (A.StateBucketId != B.StateBucketId)
		{
			return A.StateBucketId < B.StateBucketId;
		}
```

默认非 OIT 下，距离编码使更远的物体排在更前面，即 back-to-front；OIT 特定情况下会启用 front-to-back 的 `bInverseSorting`。

## 3. 怎么修改深度测试方式，以及 enum 在 Mobile Forward 中哪里用到

### 3.1 enum 含义

`ECompareFunction` 是 RHI 级深度/模板比较函数：

- `CF_Less`：新值小于已有值通过。
- `CF_LessEqual`：新值小于等于已有值通过。
- `CF_Greater`：新值大于已有值通过。
- `CF_GreaterEqual`：新值大于等于已有值通过。
- `CF_Equal`：相等通过。
- `CF_NotEqual`：不相等通过。
- `CF_Never`：永不通过。
- `CF_Always`：总是通过。

后面的 utility enum 用“近/远”语义屏蔽是否 inverted Z：

- `CF_DepthNearOrEqual`：更靠近相机或相等通过。UE inverted Z 下是 `CF_GreaterEqual`；非 inverted Z 下是 `CF_LessEqual`。
- `CF_DepthNear`：更靠近相机通过。UE inverted Z 下是 `CF_Greater`；非 inverted Z 下是 `CF_Less`。
- `CF_DepthFartherOrEqual`：更远或相等通过。UE inverted Z 下是 `CF_LessEqual`；非 inverted Z 下是 `CF_GreaterEqual`。
- `CF_DepthFarther`：更远通过。UE inverted Z 下是 `CF_Less`；非 inverted Z 下是 `CF_Greater`。

### 3.2 Mobile Forward Translucency 修改点

普通 Mobile Forward Translucency 修改这里：

```cpp
// Engine/Source/Runtime/Renderer/Private/MobileBasePass.cpp:1184
FMeshPassProcessor* CreateMobileTranslucencyStandardPassProcessor(ERHIFeatureLevel::Type FeatureLevel, const FScene* Scene, const FSceneView* InViewIfDynamicMeshCommand, FMeshPassDrawListContext* InDrawListContext)
{
	FMeshPassProcessorRenderState PassDrawRenderState;
	PassDrawRenderState.SetDepthStencilState(TStaticDepthStencilState<false, CF_DepthNearOrEqual>::GetRHI());
	PassDrawRenderState.SetDepthStencilAccess(FExclusiveDepthStencil::DepthRead_StencilRead);
```

例如：

```cpp
PassDrawRenderState.SetDepthStencilState(TStaticDepthStencilState<false, CF_Always>::GetRHI());
```

会让普通半透明总是通过深度测试，但仍不写深度。

如果要改 AfterDOF / AllTranslucency，同样要改：

```cpp
// Engine/Source/Runtime/Renderer/Private/MobileBasePass.cpp:1196
FMeshPassProcessor* CreateMobileTranslucencyAfterDOFProcessor(...)
{
	FMeshPassProcessorRenderState PassDrawRenderState;
	PassDrawRenderState.SetDepthStencilState(TStaticDepthStencilState<false, CF_DepthNearOrEqual>::GetRHI());
	PassDrawRenderState.SetDepthStencilAccess(FExclusiveDepthStencil::DepthRead_StencilRead);
```

```cpp
// Engine/Source/Runtime/Renderer/Private/MobileBasePass.cpp:1207
FMeshPassProcessor* CreateMobileTranslucencyAllPassProcessor(...)
{
	FMeshPassProcessorRenderState PassDrawRenderState;
	PassDrawRenderState.SetDepthStencilState(TStaticDepthStencilState<false, CF_DepthNearOrEqual>::GetRHI());
	PassDrawRenderState.SetDepthStencilAccess(FExclusiveDepthStencil::DepthRead_StencilRead);
```

如果材质 `Disable Depth Test` 还会在 `SetTranslucentRenderState` 中覆盖成 `CF_Always`，所以要注意这里：

```cpp
// Engine/Source/Runtime/Renderer/Private/MobileBasePass.cpp:740
if (Material.ShouldDisableDepthTest())
{
	DrawRenderState.SetDepthStencilState(TStaticDepthStencilState<false, CF_Always>::GetRHI());
}
```

### 3.3 Mobile Forward Opaque BasePass 修改点

BasePass processor 默认开启颜色写和深度写，深度比较是 `CF_DepthNearOrEqual`：

```cpp
// Engine/Source/Runtime/Renderer/Private/MobileBasePass.cpp:1151
FMeshPassProcessor* CreateMobileBasePassProcessor(ERHIFeatureLevel::Type FeatureLevel, const FScene* Scene, const FSceneView* InViewIfDynamicMeshCommand, FMeshPassDrawListContext* InDrawListContext)
{
	FMeshPassProcessorRenderState PassDrawRenderState;
	PassDrawRenderState.SetBlendState(TStaticBlendStateWriteMask<CW_RGBA>::GetRHI());
	const FExclusiveDepthStencil::Type DefaultBasePassDepthStencilAccess = FScene::GetDefaultBasePassDepthStencilAccess(FeatureLevel);
	PassDrawRenderState.SetDepthStencilAccess(DefaultBasePassDepthStencilAccess);
	PassDrawRenderState.SetDepthStencilState(TStaticDepthStencilState<true, CF_DepthNearOrEqual>::GetRHI());
```

如果 BasePass 中开启了 decal/deferred 相关 stencil，`SetOpaqueRenderState` 也会设置 depth state：

```cpp
// Engine/Source/Runtime/Renderer/Private/MobileBasePass.cpp:547
if (bEnableReceiveDecalOutput || bUsesDeferredShading)
{
	DrawRenderState.SetDepthStencilState(TStaticDepthStencilState<
			true, CF_DepthNearOrEqual,
			true, CF_Always, SO_Keep, SO_Keep, SO_Replace,
```

如果 EarlyZ 已经写过深度，BasePass 中部分 mesh 会改为 `CF_Equal` 且不写深度：

```cpp
// Engine/Source/Runtime/Renderer/Private/MobileBasePass.cpp:952
else if((MeshBatch.bUseForDepthPass && Scene->EarlyZPassMode == DDM_AllOpaque) || bMaskedInEarlyPass)
{
	DrawRenderState.SetDepthStencilState(TStaticDepthStencilState<false, CF_Equal>::GetRHI());
}
```

### 3.4 Mobile Forward DepthPrePass 修改点

Mobile 的 masked/full prepass 调用：

```cpp
// Engine/Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp:849
void FMobileSceneRenderer::RenderMaskedPrePass(FRHICommandList& RHICmdList, const FViewInfo& View)
{
	if (bIsMaskedOnlyDepthPrepassEnabled)
	{
		RenderPrePass(RHICmdList, View, &DepthPassInstanceCullingDrawParams);
	}
}
```

Mobile renderer 初始化 prepass 开关：

```cpp
// Engine/Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp:302
bIsFullDepthPrepassEnabled = Scene->EarlyZPassMode == DDM_AllOpaque;
bIsMaskedOnlyDepthPrepassEnabled = Scene->EarlyZPassMode == DDM_MaskedOnly;
```

Mobile prepass dispatch：

```cpp
// Engine/Source/Runtime/Renderer/Private/DepthRendering.cpp:666
void FMobileSceneRenderer::RenderPrePass(FRHICommandList& RHICmdList, const FViewInfo& View, const FInstanceCullingDrawParams* InstanceCullingDrawParams)
{
	checkSlow(RHICmdList.IsInsideRenderPass());
```

```cpp
// Engine/Source/Runtime/Renderer/Private/DepthRendering.cpp:677
SetStereoViewport(RHICmdList, View);
View.ParallelMeshDrawCommandPasses[EMeshPass::DepthPass].DispatchDraw(nullptr, RHICmdList, InstanceCullingDrawParams);
```

Depth pass 的标准状态是关闭颜色写入、开启深度写入和 `CF_DepthNearOrEqual`：

```cpp
// Engine/Source/Runtime/Renderer/Private/DepthRendering.cpp:486
void SetupDepthPassState(FMeshPassProcessorRenderState& DrawRenderState)
{
	// Disable color writes, enable depth tests and writes.
	DrawRenderState.SetBlendState(TStaticBlendState<CW_NONE>::GetRHI());
	DrawRenderState.SetDepthStencilState(TStaticDepthStencilState<true, CF_DepthNearOrEqual>::GetRHI());
}
```

Depth pass 使用 depth-only shader：

```cpp
// Engine/Source/Runtime/Renderer/Private/DepthRendering.cpp:158
IMPLEMENT_MATERIAL_SHADER_TYPE(template<>,TDepthOnlyVS<true>,TEXT("/Engine/Private/PositionOnlyDepthVertexShader.usf"),TEXT("Main"),SF_Vertex);
IMPLEMENT_MATERIAL_SHADER_TYPE(template<>,TDepthOnlyVS<false>,TEXT("/Engine/Private/DepthOnlyVertexShader.usf"),TEXT("Main"),SF_Vertex);
```

```cpp
// Engine/Source/Runtime/Renderer/Private/DepthRendering.cpp:163
IMPLEMENT_SHADERPIPELINE_TYPE_VS(DepthNoPixelPipeline, TDepthOnlyVS<false>, true);
IMPLEMENT_SHADERPIPELINE_TYPE_VS(DepthPosOnlyNoPixelPipeline, TDepthOnlyVS<true>, true);
IMPLEMENT_SHADERPIPELINE_TYPE_VSPS(DepthPipeline, TDepthOnlyVS<false>, FDepthOnlyPS, true);
```

## 4. `NextSubpass()`、深度写入、颜色写入、以及只写深度的做法

### 4.1 Single-pass Forward render target 初始化

Forward render target depth/stencil 初始 access：

```cpp
// Engine/Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp:1494
BasePassRenderTargets.DepthStencil = bIsFullDepthPrepassEnabled ? 
	FDepthStencilBinding(SceneDepth, ERenderTargetLoadAction::ELoad, ERenderTargetLoadAction::ELoad, FExclusiveDepthStencil::DepthRead_StencilWrite) : 
	FDepthStencilBinding(SceneDepth, ERenderTargetLoadAction::EClear, ERenderTargetLoadAction::EClear, FExclusiveDepthStencil::DepthWrite_StencilWrite);
BasePassRenderTargets.SubpassHint = ESubpassHint::None;
```

进入 `RenderForwardSinglePass` 时设置 subpass hint：

```cpp
// Engine/Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp:1586
PassParameters->RenderTargets.SubpassHint = bTonemapSubpassInline ? ESubpassHint::CustomResolveSubpass : ESubpassHint::DepthReadSubpass;
```

`ESubpassHint::DepthReadSubpass` 的定义：

```cpp
// Engine/Source/Runtime/RHI/Public/RHIResources.h:3688
enum class ESubpassHint : uint8
{
	// Regular rendering
	None,

	// Render pass has depth reading subpass
	DepthReadSubpass,
```

BasePass 后调用：

```cpp
// Engine/Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp:1613
// scene depth is read only and can be fetched
RHICmdList.NextSubpass();
RHICmdList.SetCurrentStat(GET_STATID(STAT_CLMM_Translucency));
```

关键点：BasePass 阶段可以写 depth；`NextSubpass()` 后进入 depth-read subpass，后续 decals/fog/translucency 读已有 scene depth。半透明自身的 PSO 又设置为 `TStaticDepthStencilState<false, CF_DepthNearOrEqual>` 和 `DepthRead_StencilRead`，因此不写深度。

`FExclusiveDepthStencil` 的 read/write 含义：

```cpp
// Engine/Source/Runtime/RHI/Public/RHIResources.h:420
{
	// don't use those directly, use the combined versions below
	// 4 bits are used for depth and 4 for stencil to make the hex value readable and non overlapping
	DepthNop = 0x00,
	DepthRead = 0x01,
	DepthWrite = 0x02,
```

```cpp
// Engine/Source/Runtime/RHI/Public/RHIResources.h:432
// use those:
DepthNop_StencilNop = DepthNop + StencilNop,
DepthRead_StencilNop = DepthRead + StencilNop,
DepthWrite_StencilNop = DepthWrite + StencilNop,
DepthNop_StencilRead = DepthNop + StencilRead,
DepthRead_StencilRead = DepthRead + StencilRead,
DepthWrite_StencilRead = DepthWrite + StencilRead,
DepthNop_StencilWrite = DepthNop + StencilWrite,
DepthRead_StencilWrite = DepthRead + StencilWrite,
DepthWrite_StencilWrite = DepthWrite + StencilWrite,
```

RHI validation 也要求：如果 render pass depth 是 readonly，PSO 不能写 depth：

```cpp
// Engine/Source/Runtime/RHI/Public/RHIValidationContext.h:1209
// assert depth is in the correct mode
if (DSMode.IsUsingDepth())
{
	checkf(DSV.ExclusiveDepthStencil.IsUsingDepth(), TEXT("Graphics PSO is using depth but it's not enabled on the RenderPass."));
	checkf(DSMode.IsDepthRead() || DSV.ExclusiveDepthStencil.IsDepthWrite(), TEXT("Graphics PSO is writing to depth but RenderPass depth is ReadOnly."));
}
```

### 4.2 `TStaticBlendStateWriteMask<CW_RGBA>` 是否开启颜色写入

是。`TStaticBlendStateWriteMask` 第一个模板参数就是 RT0 color write mask，默认是 `CW_RGBA`：

```cpp
// Engine/Source/Runtime/RenderCore/Public/RHIStaticStates.h:372
EColorWriteMask RT0ColorWriteMask = CW_RGBA,
EColorWriteMask RT1ColorWriteMask = CW_RGBA,
```

```cpp
// Engine/Source/Runtime/RenderCore/Public/RHIStaticStates.h:382
class TStaticBlendStateWriteMask : public TStaticBlendState<
	RT0ColorWriteMask,BO_Add,BF_One,BF_Zero,BO_Add,BF_One,BF_Zero,
	RT1ColorWriteMask,BO_Add,BF_One,BF_Zero,BO_Add,BF_One,BF_Zero,
```

BasePass processor 中：

```cpp
// Engine/Source/Runtime/Renderer/Private/MobileBasePass.cpp:1153
FMeshPassProcessorRenderState PassDrawRenderState;
PassDrawRenderState.SetBlendState(TStaticBlendStateWriteMask<CW_RGBA>::GetRHI());
const FExclusiveDepthStencil::Type DefaultBasePassDepthStencilAccess = FScene::GetDefaultBasePassDepthStencilAccess(FeatureLevel);
PassDrawRenderState.SetDepthStencilAccess(DefaultBasePassDepthStencilAccess);
PassDrawRenderState.SetDepthStencilState(TStaticDepthStencilState<true, CF_DepthNearOrEqual>::GetRHI());
```

`CW_RGBA` 代表 RT0 的 RGBA 都允许写入。

### 4.3 如何设置为不写颜色，只写深度

如果只改这个 BasePass processor 的默认状态，可以改成：

```cpp
PassDrawRenderState.SetBlendState(TStaticBlendStateWriteMask<CW_NONE>::GetRHI());
PassDrawRenderState.SetDepthStencilState(TStaticDepthStencilState<true, CF_DepthNearOrEqual>::GetRHI());
```

或参考 DepthPass 的现有写法：

```cpp
// Engine/Source/Runtime/Renderer/Private/DepthRendering.cpp:486
void SetupDepthPassState(FMeshPassProcessorRenderState& DrawRenderState)
{
	// Disable color writes, enable depth tests and writes.
	DrawRenderState.SetBlendState(TStaticBlendState<CW_NONE>::GetRHI());
	DrawRenderState.SetDepthStencilState(TStaticDepthStencilState<true, CF_DepthNearOrEqual>::GetRHI());
}
```

注意：如果直接把 `CreateMobileBasePassProcessor` 的 `CW_RGBA` 改为 `CW_NONE`，会影响 Mobile BasePass 的颜色输出。Opaque/masked 对象仍执行 BasePass draw，但不会写 SceneColor；后续 Translucency 仍可基于 depth 做测试。

### 4.4 关闭颜色写入后，还会走 BasePass VS/PS 吗

会。仅改 blend state/color write mask 不会把 mesh pass 从 `EMeshPass::BasePass` 变成 `EMeshPass::DepthPass`，也不会自动替换 shader。

Mobile BasePass 处理 mesh 时获取的是 MobileBasePass shader：

```cpp
// Engine/Source/Runtime/Renderer/Private/MobileBasePass.cpp:930
if (!MobileBasePass::GetShaders(
	LightMapPolicyType,
	LocalLightSetting,
	MaterialResource,
	MeshBatch.VertexFactory->GetType(),
	bEnableSkyLight,
	BasePassShaders.VertexShader,
	BasePassShaders.PixelShader))
{
	return false;
}
```

随后用这些 BasePass shaders 构建 draw command：

```cpp
// Engine/Source/Runtime/Renderer/Private/MobileBasePass.cpp:990
BuildMeshDrawCommands(
	MeshBatch,
	BatchElementMask,
	PrimitiveSceneProxy,
	MaterialRenderProxy,
	MaterialResource,
	DrawRenderState,
	BasePassShaders,
	MeshFillMode,
	MeshCullMode,
	SortKey,
	EMeshPassFeatures::Default,
	ShaderElementData);
```

所以：

- Vertex Shader：仍然是 Mobile BasePass 的 VS。
- Pixel/Fragment Shader：通常仍然是 Mobile BasePass 的 PS，并参与 PSO；只是 RT color write mask 为 `CW_NONE`，最终不写颜色。
- 是否被 GPU early-z / late-z / driver 优化掉部分 pixel work，不能作为确定行为依赖。
- 如果材质是 masked，仍可能需要 pixel shader 做 alpha clip/discard，否则深度结果不正确。

### 4.5 如果想真正走 depth-only shader，应使用 DepthPass 或新建 depth-only pass

DepthPass 已经有现成的“关闭颜色写入 + 深度写入 + depth-only shader”路径：

```cpp
// Engine/Source/Runtime/Renderer/Private/DepthRendering.cpp:486
void SetupDepthPassState(FMeshPassProcessorRenderState& DrawRenderState)
{
	// Disable color writes, enable depth tests and writes.
	DrawRenderState.SetBlendState(TStaticBlendState<CW_NONE>::GetRHI());
	DrawRenderState.SetDepthStencilState(TStaticDepthStencilState<true, CF_DepthNearOrEqual>::GetRHI());
}
```

Depth-only shader 注册：

```cpp
// Engine/Source/Runtime/Renderer/Private/DepthRendering.cpp:158
IMPLEMENT_MATERIAL_SHADER_TYPE(template<>,TDepthOnlyVS<true>,TEXT("/Engine/Private/PositionOnlyDepthVertexShader.usf"),TEXT("Main"),SF_Vertex);
IMPLEMENT_MATERIAL_SHADER_TYPE(template<>,TDepthOnlyVS<false>,TEXT("/Engine/Private/DepthOnlyVertexShader.usf"),TEXT("Main"),SF_Vertex);
```

```cpp
// Engine/Source/Runtime/Renderer/Private/DepthRendering.cpp:163
IMPLEMENT_SHADERPIPELINE_TYPE_VS(DepthNoPixelPipeline, TDepthOnlyVS<false>, true);
IMPLEMENT_SHADERPIPELINE_TYPE_VS(DepthPosOnlyNoPixelPipeline, TDepthOnlyVS<true>, true);
IMPLEMENT_SHADERPIPELINE_TYPE_VSPS(DepthPipeline, TDepthOnlyVS<false>, FDepthOnlyPS, true);
```

Mobile Forward 中调用 depth prepass：

```cpp
// Engine/Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp:849
void FMobileSceneRenderer::RenderMaskedPrePass(FRHICommandList& RHICmdList, const FViewInfo& View)
{
	if (bIsMaskedOnlyDepthPrepassEnabled)
	{
		RenderPrePass(RHICmdList, View, &DepthPassInstanceCullingDrawParams);
	}
}
```

```cpp
// Engine/Source/Runtime/Renderer/Private/DepthRendering.cpp:677
SetStereoViewport(RHICmdList, View);
View.ParallelMeshDrawCommandPasses[EMeshPass::DepthPass].DispatchDraw(nullptr, RHICmdList, InstanceCullingDrawParams);
```

因此如果目标是真正“只写深度、尽量不跑 BasePass color fragment shader”，更合适的方案是：

1. 使用已有 EarlyZ / DepthPass 配置，让目标 mesh 进入 `EMeshPass::DepthPass`。
2. 或新增一个类似 DepthPass 的 mesh pass / processor，使用 `SetupDepthPassState` 和 `FDepthPassMeshProcessor` 类似逻辑。
3. 不建议仅通过修改 BasePass 的 `CW_RGBA -> CW_NONE` 来追求 depth-only shader，因为这只改颜色写 mask，不改 shader pass。

## 直接回答四个问题

### Q1：Translucency 深度测试的方式是什么？LessEqual 吗？

默认是 `TStaticDepthStencilState<false, CF_DepthNearOrEqual>`：关闭深度写入，深度比较为 near-or-equal。

在 UE 默认 inverted Z 下，`CF_DepthNearOrEqual = CF_GreaterEqual`，Android Vulkan 是 `VK_COMPARE_OP_GREATER_OR_EQUAL`，OpenGL ES 是 `GL_GEQUAL`。所以不是直接的 `LessEqual`；只有非 inverted Z 时才等价于 `CF_LessEqual`。

### Q2：Translucency Sort Priority 原理？

`TranslucencySortPriority` 从 `UPrimitiveComponent` 进入 `FPrimitiveSceneProxy`，再写入 `FMeshDrawCommandSortKey::Translucent.Priority` 的高 16 bit。排序升序：低 priority 先画，高 priority 后画；同 priority 按距离 back-to-front；同 primitive 内再按 `MeshIdInPrimitive` 稳定排序。Mobile 额外让 SingleLayerWater / pixel projected reflection 特殊对象 priority 为 0，其他半透明整体 +1。

### Q3：怎么修改深度测试方式？enum 在 Mobile Forward 哪里用到？

普通半透明改这里：

```cpp
// Engine/Source/Runtime/Renderer/Private/MobileBasePass.cpp:1187
PassDrawRenderState.SetDepthStencilState(TStaticDepthStencilState<false, CF_DepthNearOrEqual>::GetRHI());
```

根据需求把第二个模板参数改成 `CF_Always`、`CF_DepthNear`、`CF_DepthFartherOrEqual` 等。AfterDOF / AllTranslucency 还要同步改 `MobileBasePass.cpp:1199`、`MobileBasePass.cpp:1210`。如果要改 BasePass opaque 默认深度测试，改 `MobileBasePass.cpp:1157`；如果开启 stencil/decal/deferred 相关，还要注意 `MobileBasePass.cpp:549`。

### Q4：BasePass 颜色写、只写深度、Shader 是否变化？

`TStaticBlendStateWriteMask<CW_RGBA>` 是开启 RT0 RGBA 颜色写。改为下面即可关闭颜色写：

```cpp
PassDrawRenderState.SetBlendState(TStaticBlendStateWriteMask<CW_NONE>::GetRHI());
PassDrawRenderState.SetDepthStencilState(TStaticDepthStencilState<true, CF_DepthNearOrEqual>::GetRHI());
```

但如果仍走 `EMeshPass::BasePass`，仍会构建 Mobile BasePass 的 VS/PS；不会自动走 DepthPass 的 depth-only shader。要真正走 depth-only shader，应使用/扩展 `EMeshPass::DepthPass`，参考 `DepthRendering.cpp:486` 的 `SetupDepthPassState` 和 `DepthRendering.cpp:158` 起的 depth-only shader 注册。

## 补充验证：DepthStencilState、Disable Depth Test、BasePass 只写深度与 shader 绑定

> 追加问题：
>
> 1. `PassDrawRenderState.SetDepthStencilState(TStaticDepthStencilState<false, CF_DepthNearOrEqual>::GetRHI());` 这句代码中会设置深度测试模式为 `CF_DepthNearOrEqual` 吗？
> 2. 材质勾选 Disable Depth Test 会被改成 `CF_Always` 是在哪里设置的？
> 3. 只在 BasePass 中写入深度，但不绑定颜色 VS/PS 可以吗？移动端 Forward BasePass 中写入深度具体怎么完成？不开启 EarlyZPass 时深度写入 shader 在哪？`TStaticBlendState<CW_NONE>` 关闭颜色写入时 BasePass VS/PS 是否仍包含在内？
> 4. 附到文档后方。

### 5.1 `TStaticDepthStencilState<false, CF_DepthNearOrEqual>` 是否真的设置了深度测试为 `CF_DepthNearOrEqual`

结论：会。第二个模板参数 `DepthTest` 会进入 `FDepthStencilStateInitializerRHI::DepthTest`，再进入具体 RHI 的 depth compare op。

`TStaticDepthStencilState` 模板第二个参数就是 `DepthTest`：

```cpp
// Engine/Source/Runtime/RenderCore/Public/RHIStaticStates.h:197
template<
	bool bEnableDepthWrite = true,
	ECompareFunction DepthTest = CF_DepthNearOrEqual,
	bool bEnableFrontFaceStencil = false,
```

创建 RHI state 时把模板参数传入 initializer：

```cpp
// Engine/Source/Runtime/RenderCore/Public/RHIStaticStates.h:235
static FDepthStencilStateRHIRef CreateRHI()
{
	FDepthStencilStateInitializerRHI Initializer(
		bEnableDepthWrite,
		DepthTest,
```

`FDepthStencilStateInitializerRHI` 保存这个 `InDepthTest`：

```cpp
// Engine/Source/Runtime/RHI/Public/RHI.h:368
struct FDepthStencilStateInitializerRHI
{
	bool bEnableDepthWrite;
	TEnumAsByte<ECompareFunction> DepthTest;
```

```cpp
// Engine/Source/Runtime/RHI/Public/RHI.h:386
FDepthStencilStateInitializerRHI(
	bool bInEnableDepthWrite = true,
	ECompareFunction InDepthTest = CF_LessEqual,
```

```cpp
// Engine/Source/Runtime/RHI/Public/RHI.h:402
: bEnableDepthWrite(bInEnableDepthWrite)
, DepthTest(InDepthTest)
```

Vulkan RHI 使用 `Initializer.DepthTest` 设置 compare op：

```cpp
// Engine/Source/Runtime/VulkanRHI/Private/VulkanState.cpp:265
void FVulkanDepthStencilState::SetupCreateInfo(const FGraphicsPipelineStateInitializer& GfxPSOInit, VkPipelineDepthStencilStateCreateInfo& OutDepthStencilState)
{
	ZeroVulkanStruct(OutDepthStencilState, VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO);

	OutDepthStencilState.depthTestEnable = (Initializer.DepthTest != CF_Always || Initializer.bEnableDepthWrite) ? VK_TRUE : VK_FALSE;
	OutDepthStencilState.depthCompareOp = CompareOpToVulkan(Initializer.DepthTest);
	OutDepthStencilState.depthWriteEnable = Initializer.bEnableDepthWrite ? VK_TRUE : VK_FALSE;
```

OpenGL RHI 同样使用 `Initializer.DepthTest`：

```cpp
// Engine/Source/Runtime/OpenGLDrv/Private/OpenGLState.cpp:363
FDepthStencilStateRHIRef FOpenGLDynamicRHI::RHICreateDepthStencilState(const FDepthStencilStateInitializerRHI& Initializer)
{
	FOpenGLDepthStencilState* DepthStencilState = new FOpenGLDepthStencilState;
	DepthStencilState->Data.bZEnable = Initializer.DepthTest != CF_Always || Initializer.bEnableDepthWrite;
	DepthStencilState->Data.bZWriteEnable = Initializer.bEnableDepthWrite;
	DepthStencilState->Data.ZFunc = TranslateCompareFunction(Initializer.DepthTest);
```

所以这句：

```cpp
PassDrawRenderState.SetDepthStencilState(TStaticDepthStencilState<false, CF_DepthNearOrEqual>::GetRHI());
```

含义是：

- `bEnableDepthWrite = false`：不写深度。
- `DepthTest = CF_DepthNearOrEqual`：深度比较模式为 `CF_DepthNearOrEqual`。
- RHI 层 depth test 是否启用由 `(DepthTest != CF_Always || bEnableDepthWrite)` 判断；这里 `CF_DepthNearOrEqual != CF_Always`，所以 depth test 启用。

RHI validation 也按同样逻辑判断这个 state 是 depth read 还是 depth write：

```cpp
// Engine/Source/Runtime/RHI/Public/RHIValidation.h:120
// @todo: remove this and use the PSO's dsmode instead?
// Determine the actual depth stencil mode that applies for this state
FExclusiveDepthStencil::Type DepthStencilMode = FExclusiveDepthStencil::DepthNop_StencilNop;
if (Initializer.DepthTest != CF_Always || Initializer.bEnableDepthWrite)
{
	DepthStencilMode = Initializer.bEnableDepthWrite
		? FExclusiveDepthStencil::DepthWrite
		: FExclusiveDepthStencil::DepthRead;
}
```

### 5.2 Disable Depth Test 在哪里被改成 `CF_Always`

材质属性定义：

```cpp
// Engine/Source/Runtime/Engine/Classes/Materials/Material.h:617
/** Whether to draw on top of opaque pixels even if behind them. This only has meaning for translucency. */
UPROPERTY(EditAnywhere, Category=Translucency, AdvancedDisplay)
uint8 bDisableDepthTest : 1;
```

`FMaterialResource::ShouldDisableDepthTest()` 直接返回这个材质属性：

```cpp
// Engine/Source/Runtime/Engine/Private/Materials/MaterialShared.cpp:1719
bool FMaterialResource::ShouldDisableDepthTest() const { return Material->bDisableDepthTest; }
```

Mobile 半透明 render state 覆盖点在 `MobileBasePass::SetTranslucentRenderState`：

```cpp
// Engine/Source/Runtime/Renderer/Private/MobileBasePass.cpp:740
if (Material.ShouldDisableDepthTest())
{
	DrawRenderState.SetDepthStencilState(TStaticDepthStencilState<false, CF_Always>::GetRHI());
}
```

调用路径：Mobile translucency pass processor 先设置默认深度测试：

```cpp
// Engine/Source/Runtime/Renderer/Private/MobileBasePass.cpp:1184
FMeshPassProcessor* CreateMobileTranslucencyStandardPassProcessor(ERHIFeatureLevel::Type FeatureLevel, const FScene* Scene, const FSceneView* InViewIfDynamicMeshCommand, FMeshPassDrawListContext* InDrawListContext)
{
	FMeshPassProcessorRenderState PassDrawRenderState;
	PassDrawRenderState.SetDepthStencilState(TStaticDepthStencilState<false, CF_DepthNearOrEqual>::GetRHI());
	PassDrawRenderState.SetDepthStencilAccess(FExclusiveDepthStencil::DepthRead_StencilRead);
```

然后 `Process()` 里复制 `PassDrawRenderState`，半透明时调用 `SetTranslucentRenderState`，所以材质的 Disable Depth Test 会覆盖默认 state：

```cpp
// Engine/Source/Runtime/Renderer/Private/MobileBasePass.cpp:945
FMeshPassProcessorRenderState DrawRenderState(PassDrawRenderState);
if (!bForcePassDrawRenderState)
{
	if (bTranslucentBasePass)
	{
		MobileBasePass::SetTranslucentRenderState(DrawRenderState, MaterialResource, ShadingModels);
	}
```

### 5.3 只在 BasePass 写深度但不绑定颜色 VS/PS 可以吗

结论：如果仍使用现有 Mobile `EMeshPass::BasePass` / `FMobileBasePassMeshProcessor`，不可以只靠 `CW_NONE` 达成“不绑定颜色 VS/PS”。`CW_NONE` 只关闭颜色写入，不改变 mesh pass，也不改变 shader 选择。现有 Mobile BasePass 会获取并绑定 MobileBasePass VS 和 MobileBasePass PS。

Mobile BasePass processor 中明确声明 shader 类型是 Mobile BasePass VS/PS：

```cpp
// Engine/Source/Runtime/Renderer/Private/MobileBasePass.cpp:906
TMeshProcessorShaders<
	TMobileBasePassVSPolicyParamType<FUniformLightMapPolicy>,
	TMobileBasePassPSPolicyParamType<FUniformLightMapPolicy>> BasePassShaders;
```

它调用 `MobileBasePass::GetShaders()` 获取 vertex shader 和 pixel shader：

```cpp
// Engine/Source/Runtime/Renderer/Private/MobileBasePass.cpp:930
if (!MobileBasePass::GetShaders(
	LightMapPolicyType,
	LocalLightSetting,
	MaterialResource,
	MeshBatch.VertexFactory->GetType(),
	bEnableSkyLight,
	BasePassShaders.VertexShader,
	BasePassShaders.PixelShader))
{
	return false;
}
```

`GetShaders()` 的签名也说明会返回 MobileBasePass VS 和 PS：

```cpp
// Engine/Source/Runtime/Renderer/Private/MobileBasePassRendering.h:330
bool GetShaders(
	ELightMapPolicyType LightMapPolicyType,
	EMobileLocalLightSetting LocalLightSetting,
	const FMaterial& MaterialResource,
	const FVertexFactoryType* VertexFactoryType,
	bool bEnableSkyLight, 
	TShaderRef<TMobileBasePassVSPolicyParamType<FUniformLightMapPolicy>>& VertexShader,
	TShaderRef<TMobileBasePassPSPolicyParamType<FUniformLightMapPolicy>>& PixelShader);
```

MobileBasePass shader 类型注册到具体 `.usf` 文件：

```cpp
// Engine/Source/Runtime/Renderer/Private/MobileBasePassRendering.cpp:134
#define IMPLEMENT_MOBILE_SHADING_BASEPASS_LIGHTMAPPED_VERTEX_SHADER_TYPE(LightMapPolicyType,LightMapPolicyName) \
	typedef TMobileBasePassVS< LightMapPolicyType, LDR_GAMMA_32 > TMobileBasePassVS##LightMapPolicyName##LDRGamma32; \
	typedef TMobileBasePassVS< LightMapPolicyType, HDR_LINEAR_64 > TMobileBasePassVS##LightMapPolicyName##HDRLinear64; \
	IMPLEMENT_MATERIAL_SHADER_TYPE(template<>, TMobileBasePassVS##LightMapPolicyName##LDRGamma32, TEXT("/Engine/Private/MobileBasePassVertexShader.usf"), TEXT("Main"), SF_Vertex); \
	IMPLEMENT_MATERIAL_SHADER_TYPE(template<>, TMobileBasePassVS##LightMapPolicyName##HDRLinear64, TEXT("/Engine/Private/MobileBasePassVertexShader.usf"), TEXT("Main"), SF_Vertex);
```

```cpp
// Engine/Source/Runtime/Renderer/Private/MobileBasePassRendering.cpp:140
#define IMPLEMENT_MOBILE_SHADING_BASEPASS_LIGHTMAPPED_PIXEL_SHADER_TYPE2(LightMapPolicyType, LightMapPolicyName, LocalLightSetting, ThinTranslucencyEnum, ThinTranslucencyName) \
	typedef TMobileBasePassPS< LightMapPolicyType, LDR_GAMMA_32, false, LocalLightSetting, EMobileTranslucentColorTransmittanceMode::ThinTranslucencyEnum > TMobileBasePassPS##LightMapPolicyName##LDRGamma32##LocalLightSetting##ThinTranslucencyName; \
	typedef TMobileBasePassPS< LightMapPolicyType, HDR_LINEAR_64, false, LocalLightSetting, EMobileTranslucentColorTransmittanceMode::ThinTranslucencyEnum > TMobileBasePassPS##LightMapPolicyName##HDRLinear64##LocalLightSetting##ThinTranslucencyName; \
	typedef TMobileBasePassPS< LightMapPolicyType, LDR_GAMMA_32, true, LocalLightSetting, EMobileTranslucentColorTransmittanceMode::ThinTranslucencyEnum > TMobileBasePassPS##LightMapPolicyName##LDRGamma32##Skylight##LocalLightSetting##ThinTranslucencyName; \
	typedef TMobileBasePassPS< LightMapPolicyType, HDR_LINEAR_64, true, LocalLightSetting, EMobileTranslucentColorTransmittanceMode::ThinTranslucencyEnum > TMobileBasePassPS##LightMapPolicyName##HDRLinear64##Skylight##LocalLightSetting##ThinTranslucencyName; \
	IMPLEMENT_MATERIAL_SHADER_TYPE(template<>, TMobileBasePassPS##LightMapPolicyName##LDRGamma32##LocalLightSetting##ThinTranslucencyName, TEXT("/Engine/Private/MobileBasePassPixelShader.usf"), TEXT("Main"), SF_Pixel); \
```

`BuildMeshDrawCommands()` 会把这些 shader 放进 pipeline state，并绑定 VS/PS 参数：

```cpp
// Engine/Source/Runtime/Renderer/Public/MeshPassProcessor.inl:93
const FMeshProcessorShaders MeshProcessorShaders = PassShaders.GetUntypedShaders();
PipelineState.SetupBoundShaderState(VertexDeclaration, MeshProcessorShaders);
```

```cpp
// Engine/Source/Runtime/Renderer/Public/MeshPassProcessor.inl:103
PipelineState.BlendState = DrawRenderState.GetBlendState();
PipelineState.DepthStencilState = DrawRenderState.GetDepthStencilState();
```

```cpp
// Engine/Source/Runtime/Renderer/Public/MeshPassProcessor.inl:132
int32 DataOffset = 0;
if (PassShaders.VertexShader.IsValid())
{
	FMeshDrawSingleShaderBindings ShaderBindings = SharedMeshDrawCommand.ShaderBindings.GetSingleShaderBindings(SF_Vertex, DataOffset);
	PassShaders.VertexShader->GetShaderBindings(Scene, FeatureLevel, PrimitiveSceneProxy, MaterialRenderProxy, MaterialResource, ShaderElementData, ShaderBindings);
}

if (PassShaders.PixelShader.IsValid())
{
	FMeshDrawSingleShaderBindings ShaderBindings = SharedMeshDrawCommand.ShaderBindings.GetSingleShaderBindings(SF_Pixel, DataOffset);
	PassShaders.PixelShader->GetShaderBindings(Scene, FeatureLevel, PrimitiveSceneProxy, MaterialRenderProxy, MaterialResource, ShaderElementData, ShaderBindings);
}
```

```cpp
// Engine/Source/Runtime/Renderer/Public/MeshPassProcessor.inl:181
DataOffset = 0;
if (PassShaders.VertexShader.IsValid())
{
	FMeshDrawSingleShaderBindings VertexShaderBindings = MeshDrawCommand.ShaderBindings.GetSingleShaderBindings(SF_Vertex, DataOffset);
	FMeshMaterialShader::GetElementShaderBindings(PassShaders.VertexShader, Scene, ViewIfDynamicMeshCommand, VertexFactory, InputStreamType, FeatureLevel, PrimitiveSceneProxy, MeshBatch, BatchElement, ShaderElementData, VertexShaderBindings, MeshDrawCommand.VertexStreams);
}

if (PassShaders.PixelShader.IsValid())
{
	FMeshDrawSingleShaderBindings PixelShaderBindings = MeshDrawCommand.ShaderBindings.GetSingleShaderBindings(SF_Pixel, DataOffset);
	FMeshMaterialShader::GetElementShaderBindings(PassShaders.PixelShader, Scene, ViewIfDynamicMeshCommand, VertexFactory, EVertexInputStreamType::Default, FeatureLevel, PrimitiveSceneProxy, MeshBatch, BatchElement, ShaderElementData, PixelShaderBindings, MeshDrawCommand.VertexStreams);
}
```

所以 `TStaticBlendState<CW_NONE>` / `TStaticBlendStateWriteMask<CW_NONE>` 只是改变 `PipelineState.BlendState`，不会让 `PassShaders.PixelShader` 失效，也不会换成 DepthOnly shader。

### 5.4 不开启 EarlyZPass 时，Mobile Forward BasePass 中深度写入如何完成

不开 EarlyZPass 时，opaque/masked 深度主要在 BasePass 写入。

Mobile Forward render pass 的 depth attachment 在无 full prepass 时是 `DepthWrite_StencilWrite`：

```cpp
// Engine/Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp:1494
BasePassRenderTargets.DepthStencil = bIsFullDepthPrepassEnabled ? 
	FDepthStencilBinding(SceneDepth, ERenderTargetLoadAction::ELoad, ERenderTargetLoadAction::ELoad, FExclusiveDepthStencil::DepthRead_StencilWrite) : 
	FDepthStencilBinding(SceneDepth, ERenderTargetLoadAction::EClear, ERenderTargetLoadAction::EClear, FExclusiveDepthStencil::DepthWrite_StencilWrite);
```

Mobile shading renderer 初始化 full/masked prepass 开关：

```cpp
// Engine/Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp:302
bIsFullDepthPrepassEnabled = Scene->EarlyZPassMode == DDM_AllOpaque;
bIsMaskedOnlyDepthPrepassEnabled = Scene->EarlyZPassMode == DDM_MaskedOnly;
```

Mobile shading path 下，默认 BasePass depth/stencil access 是 `DepthWrite_StencilWrite`。`GetDefaultBasePassDepthStencilAccess()` 只在 Deferred shading path 且强制 full depth pass 时改成 read，否则保持初始值：

```cpp
// Engine/Source/Runtime/Renderer/Private/RendererScene.cpp:4648
FExclusiveDepthStencil::Type FScene::GetDefaultBasePassDepthStencilAccess(ERHIFeatureLevel::Type InFeatureLevel)
{
	FExclusiveDepthStencil::Type BasePassDepthStencilAccess = FExclusiveDepthStencil::DepthWrite_StencilWrite;

	if (GetFeatureLevelShadingPath(InFeatureLevel) == EShadingPath::Deferred)
	{
```

```cpp
// Engine/Source/Runtime/Renderer/Private/RendererScene.cpp:4662
return BasePassDepthStencilAccess;
```

Mobile BasePass processor 设置深度写入开启、比较为 `CF_DepthNearOrEqual`：

```cpp
// Engine/Source/Runtime/Renderer/Private/MobileBasePass.cpp:1153
FMeshPassProcessorRenderState PassDrawRenderState;
PassDrawRenderState.SetBlendState(TStaticBlendStateWriteMask<CW_RGBA>::GetRHI());
const FExclusiveDepthStencil::Type DefaultBasePassDepthStencilAccess = FScene::GetDefaultBasePassDepthStencilAccess(FeatureLevel);
PassDrawRenderState.SetDepthStencilAccess(DefaultBasePassDepthStencilAccess);
PassDrawRenderState.SetDepthStencilState(TStaticDepthStencilState<true, CF_DepthNearOrEqual>::GetRHI());
```

BasePass draw 调用是：

```cpp
// Engine/Source/Runtime/Renderer/Private/MobileBasePassRendering.cpp:470
void FMobileSceneRenderer::RenderMobileBasePass(FRHICommandList& RHICmdList, const FViewInfo& View, const FInstanceCullingDrawParams* InstanceCullingDrawParams)
{
	CSV_SCOPED_TIMING_STAT_EXCLUSIVE(RenderBasePass);
	SCOPED_DRAW_EVENT(RHICmdList, MobileBasePass);
	SCOPE_CYCLE_COUNTER(STAT_BasePassDrawTime);
	SCOPED_GPU_STAT(RHICmdList, Basepass);

	RHICmdList.SetViewport(View.ViewRect.Min.X, View.ViewRect.Min.Y, 0, View.ViewRect.Max.X, View.ViewRect.Max.Y, 1);
	View.ParallelMeshDrawCommandPasses[EMeshPass::BasePass].DispatchDraw(nullptr, RHICmdList, InstanceCullingDrawParams);
```

BasePass 中如果没有 full prepass / masked prepass 条件命中，就保持 `PassDrawRenderState` 的深度写入 state，或进入 `SetOpaqueRenderState()` 继续使用 `true, CF_DepthNearOrEqual`：

```cpp
// Engine/Source/Runtime/Renderer/Private/MobileBasePass.cpp:945
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

`SetOpaqueRenderState()` 在需要 stencil/decal/deferred 输出时也设置为 depth write true：

```cpp
// Engine/Source/Runtime/Renderer/Private/MobileBasePass.cpp:547
if (bEnableReceiveDecalOutput || bUsesDeferredShading)
{
	DrawRenderState.SetDepthStencilState(TStaticDepthStencilState<
			true, CF_DepthNearOrEqual,
			true, CF_Always, SO_Keep, SO_Keep, SO_Replace,
```

RHI 层最终根据 `bEnableDepthWrite` 开启真实 depth write。Vulkan 示例：

```cpp
// Engine/Source/Runtime/VulkanRHI/Private/VulkanState.cpp:269
OutDepthStencilState.depthTestEnable = (Initializer.DepthTest != CF_Always || Initializer.bEnableDepthWrite) ? VK_TRUE : VK_FALSE;
OutDepthStencilState.depthCompareOp = CompareOpToVulkan(Initializer.DepthTest);
OutDepthStencilState.depthWriteEnable = Initializer.bEnableDepthWrite ? VK_TRUE : VK_FALSE;
```

因此，不开 EarlyZPass 时，Mobile Forward BasePass 写深度不是靠单独 depth-only shader，而是靠 BasePass PSO 的 depth stencil state：`bEnableDepthWrite=true`，VS 输出 `SV_POSITION` 后，硬件固定管线将片元深度写入 depth attachment。若材质使用 `OUTPUT_PIXEL_DEPTH_OFFSET`，MobileBasePassPixelShader 可输出 `SV_Depth`。

MobileBasePass VS 输出 `SV_POSITION`：

```cpp
// Engine/Shaders/Private/MobileBasePassVertexShader.usf:27
struct FMobileShadingBasePassVSToPS
{
	FVertexFactoryInterpolantsVSToPS FactoryInterpolants;
	FMobileBasePassInterpolantsVSToPS BasePassInterpolants;
	INVARIANT_OUTPUT float4 Position : SV_POSITION;
};
```

```cpp
// Engine/Shaders/Private/MobileBasePassVertexShader.usf:80
float4 RasterizedWorldPosition = VertexFactoryGetRasterizedWorldPosition(Input, VFIntermediates, WorldPosition);
Output.Position = INVARIANT(mul(RasterizedWorldPosition, ResolvedView.TranslatedWorldToClip));
```

MobileBasePass PS 是 color/basepass shader，并且声明 `SV_Target0` 输出；在 pixel depth offset 时还可输出 `SV_Depth`：

```cpp
// Engine/Shaders/Private/MobileBasePassPixelShader.usf:290
PIXELSHADER_EARLYDEPTHSTENCIL
void Main( 
	FVertexFactoryInterpolantsVSToPS Interpolants
	, FMobileBasePassInterpolantsVSToPS BasePassInterpolants
	, in float4 SvPosition : SV_Position
```

```cpp
// Engine/Shaders/Private/MobileBasePassPixelShader.usf:310
#else
	#if MOBILE_TRANSLUCENT_COLOR_TRANSMITTANCE_DUAL_SRC_BLENDING
	, out HALF4_TYPE OutColor DUAL_SOURCE_BLENDING_SLOT(0) : SV_Target0
	, out HALF4_TYPE OutColor1 DUAL_SOURCE_BLENDING_SLOT(1) : SV_Target1
	#elif MOBILE_TRANSLUCENT_COLOR_TRANSMITTANCE_PROGRAMMABLE_BLENDING
	, out HALF4_TYPE OutProgrammableBlending : SV_Target0
	#else
	, out HALF4_TYPE OutColor : SV_Target0
	#endif
#endif
```

```cpp
// Engine/Shaders/Private/MobileBasePassPixelShader.usf:323
#if OUTPUT_PIXEL_DEPTH_OFFSET
	, out float OutDepth : SV_Depth
#endif
```

即使颜色写被 `CW_NONE` 屏蔽，该 pixel shader 仍是 BasePass pixel shader；`CW_NONE` 只是不把 `SV_Target0` 写入 render target。

### 5.5 真正 depth-only shader 在哪里

真正的 depth-only pass 使用 `DepthOnlyVertexShader.usf` / `DepthOnlyPixelShader.usf`，不是 MobileBasePassPixelShader。

Depth-only shader 注册：

```cpp
// Engine/Source/Runtime/Renderer/Private/DepthRendering.cpp:158
IMPLEMENT_MATERIAL_SHADER_TYPE(template<>,TDepthOnlyVS<true>,TEXT("/Engine/Private/PositionOnlyDepthVertexShader.usf"),TEXT("Main"),SF_Vertex);
IMPLEMENT_MATERIAL_SHADER_TYPE(template<>,TDepthOnlyVS<false>,TEXT("/Engine/Private/DepthOnlyVertexShader.usf"),TEXT("Main"),SF_Vertex);
```

```cpp
// Engine/Source/Runtime/Renderer/Private/DepthRendering.cpp:163
IMPLEMENT_SHADERPIPELINE_TYPE_VS(DepthNoPixelPipeline, TDepthOnlyVS<false>, true);
IMPLEMENT_SHADERPIPELINE_TYPE_VS(DepthPosOnlyNoPixelPipeline, TDepthOnlyVS<true>, true);
IMPLEMENT_SHADERPIPELINE_TYPE_VSPS(DepthPipeline, TDepthOnlyVS<false>, FDepthOnlyPS, true);
```

Depth pass state 明确关闭颜色写入、开启深度写入：

```cpp
// Engine/Source/Runtime/Renderer/Private/DepthRendering.cpp:486
void SetupDepthPassState(FMeshPassProcessorRenderState& DrawRenderState)
{
	// Disable color writes, enable depth tests and writes.
	DrawRenderState.SetBlendState(TStaticBlendState<CW_NONE>::GetRHI());
	DrawRenderState.SetDepthStencilState(TStaticDepthStencilState<true, CF_DepthNearOrEqual>::GetRHI());
}
```

Depth-only VS 输出 `SV_POSITION`：

```cpp
// Engine/Shaders/Private/DepthOnlyVertexShader.usf:13
struct FDepthOnlyVSToPS
{
	INVARIANT_OUTPUT float4 Position : SV_POSITION;
```

Depth-only PS 只做必要材质裁剪 / depth offset，最后输出 0 色：

```cpp
// Engine/Shaders/Private/DepthOnlyPixelShader.usf:15
void Main(
#if !MATERIALBLENDING_SOLID || OUTPUT_PIXEL_DEPTH_OFFSET
	in INPUT_POSITION_QUALIFIERS float4 SvPosition : SV_Position,
#endif
```

```cpp
// Engine/Shaders/Private/DepthOnlyPixelShader.usf:93
#if MATERIALBLENDING_TRANSLUCENT
	clip(MaterialOpacity - GetMaterialOpacityMaskClipValue());
#elif MATERIALBLENDING_MASKED_USING_COVERAGE
	OutCoverage = DiscardMaterialWithPixelCoverage(MaterialParameters, PixelMaterialInputs);
#else
	GetMaterialCoverageAndClipping(MaterialParameters, PixelMaterialInputs);
#endif
#endif

OutColor = 0;
```

因此如果目标是“BasePass 阶段只写深度、不要绑定 MobileBasePassPixelShader”，实现方式不是改 `BlendState`，而是需要：

1. 让对象进入 `EMeshPass::DepthPass`，使用现有 depth-only processor/shader；或
2. 新建一个在 BasePass 前/内调度的自定义 depth-only mesh pass / processor，使用 `TDepthOnlyVS` / 可选 `FDepthOnlyPS`，并设置 `TStaticBlendState<CW_NONE>` 与 `TStaticDepthStencilState<true, CF_DepthNearOrEqual>`；或
3. 修改 Mobile BasePass processor 使它在特定条件下不使用 `MobileBasePass::GetShaders()` 的 PS，而改走 depth-only shader。这属于改渲染管线和 PSO 组合，不是单行 blend state 能完成。

对于“`TStaticBlendState<CW_NONE>` 可以关闭颜色写入，但如果仍走 BasePass，仍会构建并绑定 Mobile BasePass 的 VS/PS；这个 VS/PS 是否包含在你说的这个 VS/PS 当中？”答案是：包含。这里说的 VS/PS 就是 `TMobileBasePassVS` / `TMobileBasePassPS`，对应 `Engine/Shaders/Private/MobileBasePassVertexShader.usf` 和 `Engine/Shaders/Private/MobileBasePassPixelShader.usf`。
