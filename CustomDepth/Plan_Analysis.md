# Plan_CX_5.5 CustomDepth Mobile MultiView 分析结果

## 范围

- 当前工程：`E:\Unreal Engine Work Projects\MR01_DaNaoTianGong_Main\Engine`
- 当前工程版本：`Build\Build.version` 为 UE 5.4.4
- 对比源码：`E:\Unreal Engine Work Projects\UnrealEngine\UnrealEngine5.5\Engine`
- 对比源码版本：UE 5.5.4
- 分析方式：只做源码比对和链路分析；未编译，未做设备运行或 RenderDoc/AGI GPU capture。

## 总结结论

当前修改没有全部完成，且 Mobile Forward 下目前不能判定为正确。

最关键的未完成项在 `Source\Runtime\Renderer\Private\SceneTextures.cpp`：当前只在默认初始化时设置了 `CustomDepthTextureArray` 和 `CustomStencilTextureArray`，但在 `CustomDepth` 已 produced 的分支没有把 array uniform 绑定到实际的 `CustomDepthTextures.Depth` / `CustomDepthTextures.Stencil`。Mobile MultiView shader 已经改为读取 `MobileSceneTextures.CustomDepthTextureArray` 和 `CustomStencilTextureArray`，所以在当前状态下 shader 会读默认 array 纹理和 dummy stencil，而不是 CustomDepth pass 写入的结果。

此外，`CustomDepthRendering.cpp` 的 `MultiViewCount` 与 UE5.5/计划不完全一致；Mobile Forward 主 base pass 有 multiview 设置，但 full depth prepass 仍缺少 UE5.5 的 `MultiViewCount` 设置。后者不属于 CustomDepth 读取链路的核心补丁，但会影响开启 full depth prepass 时的 Mobile Forward multiview 正确性。

## 核心修改完成情况

| 项目 | 当前状态 | 结论 |
| --- | --- | --- |
| `CustomDepthRendering.h` 增加 `bRequireMultiView` 参数 | 已完成，当前声明为 `Create(..., bool bRequireMultiView)` | 正确 |
| `CustomDepthRendering.cpp` 创建 `Texture2DArray` | 已完成，`bRequireMultiView ? Create2DArray(..., 2) : Create2D(...)` | 正确 |
| `CustomDepthRendering.cpp` pass `MultiViewCount` | 当前为 `View.bIsMobileMultiViewEnabled ? 2 : 0` | 部分完成，少了 UE5.5 的 aspect-only fallback |
| `SceneTextures.cpp` 调用 `FCustomDepthTextures::Create(..., Config.bRequireMultiView)` | 已完成 | 正确 |
| `SceneTextures.cpp` mobile uniform 默认 array 绑定 | 已完成 | 正确 |
| `SceneTextures.cpp` mobile uniform produced 分支 array 绑定 | 未完成 | 阻断问题 |
| `SceneTexturesConfig.h` 增加 array uniform 成员 | 已完成 | 正确 |
| `DeferredShadingCommon.ush` 增加 `ViewId` 并按 array 读取 | 已完成 | Forward 路径正确 |
| `MaterialTemplate.ush` 增加 `ViewId` 并传给 `MobileFetchAndDecodeGBuffer` | 已完成 | 正确 |
| `MobileBasePassPixelShader.usf` 写入 `MaterialParameters.ViewId` | 已完成 | 正确 |
| `MobileBasePassCommon.ush` / `MobileBasePassVertexShader.usf` 提供 `MultiViewId` | 已存在 | 正确 |
| `SceneTexturesCommon.ush` 不修改 | 符合计划 | 正确 |

## 关键缺口 1：Mobile uniform produced 分支没有绑定 array

当前工程 `Source\Runtime\Renderer\Private\SceneTextures.cpp:1039-1045`：

```cpp
if (EnumHasAnyFlags(SetupMode, EMobileSceneTextureSetupMode::CustomDepth))
{
	const FCustomDepthTextures& CustomDepthTextures = SceneTextures->CustomDepth;

	bool bCustomDepthProduced = HasBeenProduced(CustomDepthTextures.Depth);
	SceneTextureParameters.CustomDepthTexture = bCustomDepthProduced ? CustomDepthTextures.Depth : SystemTextures.DepthDummy;
	SceneTextureParameters.CustomStencilTexture = bCustomDepthProduced ? CustomDepthTextures.Stencil : SystemTextures.StencilDummySRV;
}
```

UE5.5.4 对应逻辑有两行额外绑定：

```cpp
SceneTextureParameters.CustomDepthTextureArray = bCustomDepthProduced ? CustomDepthTextures.Depth : SystemTextures.DepthDummy;
SceneTextureParameters.CustomStencilTextureArray = bCustomDepthProduced ? CustomDepthTextures.Stencil : SystemTextures.StencilDummySRV;
```

当前工程通过 `rg` 只找到两处 array 赋值：

