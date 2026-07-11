# 移动端 Forward 渲染路径中 MobileBasePass 的深度写入分析

> 适用范围：UE Mobile / Android Forward Shading（非 Deferred）路径，未开启 EarlyZ Pass（`Scene->EarlyZPassMode != DDM_AllOpaque`）。
>
> 目标：梳理 BasePass 是 **如何** 写入 SceneDepth 的，列出从注册 MeshPass 到底层 RHI Draw 的完整调用链，并给出 **真正发生“深度写入”的代码位置**。

---

## 0. 结论速览

在移动端 Forward 路径下，没有独立的 Depth-Only Pass。Opaque/Masked 物体的深度是在 **MobileBasePass 的 PixelShader 输出阶段、由硬件 ROP 自动写入** 当前 RenderPass 绑定的 DepthStencil Attachment（即 `SceneTextures.Depth.Target`）。

要发生“深度写入”需要同时满足 3 个条件，三者是分离配置的：

| 条件 | 由谁决定 | 关键代码位置 |
|---|---|---|
| ① RenderPass 的 DepthStencil 附件以 `DepthWrite_StencilWrite` 方式绑定 | `FMobileSceneRenderer::InitRenderTargetBindings_Forward` | `MobileShadingRenderer.cpp:1494` |
| ② PSO 的 `DepthStencilState` 开启 DepthWrite（`bEnableDepthWrite=true`） | `CreateMobileBasePassProcessor` 中设置的 `DrawRenderState` → `BuildMeshDrawCommands` 写入 `PipelineState.DepthStencilState` | `MobileBasePass.cpp:1157` → `MeshPassProcessor.inl:104` |
| ③ 真正发起 DrawCall（DrawIndexedPrimitive） | `FMeshDrawCommand::SubmitDrawEnd` | `MeshPassProcessor.cpp:1446 / 1469` |

只要 ①②③ 同时成立，PixelShader 输出 SV_Depth（或默认插值深度）时，硬件就会把片元深度写入 ① 绑定的 DepthStencil 附件。这一步在 UE 的 C++ 代码里 **没有显式的 “WriteDepth” 调用**——它是 GPU 固定功能完成的。

---

## 1. 入口：Forward 路径主流程

### 1.1 `FMobileSceneRenderer::RenderForward`

`Engine/Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp:1503`

构建 RenderTarget 绑定，并对每个 View 进入 `RenderForwardSinglePass` 或 `RenderForwardMultiPass`。

```cpp
FRenderTargetBindingSlots BasePassRenderTargets = InitRenderTargetBindings_Forward(ViewFamilyTexture, SceneTextures); // L1510
...
RenderForwardSinglePass(GraphBuilder, PassParameters, ViewContext, SceneTextures); // L1573
```

### 1.2 `InitRenderTargetBindings_Forward` —— **决定 Depth 是否可写**

`MobileShadingRenderer.cpp:1448`，核心片段：

```cpp
BasePassRenderTargets.DepthStencil = bIsFullDepthPrepassEnabled ?
    FDepthStencilBinding(SceneDepth, ELoad, ELoad, FExclusiveDepthStencil::DepthRead_StencilWrite) :
    FDepthStencilBinding(SceneDepth, EClear, EClear, FExclusiveDepthStencil::DepthWrite_StencilWrite); // L1494-1496
```

- `bIsFullDepthPrepassEnabled = (Scene->EarlyZPassMode == DDM_AllOpaque)`（`MobileShadingRenderer.cpp:302`）。
- 未开 EarlyZPass → 走 `DepthWrite_StencilWrite` 分支：DepthStencil 附件以 `EClear` 加载，并允许写入。这是 BasePass 能写入深度的 **RenderPass 级前提**。

### 1.3 `FMobileSceneRenderer::RenderForwardSinglePass`

`MobileShadingRenderer.cpp:1578`，把所有绘制塞进一个 RDG Raster Pass（移动端 TBR/Subpass 友好）。Pass 的 RT 绑定来自上一步：

