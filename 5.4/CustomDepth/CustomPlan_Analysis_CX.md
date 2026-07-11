# UE 5.4 Mobile MultiView CustomDepth 修复验证

验证对象：

- 当前工程：`E:\Unreal Engine Work Projects\MR01_DaNaoTianGong_Main\Engine`
- 对照源码：`E:\Unreal Engine Work Projects\UnrealEngine\UnrealEngine5.5\Engine`
- 计划文件：`Docs\Plan_CX_5.5.md`

## 总结结论

当前修改已经覆盖 UE5.5 修复 CustomDepth 单眼问题的核心闭环：

1. CustomDepth 创建接口已经接收 `bRequireMultiView`。
2. MultiView 下 CustomDepth target 已经创建为 `Texture2DArray`，array size 为 2。
3. CustomDepth pass 已经设置 `RenderTargets.MultiViewCount`。
4. `FMobileSceneTextureUniformParameters` 已经增加 CustomDepth/CustomStencil 的 array 版本。
5. mobile scene texture uniform 已经绑定 CustomDepth/CustomStencil array 参数。
6. mobile shader 已经在 `MOBILE_MULTI_VIEW` 下按 `ViewId` 读取 `CustomDepthTextureArray` / `CustomStencilTextureArray`。
7. `MaterialTemplate.ush -> MobileSceneTextureLookup` 已经把当前材质像素的 `ViewId` 传入 `MobileFetchAndDecodeGBuffer`。
8. `MobileBasePassPixelShader.usf` 已经把 `BasePassInterpolants.MultiViewId` 写入 `MaterialParameters.ViewId`。

因此，对普通移动端 VR MultiView + mobile base pass 材质读取 `SceneTexture:CustomDepth/CustomStencil` 的问题，当前修改已经覆盖根因：以前只存在/绑定/读取 2D CustomDepth，现在能写入和读取对应 eye 的 array layer。

但当前修改还不能算 100% 完整等价 UE5.5。发现 1 个需要按 UE5.4 写法评估的兼容差异，以及几个需要根据项目渲染路径决定是否继续 backport 的审计点。

## 已验证完成项

### 1. CustomDepth 创建和调用链

当前工程：

- `Source\Runtime\Renderer\Internal\CustomDepthRendering.h:24`
- `Source\Runtime\Renderer\Private\CustomDepthRendering.cpp:63`
- `Source\Runtime\Renderer\Private\SceneTextures.cpp:495`

已与 UE5.5 对齐：`FCustomDepthTextures::Create(...)` 增加 `bool bRequireMultiView`，并由 `Config.bRequireMultiView` 传入。

### 2. CustomDepth 资源维度

当前工程：

- `Source\Runtime\Renderer\Private\CustomDepthRendering.cpp:86-88`

已与 UE5.5 对齐：MultiView 下使用 `FRDGTextureDesc::Create2DArray(..., 2)`，非 MultiView 下仍使用 `Create2D(...)`。

这是修复“CustomDepth 只按 Texture2D 创建”的关键之一。

### 3. Mobile uniform 参数

当前工程：

- `Source\Runtime\Engine\Public\SceneTexturesConfig.h:49`
- `Source\Runtime\Engine\Public\SceneTexturesConfig.h:52`
- `Source\Runtime\Renderer\Private\SceneTextures.cpp:954`
- `Source\Runtime\Renderer\Private\SceneTextures.cpp:957`
- `Source\Runtime\Renderer\Private\SceneTextures.cpp:1045`
- `Source\Runtime\Renderer\Private\SceneTextures.cpp:1047`

已与 UE5.5 对齐：mobile scene texture uniform 中存在并绑定：

- `CustomDepthTextureArray`
- `CustomStencilTextureArray`

注意：produced=false 分支里 `CustomDepthTextureArray` fallback 仍是 `SystemTextures.DepthDummy`，这和 UE5.5 一致。若 5.4 本地 RDG 参数校验对 Texture2DArray fallback 维度更严格，再改为保持默认的 2DArray fallback。

### 4. Shader 按 ViewId 读取 CustomDepth/Stencil

当前工程：

- `Shaders\Private\DeferredShadingCommon.ush:801`
- `Shaders\Private\DeferredShadingCommon.ush:818-819`
- `Shaders\Private\DeferredShadingCommon.ush:828-830`
- `Shaders\Private\MaterialTemplate.ush:496`
- `Shaders\Private\MaterialTemplate.ush:977`
- `Shaders\Private\MaterialTemplate.ush:2865`
- `Shaders\Private\MobileBasePassPixelShader.usf:360`