- `SceneTextures.cpp:954` 默认 `CustomDepthTextureArray`
- `SceneTextures.cpp:957` 默认 `CustomStencilTextureArray`

没有 produced 分支赋值。因此当 `SetupMode` 包含 `EMobileSceneTextureSetupMode::CustomDepth` 时，2D uniform 会更新为实际 CustomDepth，但 array uniform 仍停留在默认值。

这会直接破坏已改好的 shader 链路：

- `DeferredShadingCommon.ush:818` 在 `MOBILE_MULTI_VIEW` 下读取 `MobileSceneTextures.CustomDepthTextureArray`
- `DeferredShadingCommon.ush:819` 读取 `MobileSceneTextures.CustomStencilTextureArray`
- `MaterialTemplate.ush:2865` 已把 `GetViewId(Parameters)` 传入
- `MobileBasePassPixelShader.usf:360` 已设置 `MaterialParameters.ViewId`

也就是说，C++ uniform 绑定没有把实际资源交给 shader。这个问题不只是左右眼 layer 错误，而是 mobile multiview 下 CustomDepth/CustomStencil array 读取大概率一直是默认纹理或 dummy stencil。

## 关键缺口 2：CustomDepth pass 的 MultiViewCount 只完成了一半

当前工程 `Source\Runtime\Renderer\Private\CustomDepthRendering.cpp:281`：

```cpp
PassParameters->RenderTargets.MultiViewCount = View.bIsMobileMultiViewEnabled ? 2 : 0;
```

UE5.5.4 对应逻辑：

```cpp
PassParameters->RenderTargets.MultiViewCount = (View.bIsMobileMultiViewEnabled) ? 2 : (View.Aspects.IsMobileMultiViewEnabled() ? 1 : 0);
```

普通 mobile VR multiview 且 `View.bIsMobileMultiViewEnabled == true` 时，当前代码会设置为 2，可以让 CustomDepth pass 按 multiview 写入 array layer。这个场景是本次 bug 的核心场景，当前逻辑基本覆盖。

但与 UE5.5 相比，当前缺少 "shader platform 支持/启用 mobile multiview，但当前 view family 不需要双眼 multiview" 的 single-view multiview fallback。需要注意：当前 UE5.4.4 的 `FSceneView` 没有 UE5.5 的 `Aspects` 成员，不能直接照抄 `View.Aspects.IsMobileMultiViewEnabled()`；如果要回迁这个行为，需要用 5.4 可用的 `UE::StereoRenderUtils::FStereoShaderAspects(View.GetShaderPlatform()).IsMobileMultiViewEnabled()` 或等价 helper。

## Mobile Forward 链路核验

### CustomDepth 写入

`FSceneTextures::InitializeViewFamily` 已把 `Config.bRequireMultiView` 传入 `FCustomDepthTextures::Create`，而 `FCustomDepthTextures::Create` 已在 multiview 下创建 `Texture2DArray`，array size 为 2。这部分与 UE5.5 一致。

CustomDepth raster pass 已设置 `RenderTargets.DepthStencil = CustomDepthTextures.Depth`，并在 `View.bIsMobileMultiViewEnabled` 时设置 `MultiViewCount = 2`。对于正常双眼 mobile multiview，这条写入链路是成立的。

`SetStereoViewport` 保留，`DispatchDraw` 保留。由于当前 UE5.4.4 的 mesh draw command API 仍使用 `DispatchDraw`，没有必要按 UE5.5 改成 `Draw`。

### CustomStencil SRV

`FRDGTextureSRVDesc::CreateWithPixelFormat(Texture, PF_X24_G8)` 内部基于 `FRDGTextureSRVDesc::Create(Texture)`，而 `Create(Texture)` 在 `Texture->Desc.IsTextureArray()` 时会把 `NumArraySlices` 设置为源纹理的 `ArraySize`。因此当 `CustomDepthTextures.Depth` 是 `Texture2DArray` 时，stencil SRV 会保留 array 维度。这里与 UE5.5 一致，源码层面没有发现额外缺口。

### Mobile Forward 读取

Mobile Forward base pass 的 shader 侧链路已经基本补齐：

- `MobileBasePassVertexShader.usf` 写出 `BasePassInterpolants.MultiViewId`
- `MobileBasePassCommon.ush` 定义 `nointerpolation uint MultiViewId : VIEW_ID`
- `MobileBasePassPixelShader.usf` 把 `BasePassInterpolants.MultiViewId` 写到 `MaterialParameters.ViewId`
- `MaterialTemplate.ush` 通过 `GetViewId(Parameters)` 传入 `MobileFetchAndDecodeGBuffer`
- `DeferredShadingCommon.ush` 在 `MOBILE_MULTI_VIEW` 下按 `Texture2DArray` + `ViewId` 读取 CustomDepth/CustomStencil

但 C++ uniform produced 分支缺少 array 绑定，所以读取链路最后拿不到实际 CustomDepth/CustomStencil。当前 Mobile Forward 下不正确。

