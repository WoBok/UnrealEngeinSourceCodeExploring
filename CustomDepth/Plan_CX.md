# UE 5.4 Mobile Multi-View CustomDepth 修复计划

## 范围

只做修复方案，不执行源码修改。目标是在 `UnrealEngine5.4` 上回迁 UE 5.5/5.6 对移动端 VR Multi-View CustomDepth 的关键修复，使 CustomDepth/CustomStencil 在左右眼都正确写入和采样。

## 结论

`UnrealEngine5.4/Engine/Source/Runtime/Renderer/Private/CustomDepthRendering.cpp:287` 的 `SetStereoViewport(RHICmdList, View, 1.0f)` 确实和立体渲染视口有关，但它本身不足以让移动端 Multi-View 正确写入双眼 CustomDepth。

5.4 的问题主要是三层：

1. `CustomDepth` 只创建为 `Texture2D`，没有在 Multi-View 下创建为 `Texture2DArray`。
2. CustomDepth mesh pass 没有设置 `PassParameters->RenderTargets.MultiViewCount`，导致 PSO/render pass 没有按 multiview 输出到多个 view/layer。
3. 移动端 shader 的 `SceneTexture:CustomDepth/CustomStencil` 采样路径仍只绑定和读取 2D texture，没有按 `ViewId` 读取 array layer。

UE 5.6 的源码已经覆盖了这些点。5.4 修复应以 5.6 的实现为准，但可根据 5.4 现有结构做最小回迁。

## 外部资料依据

- Epic 文档的 Mobile Multi-View 说明：Mobile Multi-View 是移动端优化的 stereo rendering path，类似桌面 Instanced Stereo。来源：<https://dev.epicgames.com/documentation/en-us/unreal-engine/xr-performance-features-in-unreal-engine>
- Khronos `GL_OVR_multiview` 规范说明 multiview 会同时渲染到 2D texture array 的多个元素/layer。来源：<https://registry.khronos.org/OpenGL/extensions/OVR/OVR_multiview.txt>

这和本次本地源码对比吻合：CustomDepth 在 5.4 中没有 array texture 和 MultiViewCount，因此只能得到单眼或错误 layer 的结果。

## 5.4 与 5.6 关键差异

### 1. CustomDepth texture 创建

5.4：

- `UnrealEngine5.4/Engine/Source/Runtime/Renderer/Internal/CustomDepthRendering.h:24`
  - `FCustomDepthTextures::Create` 只有 `GraphBuilder, CustomDepthExtent, ShaderPlatform` 三个参数。
- `UnrealEngine5.4/Engine/Source/Runtime/Renderer/Private/CustomDepthRendering.cpp:63`
  - definition 同样只有三个参数。
- `UnrealEngine5.4/Engine/Source/Runtime/Renderer/Private/CustomDepthRendering.cpp:86`
  - 使用 `FRDGTextureDesc::Create2D(...)` 创建单张 2D depth/stencil。

5.6：

- `UnrealEngine5.6/Engine/Source/Runtime/Renderer/Internal/CustomDepthRendering.h:24`
  - signature 增加 `bool bRequireMultiView, uint16 MobileMultiViewRenderTargetNumLayers`。
- `UnrealEngine5.6/Engine/Source/Runtime/Renderer/Private/CustomDepthRendering.cpp:63`
  - definition 同步增加上述参数。
- `UnrealEngine5.6/Engine/Source/Runtime/Renderer/Private/CustomDepthRendering.cpp:85`
  - 使用 `FRDGTextureDesc::CreateRenderTargetTextureDesc(..., bRequireMultiView, MobileMultiViewRenderTargetNumLayers)`，Multi-View 下会创建 `Texture2DArray`。

### 2. SceneTextures 创建 CustomDepth 时传入 Multi-View 信息

5.4：

- `UnrealEngine5.4/Engine/Source/Runtime/Renderer/Private/SceneTextures.cpp:495`
  - `FCustomDepthTextures::Create(GraphBuilder, Config.Extent, Config.ShaderPlatform);`

5.6：

- `UnrealEngine5.6/Engine/Source/Runtime/Renderer/Private/SceneTextures.cpp:497`
  - 调用时传入 `Config.bRequireMultiView, Config.MobileMultiViewRenderTargetNumLayers`。