```cpp
GraphBuilder.AddPass(
    RDG_EVENT_NAME("SceneColorRendering"),
    PassParameters,                                          // 包含 RenderTargets（含 DepthStencil 绑定）
    ERDGPassFlags::Raster | ERDGPassFlags::NeverMerge,
    [...](FRHICommandList& RHICmdList)
    {
        ...
        RenderMaskedPrePass(RHICmdList, View);               // 仅 bIsMaskedOnlyDepthPrepassEnabled 时才有效
        RenderMobileBasePass(RHICmdList, View, &PassParameters->InstanceCullingDrawParams); // L1609 ★
        ...
    });
```

> RDG 在执行这个 Lambda 之前，会调用 `RHIBeginRenderPass` 并把 `BasePassRenderTargets.DepthStencil` 绑定到硬件，因此之后任何 DrawCall 输出的深度都会落到 `SceneDepth` 上。

---

## 2. `RenderMobileBasePass` —— 派发 Mesh Draw Commands

`Engine/Source/Runtime/Renderer/Private/MobileBasePassRendering.cpp:470`

```cpp
void FMobileSceneRenderer::RenderMobileBasePass(FRHICommandList& RHICmdList, const FViewInfo& View, const FInstanceCullingDrawParams* InstanceCullingDrawParams)
{
    RHICmdList.SetViewport(...);
    View.ParallelMeshDrawCommandPasses[EMeshPass::BasePass].DispatchDraw(nullptr, RHICmdList, InstanceCullingDrawParams); // L478 ★
    ...
}
```

这里把 `EMeshPass::BasePass` 这一通道里 **缓存好的 MeshDrawCommand 列表** 派发到 RHI。

---

## 3. MeshDrawCommand 的 “深度写入状态” 是什么时候确定的？

### 3.1 注册并创建 BasePass MeshPass Processor

`Engine/Source/Runtime/Renderer/Private/MobileBasePass.cpp:1151`

```cpp
FMeshPassProcessor* CreateMobileBasePassProcessor(...)
{
    FMeshPassProcessorRenderState PassDrawRenderState;
    PassDrawRenderState.SetBlendState(TStaticBlendStateWriteMask<CW_RGBA>::GetRHI());

    const FExclusiveDepthStencil::Type DefaultBasePassDepthStencilAccess
        = FScene::GetDefaultBasePassDepthStencilAccess(FeatureLevel); // 未开 EarlyZ ⇒ DepthWrite_StencilWrite
    PassDrawRenderState.SetDepthStencilAccess(DefaultBasePassDepthStencilAccess);

    PassDrawRenderState.SetDepthStencilState(
        TStaticDepthStencilState<true, CF_DepthNearOrEqual>::GetRHI());     // ★ 第一个模板参数 true = bEnableDepthWrite

    ...
    return new FMobileBasePassMeshProcessor(EMeshPass::BasePass, ..., PassDrawRenderState, ...);
}
```

> `TStaticDepthStencilState<true, CF_DepthNearOrEqual>` ：开启 DepthWrite，比较函数为 “近或相等”。这是 PSO 层面允许深度写入的关键状态。

随后通过宏注册到全局：

```cpp
REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR(MobileBasePass, CreateMobileBasePassProcessor, EShadingPath::Mobile,
    EMeshPass::BasePass, EMeshPassFlags::CachedMeshCommands | EMeshPassFlags::MainView); // L1218
```

### 3.2 不透明物体细化的 DepthStencil 状态：`SetOpaqueRenderState`

`MobileBasePass.cpp:531`：在需要写 Stencil（Decal Receive / Deferred ShadingModel Mask）时，会用一个 **仍然 `bEnableDepthWrite=true`、`CF_DepthNearOrEqual`** 的复合状态覆盖之：

```cpp
DrawRenderState.SetDepthStencilState(TStaticDepthStencilState<
    true, CF_DepthNearOrEqual,                                  // ★ 深度仍然可写
    true, CF_Always, SO_Keep, SO_Keep, SO_Replace,              // FrontFace stencil
    false, CF_Always, SO_Keep, SO_Keep, SO_Keep,
    0x00, 0xff>::GetRHI()); // L549
```

Forward + 非 Decal 普通材质走 `else` 分支不做修改，保留 §3.1 中的默认 `<true, CF_DepthNearOrEqual>`。

### 3.3 把 DepthStencilState 烘焙进 MeshDrawCommand 的 PSO

由 `FMobileBasePassMeshProcessor::TryAddMeshBatch / Process` 调用模板函数 `FMeshPassProcessor::BuildMeshDrawCommands`：

