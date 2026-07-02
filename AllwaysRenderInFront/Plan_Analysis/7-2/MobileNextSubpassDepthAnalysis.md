# MobileShadingRenderer.cpp:1614 NextSubpass 与深度写入分析

## 结论

`MobileShadingRenderer.cpp:1614` 的

```cpp
// scene depth is read only and can be fetched
RHICmdList.NextSubpass();
```

不是在这里“创建”了一个 subpass。真正的 native render pass/subpass 在这个 RDG raster pass 开始时，由 `RenderTargets.SubpassHint` 创建；`NextSubpass()` 只是把正在执行的 render pass 推进到已经创建好的下一个 subpass。

对 `RenderForwardSinglePass()` 这条路径，`NextSubpass()` 之后进入的是 `ESubpassHint::DepthReadSubpass` 的第二个 native subpass。Vulkan 下该 subpass 把 depth attachment 设置为 `VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL`，同时把 depth 作为 input attachment 传入，因此之后的 subpass 不应再写 depth；它只能读/fetch depth。Stencil 另说：这个 layout 名字里是 `STENCIL_ATTACHMENT`，所以设计上仍允许 stencil attachment 用法。

最终能不能写 depth 不是由 `CreateMobileBasePassProcessor()` 这类 processor 单独决定的。更准确地说：

- render pass/subpass 的 depth attachment 读写能力由 `FRenderTargetBindingSlots::DepthStencil` 和 `SubpassHint` 决定；
- processor / mesh pass render state 只决定每个 draw/PSO 是否启用 depth write/test、stencil write/test；
- 两者必须同时允许，才可能真正写 depth。`NextSubpass()` 后的 depth-read subpass 已经在 render pass/subpass 层把 depth 设成只读，因此只改 processor 不能让它合法写 depth。

## 代码证明链

### 1. Mobile 侧先声明这是一个 depth-read subpass render pass

`RenderForwardSinglePass()` 进入 RDG pass 之前设置 `SubpassHint`：

```cpp
PassParameters->RenderTargets.SubpassHint =
    bTonemapSubpassInline ? ESubpassHint::CustomResolveSubpass : ESubpassHint::DepthReadSubpass;
```

位置：`Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp:1586`

同一个函数里，先执行 depth pre-pass / base pass，然后才调用 `NextSubpass()`：

```cpp
RenderMaskedPrePass(RHICmdList, View);
RenderMobileBasePass(RHICmdList, View, &PassParameters->InstanceCullingDrawParams);
...
// scene depth is read only and can be fetched
RHICmdList.NextSubpass();
RenderDecals(...);
RenderTranslucency(...);
```

位置：`Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp:1604-1623`

`DepthReadSubpass` 的 RHI 枚举注释也直接说明它是“render pass has depth reading subpass”：

```cpp
enum class ESubpassHint : uint8
{
    None,
    DepthReadSubpass,
    DeferredShadingSubpass,
    CustomResolveSubpass,
};
```

位置：`Source/Runtime/RHI/Public/RHIResources.h:3687-3700`

### 2. RDG 把 RenderTargets.SubpassHint 变成 FRHIRenderPassInfo

RDG 参数转 RHI render pass 时，会把 `RenderTargets.SubpassHint` 原样复制到 `FRHIRenderPassInfo`：

```cpp
RenderPassInfo.SubpassHint = RenderTargets.SubpassHint;
```

位置：`Source/Runtime/RenderCore/Private/RenderGraphPass.cpp:89-92`

RDG 执行 raster pass 时，真正调用 `BeginRenderPass`：

```cpp
static_cast<FRHICommandList&>(RHICmdListPass)
    .BeginRenderPass(Pass->GetParameters().GetRenderPassInfo(), Pass->GetName());
```

位置：`Source/Runtime/RenderCore/Private/RenderGraphBuilder.cpp:2918-2922`

### 3. FRHICommandList 在 BeginRenderPass 时初始化 subpass 状态；NextSubpass 只递增 index

`BeginRenderPass()` 会缓存 render targets，并把 command list 的 subpass 状态重置到 `InInfo.SubpassHint` 和 index 0：