### 3. CustomDepth pass 设置 MultiViewCount

5.4：

- `UnrealEngine5.4/Engine/Source/Runtime/Renderer/Private/CustomDepthRendering.cpp:273-277`
  - 只设置 `DepthStencil`。
- `UnrealEngine5.4/Engine/Source/Runtime/Renderer/Private/CustomDepthRendering.cpp:287`
  - 调用 `SetStereoViewport(...)`。
- 缺少 `PassParameters->RenderTargets.MultiViewCount`。

5.6：

- `UnrealEngine5.6/Engine/Source/Runtime/Renderer/Private/CustomDepthRendering.cpp:278`
  - 增加：

```cpp
PassParameters->RenderTargets.MultiViewCount = (View.bIsMobileMultiViewEnabled) ? 2 : (View.Aspects.IsMobileMultiViewEnabled() ? 1 : 0);
```

5.4 中 `UnrealEngine5.4/Engine/Source/Runtime/Renderer/Private/MeshPassProcessor.cpp:1619` 已经会把 `RenderTargetsInfo.MultiViewCount` 写入 `GraphicsPSOInit.MultiViewCount`，所以 CustomDepth pass 只要正确设置 `RenderTargets.MultiViewCount`，底层 PSO 路径已有承接点。

### 4. Mobile shader 采样 CustomDepth/CustomStencil

5.4：

- `UnrealEngine5.4/Engine/Source/Runtime/Engine/Public/SceneTexturesConfig.h:48-50`
  - `FMobileSceneTextureUniformParameters` 只有 `Texture2D CustomDepthTexture` 和 `Texture2D<uint2> CustomStencilTexture`。
- `UnrealEngine5.4/Engine/Source/Runtime/Renderer/Private/SceneTextures.cpp:953-955`
  - mobile scene texture 默认绑定只设置 2D custom depth/stencil。
- `UnrealEngine5.4/Engine/Source/Runtime/Renderer/Private/SceneTextures.cpp:1042-1043`
  - custom depth produced 后也只绑定 2D custom depth/stencil。
- `UnrealEngine5.4/Engine/Shaders/Private/DeferredShadingCommon.ush:801`
  - `MobileFetchAndDecodeGBuffer` 没有 `ViewId` 参数。
- `UnrealEngine5.4/Engine/Shaders/Private/DeferredShadingCommon.ush:817-818`
  - 只从 2D `CustomDepthTexture` / `CustomStencilTexture` 采样。
- `UnrealEngine5.4/Engine/Shaders/Private/MaterialTemplate.ush:2852`
  - 调用 `MobileFetchAndDecodeGBuffer(UV, PixelPos)`，没有传递 view/layer。

5.6：

- `UnrealEngine5.6/Engine/Source/Runtime/Engine/Public/SceneTexturesConfig.h:49`
  - 增加 `Texture2DArray CustomDepthTextureArray`。
- `UnrealEngine5.6/Engine/Source/Runtime/Engine/Public/SceneTexturesConfig.h:52`
  - 增加 `Texture2DArray<uint2> CustomStencilTextureArray`。
- `UnrealEngine5.6/Engine/Source/Runtime/Renderer/Private/SceneTextures.cpp:1156,1159`
  - 默认绑定 array texture/srv。
- `UnrealEngine5.6/Engine/Source/Runtime/Renderer/Private/SceneTextures.cpp:1252,1254`
  - custom depth produced 后绑定 array texture/srv。
- `UnrealEngine5.6/Engine/Shaders/Private/DeferredShadingCommon.ush:808`
  - `MobileFetchAndDecodeGBuffer` 增加 `uint ViewId` 参数。
- `UnrealEngine5.6/Engine/Shaders/Private/DeferredShadingCommon.ush:823-826`
  - `#if MOBILE_MULTI_VIEW` 下从 `CustomDepthTextureArray` / `CustomStencilTextureArray` 按 `ViewId` 采样。
- `UnrealEngine5.6/Engine/Shaders/Private/MaterialTemplate.ush:528-530`
  - `FMaterialPixelParameters` 增加 `ViewId`。
- `UnrealEngine5.6/Engine/Shaders/Private/MaterialTemplate.ush:1099-1106`
  - 增加 `GetViewId(...)`。