`Engine/Source/Runtime/Renderer/Public/MeshPassProcessor.inl:47-105`

```cpp
check(DrawRenderState.GetDepthStencilState());
...
PipelineState.BlendState        = DrawRenderState.GetBlendState();
PipelineState.DepthStencilState = DrawRenderState.GetDepthStencilState(); // L104 ★
```

完成后 PSO 描述符里就带有 “DepthWrite=true” 的 `FRHIDepthStencilState`，最终 `DrawListContext->AddCommand(SharedMeshDrawCommand, NumElements)` 把这个 MeshDrawCommand 加入 `EMeshPass::BasePass` 的命令列表/缓存中。

---

## 4. 派发到 RHI：`DispatchDraw` → `SubmitDraw` → DrawCall

### 4.1 `FParallelMeshDrawCommandPass::DispatchDraw`

`Engine/Source/Runtime/Renderer/Private/MeshDrawCommands.cpp:1640`

非并行路径（移动端常走）：

```cpp
TaskContext.InstanceCullingContext.SubmitDrawCommands(
    TaskContext.MeshDrawCommands,
    TaskContext.MinimalPipelineStatePassSet,
    OverrideArgs,
    0,
    TaskContext.MeshDrawCommands.Num(),
    TaskContext.InstanceFactor,
    RHICmdList); // L1701
```

### 4.2 `FInstanceCullingContext::SubmitDrawCommands`

`Engine/Source/Runtime/Renderer/Private/InstanceCulling/InstanceCullingContext.cpp:1663`

循环每条 MeshDrawCommand，调用：

```cpp
FMeshDrawCommand::SubmitDraw(*VisibleMeshDrawCommand.MeshDrawCommand,
    GraphicsMinimalPipelineStateSet, SceneArgs, InstanceFactor, RHICmdList, StateCache); // L1715
```

### 4.3 `FMeshDrawCommand::SubmitDraw → SubmitDrawBegin / SubmitDrawEnd`

`Engine/Source/Runtime/Renderer/Private/MeshPassProcessor.cpp`

```cpp
// SubmitDrawBegin (L1358)
SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit, MeshDrawCommand.StencilRef,
    EApplyRendertargetOption::CheckApply, bApplyAdditionalState, PSOPrecacheResult); // L1398
// ↑ 此时 RHI 实际绑定带有 DepthWrite=true 的 DepthStencilState 到管线
```

```cpp
// SubmitDrawEnd (L1433) —— ★ 真正的“发出 DrawCall”
RHICmdList.DrawIndexedPrimitive(
    MeshDrawCommand.IndexBuffer,
    MeshDrawCommand.VertexParams.BaseVertexIndex, 0,
    MeshDrawCommand.VertexParams.NumVertices,
    MeshDrawCommand.FirstIndex,
    MeshDrawCommand.NumPrimitives,
    MeshDrawCommand.NumInstances * InstanceFactor); // L1446
// 或：RHICmdList.DrawPrimitive(...)                  // L1469
// 或：RHICmdList.DrawIndexedPrimitiveIndirect(...)   // L1458
```

> 这一行 `DrawIndexedPrimitive / DrawPrimitive` 就是 “BasePass 写入深度” 在 CPU 侧的最后一行可见代码。GPU 端：VS→Raster→PS→ROP；ROP 阶段根据当前绑定的 `DepthStencilState`（DepthWrite=true, CF_DepthNearOrEqual）把通过测试的片元的 `SV_Depth` 写入当前 RenderPass 绑定的 DepthStencil 附件（即 `SceneTextures.Depth.Target`）。

---

## 5. 完整调用链汇总