已覆盖 UE5.5 的关键读取链：

```text
MobileBasePassPixelShader.usf
  BasePassInterpolants.MultiViewId -> MaterialParameters.ViewId

MaterialTemplate.ush
  GetViewId(Parameters) -> MobileFetchAndDecodeGBuffer(UV, PixelPos, ViewId)

DeferredShadingCommon.ush
  MOBILE_MULTI_VIEW -> CustomDepthTextureArray / CustomStencilTextureArray layer ViewId
```

这能避免材质 `SceneTexture:CustomDepth/CustomStencil` 在右眼仍默认读 layer 0。

### 5. 不需要修改的点

计划中保留不改的判断是成立的：

- `SetStereoViewport(...)` 不是本问题根因，UE5.5 仍保留。
- `SceneTexturesCommon.ush::CalcSceneCustomDepth` 不是 mobile multiview 这条读取链路，UE5.5 也没有把 deferred `SceneTexturesStruct.CustomDepthTexture` 改成 array。
- `MobileBasePassCommon.ush` 和 `MobileBasePassVertexShader.usf` 已经提供 `MultiViewId`，不需要为这个问题额外改。
- 其他 shader 中保留两参数 `MobileFetchAndDecodeGBuffer(UV, PixelPos)` wrapper 调用并非遗漏；UE5.5 也仍保留这些调用，只有材质 `MobileSceneTextureLookup` 显式传入 `ViewId`。

## 发现的兼容差异

### 1. `CustomDepthRendering.cpp` 的 `MultiViewCount` 公式不能直接照搬 UE5.5

当前工程：

```cpp
PassParameters->RenderTargets.MultiViewCount = View.bIsMobileMultiViewEnabled ? 2 : 0;
```

UE5.5：

```cpp
PassParameters->RenderTargets.MultiViewCount = (View.bIsMobileMultiViewEnabled) ? 2 : (View.Aspects.IsMobileMultiViewEnabled() ? 1 : 0);
```

位置：

- 当前：`Source\Runtime\Renderer\Private\CustomDepthRendering.cpp:281`
- UE5.5：`Source\Runtime\Renderer\Private\CustomDepthRendering.cpp:280`

影响判断：

- 在普通 VR MultiView 主渲染路径中，如果 `View.bIsMobileMultiViewEnabled == true`，当前代码会设置 `MultiViewCount = 2`，CustomDepth pass 应能写两层。
- 但 UE5.4 的 `FViewInfo/FSceneView` 没有 `View.Aspects` 成员，所以不能直接照搬 UE5.5 这一句。
- UE5.5 的后半段回退用于 shader aspects 已启用 Mobile MultiView、但当前 view 不以双眼 multiview 绘制的情况，使 render pass 仍以 single-view multiview 方式建立。
- 对本问题的主路径来说，`View.bIsMobileMultiViewEnabled ? 2 : 0` 已经能覆盖双眼 MultiView CustomDepth 写入；缺少的是 single-view multiview 兼容回退，不是导致普通 VR 双眼 CustomDepth 单眼的最大根因。

若要在 UE5.4 中补齐等价回退，应使用 5.4 已有的 `UE::StereoRenderUtils::FStereoShaderAspects` 临时判断。`ShadowRendering.cpp` 已经有同类写法：

```cpp
auto IsMobileMultiViewEnabledInAspects = [](const FSceneView& View)
{
	const UE::StereoRenderUtils::FStereoShaderAspects Aspects(View.GetShaderPlatform());
	return Aspects.IsMobileMultiViewEnabled();
};

PassParameters->RenderTargets.MultiViewCount =
	View.bIsMobileMultiViewEnabled ? 2 : (IsMobileMultiViewEnabledInAspects(View) ? 1 : 0);
```

这个补丁不是直接照搬 UE5.5，而是 UE5.4 兼容写法。若编译环境没有通过现有 include 暴露 `FStereoShaderAspects`，再补对应头文件；当前工程里 `ShadowRendering.cpp` 能直接使用该类型，可作为本地参考。

## 可选补充审计点

### 1. Mobile deferred 路径没有完全同步 UE5.5

当前计划聚焦 CustomDepth + mobile base pass 材质读取，这是合理的最小修复。

但 UE5.5 在 `SceneTextures.cpp` 中还把 mobile/deferred GBuffer A/B/C/D/E/F 在 `Config.bRequireMultiView` 下创建为 `Texture2DArray`。当前工程对应位置仍是 `Create2D(...)`：