- `UnrealEngine5.6/Engine/Shaders/Private/MaterialTemplate.ush:3029`
  - 调用 `MobileFetchAndDecodeGBuffer(UV, PixelPos, GetViewId(Parameters))`。
- `UnrealEngine5.6/Engine/Shaders/Private/MobileBasePassPixelShader.usf:368-370`
  - Multi-View 下把 eye index 写入 `MaterialParameters.ViewId`。

## 推荐修复方案

优先采用“最小回迁 + 必要 shader 修复”。除非希望彻底对齐 5.6 的 RenderGraph helper，否则不必大范围重构 5.4 已有 scene color/depth 创建逻辑。

### 必改 1：CustomDepthRendering.h

文件：

- `UnrealEngine5.4/Engine/Source/Runtime/Renderer/Internal/CustomDepthRendering.h`

位置：

- `:24`

修改计划：

将：

```cpp
static FCustomDepthTextures Create(FRDGBuilder& GraphBuilder, FIntPoint CustomDepthExtent, EShaderPlatform ShaderPlatform);
```

改为：

```cpp
static FCustomDepthTextures Create(
	FRDGBuilder& GraphBuilder,
	FIntPoint CustomDepthExtent,
	EShaderPlatform ShaderPlatform,
	bool bRequireMultiView,
	uint16 MobileMultiViewRenderTargetNumLayers);
```

目的：

- 让 CustomDepth texture 创建阶段知道当前 scene textures 是否要求 Multi-View。
- 和 UE 5.6 的接口保持一致，方便后续维护和对比。

### 必改 2：CustomDepthRendering.cpp 创建 Texture2DArray

文件：

- `UnrealEngine5.4/Engine/Source/Runtime/Renderer/Private/CustomDepthRendering.cpp`

位置：

- `:63`
- `:86`

修改计划：

1. 将 `:63` 的 `FCustomDepthTextures::Create` definition 参数同步改成 header 中的新 signature。

2. 将 `:86` 的 `Create2D` 替换为 Multi-View aware 创建逻辑。

5.4 最小改法：

```cpp
const FRDGTextureDesc CustomDepthDesc = bRequireMultiView
	? FRDGTextureDesc::Create2DArray(
		CustomDepthExtent,
		PF_DepthStencil,
		FClearValueBinding::DepthFar,
		CreateFlags,
		MobileMultiViewRenderTargetNumLayers)
	: FRDGTextureDesc::Create2D(
		CustomDepthExtent,
		PF_DepthStencil,
		FClearValueBinding::DepthFar,
		CreateFlags);
```

目的：

- Multi-View 下把 CustomDepth 从单张 `Texture2D` 改为 `Texture2DArray`。
- array layer 数使用 `MobileMultiViewRenderTargetNumLayers`，默认应为 2。

注意：

- `CustomDepthTextures.Depth`、`CustomDepthTextures.Stencil` 后续仍使用同一个 RDG texture 派生 SRV，通常不需要额外改名。
- `CreateFlags` 保持原逻辑，确保 stencil SRV、UAV、memoryless 等平台分支不变。

### 必改 3：CustomDepth pass 设置 RenderTargets.MultiViewCount

文件：

- `UnrealEngine5.4/Engine/Source/Runtime/Renderer/Private/CustomDepthRendering.cpp`

位置：

- 在 `:273-277` 设置 `PassParameters->RenderTargets.DepthStencil` 之后
- 在 `:279` `const FMeshPassProcessorRenderState DrawRenderState(PassParameters->RenderTargets);` 之前

修改计划：

插入：

```cpp
PassParameters->RenderTargets.MultiViewCount = (View.bIsMobileMultiViewEnabled) ? 2 : (View.Aspects.IsMobileMultiViewEnabled() ? 1 : 0);
```

目的：

- 让 CustomDepth pass 的 render target binding 带上 multiview view count。
- 5.4 的 `UnrealEngine5.4/Engine/Source/Runtime/Renderer/Private/MeshPassProcessor.cpp:1619` 已经会把 `RenderTargetsInfo.MultiViewCount` 传给 `GraphicsPSOInit.MultiViewCount`，此处是 CustomDepth pass 缺失的关键连接点。

注意：

- `SetStereoViewport(RHICmdList, View, 1.0f)` 保留，不替代此修改。
- 这行是 5.6 的直接修复点，建议保持同样表达式，避免和 `View.Aspects` 的特殊情况产生偏差。