```
[RenderTarget 绑定 / 控制是否允许写入 Depth]
FMobileSceneRenderer::RenderForward                       MobileShadingRenderer.cpp:1503
 └─ InitRenderTargetBindings_Forward                      MobileShadingRenderer.cpp:1448
     └─ BasePassRenderTargets.DepthStencil = FDepthStencilBinding(SceneDepth, EClear, EClear,
                                              DepthWrite_StencilWrite)   MobileShadingRenderer.cpp:1494-1496

[执行 BasePass：RDG Raster Pass 内部]
FMobileSceneRenderer::RenderForwardSinglePass             MobileShadingRenderer.cpp:1578
 └─ GraphBuilder.AddPass(... Raster ..., Lambda)          MobileShadingRenderer.cpp:1590
     └─ RenderMobileBasePass(RHICmdList, View, ...)       MobileShadingRenderer.cpp:1609

FMobileSceneRenderer::RenderMobileBasePass                MobileBasePassRendering.cpp:470
 └─ View.ParallelMeshDrawCommandPasses[BasePass]
        .DispatchDraw(nullptr, RHICmdList, InstanceCullingDrawParams)   MobileBasePassRendering.cpp:478

FParallelMeshDrawCommandPass::DispatchDraw                MeshDrawCommands.cpp:1640
 └─ FInstanceCullingContext::SubmitDrawCommands           InstanceCullingContext.cpp:1663
     └─ FMeshDrawCommand::SubmitDraw                      InstanceCullingContext.cpp:1715
         ├─ FMeshDrawCommand::SubmitDrawBegin             MeshPassProcessor.cpp:1358
         │   └─ SetGraphicsPipelineState (含 DepthWrite=true)            MeshPassProcessor.cpp:1398
         └─ FMeshDrawCommand::SubmitDrawEnd               MeshPassProcessor.cpp:1433
             └─ ★ RHICmdList.DrawIndexedPrimitive(...)    MeshPassProcessor.cpp:1446
                 / RHICmdList.DrawPrimitive(...)          MeshPassProcessor.cpp:1469
                 ↓ GPU ROP 把 SV_Depth 写入当前绑定的 DepthStencil Attachment
                 ↓ 即 SceneTextures.Depth.Target

[PSO 中 DepthWrite=true 的来源]
CreateMobileBasePassProcessor                             MobileBasePass.cpp:1151
 ├─ PassDrawRenderState.SetDepthStencilAccess(DepthWrite_StencilWrite)  MobileBasePass.cpp:1155-1156
 └─ PassDrawRenderState.SetDepthStencilState(
       TStaticDepthStencilState<true /*DepthWrite*/, CF_DepthNearOrEqual>::GetRHI()) MobileBasePass.cpp:1157

FMobileBasePassMeshProcessor::TryAddMeshBatch / Process   MobileBasePass.cpp (~L900-L1003)
 └─ MobileBasePass::SetOpaqueRenderState                  MobileBasePass.cpp:531
     (可选覆盖为 <true, CF_DepthNearOrEqual, stencil...>，DepthWrite 仍为 true)
 └─ BuildMeshDrawCommands                                 MobileBasePass.cpp:990
     └─ FMeshPassProcessor::BuildMeshDrawCommands         MeshPassProcessor.inl:47
         └─ PipelineState.DepthStencilState = DrawRenderState.GetDepthStencilState()  MeshPassProcessor.inl:104
            （烘进 MeshDrawCommand 的 PSO，进入 EMeshPass::BasePass 的命令缓存）
```

---

## 6. 与你原始问题的对应

> **“正常情况下并没有开启 EarlyZPass，所以深度会和 BasePass 一起写入，验证移动端 Forward 渲染路径下只会和 MobileBasePass 一起渲染对吧？”**

是的，验证如下：

1. `FScene::GetDefaultBasePassDepthStencilAccess`：未开 EarlyZ ⇒ `DepthWrite_StencilWrite`。
2. `InitRenderTargetBindings_Forward`：`bIsFullDepthPrepassEnabled == false` ⇒ DepthStencil 以 `EClear + DepthWrite_StencilWrite` 绑定。
3. `RenderMaskedPrePass` 仅在 `bIsMaskedOnlyDepthPrepassEnabled` 时才会执行 `RenderPrePass`（`MobileShadingRenderer.cpp:849`），普通 Forward 不会触发。
4. `CreateMobileBasePassProcessor` 把 `<true, CF_DepthNearOrEqual>` 作为默认 DepthStencilState 写入 PSO；不透明分支 (`SetOpaqueRenderState`) 即使为支持 Decal/Deferred 加 Stencil mask，也保留 `DepthWrite=true`。
5. Translucency / Decals / Fog 等后续阶段使用 `<false, CF_DepthNearOrEqual>` 或 `DepthRead_StencilRead`，**不再写 Depth**（见 `CreateMobileTranslucency*Processor` 在 `MobileBasePass.cpp:1184-1216`，以及 `RenderForwardSinglePass` 中 `RHICmdList.NextSubpass()` 进入 “scene depth is read only and can be fetched” 的子通道）。

