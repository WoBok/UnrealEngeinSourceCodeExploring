# RenderPrePass、CustomDepthPass、MobileBasePass 的深度贴图绑定关系

## 结论

| Pass | 深度/Stencil Attachment 绑定 | RDG 资源名 | 是否是主 Scene Depth |
| --- | --- | --- | --- |
| `FDeferredShadingSceneRenderer::RenderPrePass` | `SceneDepthTexture` 参数，调用处传入 `SceneTextures.Depth.Target` | `SceneDepthZ` | 是 |
| `FSceneRenderer::RenderCustomDepthPass` | `CustomDepthTextures.Depth` | `CustomDepth` | 否，独立 CustomDepth |
| `FMobileSceneRenderer::RenderMobileBasePass` | 函数内部不直接绑定；外层 mobile render pass 的 `RenderTargets.DepthStencil` 绑定 `SceneTextures.Depth.Target` | `SceneDepthZ` | 是 |

`DepthRendering.cpp:495` 的 `FDeferredShadingSceneRenderer::RenderPrePass` 和 `MobileBasePassRendering.cpp:470` 的 `FMobileSceneRenderer::RenderMobileBasePass` 属于不同渲染器路径：Deferred renderer 与 Mobile renderer。正常同一个 ViewFamily 不会同时走这两个路径，因此不能理解为它们在同一条渲染流程中绑定同一张贴图。

但二者在各自渲染路径中绑定的“主场景深度”都是 `SceneTextures.Depth.Target`，RDG 名称都是 `SceneDepthZ`。如果讨论的是 Mobile 路径里的 `FMobileSceneRenderer::RenderPrePass`，它和 `RenderMobileBasePass` 会使用同一个 `SceneTextures.Depth.Target`。

## 1. Deferred `RenderPrePass`

位置：`Engine/Source/Runtime/Renderer/Private/DepthRendering.cpp:495`

`RenderPrePass` 接收 `FRDGTextureRef SceneDepthTexture`，真正绑定发生在 `GetDepthPassParameters`：

```cpp
PassParameters->RenderTargets.DepthStencil = FDepthStencilBinding(
    DepthTexture,
    ERenderTargetLoadAction::ELoad,
    ERenderTargetLoadAction::ELoad,
    FExclusiveDepthStencil::DepthWrite_StencilWrite);
```

参考：`Engine/Source/Runtime/Renderer/Private/DepthRendering.cpp:127`、`Engine/Source/Runtime/Renderer/Private/DepthRendering.cpp:131`

调用处在 Deferred 主流程中先清空主深度，再把 `SceneTextures.Depth.Target` 传给 `RenderPrePass`：

```cpp
AddClearDepthStencilPass(GraphBuilder, SceneTextures.Depth.Target, DepthLoadAction, StencilLoadAction);
RenderPrePass(GraphBuilder, InViews, SceneTextures.Depth.Target, InstanceCullingManager, &FirstStageDepthBuffer);
```

参考：`Engine/Source/Runtime/Renderer/Private/DeferredShadingRenderer.cpp:2039`、`Engine/Source/Runtime/Renderer/Private/DeferredShadingRenderer.cpp:2045`

所以 Deferred PrePass 写入的是主场景深度：`SceneTextures.Depth.Target` / `SceneDepthZ`。

如果启用 second stage depth pass，会额外创建 `FirstStageDepthBuffer`：

```cpp
FRDGTextureDesc FirstStageDepthBufferDesc = FRDGTextureDesc::Create2D(
    SceneDepthTexture->Desc.Extent,
    PF_R32_FLOAT,
    FClearValueBinding::Black,
    TexCreate_ShaderResource | TexCreate_UAV);
*FirstStageDepthBuffer = GraphBuilder.CreateTexture(FirstStageDepthBufferDesc, TEXT("FirstStageDepthBuffer"));
```