### 必改 4：SceneTextures.cpp 调用 CustomDepth Create 时传入 Multi-View 信息

文件：

- `UnrealEngine5.4/Engine/Source/Runtime/Renderer/Private/SceneTextures.cpp`

位置：

- `:495`

修改计划：

若按下面“必改 5”添加 `Config.MobileMultiViewRenderTargetNumLayers`，将：

```cpp
SceneTextures.CustomDepth = FCustomDepthTextures::Create(GraphBuilder, Config.Extent, Config.ShaderPlatform);
```

改为：

```cpp
SceneTextures.CustomDepth = FCustomDepthTextures::Create(GraphBuilder, Config.Extent, Config.ShaderPlatform, Config.bRequireMultiView, Config.MobileMultiViewRenderTargetNumLayers);
```

如果不添加 `MobileMultiViewRenderTargetNumLayers` 字段，则最小方案可临时写为：

```cpp
SceneTextures.CustomDepth = FCustomDepthTextures::Create(GraphBuilder, Config.Extent, Config.ShaderPlatform, Config.bRequireMultiView, 2);
```

推荐使用第一种，和 5.6 对齐。

### 必改 5：SceneTexturesConfig.h 增加 mobile custom depth array uniform

文件：

- `UnrealEngine5.4/Engine/Source/Runtime/Engine/Public/SceneTexturesConfig.h`

位置：

- `:48-50`

修改计划：

将 mobile uniform buffer 中 CustomDepth/CustomStencil 参数从只有 2D 扩展为 2D + 2DArray。

目标结构：

```cpp
SHADER_PARAMETER_RDG_TEXTURE(Texture2D, CustomDepthTexture)
SHADER_PARAMETER_RDG_TEXTURE(Texture2DArray, CustomDepthTextureArray)
SHADER_PARAMETER_SAMPLER(SamplerState, CustomDepthTextureSampler)
SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<uint2>, CustomStencilTexture)
SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2DArray<uint2>, CustomStencilTextureArray)
```

目的：

- shader 中 `MOBILE_MULTI_VIEW` 分支需要 array texture/srv 绑定。
- 保留原 2D 成员，避免非 Multi-View 移动路径和已有 shader 代码被破坏。

额外推荐修改：

在 `FSceneTexturesConfig` 中增加 5.6 同名字段：

```cpp
uint32 MobileMultiViewRenderTargetNumLayers = 2;
```

5.6 中该字段位于：

- `UnrealEngine5.6/Engine/Source/Runtime/Engine/Public/SceneTexturesConfig.h:221`

5.4 可放在同类配置字段附近，建议在 `bRequireMultiView` 附近。5.4 本地 `bRequireMultiView` 位于：

- `UnrealEngine5.4/Engine/Source/Runtime/Engine/Public/SceneTexturesConfig.h:188`

建议插入位置：

- `UnrealEngine5.4/Engine/Source/Runtime/Engine/Public/SceneTexturesConfig.h:189`

目的：

- 避免在 `SceneTextures.cpp` 中继续硬编码 `2`。
- 方便将来如果平台需要不同 layer count，可以沿用 5.6 的配置入口。

### 必改 6：SceneTextures.cpp 绑定 mobile custom depth array uniform

文件：

- `UnrealEngine5.4/Engine/Source/Runtime/Renderer/Private/SceneTextures.cpp`

位置：

- 默认绑定：`:953-955`
- CustomDepth produced 后绑定：`:1042-1043`

修改计划：

1. 在默认绑定处增加 array fallback。

5.4 当前：

```cpp
MobileSceneTextures->CustomDepthTexture = SystemTextures.Black;
MobileSceneTextures->CustomDepthTextureSampler = SystemTextures.GetDefaultSampler(SF_Point, AM_Clamp);
MobileSceneTextures->CustomStencilTexture = SystemTextures.StencilDummySRV;
```

目标：

```cpp
MobileSceneTextures->CustomDepthTexture = SystemTextures.Black;
MobileSceneTextures->CustomDepthTextureArray = GSystemTextures.GetDefaultTexture(GraphBuilder, ETextureDimension::Texture2DArray, PF_DepthStencil, FClearValueBinding::Black);
MobileSceneTextures->CustomDepthTextureSampler = SystemTextures.GetDefaultSampler(SF_Point, AM_Clamp);
MobileSceneTextures->CustomStencilTexture = SystemTextures.StencilDummySRV;
MobileSceneTextures->CustomStencilTextureArray = SystemTextures.StencilDummySRV;
```