因此 Opaque/Masked 深度写入 **只发生在 `RenderMobileBasePass` 里派发的 DrawCall**（以及前置可选的 Masked PrePass）。

---

## 7. 备注 / 拓展

- **Multi-Pass 路径**：`RenderForwardMultiPass` 会把 Translucent 分到第二个 RenderPass，BasePass 写 Depth 的逻辑与 SinglePass 一致；同样最终由 `RenderMobileBasePass → DispatchDraw → SubmitDraw → DrawXxxPrimitive` 完成。
- **Editor Primitives**：`RenderMobileEditorPrimitives`（`MobileBasePassRendering.cpp:493`）也复用同一 `DefaultBasePassDepthStencilAccess`，在 Editor 环境同样会写入深度。
- **Stencil 用途**：BasePass 的 `SO_Replace` 用来写入 Receive-Decal / ShadingModel / LightingChannels 等 bit 到 Stencil，便于后续延迟解码（Mobile Deferred）或 Decal 阶段使用。
- **真正“写入深度”的语义**：UE 在 C++ 没有显式 `WriteDepth` API；它表现为
  1. `FDepthStencilBinding` 在 `BeginRenderPass` 时把 SceneDepth 绑成 RenderPass 的 DepthStencil Attachment；
  2. PSO 的 `FRHIDepthStencilState` 把 `bEnableDepthWrite=true`；
  3. `RHICmdList.DrawIndexedPrimitive/DrawPrimitive` 触发硬件管线，由 ROP 完成实际的写入。

---

### 关键文件索引

| 模块 | 文件 | 重要行号 |
|---|---|---|
| 默认 DepthStencilAccess | `Engine/Source/Runtime/Renderer/Private/MobileBasePass.cpp` | 1151-1163（`CreateMobileBasePassProcessor`） |
| 不透明状态覆盖 | `Engine/Source/Runtime/Renderer/Private/MobileBasePass.cpp` | 531-575（`SetOpaqueRenderState`） |
| Forward RT 绑定 | `Engine/Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp` | 1448-1501（`InitRenderTargetBindings_Forward`） |
| Forward 主流程 | `Engine/Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp` | 1503-1576（`RenderForward`） |
| SinglePass RDG | `Engine/Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp` | 1578-1700（`RenderForwardSinglePass`） |
| RenderMobileBasePass | `Engine/Source/Runtime/Renderer/Private/MobileBasePassRendering.cpp` | 470-491 |
| BuildMeshDrawCommands | `Engine/Source/Runtime/Renderer/Public/MeshPassProcessor.inl` | 47-110 |
| DispatchDraw | `Engine/Source/Runtime/Renderer/Private/MeshDrawCommands.cpp` | 1640-1717 |
| SubmitDrawCommands | `Engine/Source/Runtime/Renderer/Private/InstanceCulling/InstanceCullingContext.cpp` | 1663-1729 |
| SubmitDrawBegin/End（实际 DrawCall） | `Engine/Source/Runtime/Renderer/Private/MeshPassProcessor.cpp` | 1358-1482 |

---

## 8. 追加验证：实际深度写入是在 RHI / 图形 API Draw 命令中发生

是的：前面的 `FDepthStencilBinding`、`FMeshPassProcessorRenderState`、`FRHIDepthStencilState`、`FGraphicsPipelineStateInitializer` 都是在设置 **RenderPass 附件** 和 **Graphics PSO 状态**；真正触发 GPU 深度测试/深度写入的是 RHI Draw 命令，Vulkan 后端会转换为 `vkCmdDrawIndexed` / `vkCmdDraw`。深度值是否写入由 Vulkan Graphics Pipeline 的 `VkPipelineDepthStencilStateCreateInfo.depthWriteEnable` 和当前 RenderPass 的 DepthStencil Attachment 共同决定。

### 8.1 UE 的 DrawCall 先进入 RHICommandList

`Engine/Source/Runtime/Renderer/Private/MeshPassProcessor.cpp:1433-1482`

```cpp
void FMeshDrawCommand::SubmitDrawEnd(...)
{
    if (MeshDrawCommand.IndexBuffer)
    {
        RHICmdList.DrawIndexedPrimitive(...); // L1446
    }
    else
    {
        RHICmdList.DrawPrimitive(...);        // L1469
    }
}
```

