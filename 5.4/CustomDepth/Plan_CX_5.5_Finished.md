# UE 5.4 Mobile MultiView CustomDepth 修复计划

本文只给 5.4 的修复计划，不执行源码修改。对比对象为 `UnrealEngine5.4` 与 `UnrealEngine5.5`。

## 结论

`UnrealEngine5.4/Engine/Source/Runtime/Renderer/Private/CustomDepthRendering.cpp:287` 的 `SetStereoViewport(RHICmdList, View, 1.0f);` 确实处在 CustomDepth pass 的 stereo/multiview 绘制路径上，但它不是单眼 CustomDepth 的根因。5.4 的核心问题是 CustomDepth 仍按 `Texture2D` 创建和绑定，Mobile MultiView 需要写入/读取 `Texture2DArray` 的两个 layer。

5.5 的修复闭环包含四类改动：

1. C++ 创建 CustomDepth 时把 `Config.bRequireMultiView` 传进去。
2. CustomDepth render target 在 MultiView 下创建为 `Texture2DArray`，array size 为 2。
3. CustomDepth pass 的 RDG render targets 设置 `MultiViewCount`。
4. Mobile scene texture uniform 和 shader 都增加 CustomDepth/CustomStencil 的 array 版本，并把当前 `ViewId` 传到材质 scene texture 读取链路。

## 必改文件和具体位置

### 1. `UnrealEngine5.4/Engine/Source/Runtime/Renderer/Internal/CustomDepthRendering.h`

5.4 当前声明在第 24 行：

```cpp
static FCustomDepthTextures Create(FRDGBuilder& GraphBuilder, FIntPoint CustomDepthExtent, EShaderPlatform ShaderPlatform);
```

计划改为 5.5 形式，增加 `bool bRequireMultiView`：

```cpp
static FCustomDepthTextures Create(FRDGBuilder& GraphBuilder, FIntPoint CustomDepthExtent, EShaderPlatform ShaderPlatform, bool bRequireMultiView);
```

目的：让 SceneTextures 初始化阶段把是否需要 Mobile MultiView array render target 的配置传入 CustomDepth 创建函数。

### 2. `UnrealEngine5.4/Engine/Source/Runtime/Renderer/Private/CustomDepthRendering.cpp`

第 63 行函数定义同步增加参数：

```cpp
FCustomDepthTextures FCustomDepthTextures::Create(FRDGBuilder& GraphBuilder, FIntPoint CustomDepthExtent, EShaderPlatform ShaderPlatform, bool bRequireMultiView)
```

第 86 行当前只创建 2D 深度模板纹理：

```cpp
const FRDGTextureDesc CustomDepthDesc = FRDGTextureDesc::Create2D(CustomDepthExtent, PF_DepthStencil, FClearValueBinding::DepthFar, CreateFlags);
```

计划替换为 5.5 的条件创建逻辑：

```cpp
FRDGTextureDesc CustomDepthDesc(bRequireMultiView ?
	FRDGTextureDesc::Create2DArray(CustomDepthExtent, PF_DepthStencil, FClearValueBinding::DepthFar, CreateFlags, 2) :
	FRDGTextureDesc::Create2D(CustomDepthExtent, PF_DepthStencil, FClearValueBinding::DepthFar, CreateFlags));
```

第 273-279 行 CustomDepth pass 绑定 render target 后，5.4 缺少 MultiViewCount。计划在第 279 行 `BuildRenderingCommands` 之前插入：

```cpp
PassParameters->RenderTargets.MultiViewCount = (View.bIsMobileMultiViewEnabled) ? 2 : (View.Aspects.IsMobileMultiViewEnabled() ? 1 : 0);
```

第 287 行 `SetStereoViewport` 保留。第 288 行继续使用 5.4 已有的 `DispatchDraw(...)`，不要直接改成 5.5 的 `.Draw(...)`；5.4 的 `MeshDrawCommands.h:165` 只有 `DispatchDraw`，5.5 才在 `MeshDrawCommands.h:165-171` 引入 `Draw/Dispatch` 并把 `DispatchDraw` 标 deprecated。

第 403-410 行 GLES stencil copy/SRV 路径不需要按 5.5 修改，因为 5.5 同处代码也没有变：