2. 在 produced 绑定处增加 array texture/srv。

5.4 当前：

```cpp
OutMobileSceneTextures.CustomDepthTexture = bCustomDepthProduced ? CustomDepthTextures.Depth : SystemTextures.DepthDummy;
OutMobileSceneTextures.CustomStencilTexture = bCustomDepthProduced && CustomDepthTextures.Stencil.IsValid() ? CustomDepthTextures.Stencil : SystemTextures.StencilDummySRV;
```

目标：

```cpp
OutMobileSceneTextures.CustomDepthTexture = bCustomDepthProduced ? CustomDepthTextures.Depth : SystemTextures.DepthDummy;
OutMobileSceneTextures.CustomDepthTextureArray = bCustomDepthProduced ? CustomDepthTextures.Depth : SystemTextures.DepthDummy;
OutMobileSceneTextures.CustomStencilTexture = bCustomDepthProduced && CustomDepthTextures.Stencil.IsValid() ? CustomDepthTextures.Stencil : SystemTextures.StencilDummySRV;
OutMobileSceneTextures.CustomStencilTextureArray = bCustomDepthProduced && CustomDepthTextures.Stencil.IsValid() ? CustomDepthTextures.Stencil : SystemTextures.StencilDummySRV;
```

注意：

- 这里沿用 5.6 写法。虽然 fallback `DepthDummy` / `StencilDummySRV` 的实际维度需要由 RDG shader parameter validation 覆盖验证，但 5.6 源码就是这样绑定。
- 增加 uniform 成员后需要触发 shader 重新编译。

### 必改 7：DeferredShadingCommon.ush 按 ViewId 采样 custom depth/stencil array

文件：

- `UnrealEngine5.4/Engine/Shaders/Private/DeferredShadingCommon.ush`

位置：

- `:801`
- `:817-818`

修改计划：

1. 将函数签名从：

```hlsl
FGBufferData MobileFetchAndDecodeGBuffer(in float2 UV, in float2 PixelPos)
```

改为：

```hlsl
FGBufferData MobileFetchAndDecodeGBuffer(in float2 UV, in float2 PixelPos, uint ViewId)
```

2. 将 custom depth/stencil 采样改为：

```hlsl
#if MOBILE_MULTI_VIEW
	GBuffer.CustomDepth = ConvertFromDeviceZ(Texture2DArraySample(MobileSceneTextures.CustomDepthTextureArray, MobileSceneTextures.CustomDepthTextureSampler, float3(UV, ViewId)).r);
	GBuffer.CustomStencil = MobileSceneTextures.CustomStencilTextureArray.Load(int4(PixelPos.xy, ViewId, 0)) STENCIL_COMPONENT_SWIZZLE;
#else
	GBuffer.CustomDepth = ConvertFromDeviceZ(Texture2DSample(MobileSceneTextures.CustomDepthTexture, MobileSceneTextures.CustomDepthTextureSampler, UV).r);
	GBuffer.CustomStencil = MobileSceneTextures.CustomStencilTexture.Load(int3(PixelPos.xy, 0)) STENCIL_COMPONENT_SWIZZLE;
#endif
```

3. 为兼容其他旧调用点，在该函数后增加 wrapper overload：

```hlsl
FGBufferData MobileFetchAndDecodeGBuffer(in float2 UV, in float2 PixelPos)
{
	return MobileFetchAndDecodeGBuffer(UV, PixelPos, 0);
}
```

目的：

- Multi-View 右眼必须读取 array layer 1，而不是继续读取 2D texture 或 layer 0。
- wrapper 可以降低遗漏调用点导致的 shader compile 风险。

注意：

- 5.6 非 Multi-View 分支的 2D sample 代码形态和 5.4 不完全一样。回迁到 5.4 时，非 Multi-View 分支建议保持 5.4 原本 `Texture2DSample(..., UV)`，不要无意义改成 `float3(UV, ViewId)`。

### 必改 8：MaterialTemplate.ush 给 material pixel parameters 增加 ViewId