`Engine/Source/Runtime/RHI/Public/RHICommandList.h:3550-3559`

```cpp
void DrawIndexedPrimitive(...)
{
    if (Bypass())
    {
        GetContext().RHIDrawIndexedPrimitive(...); // L3555
        return;
    }
    ALLOC_COMMAND(FRHICommandDrawIndexedPrimitive)(...); // L3558
}
```

如果没有 bypass，会记录一条 `FRHICommandDrawIndexedPrimitive`；执行时再转给当前 RHI Context：

`Engine/Source/Runtime/RHI/Public/RHICommandListCommandExecutes.inl:155-158`

```cpp
void FRHICommandDrawIndexedPrimitive::Execute(FRHICommandListBase& CmdList)
{
    RHISTAT(DrawIndexedPrimitive);
    INTERNAL_DECORATOR(RHIDrawIndexedPrimitive)(IndexBuffer, BaseVertexIndex, FirstInstance, NumVertices, StartIndex, NumPrimitives, NumInstances);
}
```

因此 `RenderMobileBasePass → RHICmdList.DrawIndexedPrimitive` 之后，最终一定进入具体平台 RHI 的 `RHIDrawIndexedPrimitive`。

### 8.2 Vulkan 后端把 RHI Draw 转成 Vulkan 命令

`Engine/Source/Runtime/VulkanRHI/Private/VulkanCommands.cpp:661-680`

```cpp
void FVulkanCommandListContext::RHIDrawIndexedPrimitive(...)
{
    CommitGraphicsResourceTables();

    FVulkanCmdBuffer* Cmd = CommandBufferManager->GetActiveCmdBuffer();
    VkCommandBuffer CmdBuffer = Cmd->GetHandle();
    PendingGfxState->PrepareForDraw(Cmd);                         // L676
    VulkanRHI::vkCmdBindIndexBuffer(CmdBuffer, IndexBuffer->GetHandle(), IndexBuffer->GetOffset(), IndexBuffer->GetIndexType()); // L677

    uint32 NumIndices = GetVertexCountForPrimitiveCount(NumPrimitives, PendingGfxState->PrimitiveType);
    VulkanRHI::vkCmdDrawIndexed(CmdBuffer, NumIndices, NumInstances, StartIndex, BaseVertexIndex, FirstInstance); // L680
}
```

非索引绘制同理：

`Engine/Source/Runtime/VulkanRHI/Private/VulkanCommands.cpp:613-627`

```cpp
void FVulkanCommandListContext::RHIDrawPrimitive(...)
{
    CommitGraphicsResourceTables();
    FVulkanCmdBuffer* CmdBuffer = CommandBufferManager->GetActiveCmdBuffer();
    PendingGfxState->PrepareForDraw(CmdBuffer);                    // L625
    VulkanRHI::vkCmdDraw(CmdBuffer->GetHandle(), NumVertices, NumInstances, BaseVertexIndex, 0); // L627
}
```

这证明移动端 Vulkan 下，UE 的 BasePass DrawCall 最终就是 Vulkan command buffer 中的 `vkCmdDrawIndexed` / `vkCmdDraw`。

### 8.3 Draw 前绑定的 Vulkan Graphics Pipeline 带有 DepthWrite=true

BasePass 创建的是：

`Engine/Source/Runtime/Renderer/Private/MobileBasePass.cpp:1157`

```cpp
PassDrawRenderState.SetDepthStencilState(TStaticDepthStencilState<true, CF_DepthNearOrEqual>::GetRHI());
```

`TStaticDepthStencilState<true, ...>` 的第一个模板参数会进入 `FDepthStencilStateInitializerRHI::bEnableDepthWrite`：

`Engine/Source/Runtime/RenderCore/Public/RHIStaticStates.h:197-254`

```cpp
template<bool bEnableDepthWrite = true, ECompareFunction DepthTest = CF_DepthNearOrEqual, ...>
class TStaticDepthStencilState ...
{
    static FDepthStencilStateRHIRef CreateRHI()
    {
        FDepthStencilStateInitializerRHI Initializer(
            bEnableDepthWrite, // L238
            DepthTest,
            ...);
        return RHICreateDepthStencilState(Initializer); // L253
    }
};
```