```cpp
FRDGTextureRef CustomStencil = GraphBuilder.CreateTexture(CustomDepthTextures.Depth->Desc, TEXT("CustomStencil"));
AddCopyTexturePass(GraphBuilder, CustomDepthTextures.Depth, CustomStencil);
CustomDepthTextures.Stencil = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::CreateWithPixelFormat(CustomStencil, PF_X24_G8));
...
CustomDepthTextures.Stencil = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::CreateWithPixelFormat(CustomDepthTextures.Depth, PF_X24_G8));
```

实施后需要在编译/GPU capture 中确认 `CreateWithPixelFormat` 对 array depth-stencil desc 生成的是 array SRV；这是验证点，不是额外源码改动点。

### 3. `UnrealEngine5.4/Engine/Source/Runtime/Renderer/Private/SceneTextures.cpp`

第 426 行已经有正确的 MultiView 配置来源：

```cpp
SceneTexturesConfigInitSettings.bRequireMultiView = ViewFamily.bRequireMultiView && bAllViewsHaveMultiviewEnabled;
```

第 455-457 行 SceneDepth 已经按 `Config.bRequireMultiView` 创建 `Texture2DArray`。第 487-489 行 SceneColor 也已经按 `Config.bRequireMultiView` 创建 `Texture2DArray`。因此 CustomDepth 应复用同一个配置。

第 495 行当前调用：

```cpp
SceneTextures.CustomDepth = FCustomDepthTextures::Create(GraphBuilder, Config.Extent, Config.ShaderPlatform);
```

计划改为：

```cpp
SceneTextures.CustomDepth = FCustomDepthTextures::Create(GraphBuilder, Config.Extent, Config.ShaderPlatform, Config.bRequireMultiView);
```

第 937 行开始的 `SetupMobileSceneTextureUniformParameters(...)` 需要补 mobile uniform 的 array 绑定。

第 953-955 行当前默认值：

```cpp
SceneTextureParameters.CustomDepthTexture = SystemTextures.Black;
SceneTextureParameters.CustomDepthTextureSampler = TStaticSamplerState<>::GetRHI();
SceneTextureParameters.CustomStencilTexture = SystemTextures.StencilDummySRV;
```

计划在第 953 行后增加：

```cpp
SceneTextureParameters.CustomDepthTextureArray = GSystemTextures.GetDefaultTexture(GraphBuilder, ETextureDimension::Texture2DArray, PF_DepthStencil, FClearValueBinding::Black);
```

计划在第 955 行后增加：

```cpp
SceneTextureParameters.CustomStencilTextureArray = SystemTextures.StencilDummySRV;
```

第 1041-1043 行当前只绑定 2D 版本：

```cpp
bool bCustomDepthProduced = HasBeenProduced(CustomDepthTextures.Depth);
SceneTextureParameters.CustomDepthTexture = bCustomDepthProduced ? CustomDepthTextures.Depth : SystemTextures.DepthDummy;
SceneTextureParameters.CustomStencilTexture = bCustomDepthProduced ? CustomDepthTextures.Stencil : SystemTextures.StencilDummySRV;
```

计划改为 5.5 形式：

```cpp
bool bCustomDepthProduced = HasBeenProduced(CustomDepthTextures.Depth);
SceneTextureParameters.CustomDepthTexture = bCustomDepthProduced ? CustomDepthTextures.Depth : SystemTextures.DepthDummy;
SceneTextureParameters.CustomDepthTextureArray = bCustomDepthProduced ? CustomDepthTextures.Depth : SystemTextures.DepthDummy;
SceneTextureParameters.CustomStencilTexture = bCustomDepthProduced ? CustomDepthTextures.Stencil : SystemTextures.StencilDummySRV;
SceneTextureParameters.CustomStencilTextureArray = bCustomDepthProduced ? CustomDepthTextures.Stencil : SystemTextures.StencilDummySRV;
```

`Source\Runtime\Renderer\Private\SceneTextures.cpp:1039-1045`：