文件：

- `UnrealEngine5.4/Engine/Shaders/Private/MaterialTemplate.ush`

位置：

- `:376` `struct FMaterialPixelParameters` 起始
- `:489` `uint PrimitiveId;`
- `:491-493` `#if IS_NANITE_PASS ... #endif`
- `:963-970` `GetScreenPosition(FMaterialPixelParameters Parameters)` 附近
- `:2852`

修改计划：

1. 在 `FMaterialPixelParameters` 中添加 `ViewId`。

建议插入在 `PrimitiveId` / `InstanceId` 附近：

```hlsl
#if MOBILE_MULTI_VIEW
	uint ViewId;
#endif
```

2. 增加 helper：

建议放在 `GetScreenPosition(FMaterialPixelParameters Parameters)` 后：

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

3. 将 `:2852` 的调用从：

```hlsl
FGBufferData GBuffer = MobileFetchAndDecodeGBuffer(UV, PixelPos);
```

改为：

```hlsl
FGBufferData GBuffer = MobileFetchAndDecodeGBuffer(UV, PixelPos, GetViewId(Parameters));
```

目的：

- 让 material 的 `SceneTexture:CustomDepth` / `SceneTexture:CustomStencil` 节点在移动 Multi-View 下能得到当前 eye/layer。

### 必改 9：MobileBasePassPixelShader.usf 写入 MaterialParameters.ViewId

文件：

- `UnrealEngine5.4/Engine/Shaders/Private/MobileBasePassPixelShader.usf`

位置：

- `:342`
- `:356`
- `:358`

5.4 已有：

```hlsl
ResolvedView = ResolveView(BasePassInterpolants.MultiViewId);
...
FMaterialPixelParameters MaterialParameters = GetMaterialPixelParameters(...);
...
float4 ScreenPosition = SvPositionToResolvedScreenPosition(MaterialParameters.SvPosition);
```

修改计划：

在 `FMaterialPixelParameters MaterialParameters = ...` 之后、使用 material parameters 之前插入：

```hlsl
#if MOBILE_MULTI_VIEW
	MaterialParameters.ViewId = BasePassInterpolants.MultiViewId;
#endif
```

建议插入点：

- `UnrealEngine5.4/Engine/Shaders/Private/MobileBasePassPixelShader.usf:358` 之后
- `float4 ScreenPosition = ...` 之前

目的：

- 5.4 的 mobile base pass interpolants 已经带有 `MultiViewId`：
  - `UnrealEngine5.4/Engine/Shaders/Private/MobileBasePassCommon.ush:47-49`
- 此处只需要把它传入 `FMaterialPixelParameters.ViewId`。
- 这对应 5.6 的 `MobileBasePassPixelShader.usf:368-370`，但 5.4 可直接使用已有 `BasePassInterpolants.MultiViewId`。

## 可选增强：完全对齐 5.6 的 RenderGraph helper

这部分不是最小修复必需，但如果希望后续和 5.6 更一致，建议一起回迁。

### 可选 1：RenderGraphDefinitions.h 添加 CreateRenderTargetTextureDesc

文件：

- `UnrealEngine5.4/Engine/Source/Runtime/RenderCore/Public/RenderGraphDefinitions.h`

5.6 参考：

- `UnrealEngine5.6/Engine/Source/Runtime/RenderCore/Public/RenderGraphDefinitions.h:709-721`

修改计划：

添加静态 helper：

```cpp
static FRDGTextureDesc CreateRenderTargetTextureDesc(
	FIntPoint Extent,
	EPixelFormat Format,
	const FClearValueBinding& ClearValue,
	ETextureCreateFlags InFlags,
	bool bRequireMultiView,
	uint16 MobileMultiViewRenderTargetNumLayers)
{
	return bRequireMultiView
		? Create2DArray(Extent, Format, ClearValue, InFlags, MobileMultiViewRenderTargetNumLayers)
		: Create2D(Extent, Format, ClearValue, InFlags);
}
```

目的：

- 和 5.6 的 `CustomDepthRendering.cpp` 写法完全一致。
- 后续可逐步消除 `SceneTextures.cpp` 中 scene color/depth 的硬编码 layer count。

### 可选 2：SceneTextures.cpp 替换已有硬编码 Multi-View texture 创建

文件：