Vulkan RHI 创建 `VkPipelineDepthStencilStateCreateInfo` 时直接使用这个 `bEnableDepthWrite`：

`Engine/Source/Runtime/VulkanRHI/Private/VulkanState.cpp:265-271`

```cpp
void FVulkanDepthStencilState::SetupCreateInfo(..., VkPipelineDepthStencilStateCreateInfo& OutDepthStencilState)
{
    OutDepthStencilState.depthTestEnable = (Initializer.DepthTest != CF_Always || Initializer.bEnableDepthWrite) ? VK_TRUE : VK_FALSE;
    OutDepthStencilState.depthCompareOp = CompareOpToVulkan(Initializer.DepthTest);
    OutDepthStencilState.depthWriteEnable = Initializer.bEnableDepthWrite ? VK_TRUE : VK_FALSE; // L271 ★
}
```

PSO 创建时把该 DepthStencilState 写进 Vulkan Pipeline 描述：

`Engine/Source/Runtime/VulkanRHI/Private/VulkanPipeline.cpp:1873-1878`

```cpp
VkPipelineDepthStencilStateCreateInfo DSInfo;
ResourceCast(PSOInitializer.DepthStencilState)->SetupCreateInfo(PSOInitializer, DSInfo);
OutGfxEntry->DepthStencil.ReadFrom(DSInfo);
```

随后 `DepthStencil` 被写回 `VkGraphicsPipelineCreateInfo.pDepthStencilState`：

`Engine/Source/Runtime/VulkanRHI/Private/VulkanPipeline.cpp:1338-1343`

```cpp
VkPipelineDepthStencilStateCreateInfo DepthStencilState;
GfxEntry->DepthStencil.WriteInto(DepthStencilState);
PipelineInfo.pDepthStencilState = &DepthStencilState; // L1343
```

最终调用 Vulkan 创建 Graphics Pipeline：

`Engine/Source/Runtime/VulkanRHI/Private/VulkanPipeline.cpp:1491`

```cpp
Result = VulkanRHI::vkCreateGraphicsPipelines(Device->GetInstanceHandle(), PipelineCache, 1, &PipelineInfo, VULKAN_CPU_ALLOCATOR, &PSO->VulkanPipeline);
```

因此 BasePass 的 `TStaticDepthStencilState<true, CF_DepthNearOrEqual>` 会变成 Vulkan Pipeline 里的：

```cpp
VkPipelineDepthStencilStateCreateInfo.depthWriteEnable = VK_TRUE;
VkPipelineDepthStencilStateCreateInfo.depthCompareOp   = CompareOpToVulkan(CF_DepthNearOrEqual);
```

### 8.4 Draw 前绑定该 Vulkan Pipeline

UE 在 `SubmitDrawBegin` 中设置 Graphics Pipeline：

`Engine/Source/Runtime/Renderer/Private/MeshPassProcessor.cpp:1398`

```cpp
SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit, MeshDrawCommand.StencilRef, ...);
```

Vulkan RHI 对应实现：

`Engine/Source/Runtime/VulkanRHI/Private/VulkanPipelineState.cpp:559-575`

```cpp
void FVulkanCommandListContext::RHISetGraphicsPipelineState(...)
{
    FVulkanRHIGraphicsPipelineState* Pipeline = ResourceCast(GraphicsState);
    ...
    if (PendingGfxState->SetGfxPipeline(Pipeline, bForceResetPipeline))
    {
        PendingGfxState->Bind(CmdBuffer->GetHandle()); // L574
    }
}
```

`PendingGfxState->Bind` 调到 Graphics Pipeline 的 `Bind`：

`Engine/Source/Runtime/VulkanRHI/Private/VulkanPendingState.h:273-276`

```cpp
inline void Bind(VkCommandBuffer CmdBuffer)
{
    CurrentPipeline->Bind(CmdBuffer);
}
```

`Engine/Source/Runtime/VulkanRHI/Private/VulkanPipeline.h:727-729`

```cpp
inline void Bind(VkCommandBuffer CmdBuffer)
{
    VulkanRHI::vkCmdBindPipeline(CmdBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, VulkanPipeline);
}
```

所以顺序是：先 `vkCmdBindPipeline` 绑定含 `depthWriteEnable=VK_TRUE` 的 Graphics Pipeline，再执行 `vkCmdDrawIndexed`。