```cpp
if (EnumHasAnyFlags(SetupMode, EMobileSceneTextureSetupMode::CustomDepth))
{
	const FCustomDepthTextures& CustomDepthTextures = SceneTextures->CustomDepth;

	bool bCustomDepthProduced = HasBeenProduced(CustomDepthTextures.Depth);
	SceneTextureParameters.CustomDepthTexture = bCustomDepthProduced ? CustomDepthTextures.Depth : SystemTextures.DepthDummy;
	SceneTextureParameters.CustomStencilTexture = bCustomDepthProduced ? CustomDepthTextures.Stencil : SystemTextures.StencilDummySRV;
}
```

UE5.4 对应逻辑有两行额外绑定：

```cpp
if (EnumHasAnyFlags(SetupMode, EMobileSceneTextureSetupMode::CustomDepth))
{
	const FCustomDepthTextures& CustomDepthTextures = SceneTextures->CustomDepth;

	bool bCustomDepthProduced = HasBeenProduced(CustomDepthTextures.Depth);
	SceneTextureParameters.CustomDepthTexture = bCustomDepthProduced ? CustomDepthTextures.Depth : SystemTextures.DepthDummy;
	SceneTextureParameters.CustomDepthTextureArray = bCustomDepthProduced ? CustomDepthTextures.Depth : SystemTextures.DepthDummy;
	SceneTextureParameters.CustomStencilTexture = bCustomDepthProduced ? CustomDepthTextures.Stencil : SystemTextures.StencilDummySRV;
	SceneTextureParameters.CustomStencilTextureArray = bCustomDepthProduced ? CustomDepthTextures.Stencil : SystemTextures.StencilDummySRV;

}
```

注意：5.5 在 produced 分支的 fallback 也是 `SystemTextures.DepthDummy`，不是显式 2DArray dummy。先按 5.5 backport；如果 5.4 RDG 参数校验对 fallback texture dimension 更严格，再把 fallback 调整为和默认初始化一致的 2DArray default texture。

### 4. `UnrealEngine5.4/Engine/Source/Runtime/Engine/Public/SceneTexturesConfig.h`

`FMobileSceneTextureUniformParameters` 从第 40 行开始。第 48-50 行当前是：

```cpp
SHADER_PARAMETER_RDG_TEXTURE(Texture2D, CustomDepthTexture)
SHADER_PARAMETER_SAMPLER(SamplerState, CustomDepthTextureSampler)
SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<uint2>, CustomStencilTexture)
```

计划改为：

```cpp
SHADER_PARAMETER_RDG_TEXTURE(Texture2D, CustomDepthTexture)
SHADER_PARAMETER_RDG_TEXTURE(Texture2DArray, CustomDepthTextureArray)
SHADER_PARAMETER_SAMPLER(SamplerState, CustomDepthTextureSampler)
SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<uint2>, CustomStencilTexture)
SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2DArray<uint2>, CustomStencilTextureArray)
```

目的：让 shader 中的 `MobileSceneTextures.CustomDepthTextureArray` 和 `CustomStencilTextureArray` 有合法 uniform 绑定。

### 5. `UnrealEngine5.4/Engine/Shaders/Private/DeferredShadingCommon.ush`

第 801-821 行当前 `MobileFetchAndDecodeGBuffer` 只有 2 参数版本，并且 CustomDepth/Stencil 固定读取 2D：

```hlsl
FGBufferData MobileFetchAndDecodeGBuffer(in float2 UV, in float2 PixelPos)
{
	...
	GBuffer.CustomDepth = ConvertFromDeviceZ(Texture2DSample(MobileSceneTextures.CustomDepthTexture, MobileSceneTextures.CustomDepthTextureSampler, UV).r);
	GBuffer.CustomStencil = MobileSceneTextures.CustomStencilTexture.Load(int3(PixelPos.xy, 0)) STENCIL_COMPONENT_SWIZZLE;
	return GBuffer;
}
```

计划替换为 3 参数版本并保留 2 参数 wrapper：