- `UnrealEngine5.4/Engine/Source/Runtime/Renderer/Private/SceneTextures.cpp`

可选检查点：

- `:455-457` SceneDepth 已有 `Create2DArray(..., 2)`。
- `:487-489` SceneColor 已有 `Create2DArray(..., 2)`。
- `:604-606` SceneDepthAux 已有 `Create2DArray(..., 2)`。

计划：

- 如果已添加 `Config.MobileMultiViewRenderTargetNumLayers` 和 `CreateRenderTargetTextureDesc`，可以把这些硬编码 `2` 一并替换为 config 字段/helper。
- 如果目标是最小风险修复 CustomDepth，则这些位置可以不改，因为 5.4 scene color/depth 本身已经支持 Multi-View array。

## 需要特别检查的相关路径

### GLES stencil copy path

文件：

- `UnrealEngine5.4/Engine/Source/Runtime/Renderer/Private/CustomDepthRendering.cpp`

位置：

- `:397-410`

现状：

- 在不支持 texture view 的 GLES 路径，会创建 `CustomStencil` 并通过 `AddCopyTexturePass` 从 `CustomDepthTextures.Depth` 拷贝。
- 因为目标 desc 使用 `CustomDepthTextures.Depth->Desc`，CustomDepth 改成 `Texture2DArray` 后，CustomStencil 理论上也会继承 array desc。

计划：

- 第一版不需要单独修改。
- 验证时必须在目标 RHI 上确认 `AddCopyTexturePass` 是否正确处理 2DArray depth/stencil。
- 如果 GLES 平台验证发现只复制 layer 0，则需要补一个 layer-by-layer copy 或 RHI 支持分支。

### Nanite custom depth

文件：

- `UnrealEngine5.4/Engine/Source/Runtime/Renderer/Private/CustomDepthRendering.cpp`

位置：

- `:350` 附近 Nanite `EmitCustomDepthStencilTargets`

现状：

- 5.4 与 5.6 对 Nanite custom depth 没看到同类 MultiViewCount 差异。
- 移动 VR 通常不走 Nanite，但如果项目启用 mobile Nanite，需要单独验证。

计划：

- 本轮修复不主动改 Nanite path。
- 验证时确认 Nanite 对象在 CustomDepth 中左右眼是否都写入。

## 修改清单汇总

必改文件：

1. `UnrealEngine5.4/Engine/Source/Runtime/Renderer/Internal/CustomDepthRendering.h`
   - `:24` 修改 `FCustomDepthTextures::Create` signature。

2. `UnrealEngine5.4/Engine/Source/Runtime/Renderer/Private/CustomDepthRendering.cpp`
   - `:63` 修改 `FCustomDepthTextures::Create` definition signature。
   - `:86` `Create2D` 改为 Multi-View 下 `Create2DArray`。
   - `:278` 附近新增 `PassParameters->RenderTargets.MultiViewCount = ...`。

3. `UnrealEngine5.4/Engine/Source/Runtime/Renderer/Private/SceneTextures.cpp`
   - `:495` 调用 `FCustomDepthTextures::Create` 时传入 `Config.bRequireMultiView` 和 layer count。
   - `:953-955` mobile scene texture 默认绑定增加 `CustomDepthTextureArray` / `CustomStencilTextureArray`。
   - `:1042-1043` produced 绑定增加 `CustomDepthTextureArray` / `CustomStencilTextureArray`。

4. `UnrealEngine5.4/Engine/Source/Runtime/Engine/Public/SceneTexturesConfig.h`
   - `:48-50` `FMobileSceneTextureUniformParameters` 增加 array texture/srv 参数。
   - `:189` 附近建议增加 `MobileMultiViewRenderTargetNumLayers = 2`。

5. `UnrealEngine5.4/Engine/Shaders/Private/DeferredShadingCommon.ush`
   - `:801` `MobileFetchAndDecodeGBuffer` 增加 `uint ViewId` 参数。
   - `:817-818` CustomDepth/CustomStencil 在 `MOBILE_MULTI_VIEW` 下改为 array 采样。
   - 函数后增加旧签名 wrapper。