## Mobile Forward 额外风险：Full Depth Prepass

UE5.5.4 的 `FMobileSceneRenderer::RenderFullDepthPrepass` 在 full depth prepass 的 pass parameters 上设置：

```cpp
PassParameters->RenderTargets.MultiViewCount = (View.bIsMobileMultiViewEnabled) ? 2 : (View.Aspects.IsMobileMultiViewEnabled() ? 1 : 0);
```

当前 UE5.4.4 的 `RenderFullDepthPrepass` 没有设置 `MultiViewCount`。如果项目启用了 mobile full depth prepass，例如当前构造函数里 `Scene->EarlyZPassMode == DDM_AllOpaque` 时，mobile multiview 下 full depth prepass 可能只写 array layer 0。因为 mobile multiview 场景通常只渲染 primary view pass，缺少 `MultiViewCount` 会让第二眼 depth layer 没有同一 pass 写入。

这不是 `SceneTexture:CustomDepth` 单眼问题的核心必改项，但属于 Mobile Forward 管线正确性的相关风险。若项目启用 full depth prepass，应单独评估并回迁 UE5.5 的 full depth prepass multiview 设置，且需要使用 5.4 可编译的 aspect 判断方式。

## Mobile Deferred 说明

计划里提到的 `MobileShadingRenderer.cpp` deferred render target `MultiViewCount = 0` 当前仍未回迁 UE5.5 逻辑。由于本次要求重点是 Mobile Forward，这不阻断 Forward 结论；但如果项目同时启用 mobile deferred + multiview，当前仍存在 UE5.5 已修正而本工程未覆盖的风险。

`DeferredShadingCommon.ush` 中 `MobileFetchAndDecodeGBuffer` 当前仍保留 5.4 的较窄条件：

```cpp
#if (MOBILE_DEFERRED_SHADING && IS_MOBILE_DEFERREDSHADING_SUBPASS && PIXELSHADER)
```

UE5.5 已改为：

```cpp
#if MOBILE_DEFERRED_SHADING
```

对 Mobile Forward 的 CustomDepth 读取不构成问题，因为 Forward 路径会走 `CalcSceneDepth(UV)` 分支，CustomDepth/CustomStencil 的 array 读取不依赖这个 deferred 条件。

## 不需要改的点

`SceneTexturesCommon.ush` 的 `CalcSceneCustomDepth` 不属于 mobile multiview material scene texture 读取链路。Mobile 路径实际走：

```text
MaterialTemplate.ush
  -> MobileSceneTextureLookup(...)
  -> MobileFetchAndDecodeGBuffer(...)
  -> MobileSceneTextures.CustomDepthTextureArray / CustomStencilTextureArray
```

所以不修改 `SceneTexturesCommon.ush` 是正确的。

`MobileBasePassVertexShader.usf` 与 UE5.5 有一些 instanced stereo 条件整理差异，但当前已经能在 `MOBILE_MULTI_VIEW` 下传出 `MultiViewId`。这不是当前 CustomDepth bug 的必要改动。

## 建议修复项

1. 必须补齐 `Source\Runtime\Renderer\Private\SceneTextures.cpp` produced 分支：

```cpp
SceneTextureParameters.CustomDepthTextureArray = bCustomDepthProduced ? CustomDepthTextures.Depth : SystemTextures.DepthDummy;
SceneTextureParameters.CustomStencilTextureArray = bCustomDepthProduced ? CustomDepthTextures.Stencil : SystemTextures.StencilDummySRV;
```

2. 建议把 `CustomDepthRendering.cpp` 的 `RenderTargets.MultiViewCount` 改成 5.4 兼容的 UE5.5 等价判断，覆盖 single-view multiview fallback。不能直接使用 `View.Aspects`，除非同时回迁 UE5.5 的 `FSceneView::Aspects` 成员。

3. 如果项目启用 Mobile Forward full depth prepass，建议单独回迁 `RenderFullDepthPrepass` 的 `MultiViewCount` 设置。否则 scene depth prepass 在 mobile multiview 下仍可能只写单层。

4. 如果项目启用 Mobile Deferred + multiview，再评估 `InitRenderTargetBindings_Deferred` 的 `MultiViewCount` 与 `DeferredShadingCommon.ush` 的 UE5.5 条件变更。

## 最终判断

当前 CustomDepth 创建、CustomDepth pass 写入、shader `ViewId` 传递、shader array 读取大体已经回迁，但 C++ mobile uniform 的实际 array 资源绑定缺失，导致闭环断开。

因此结论是：修改未全部完成；当前 Mobile Forward 下 `SceneTexture:CustomDepth` / `SceneTexture:CustomStencil` 的 Mobile MultiView 读取仍不正确。补齐 `SceneTextures.cpp` produced 分支 array 绑定后，普通双眼 Mobile Forward multiview 的 CustomDepth/CustomStencil 读取链路才基本闭合；full depth prepass 和 aspect fallback 仍是额外风险点。