```hlsl
FGBufferData MobileFetchAndDecodeGBuffer(in float2 UV, in float2 PixelPos, uint ViewId)
{
	FGBufferData GBuffer = (FGBufferData)0;
#if (MOBILE_DEFERRED_SHADING && IS_MOBILE_DEFERREDSHADING_SUBPASS && PIXELSHADER)
	float SceneDepth = 0;
	half4 GBufferA = 0;
	half4 GBufferB = 0;
	half4 GBufferC = 0;
	half4 GBufferD = 0;
	MobileFetchGBuffer(UV, GBufferA, GBufferB, GBufferC, GBufferD, SceneDepth);
	GBuffer = MobileDecodeGBuffer(GBufferA, GBufferB, GBufferC, GBufferD);
	GBuffer.Depth = SceneDepth;
#else
	GBuffer.Depth = CalcSceneDepth(UV);
#endif
#if MOBILE_MULTI_VIEW
	GBuffer.CustomDepth = ConvertFromDeviceZ(Texture2DArraySample(MobileSceneTextures.CustomDepthTextureArray, MobileSceneTextures.CustomDepthTextureSampler, float3(UV, ViewId)).r);
	GBuffer.CustomStencil = MobileSceneTextures.CustomStencilTextureArray.Load(int4(PixelPos.xy, ViewId, 0)) STENCIL_COMPONENT_SWIZZLE;
#else
	GBuffer.CustomDepth = ConvertFromDeviceZ(Texture2DSample(MobileSceneTextures.CustomDepthTexture, MobileSceneTextures.CustomDepthTextureSampler, UV).r);
	GBuffer.CustomStencil = MobileSceneTextures.CustomStencilTexture.Load(int3(PixelPos.xy, 0)) STENCIL_COMPONENT_SWIZZLE;
#endif
	return GBuffer;
}

FGBufferData MobileFetchAndDecodeGBuffer(in float2 UV, in float2 PixelPos)
{
	return MobileFetchAndDecodeGBuffer(UV, PixelPos, 0);
}
```

说明：5.5 把内部条件改成了 `#if MOBILE_DEFERRED_SHADING`，但 5.4 的 subpass 条件更窄。为降低 backport 风险，计划先保留 5.4 的 `#if (MOBILE_DEFERRED_SHADING && IS_MOBILE_DEFERREDSHADING_SUBPASS && PIXELSHADER)`，只补 CustomDepth array 读取。如果后续 mobile deferred shader 编译报宏路径不一致，再单独评估是否同步 5.5 的 broader 条件。

### 6. `UnrealEngine5.4/Engine/Shaders/Private/MaterialTemplate.ush`

第 489-493 行 `FMaterialPixelParameters` 当前只有 PrimitiveId/InstanceId：

```hlsl
uint PrimitiveId;

#if IS_NANITE_PASS
uint InstanceId;
#endif
```

计划在第 493 行后插入：

```hlsl
#if MOBILE_MULTI_VIEW
uint ViewId;
#endif
```

第 968-971 行 `GetScreenPosition(FMaterialPixelParameters Parameters)` 后、第 974 行 `GetPixelDepth(...)` 前，计划插入 5.5 新增的 helper：

```hlsl
uint GetViewId(FMaterialPixelParameters Parameters)
{
#if MOBILE_MULTI_VIEW
	return Parameters.ViewId;
#else
	return 0;
#endif
}
```

第 2852 行当前 mobile 材质 scene texture 查询调用：

```hlsl
FGBufferData GBuffer = MobileFetchAndDecodeGBuffer(UV, PixelPos);
```

计划改为：

```hlsl
FGBufferData GBuffer = MobileFetchAndDecodeGBuffer(UV, PixelPos, GetViewId(Parameters));
```

第 2881-2882 行 `PPI_CustomDepth` 返回 `GBuffer.CustomDepth`，第 2906-2907 行 `PPI_CustomStencil` 返回 `GBuffer.CustomStencil`，不需要改；它们会通过上面的 `GBuffer` 获取对应 eye/layer 的值。

非 mobile 的 `SceneTextureLookup(...)` 第 2984 行 `CalcSceneCustomDepth(UV)` 和第 2994 行 `CalcSceneCustomStencil(PixelPos)` 不改。这里是 deferred/non-mobile 路径，不是本次 Mobile MultiView bug 的入口。

### 7. `UnrealEngine5.4/Engine/Shaders/Private/MobileBasePassPixelShader.usf`

5.4 第 356-359 行当前创建 `FMaterialPixelParameters` 后直接计算 screen/world position：

```hlsl
FMaterialPixelParameters MaterialParameters = GetMaterialPixelParameters(Interpolants, SvPosition);
FPixelMaterialInputs PixelMaterialInputs;
{
	float4 ScreenPosition = SvPositionToResolvedScreenPosition(SvPosition);
```