6. `UnrealEngine5.4/Engine/Shaders/Private/MaterialTemplate.ush`
   - `:489-493` 附近给 `FMaterialPixelParameters` 增加 `ViewId`。
   - `:963-970` 附近增加 `GetViewId(...)` helper。
   - `:2852` `MobileFetchAndDecodeGBuffer` 调用传入 `GetViewId(Parameters)`。

7. `UnrealEngine5.4/Engine/Shaders/Private/MobileBasePassPixelShader.usf`
   - `:358` 附近给 `MaterialParameters.ViewId` 赋 `BasePassInterpolants.MultiViewId`。

可选文件：

8. `UnrealEngine5.4/Engine/Source/Runtime/RenderCore/Public/RenderGraphDefinitions.h`
   - 回迁 5.6 `CreateRenderTargetTextureDesc` helper。

9. `UnrealEngine5.4/Engine/Source/Runtime/Renderer/Private/SceneTextures.cpp`
   - `:455-457`, `:487-489`, `:604-606` 可选替换硬编码 `2`。

## 验证计划

### 编译验证

1. 编译 `Renderer` 模块。
2. 如果回迁 `RenderGraphDefinitions.h` helper，额外关注 `RenderCore` 相关编译。
3. 清理或刷新 shader DDC，确保以下 shader 重新编译：
   - `DeferredShadingCommon.ush`
   - `MaterialTemplate.ush`
   - `MobileBasePassPixelShader.usf`
4. 启动目标移动 VR 配置，确认 mobile scene texture uniform buffer layout 没有 RDG validation / shader parameter mismatch。

### 运行验证

测试条件：

- 启用 Mobile Multi-View。
- 使用目标移动 VR RHI，例如 Android Vulkan / Quest 平台。
- 场景中放置写入 CustomDepth/CustomStencil 的 mesh。
- 使用 material 或 post process 读取 `SceneTexture:CustomDepth` / `SceneTexture:CustomStencil`，左右眼都可观察。

应验证：

1. RenderDoc / AGI 中 `CustomDepth` texture dimension 为 `Texture2DArray`，array size 为 2。
2. CustomDepth pass 的 render pass / PSO multiview count 为 2。
3. layer 0 和 layer 1 都有正确 CustomDepth 内容。
4. 右眼读取 CustomDepth 时采样 layer 1，而不是 layer 0。
5. 如果使用 CustomStencil，左右眼 stencil 值也一致正确。
6. 非 Multi-View mobile 路径仍使用 2D texture，不能回归。
7. 桌面 deferred / non-mobile 路径不受影响。

### 回归风险

1. `FMobileSceneTextureUniformParameters` 增加成员会导致 shader ABI 变化，必须完整重编译 shader。
2. 如果只改 texture 创建和 `MultiViewCount`，不改 shader array 采样，则 CustomDepth 写入可能正确，但材质读取右眼仍可能读错。
3. GLES stencil copy path 需要真机验证 `AddCopyTexturePass` 对 array depth/stencil 的行为。
4. Nanite custom depth 在移动 VR 项目中如果启用，需要单独验证，不应假定由 base pass 修改覆盖。

## 建议实施顺序

1. 先改 C++ texture 创建链路：
   - `SceneTexturesConfig.h`
   - `CustomDepthRendering.h`
   - `CustomDepthRendering.cpp`
   - `SceneTextures.cpp`
2. 再改 mobile uniform 绑定：
   - `SceneTexturesConfig.h`
   - `SceneTextures.cpp`
3. 最后改 shader view/layer 传递和采样：
   - `MaterialTemplate.ush`
   - `MobileBasePassPixelShader.usf`
   - `DeferredShadingCommon.ush`
4. 编译后先做非 Multi-View mobile smoke test，再做 Multi-View VR 真机验证。

## 最小补丁边界

如果只想修复用户描述的“5.4 移动端 VR Multi-View CustomDepth 只渲染单眼深度”问题，最小必需边界是：

- CustomDepth texture 在 Multi-View 下必须是 `Texture2DArray`。
- CustomDepth pass 必须设置 `RenderTargets.MultiViewCount`。
- mobile shader 必须能按当前 `ViewId` 读取 `CustomDepthTextureArray` / `CustomStencilTextureArray`。

只改 `SetStereoViewport` 附近是不完整的；`SetStereoViewport` 只能解决 viewport/stereo view rect，不能让单张 2D CustomDepth 变成双 layer，也不能让 shader 自动采样正确 layer。