### 8.5 RenderPass / Framebuffer 绑定 DepthStencil Attachment

`InitRenderTargetBindings_Forward` 给 RDG Pass 设置 `SceneDepth` 为 DepthStencilTarget。RDG 执行时进入 Vulkan 的 `RHIBeginRenderPass`：

`Engine/Source/Runtime/VulkanRHI/Private/VulkanRenderTarget.cpp:604-689`

```cpp
FRHITexture* DSTexture = InInfo.DepthStencilRenderTarget.DepthStencilTarget; // L604
...
const FExclusiveDepthStencil ExclusiveDepthStencil = InInfo.DepthStencilRenderTarget.ExclusiveDepthStencil;
if (ExclusiveDepthStencil.IsDepthWrite())
{
    CurrentDepthLayout = VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL; // L617-620
}
...
FVulkanRenderTargetLayout RTLayout(*Device, InInfo, CurrentDepthLayout, CurrentStencilLayout); // L680
FVulkanRenderPass* RenderPass = Device->GetRenderPassManager().GetOrCreateRenderPass(RTLayout); // L683
FVulkanFramebuffer* Framebuffer = Device->GetRenderPassManager().GetOrCreateFramebuffer(RTInfo, RTLayout, RenderPass); // L687
Device->GetRenderPassManager().BeginRenderPass(...); // L689
```

随后实际调用 Vulkan BeginRenderPass：

`Engine/Source/Runtime/VulkanRHI/Private/VulkanRenderpass.cpp:175-209`

```cpp
FRHITexture* DSTexture = RPInfo.DepthStencilRenderTarget.DepthStencilTarget; // L175
...
CmdBuffer->BeginRenderPass(RenderPass->GetLayout(), RenderPass, Framebuffer, ClearValues); // L209
```

`Engine/Source/Runtime/VulkanRHI/Private/VulkanCommandBuffer.cpp:186-216`

```cpp
VkRenderPassBeginInfo Info;
Info.renderPass = RenderPass->GetHandle();
Info.framebuffer = Framebuffer->GetHandle();
...
VulkanRHI::vkCmdBeginRenderPass(CommandBufferHandle, &Info, VK_SUBPASS_CONTENTS_INLINE); // L216
// 或 vkCmdBeginRenderPass2KHR // L212
```

这证明 `SceneDepth` 已经作为 Vulkan RenderPass/Framebuffer 的 DepthStencil Attachment 被绑定。

### 8.6 最终 Vulkan 层面的深度写入条件

对于 MobileBasePass，Vulkan 命令流语义可以简化为：

```cpp
vkCmdBeginRenderPass(... SceneDepth as depth/stencil attachment ...);
vkCmdBindPipeline(... graphics pipeline with depthWriteEnable = VK_TRUE ...);
vkCmdBindIndexBuffer(...);
vkCmdDrawIndexed(...);
```

深度写入不是一个单独的 `vkCmdWriteDepth` 命令；Vulkan 也没有这种 Draw 外的显式接口。它发生在 `vkCmdDrawIndexed` 执行的图形管线中：片元经过 Depth Test（`depthCompareOp`）后，如果通过且 `depthWriteEnable=VK_TRUE`，GPU 会把片元深度写入当前 RenderPass 的 DepthStencil Attachment。

所以可以确认：

1. **前面的 UE 代码主要是在设置状态**：RenderPass 的 DepthStencil 附件、DepthStencilAccess、Graphics PSO 的 DepthStencilState、StencilRef、Viewport、Vertex/Index Buffer、Shader Binding。
2. **实际触发写深度的是 RHI Draw 命令**：`RHICmdList.DrawIndexedPrimitive` → `RHIDrawIndexedPrimitive`。
3. **在 Vulkan Android 后端会转换成 Vulkan 命令**：`vkCmdBindPipeline` + `vkCmdDrawIndexed`，其中 `vkCmdDrawIndexed` 执行时由硬件固定功能把深度写入 Attachment。
4. **是否写深度由状态决定**：BasePass 的 `<true, CF_DepthNearOrEqual>` → `depthWriteEnable = VK_TRUE`，而 Translucency 的 `<false, ...>` → `depthWriteEnable = VK_FALSE`，因此后续半透明不会再写 SceneDepth。