```cpp
CacheActiveRenderTargets(InInfo);
ResetSubpass(InInfo.SubpassHint);
PersistentState.bInsideRenderPass = true;
```

位置：`Source/Runtime/RHI/Public/RHICommandList.h:3837-3856`

`NextSubpass()` 的 RHI 层行为只有两件事：发出 `RHINextSubpass()` 命令，然后 `IncrementSubpass()`：

```cpp
FORCEINLINE_DEBUGGABLE void NextSubpass()
{
    check(IsInsideRenderPass());
    ...
    GetContext().RHINextSubpass();
    ...
    IncrementSubpass();
}
```

位置：`Source/Runtime/RHI/Public/RHICommandList.h:3888-3899`

`IncrementSubpass()` 只是 `PersistentState.SubpassIndex++`：

```cpp
void IncrementSubpass()
{
    PersistentState.SubpassIndex++;
}
```

位置：`Source/Runtime/RHI/Public/RHICommandList.h:1070-1073`

也就是说，`NextSubpass()` 没有创建新的 attachment 或 render pass layout。

### 4. Vulkan BeginRenderPass 时创建/复用 native render pass

Vulkan 的 `RHIBeginRenderPass()` 会用 `FRHIRenderPassInfo` 创建 `FVulkanRenderTargetLayout`，然后从 render pass manager 取或建 `FVulkanRenderPass`：

```cpp
FVulkanRenderTargetLayout RTLayout(*Device, InInfo, CurrentDepthLayout, CurrentStencilLayout);
FVulkanRenderPass* RenderPass = Device->GetRenderPassManager().GetOrCreateRenderPass(RTLayout);
...
Device->GetRenderPassManager().BeginRenderPass(...);
```

位置：`Source/Runtime/VulkanRHI/Private/VulkanRenderTarget.cpp:680-689`

`GetOrCreateRenderPass()` 找不到缓存时创建：

```cpp
FVulkanRenderPass* RenderPass = new FVulkanRenderPass(*Device, RTLayout);
```

位置：`Source/Runtime/VulkanRHI/Private/VulkanRenderpass.h:803-827`

`FVulkanRenderPass` 构造函数里调用 `CreateVulkanRenderPass()`：

```cpp
RenderPass = CreateVulkanRenderPass(InDevice, InRTLayout);
```

位置：`Source/Runtime/VulkanRHI/Private/VulkanRHI.cpp:1885-1892`

`CreateVulkanRenderPass()` 用 `FVulkanRenderPassBuilder` 生成 `VkRenderPass`：

```cpp
FVulkanRenderPassBuilder<...> Creator(InDevice);
OutRenderpass = Creator.Create(RTLayout);
```

位置：`Source/Runtime/VulkanRHI/Private/VulkanRenderpass.cpp:11-24`

### 5. Vulkan subpass 数组由 SubpassHint 创建

`FVulkanRenderPassBuilder::BuildCreateInfo()` 读取 `RTLayout.GetSubpassHint()`，决定是否创建 depth-read subpass：

```cpp
const bool bCustomResolveSubpass = RTLayout.GetSubpassHint() == ESubpassHint::CustomResolveSubpass;
const bool bDepthReadSubpass = bCustomResolveSubpass ||
    (RTLayout.GetSubpassHint() == ESubpassHint::DepthReadSubpass);
```

位置：`Source/Runtime/VulkanRHI/Private/VulkanRenderpass.h:426-434`

当存在 depth-read subpass 时，它先为后续 subpass 准备一个 depth 只读、stencil attachment 的 reference：

```cpp
DepthStencilAttachment.layout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL;
DepthInputAttachment = DepthStencilAttachment.attachment;
DepthInputAttachmentLayout = DepthStencilAttachment.layout;
DepthInputAspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
```

位置：`Source/Runtime/VulkanRHI/Private/VulkanRenderpass.h:467-485`

第 0 个 subpass 是 main subpass，使用原始 depth/stencil attachment reference：