参考：`Engine/Source/Runtime/Renderer/Private/DepthRendering.cpp:610`、`Engine/Source/Runtime/Renderer/Private/DepthRendering.cpp:611`

这个 `FirstStageDepthBuffer` 是深度拷贝/辅助资源，不是 PrePass 的 DepthStencil attachment。

## 2. `RenderCustomDepthPass`

位置：`Engine/Source/Runtime/Renderer/Private/CustomDepthRendering.cpp:179`

CustomDepth 资源创建在 `FCustomDepthTextures::Create`：

```cpp
const FRDGTextureDesc CustomDepthDesc = FRDGTextureDesc::Create2D(
    CustomDepthExtent,
    PF_DepthStencil,
    FClearValueBinding::DepthFar,
    CreateFlags);

CustomDepthTextures.Depth = GraphBuilder.CreateTexture(CustomDepthDesc, TEXT("CustomDepth"));
```

参考：`Engine/Source/Runtime/Renderer/Private/CustomDepthRendering.cpp:86`、`Engine/Source/Runtime/Renderer/Private/CustomDepthRendering.cpp:88`

`RenderCustomDepthPass` 绑定的是：

```cpp
PassParameters->RenderTargets.DepthStencil = FDepthStencilBinding(
    CustomDepthTextures.Depth,
    DepthLoadAction,
    StencilLoadAction,
    FExclusiveDepthStencil::DepthWrite_StencilWrite);
```

参考：`Engine/Source/Runtime/Renderer/Private/CustomDepthRendering.cpp:273`、`Engine/Source/Runtime/Renderer/Private/CustomDepthRendering.cpp:274`

因此 CustomDepthPass 不写 `SceneDepthZ`，而是写单独的 `CustomDepth`。它和主 Scene Depth 是两张不同的 RDG texture。

## 3. Mobile `RenderMobileBasePass`

位置：`Engine/Source/Runtime/Renderer/Private/MobileBasePassRendering.cpp:470`

`RenderMobileBasePass` 函数内部只设置 viewport 并 dispatch mesh draw command：

```cpp
RHICmdList.SetViewport(View.ViewRect.Min.X, View.ViewRect.Min.Y, 0, View.ViewRect.Max.X, View.ViewRect.Max.Y, 1);
View.ParallelMeshDrawCommandPasses[EMeshPass::BasePass].DispatchDraw(nullptr, RHICmdList, InstanceCullingDrawParams);
```

参考：`Engine/Source/Runtime/Renderer/Private/MobileBasePassRendering.cpp:477`、`Engine/Source/Runtime/Renderer/Private/MobileBasePassRendering.cpp:478`

深度附件由外层 `FMobileRenderPassParameters::RenderTargets` 提前绑定。Mobile Forward 路径：

```cpp
SceneDepth = SceneTextures.Depth.Target;

BasePassRenderTargets.DepthStencil = bIsFullDepthPrepassEnabled ?
    FDepthStencilBinding(SceneDepth, ERenderTargetLoadAction::ELoad, ERenderTargetLoadAction::ELoad, FExclusiveDepthStencil::DepthRead_StencilWrite) :
    FDepthStencilBinding(SceneDepth, ERenderTargetLoadAction::EClear, ERenderTargetLoadAction::EClear, FExclusiveDepthStencil::DepthWrite_StencilWrite);
```

参考：`Engine/Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp:1470`、`Engine/Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp:1494`

Mobile Deferred 路径同样绑定：

```cpp
BasePassRenderTargets.DepthStencil = bIsFullDepthPrepassEnabled ?
    FDepthStencilBinding(SceneTextures.Depth.Target, ERenderTargetLoadAction::ELoad, ERenderTargetLoadAction::ELoad, FExclusiveDepthStencil::DepthRead_StencilWrite) :
    FDepthStencilBinding(SceneTextures.Depth.Target, ERenderTargetLoadAction::EClear, ERenderTargetLoadAction::EClear, FExclusiveDepthStencil::DepthWrite_StencilWrite);
```