计划在第 358 行 `{` 后、第 359 行 `float4 ScreenPosition...` 前插入：

```hlsl
#if MOBILE_MULTI_VIEW
	MaterialParameters.ViewId = BasePassInterpolants.MultiViewId;
#endif
```

原因：`BasePassInterpolants.MultiViewId` 已经在 shader 链路中存在，但 5.4 没有传入 `MaterialParameters`，导致材质 `SceneTexture:CustomDepth/CustomStencil` 只能按默认 view 0 读取。

### 8. `UnrealEngine5.4/Engine/Shaders/Private/MobileBasePassCommon.ush`

第 47-49 行已经有：

```hlsl
#if MOBILE_MULTI_VIEW
	nointerpolation uint MultiViewId : VIEW_ID;
#endif
```

不需要修改。这里已经提供 pixel shader 可用的 view id 插值。

### 9. `UnrealEngine5.4/Engine/Shaders/Private/MobileBasePassVertexShader.usf`

第 44-55 行已经在 Mobile MultiView 下接收 `SV_ViewID` 并写入 `Output.BasePassInterpolants.MultiViewId`：

```hlsl
#elif MOBILE_MULTI_VIEW
	, in uint ViewId : SV_ViewID
#endif
...
#elif MOBILE_MULTI_VIEW
	ResolvedView = ResolveView(ViewId);
	Output.BasePassInterpolants.MultiViewId = ViewId;
```

不需要修改。5.5 有其他 instanced stereo 条件整理，但不是 CustomDepth 单眼问题的必要修复。

## 可选审计点，不列入核心修复

`UnrealEngine5.4/Engine/Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp` 中 5.5 还有更广泛的 MultiViewCount 清理：

- 5.4 第 1517 行类似 `MainView.bIsMobileMultiViewEnabled ? 2 : (bIsMultiViewApplication ? 1 : 0)`，5.5 改为使用 `MainView.Aspects.IsMobileMultiViewEnabled()`。
- 5.4 第 1881 行 deferred render target binding 仍设置 `MultiViewCount = 0`，5.5 第 1933 行改成按 `MainView.bIsMobileMultiViewEnabled / MainView.Aspects.IsMobileMultiViewEnabled()` 设置。

这些属于 mobile renderer 更大范围的 multiview 修复。若目标只修 CustomDepth 在 Mobile MultiView 下单眼深度，核心修复应以上面 1-7 为准；如果项目同时使用 mobile deferred + multiview 并发现其他 render target 也只写单眼，再单独评估是否 backport 这些 renderer-level 改动。

## 验证计划

1. 编译 Renderer 与 Engine shader，确认 C++ uniform 参数和 shader 参数类型一致。
2. 清理/重编译 shader cache，至少覆盖 `MOBILE_MULTI_VIEW=1` 的 mobile base pass material permutation。
3. 在移动端 VR MultiView 场景中启用 CustomDepth/CustomStencil，对左右眼分别放置只写 CustomDepth 的对象。
4. 用材质 `SceneTexture:CustomDepth` 和 `SceneTexture:CustomStencil` 在 mobile base pass 中读取，确认左右眼读取各自 layer。
5. 用 RenderDoc/AGI 捕获 CustomDepth pass，确认 `CustomDepth` 为 `Texture2DArray`，array size 为 2，CustomDepth pass 的 multiview layer count 为 2。
6. 检查 GLES 路径下 CustomStencil copy 后的 SRV 维度；若 SRV 没有保留 array 维度，再针对第 403-410 行增加显式 array SRV desc，但 5.5 对这里没有源码改动，应优先验证后再决定。

## 最小补丁范围总结

核心必改 7 个文件：

1. `UnrealEngine5.4/Engine/Source/Runtime/Renderer/Internal/CustomDepthRendering.h`
2. `UnrealEngine5.4/Engine/Source/Runtime/Renderer/Private/CustomDepthRendering.cpp`
3. `UnrealEngine5.4/Engine/Source/Runtime/Renderer/Private/SceneTextures.cpp`
4. `UnrealEngine5.4/Engine/Source/Runtime/Engine/Public/SceneTexturesConfig.h`
5. `UnrealEngine5.4/Engine/Shaders/Private/DeferredShadingCommon.ush`
6. `UnrealEngine5.4/Engine/Shaders/Private/MaterialTemplate.ush`
7. `UnrealEngine5.4/Engine/Shaders/Private/MobileBasePassPixelShader.usf`