```cpp
SubpassDesc.SetColorAttachments(ColorAttachmentReferences, NumColorAttachments);
SubpassDesc.SetDepthStencilAttachment(&DepthStencilAttachmentReference);
```

位置：`Source/Runtime/VulkanRHI/Private/VulkanRenderpass.h:490-499`

第 1 个 subpass 是 “Color write and depth read sub-pass”。它把 depth 同时设成 input attachment 和 depth/stencil attachment，但 attachment layout 是上面的 depth-read layout：

```cpp
InputAttachments1[0].attachment = DepthInputAttachment;
InputAttachments1[0].layout = DepthInputAttachmentLayout;
InputAttachments1[0].SetAspect(DepthInputAspectMask);
SubpassDesc.SetInputAttachments(InputAttachments1, InputAttachment1Count);
SubpassDesc.SetDepthStencilAttachment(&DepthStencilAttachment);
```

位置：`Source/Runtime/VulkanRHI/Private/VulkanRenderpass.h:508-523`

最后，builder 把这些 subpass description 写进 `VkRenderPassCreateInfo`：

```cpp
CreateInfo.subpassCount = NumSubpasses;
CreateInfo.pSubpasses = SubpassDescriptions;
CreateInfo.pDependencies = SubpassDependencies;
```

位置：`Source/Runtime/VulkanRHI/Private/VulkanRenderpass.h:688-693`

所以 subpass 是在 RHI `BeginRenderPass` 内部、发出 native `vkCmdBeginRenderPass` 前的 render pass 创建/缓存路径里确定的，不是在 `RHICmdList.NextSubpass()` 行现场创建的。

### 6. Vulkan NextSubpass 只是 vkCmdNextSubpass

Vulkan 的 `RHINextSubpass()` 是：

```cpp
void FVulkanCommandListContext::RHINextSubpass()
{
    check(CurrentRenderPass);
    ...
    VulkanRHI::vkCmdNextSubpass(Cmd, VK_SUBPASS_CONTENTS_INLINE);
}
```

位置：`Source/Runtime/VulkanRHI/Private/VulkanRenderTarget.cpp:721-726`

这进一步证明：native subpass 已经存在；这里仅切换到下一个 subpass。

## NextSubpass 之后还能写 depth 吗？

对 `RenderForwardSinglePass()` 的这句 `NextSubpass()` 之后：不能写 depth。

原因是第二个 subpass 的 depth attachment layout 是：

```cpp
VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL
```

这个 layout 明确表达 depth read-only。并且同一个 subpass 把 depth 作为 input attachment：

```cpp
SubpassDesc.SetInputAttachments(InputAttachments1, InputAttachment1Count);
```

这正对应注释中的 “scene depth is read only and can be fetched”。

RHI 的 validate 也要求 depth-read subpass 的 depth target 支持 input attachment：

```cpp
if (SubpassHint == ESubpassHint::DepthReadSubpass ||
    SubpassHint == ESubpassHint::CustomResolveSubpass)
{
    ensure(EnumHasAnyFlags(
        DepthStencilRenderTarget.DepthStencilTarget->GetFlags(),
        TexCreate_InputAttachmentRead));
}
```

位置：`Source/Runtime/RHI/Private/RHI.cpp:1653-1658`

注意，这里的结论是 depth 不能写；stencil 可能仍可写，因为 Vulkan layout 用的是 `DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL`，不是 depth/stencil 都只读的 layout。

## Processor 与 Render Pass 初始状态分别决定什么

`CreateMobileBasePassProcessor()` 会创建 mesh pass processor，并设置默认 draw state：

```cpp
PassDrawRenderState.SetDepthStencilAccess(DefaultBasePassDepthStencilAccess);
PassDrawRenderState.SetDepthStencilState(
    TStaticDepthStencilState<true, CF_DepthNearOrEqual>::GetRHI());
```

位置：`Source/Runtime/Renderer/Private/MobileBasePass.cpp:1151-1162`

`MobileBasePass::SetOpaqueRenderState()` 还可能为 opaque base pass 设置带 stencil replace 的 depth/stencil state：