- `Source\Runtime\Renderer\Private\SceneTextures.cpp:552`
- `Source\Runtime\Renderer\Private\SceneTextures.cpp:558`
- `Source\Runtime\Renderer\Private\SceneTextures.cpp:564`
- `Source\Runtime\Renderer\Private\SceneTextures.cpp:570`
- `Source\Runtime\Renderer\Private\SceneTextures.cpp:576`
- `Source\Runtime\Renderer\Private\SceneTextures.cpp:585`

如果项目只针对 mobile forward/base pass 中的 `SceneTexture:CustomDepth/CustomStencil`，这不是必须项。

如果项目启用 mobile deferred + MultiView，并且看到 GBuffer、deferred lighting 或其他 render target 也有单眼/错层问题，应单独 backport UE5.5 这组 GBuffer array 创建和 deferred render target `MultiViewCount` 调整。

### 2. `MobileShadingRenderer.cpp` 仍有 UE5.4 的旧 MultiViewCount 逻辑

当前工程：

- `Source\Runtime\Renderer\Private\MobileShadingRenderer.cpp:1517`
- `Source\Runtime\Renderer\Private\MobileShadingRenderer.cpp:1881`

UE5.5 对这里有更广范围的 cleanup。它不是 CustomDepth 单眼问题的最小必改项，但如果 mobile deferred 或其他 render target 也出现单眼，应继续对齐。

### 3. `DeferredShadingCommon.ush` 的 mobile deferred 条件没有完全同步 UE5.5

当前工程保留 5.4 条件：

```hlsl
#if (MOBILE_DEFERRED_SHADING && IS_MOBILE_DEFERREDSHADING_SUBPASS && PIXELSHADER)
```

UE5.5 使用：

```hlsl
#if MOBILE_DEFERRED_SHADING
```

这不是 CustomDepth array 读取的必要条件，当前保守 backport 可以接受。若后续 mobile deferred shader 编译或运行路径发现 GBuffer/depth decode 不一致，再单独评估是否同步。

### 4. GLES CustomStencil SRV 需要实机或 capture 验证

`CustomDepthRendering.cpp` 中 GLES stencil copy/SRV 路径与 UE5.5 一致，没有源码差异。

仍建议在 GLES 或对应移动 RHI 上确认：

- `CustomStencil` copy 后是否仍保持 array desc。
- `CreateWithPixelFormat(..., PF_X24_G8)` 生成的 SRV 是否是 array SRV。

如果本地 5.4 RHI/RDG 没保留 array 维度，再考虑显式构造 array SRV desc。

## 是否已解决问题

基于源码对照，当前修改已经解决导致 UE5.4 mobile VR MultiView CustomDepth 单眼的主要原因：CustomDepth 不再只是 2D 资源，CustomDepth pass 能在 `bIsMobileMultiViewEnabled` 时以 `MultiViewCount = 2` 绘制，材质读取也会按当前 eye 的 `ViewId` 访问 array layer。

当前 `CustomDepthRendering.cpp:281` 的 `View.bIsMobileMultiViewEnabled ? 2 : 0` 对普通 VR 双眼 MultiView 主路径是有效的。若要覆盖 UE5.5 的 single-view multiview 回退场景，应按 UE5.4 兼容方式使用 `FStereoShaderAspects(View.GetShaderPlatform())`，不能写 `View.Aspects`。

## 建议验证步骤

1. 编译 Renderer/Engine，确认 C++ shader parameter layout 与 shader uniform 引用一致。
2. 清理并重编 mobile shader cache，确保覆盖 `MOBILE_MULTI_VIEW=1` 的 material/base pass permutation。
3. 在移动端 VR MultiView 场景中启用 CustomDepth/CustomStencil，让左右眼都能看到写 CustomDepth 的对象。
4. 用材质 `SceneTexture:CustomDepth` / `SceneTexture:CustomStencil` 在 mobile base pass 中读取，确认左右眼分别读取各自 layer，不再固定为左眼/layer 0。
5. 用 RenderDoc/AGI 抓帧确认：
   - `CustomDepth` 是 `Texture2DArray`。
   - array size 为 2。
   - CustomDepth pass 的 multiview count 为 2。
   - 两个 array layer 都有有效写入。
6. 若启用 GLES CustomStencil，确认 stencil copy 后 SRV 维度仍为 array。
7. 若启用 mobile deferred + MultiView，额外检查 GBuffer 和 deferred render targets 是否也需要同步 UE5.5 的 array/multiview 改动。