确认不需要修改但需要纳入分析的文件：

1. `UnrealEngine5.4/Engine/Shaders/Private/MobileBasePassCommon.ush`
2. `UnrealEngine5.4/Engine/Shaders/Private/MobileBasePassVertexShader.usf`
3. `UnrealEngine5.4/Engine/Shaders/Private/SceneTexturesCommon.ush`
---
SceneTexturesCommon.ush的CalcSceneCustomDepth
不改它的原因是：你看到的 CalcSceneCustomDepth 不是 mobile multiview 这条读取链路。

在 UnrealEngine5.4/Engine/Shaders/Private/SceneTexturesCommon.ush:14 里有两个区块：

SHADING_PATH_DEFERRED 区块：

- CalcSceneCustomDepth 在第 113 行。
- 它读取 SceneTexturesStruct.CustomDepthTexture。
- 这套 uniform 是 FSceneTextureUniformParameters，5.4/5.5 都只声明了 Texture2D
  CustomDepthTexture，不是 array。

SHADING_PATH_MOBILE 区块：

- 从第 142 行开始。
- 这里有带 ArrayIndex 的 CalcSceneDepth(ScreenUV, ArrayIndex)，读取
  MobileSceneTextures.SceneDepthTextureArray。

- 但这里没有 CalcSceneCustomDepth。mobile 的 CustomDepth/Stencil 读取是在
  UnrealEngine5.4/Engine/Shaders/Private/DeferredShadingCommon.ush:801 的
  MobileFetchAndDecodeGBuffer 里完成的。

所以你说得对：CalcSceneDepth 有通过 index 读数组，是因为它是 mobile scene depth 的公共
helper。CustomDepth 在 mobile 路径没有走 CalcSceneCustomDepth，而是走：

MaterialTemplate.ush mobile 分支
MobileSceneTextureLookup(...) -> MobileFetchAndDecodeGBuffer(...) ->
MobileSceneTextures.CustomDepthTexture/CustomStencilTexture

5.5 的修复也正是沿这个链路做的：

- 给 FMobileSceneTextureUniformParameters 增加 CustomDepthTextureArray /
  CustomStencilTextureArray

- MobileFetchAndDecodeGBuffer(UV, PixelPos, ViewId) 在 MOBILE_MULTI_VIEW 下读 array
  layer

- MaterialTemplate.ush 把 GetViewId(Parameters) 传进去

如果去改 CalcSceneCustomDepth，就不是最小修复了，因为它属于 deferred
SceneTexturesStruct，还要新增 deferred uniform 的 array 版本，并处理所有 deferred
SceneTexturesStruct.CustomDepthTexture 读取点。Epic 5.5/5.6 没这样做，说明这个 bug 的
目标修复面就是 mobile uniform + MobileFetchAndDecodeGBuffer，不是 deferred helper。

# 附加
### 4.1 Shader 文件 1：`Engine/Shaders/Private/DepthOnlyVertexShader.usf`

**改动 A — 入参（5.4 第 38-42 行）**

before：
```c
#if INSTANCED_STEREO && MOBILE_MULTI_VIEW
	, out uint LayerIndex : SV_RenderTargetArrayIndex
#elif INSTANCED_STEREO
	, out uint ViewportIndex : SV_ViewPortArrayIndex
#endif
	)
```

after（在两分支之间插入 `#elif MOBILE_MULTI_VIEW`）：
```c
#if INSTANCED_STEREO && MOBILE_MULTI_VIEW
	, out uint LayerIndex : SV_RenderTargetArrayIndex
#elif MOBILE_MULTI_VIEW
	, in nointerpolation uint ViewId : SV_ViewID
#elif INSTANCED_STEREO
	, out uint ViewportIndex : SV_ViewPortArrayIndex
#endif
	)
```

**改动 B — `ResolvedView` 解析（5.4 第 53 行，整个 `#if INSTANCED_STEREO ... #endif` 块之后的那一行）**