```cpp
DrawRenderState.SetDepthStencilState(TStaticDepthStencilState<
    true, CF_DepthNearOrEqual,
    true, CF_Always, SO_Keep, SO_Keep, SO_Replace,
    ...
>::GetRHI());
```

位置：`Source/Runtime/Renderer/Private/MobileBasePass.cpp:531-556`

这些 processor 决定的是每个 mesh draw command / PSO 的 depth-stencil state。它们不会创建 Vulkan subpass，也不会改 `VkSubpassDescription` 的 attachment layout。

真正传给 render pass 的 depth/stencil attachment access 来自 `FDepthStencilBinding`：

```cpp
BasePassRenderTargets.DepthStencil = bIsFullDepthPrepassEnabled ?
    FDepthStencilBinding(SceneDepth, ..., FExclusiveDepthStencil::DepthRead_StencilWrite) :
    FDepthStencilBinding(SceneDepth, ..., FExclusiveDepthStencil::DepthWrite_StencilWrite);
```

位置：`Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp:1494-1496`

`FDepthStencilBinding` 保存的 access 会被 RDG 写入 `FRHIRenderPassInfo.DepthStencilRenderTarget.ExclusiveDepthStencil`：

```cpp
const FExclusiveDepthStencil ExclusiveDepthStencil = DepthStencil.GetDepthStencilAccess();
DepthStencilTarget.ExclusiveDepthStencil = ExclusiveDepthStencil;
```

位置：`Source/Runtime/RenderCore/Private/RenderGraphPass.cpp:70-84`

`FExclusiveDepthStencil` 的枚举定义也说明了 `DepthRead` / `DepthWrite` 的含义：

```cpp
DepthRead = 0x01,
DepthWrite = 0x02,
StencilRead = 0x10,
StencilWrite = 0x20,
DepthRead_StencilWrite = DepthRead + StencilWrite,
DepthWrite_StencilWrite = DepthWrite + StencilWrite,
```

位置：`Source/Runtime/RHI/Public/RHIResources.h:416-441`

因此最终影响可以拆成两层：

| 层级 | 由谁决定 | 影响 |
| --- | --- | --- |
| Render pass / subpass attachment layout | `FDepthStencilBinding` + `RenderTargets.SubpassHint` | 决定这个 subpass 的 depth attachment 是否可写、是否能作为 input attachment fetch |
| Draw/PSO depth-stencil state | `CreateMobileBasePassProcessor()`、`SetOpaqueRenderState()`、translucency processor 等 | 决定某个 draw 是否尝试 depth write/test/stencil write |

RHI validation 也体现了这两层要匹配：PSO 如果写 depth，但当前 render pass 的 DSV 不是 depth write，会触发检查：

```cpp
checkf(DSMode.IsDepthRead() || DSV.ExclusiveDepthStencil.IsDepthWrite(),
    TEXT("Graphics PSO is writing to depth but RenderPass depth is ReadOnly."));
```

位置：`Source/Runtime/RHI/Public/RHIValidationContext.h:1205-1221`

## 对当前代码的直接判断

`MobileShadingRenderer.cpp:1614` 后面执行的是 decals、modulated shadows、fog、translucency、occlusion、pre-tonemap 等阶段。此时处在 `DepthReadSubpass` 的后续 subpass 中，depth 设计为只读并可作为 input attachment/fetch 使用。

如果某个后续 draw/processor 试图启用 depth write：

1. processor 可能生成一个 depth-write PSO；
2. 但当前 Vulkan subpass 的 depth attachment layout 仍是 read-only；
3. 这不是合法的 depth 写入路径，修 processor 不足以改变结果；
4. 要真正写 depth，需要从 render pass/subpass 设计上改：例如不要进入 depth-read subpass，或拆成新的 render pass，并用 writable depth attachment access/layout。

所以这行注释的本质含义是：`NextSubpass()` 之后切到的 subpass 已经由 `SubpassHint` 预创建成 depth-read/input-attachment subpass；它不是 runtime 临时锁深度，也不是 `CreateMobileBasePassProcessor()` 决定的。