参考：`Engine/Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp:1875`、`Engine/Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp:1877`

所以 MobileBasePass 的 DepthStencil attachment 是主场景深度 `SceneTextures.Depth.Target` / `SceneDepthZ`。

`SceneTextures.Depth.Target` 的创建位置：

```cpp
SceneTextures.Depth = GraphBuilder.CreateTexture(Desc, TEXT("SceneDepthZ"));
```

参考：`Engine/Source/Runtime/Renderer/Private/SceneTextures.cpp:455`、`Engine/Source/Runtime/Renderer/Private/SceneTextures.cpp:459`

## 4. Mobile DepthAux 不等于 DepthStencil Attachment

Mobile 还可能创建 `SceneTextures.DepthAux`：

```cpp
SceneTextures.DepthAux = CreateTextureMSAA(GraphBuilder, Desc, TEXT("SceneDepthAuxMS"), TEXT("SceneDepthAux"));
```

参考：`Engine/Source/Runtime/Renderer/Private/SceneTextures.cpp:590`、`Engine/Source/Runtime/Renderer/Private/SceneTextures.cpp:608`

它在 Mobile Forward 中可能作为颜色 RT 绑定到 slot 1：

```cpp
BasePassRenderTargets[1] = FRenderTargetBinding(SceneTextures.DepthAux.Target, SceneTextures.DepthAux.Resolve, ERenderTargetLoadAction::EClear);
```

参考：`Engine/Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp:1481`、`Engine/Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp:1483`

这不是硬件 DepthStencil attachment，而是 Mobile 用于采样/辅助的深度颜色纹理。

## 5. 是否会绑定同一张贴图

### `DepthRendering.cpp:495` 的 Deferred PrePass vs MobileBasePass

正常不会在同一 ViewFamily 渲染路径里一起运行：

- Deferred PrePass：`FDeferredShadingSceneRenderer::RenderPrePass`
- Mobile BasePass：`FMobileSceneRenderer::RenderMobileBasePass`

它们分别属于 Deferred 和 Mobile renderer。各自都会使用本渲染路径中的 `SceneTextures.Depth.Target`，但不是“同一条流程中同一张贴图被前后复用”的关系。

### Mobile PrePass vs MobileBasePass

如果是 Mobile 渲染器中的 depth prepass，结论是会复用同一个主场景深度。

Mobile full depth prepass 绑定：

```cpp
BasePassRenderTargets.DepthStencil = FDepthStencilBinding(
    SceneTextures.Depth.Target,
    ERenderTargetLoadAction::EClear,
    ERenderTargetLoadAction::EClear,
    FExclusiveDepthStencil::DepthWrite_StencilWrite);
```

参考：`Engine/Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp:796`、`Engine/Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp:799`

随后 MobileBasePass 如果 `bIsFullDepthPrepassEnabled`，会以 load/read 方式绑定同一个 `SceneTextures.Depth.Target`：

```cpp
FDepthStencilBinding(SceneDepth, ERenderTargetLoadAction::ELoad, ERenderTargetLoadAction::ELoad, FExclusiveDepthStencil::DepthRead_StencilWrite)
```

参考：`Engine/Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp:1494`、`Engine/Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp:1495`

## 总结

- Deferred `RenderPrePass`：写 `SceneTextures.Depth.Target`，RDG 名 `SceneDepthZ`。
- `RenderCustomDepthPass`：写 `CustomDepthTextures.Depth`，RDG 名 `CustomDepth`，独立于主 Scene Depth。
- Mobile `RenderMobileBasePass`：外层 render pass 绑定 `SceneTextures.Depth.Target`，RDG 名 `SceneDepthZ`。
- `DepthRendering.cpp:495` 的 Deferred PrePass 与 MobileBasePass 正常不在同一渲染路径里同时运行。
- 如果讨论 Mobile 自己的 PrePass 与 MobileBasePass，则它们确实复用同一个 `SceneDepthZ`。