before（5.4 第 45-53 行）：
```c
#if INSTANCED_STEREO
	uint EyeIndex = GetEyeIndexFromVF(Input);
	#if MOBILE_MULTI_VIEW
		LayerIndex = EyeIndex;
	#else
		ViewportIndex = EyeIndex;
	#endif
#endif
	ResolvedView = ResolveViewFromVF(Input);
```

after：
```c
#if INSTANCED_STEREO
	uint EyeIndex = GetEyeIndexFromVF(Input);
	#if MOBILE_MULTI_VIEW
		LayerIndex = EyeIndex;
	#else
		ViewportIndex = EyeIndex;
	#endif
#endif
#if INSTANCED_STEREO
	ResolvedView = ResolveViewFromVF(Input);
#elif MOBILE_MULTI_VIEW
	ResolvedView = ResolveView(ViewId);
#else
	ResolvedView = ResolveView();
#endif
```

> 其余行（54-105）保持不变。`ResolveView()` 在 `!INSTANCED_STEREO` 时等价于原 `ResolveViewFromVF(Input)`（见 `VertexFactoryCommon.ush`），故非 multiview 平台行为不变。

### 4.2 Shader 文件 2：`Engine/Shaders/Private/PositionOnlyDepthVertexShader.usf`

整段替换 `Main`（5.4 第 11-37 行）为 5.5 版本：

before（5.4 第 11-37 行）：
```c
void Main(
	FPositionOnlyVertexFactoryInput Input,
	out INVARIANT_OUTPUT float4 OutPosition : SV_POSITION
#if USE_GLOBAL_CLIP_PLANE
	, out float OutGlobalClipPlaneDistance : SV_ClipDistance
#endif
#if INSTANCED_STEREO
	, out uint ViewportIndex : SV_ViewPortArrayIndex
#endif
	)
{
#if INSTANCED_STEREO
	const uint EyeIndex = GetEyeIndexFromVF(Input);
	ViewportIndex = EyeIndex;
#endif
	ResolvedView = ResolveViewFromVF(Input);

	float4 WorldPos = VertexFactoryGetWorldPosition(Input);

	{
		OutPosition = INVARIANT(mul(WorldPos, ResolvedView.TranslatedWorldToClip));
	}

#if USE_GLOBAL_CLIP_PLANE
	OutGlobalClipPlaneDistance = dot(ResolvedView.GlobalClippingPlane, float4(WorldPos.xyz, 1));
#endif
}
```

after（= 5.5 第 11-49 行，逐字移植）：
```c
void Main(
	FPositionOnlyVertexFactoryInput Input,
	out INVARIANT_OUTPUT float4 OutPosition : SV_POSITION
#if USE_GLOBAL_CLIP_PLANE
	, out float OutGlobalClipPlaneDistance : SV_ClipDistance
#endif
#if INSTANCED_STEREO && MOBILE_MULTI_VIEW // Mobile multi view fallback path
	, out uint LayerIndex : SV_RenderTargetArrayIndex
#elif MOBILE_MULTI_VIEW
	, in nointerpolation uint ViewId : SV_ViewID
#elif INSTANCED_STEREO
	, out uint ViewportIndex : SV_ViewPortArrayIndex
#endif
	)
{
#if INSTANCED_STEREO
	uint EyeIndex = GetEyeIndexFromVF(Input);
	ResolvedView = ResolveViewFromVF(Input);
	#if MOBILE_MULTI_VIEW
		LayerIndex = EyeIndex;
	#else
		ViewportIndex = EyeIndex;
	#endif
#elif MOBILE_MULTI_VIEW
	ResolvedView = ResolveView(ViewId);
#else
	ResolvedView = ResolveViewFromVF(Input);
#endif

	float4 WorldPos = VertexFactoryGetWorldPosition(Input);

	{
		OutPosition = INVARIANT(mul(WorldPos, ResolvedView.TranslatedWorldToClip));
	}

#if USE_GLOBAL_CLIP_PLANE
	OutGlobalClipPlaneDistance = dot(ResolvedView.GlobalClippingPlane, float4(WorldPos.xyz, 1));
#endif
}
```

> 注意 5.5 把 `ResolvedView = ResolveViewFromVF(Input);` 移进了 `#if INSTANCED_STEREO` 块内，并新增 `#elif MOBILE_MULTI_VIEW` / `#else`。务必整段替换，不要只加一行。