# MobileAfterTranslucencyPass 深度写入与 Subpass 关系分析

本文档针对 `Docs/Plan.md` 第 280–286 行中 `CreateMobileAfterTranslucencyPassProcessor` 的实现，特别是这一句：

```cpp
PassDrawRenderState.SetDepthStencilState(
    TStaticDepthStencilState<false, CF_DepthNearOrEqual>::GetRHI());
```

回答下列具体问题：

1. `TStaticDepthStencilState<false, CF_DepthNearOrEqual>::GetRHI()` 这句的行为是什么？`SetDepthStencilState` 与 `TStaticDepthStencilState` 各自作用？
2. `MobileShadingRenderer.cpp:1614` 处 `RHICmdList.NextSubpass();` 注释 "scene depth is read only and can be fetched"，与这里的 false 有什么关系？
3. 场景中所有物体在进入 `RenderForwardSinglePass` 之前是否已经在深度 Pass 中写过一遍深度了？AfterTranslucency 物体此时是否已经存在硬件深度？
4. Plan 第 280 行 `SetBlendState` 与第 281 行 `SetDepthStencilState` 分别在做什么？是否必要？
5. 分析 `MobileDepthPass` 的深度渲染路径，与我这里的 Pass 有没有联系？
6. "这里写入深度"具体指什么？我在 `RenderTranslucency` 之后调用 `RenderMobileAfterTranslucencyPass` 时读取深度，AfterTranslucency 物体此时是否已经写入了深度？

---

## 1. 两个 API 的作用与语义

### `TStaticDepthStencilState<bEnableDepthWrite, DepthTest, ...>`

- 它是 `Engine/Source/Runtime/RHI/Public/RHIStaticStates.h` 中的模板。
- **编译期**根据模板参数实例化出一个 **单例 RHI DepthStencil State 对象**（线程安全、引用计数永远保持），`GetRHI()` 返回 `FRHIDepthStencilState*`。
- 模板参数（节选最常用的两个）：
  - `bool bEnableDepthWrite`：**深度写掩码 (DepthMask)**。`true` 表示通过 ZTest 的 fragment 会把自己的 Z 值写回 DepthBuffer；`false` 表示只做测试不写回。
  - `ECompareFunction DepthTest`：深度比较函数。`CF_DepthNearOrEqual` 在 UE 的反向 Z 下相当于 `GE`（“新像素 Z >= 当前 Z 则通过”）。
- 它**不会**自动绑定到任何 Pass，它只是“PSO 的一个状态描述符”。

### `FMeshPassProcessorRenderState::SetDepthStencilState(FRHIDepthStencilState*)`

- 把上面那个静态描述符 **挂到当前 Pass 的渲染状态对象上**。
- 之后 `FMeshPassProcessor` 在 `BuildMeshDrawCommands` 时会把它写入每个 `FMeshDrawCommand` 的 `FGraphicsMinimalPipelineStateInitializer`，最终在 PSO 创建/绑定阶段生效。
- 所以 `SetDepthStencilState` 影响的是 **该 Pass 内每一个 DrawCommand 的 PSO**。

> 一句话：`TStaticDepthStencilState<...>` 制造一个“怎么测试 / 是否写”的 RHI 状态对象；`SetDepthStencilState(...)` 把它绑给本 Pass 的所有 DrawCall。

### 另一个常被混淆的：`SetDepthStencilAccess(FExclusiveDepthStencil)`

- 这是 **RDG / RenderPass 层**的访问权限声明（用于决定 `FDepthStencilBinding` 的 Access、tile 内存模式、资源 transition），例如 `DepthRead_StencilRead`、`DepthWrite_StencilWrite`。
- 它与 `SetDepthStencilState` 是两层：
  - `SetDepthStencilState` 决定 **PSO**（要不要写）；
  - `SetDepthStencilAccess` 决定 **DSV / Attachment**（被允许写吗）。
- 如果 PSO 想写但 Attachment 是只读 → 实际依然不会写（部分平台会 validation error，在 Vulkan tile pipeline 上是“硬件忽略 + 性能优化”）。

---

## 2. SinglePass + `DepthReadSubpass` 决定了“你写不进深度”

`MobileShadingRenderer.cpp:1586` 设置：

```cpp
PassParameters->RenderTargets.SubpassHint =
    bTonemapSubpassInline ? ESubpassHint::CustomResolveSubpass
                          : ESubpassHint::DepthReadSubpass;
```

这是 **整个 SceneColorRendering RDG Pass** 的 SubpassHint，它会让 RHI 在 Vulkan / Metal / 移动 OpenGL 等 Tile-based 后端上构造一个**多 subpass 的 RenderPass**：

- **Subpass 0**：BasePass（含 SkyPass、Editor Primitives、Masked PrePass 阶段产生的内容也整合在前面），DepthStencil 作为 **读写 attachment**。在这里 BasePass 用 `<true, CF_DepthNearOrEqual>`，**写入**深度。
- **`RHICmdList.NextSubpass();`（line 1614）切到 Subpass 1**：此时 DepthStencil 被声明为 **只读 InputAttachment**（即注释“scene depth is read only and can be fetched”——它可以在 PS 中通过 framebuffer fetch / SubpassLoad 读取硬件深度，但不允许写）。
- 之后的 `RenderDecals` / `RenderModulatedShadowProjections` / `RenderFog` / `RenderTranslucency` 都跑在 **Subpass 1** 里，所以它们**必须**用 `DepthWrite=false` 的 DepthStencilState。

这正是 `CreateMobileTranslucencyStandardPassProcessor` / `CreateMobileTranslucencyAfterDOFProcessor` / `CreateMobileTranslucencyAllPassProcessor` 都使用：

```cpp
PassDrawRenderState.SetDepthStencilState(
    TStaticDepthStencilState<false, CF_DepthNearOrEqual>::GetRHI());
PassDrawRenderState.SetDepthStencilAccess(
    FExclusiveDepthStencil::DepthRead_StencilRead);
```

的原因。

### 直接影响你 Plan 的结论

你的 `RenderMobileAfterTranslucencyPass` 是在 `RHICmdList.NextSubpass();` 之后、`RenderTranslucency` 之后调用的。它**也跑在 Subpass 1 里面**，DepthStencil 已经是只读 attachment：

- 即使你把模板参数从 `<false, CF_DepthNearOrEqual>` 改成 `<true, CF_DepthNearOrEqual>`，**也不会真的写入硬件深度** —— Attachment 是 read-only。在严格的 RHI 校验下还可能命中断言。
- 所以 Plan 当前写的 `false` **在 SinglePass 路径下是唯一正确的选择**。
- `SetDepthStencilAccess(FExclusiveDepthStencil::DepthRead_StencilRead)` 必须与之匹配，告诉 RDG/RHI “我不需要写权限”。

---

## 3. MobileDepthPass（PrePass）做了什么？AfterTranslucency 物体此时有深度吗？

### MobileDepthPass 的渲染状态

`DepthRendering.cpp:753`：

```cpp
void SetMobileDepthPassRenderState(...)
{
    DrawRenderState.SetDepthStencilState(TStaticDepthStencilState<
        true, CF_DepthNearOrEqual,
        true, CF_Always, SO_Keep, SO_Keep, SO_Replace,
        false, CF_Always, SO_Keep, SO_Keep, SO_Keep,
        0x00, 0xff>::GetRHI());
    ...
}
```

- DepthWrite = **true**，CF_DepthNearOrEqual。
- 这是**单独的 RenderPass / Subpass 0 之前的独立 Pass**，DepthStencil 是读写 attachment，所以能正常写深度 + Stencil。

### 调用时机

`MobileShadingRenderer.cpp`：

- `bIsFullDepthPrepassEnabled` 为 true 时：渲染 **全量 PrePass**（line 836 `RenderPrePass(...)`，DepthPassInstanceCullingDrawParams），所有不透明物体在 BasePass 之前先写一遍深度。
- 否则只渲染 **Masked PrePass**（line 849 `RenderMaskedPrePass(...)`），只对 Masked 物体写深度。

### 移动端默认是哪一种？

- 移动 Forward 路径下，`bIsFullDepthPrepassEnabled` **通常为 false**（仅 deferred 或开启 `r.Mobile.EarlyZPass` 等 CVar 才打开）。
- 即默认情况下，**Opaque 物体的硬件深度主要来自 BasePass 自身**（BasePass 是 `<true, ...>`）。

### AfterTranslucency 物体此时有深度吗？

关键问题：你把某些 Primitive 改路由到 `EMeshPass::MobileAfterTranslucencyPass`，**不再走 BasePass**。这意味着：

| Pass | 深度可写？ | AfterTranslucency 物体是否参与？ |
|---|---|---|
| MaskedPrePass / Full PrePass (`MobileDepthPass`) | 是 | **默认不参与**（除非该 primitive 仍然被 DepthPass 收集；见下文） |
| BasePass | 是 | **不参与**（按你的设计排除） |
| Subpass 1 (Decal / Fog / Translucency / AfterTranslucency) | **否（只读 attachment）** | 是 |

所以：

- 在你触发 `RenderMobileAfterTranslucencyPass` 时，SceneDepth 里的内容是 **BasePass 写完之后的状态**。
- 这个 SceneDepth **不包含 AfterTranslucency 物体的深度**（因为它没在任何写深度的 Pass 里出现过）。
- 你在 AfterTranslucency Pass 中 PS 读到的 `SceneDepth` 仅是“场景其他 opaque 物体的深度”。
- 当你的 AfterTranslucency 物体绘制完成，它的颜色被写到 SceneColor 上覆盖了半透明，但 SceneDepth **完全不知道这些物体的存在**。

> 结论：你画完 AfterTranslucency 后，硬件深度里**没有**这些物体。SinglePass 这一帧内无论怎么调 PSO 都改不了。

---

## 4. Plan 中 `SetBlendState` 与 `SetDepthStencilState` 到底必要吗？

```cpp
//PassDrawRenderState.SetBlendState(TStaticBlendStateWriteMask<CW_RGBA>::GetRHI());//是否还需要？
PassDrawRenderState.SetDepthStencilAccess(FExclusiveDepthStencil::DepthRead_StencilRead);
PassDrawRenderState.SetDepthStencilState(TStaticDepthStencilState<false, CF_DepthNearOrEqual>::GetRHI());
```

### SetBlendState

- 不调用时 `FMeshPassProcessorRenderState` 内 BlendState 默认是 `nullptr`，最终会被 `FMobileBasePassMeshProcessor` 根据材质本身的 `BlendMode` 重新计算（材质是 Opaque → 自动用 `CW_RGBA` 不混合；材质是 Translucent → 用 `BlendTranslucent` 等）。
- 因为你这个 Pass 要“用 opaque 的方式画”，**最简单的做法是不显式 SetBlendState**，让 processor 用材质自身的 Opaque blend（不混合，全写）。
- `TranslucencyStandard / AfterDOF / All` 三个 Processor 也都**没有显式 SetBlendState** —— 它们依赖材质各自的 Translucent BlendState，是同样的道理。
- 显式写 `TStaticBlendStateWriteMask<CW_RGBA>::GetRHI()` 会**强行覆盖所有材质的 BlendState 为“不混合全通道写”**。如果你的 AfterTranslucency 物体材质都用 Opaque/Masked，那这一行实际等价；但它会破坏 Masked 的 alpha-to-coverage 之类细节。**建议保留注释/删除**。

### SetDepthStencilState

- 必须显式设。`FMobileBasePassMeshProcessor` 不会自己改这个状态（它信任 PassDrawRenderState）。
- 你的物体走的是“半透明子 Pass”中的“当 Opaque 画”这种特殊语义，所以**必须**手动把 DepthStencilState 改成 SinglePass-Subpass1 允许的 `<false, CF_DepthNearOrEqual>` + `DepthRead_StencilRead`。

### CanUseDepthStencil 标志

```cpp
const FMobileBasePassMeshProcessor::EFlags Flags =
    FMobileBasePassMeshProcessor::EFlags::CanUseDepthStencil;
```

- `CreateMobileBasePassProcessor` 里之所以还多一个 `| CanReceiveCSM` 分支，是因为 BasePass 要负责 CSM 阴影接收（直接光照）。你的 AfterTranslucency 物体如果不需要接收 CSM 阴影（绝大多数 UI/穿透物体不需要），就**不需要 CanReceiveCSM**，单 `CanUseDepthStencil` 即可。
- 这一点 Plan 已经做对了。`Translucency*` 三个 processor 也都是只用 `CanUseDepthStencil`。

---

## 5. MobileDepthPass 与你这里有没有关系？

直接的“同一帧资源”关系：

- MobileDepthPass 写 SceneDepth → BasePass 继续写 SceneDepth → NextSubpass → AfterTranslucency 读 SceneDepth（只读）。
- 同一份 `SceneTextures.Depth.Target`（physical attachment）。

间接的“能否给 AfterTranslucency 物体提供硬件深度”关系：

- 如果开启 Full PrePass（`r.EarlyZPass`/`r.Mobile.EarlyZPass` 等），且你的 AfterTranslucency 物体**仍然被 `EMeshPass::DepthPass` 收集**（你只是把它从 BasePass 路由到 AfterTranslucency，但没有从 DepthPass 中排除），那么：
  - PrePass 阶段它会写深度；
  - BasePass 阶段它不画；
  - Subpass 1 / AfterTranslucency 阶段它再画一次颜色，依赖已存在的硬件 Z 进行 `CF_DepthNearOrEqual` 测试。
  - 结果：颜色正确遮挡、深度正确存在、依赖深度的后续效果（DOF/SSR/软粒子）也能识别这些物体。**这是真正“按 opaque 处理”的完整解法。**

- 反之，如果你的过滤逻辑同时把这些物体从 DepthPass 里也排除掉了（典型情况：DepthPass 只收集 “Opaque 且会出现在 BasePass 的 primitives”，你把它们标记成“不进 BasePass”就连带不进 DepthPass），那么 SceneDepth 里就永远没有它们。

> 建议：在 Plan 的 “排除 BasePass 收录” 那一步代码里，明确**只排除 BasePass，不要排除 DepthPass**。同时启用/确认 Full Depth PrePass（移动 Forward 默认可能没开），否则 PrePass 只会画 Masked，Opaque 的 AfterTranslucency 物体仍写不进深度。

---

## 6. “写入深度”指的是什么？我在 AfterTranslucency Pass 读深度时这些物体有没有写入深度？

- **“写入深度”** = 在该 DrawCall 经过 ZTest 通过后，由 ROP 把当前 fragment 的 Z 值更新到 DepthBuffer 中相应像素位置。是否发生由两件事共同决定：
  1. PSO 的 `bEnableDepthWrite` 是 `true`；
  2. 当前 RenderPass attachment 允许写（不是 DepthReadSubpass / DSV 不是 read-only）。
- 在 Plan 当前代码里：
  - PSO 是 `<false, ...>` → 第 1 个条件本身就 false；
  - 即便改成 `<true, ...>` → 第 2 个条件因 SinglePass 的 `DepthReadSubpass` 而 false。
- 因此 `RenderMobileAfterTranslucencyPass` 中绘制的物体 **不会写入硬件深度**。
- 你在那个 Pass 里“读取深度”指的是 PS 端通过 `SceneDepth` 采样（fetch）来做软粒子 / 雾深度判定之类。读到的内容是：
  - PrePass 写的（若开启 + 该物体被收进 DepthPass）
  - BasePass 写的（其它 Opaque 物体）
  - **不包含 AfterTranslucency 物体自己**

---

## 7. 关于 Plan 第 324 行的写法建议

> Plan 第 324 行：`TStaticDepthStencilState<false, CF_DepthNearOrEqual>::GetRHI()`，与 BasePass 行为不同（BasePass 写深度，这里不写）。

**校正几点先前直觉**：

1. “保留可配置项，默认 true 写深度” —— **不能这么做**。在 SinglePass + `DepthReadSubpass` 路径下，把 PSO 改成 true **没有效果**，因为 attachment 是只读；甚至可能在 Vulkan validation 下出错。如果你需要写深度，必须**改变上层渲染管线结构**（拆出独立 RenderPass 或者去 DepthPass 写）。
2. “要遮挡半透明，颜色覆盖即可” —— 是的，且这是 SinglePass 路径下唯一可达成的事。视觉上没问题。
3. 后续依赖深度的效果识别不到这些物体 —— **属实**。要解决必须让它们的深度在 **BasePass 之前**（即 MobileDepthPass / PrePass）就写入。

**推荐的取舍方案**：

- **方案 A（推荐，最小改动）**：保留 PSO `<false, CF_DepthNearOrEqual>` + `DepthRead_StencilRead`；同时**确保这些 primitives 仍然进入 `EMeshPass::DepthPass`**，并启用 `r.Mobile.EarlyZPass` / `MobileDepthPass` Full PrePass。这样：
  - 它们在 PrePass 写硬件深度（写颜色：no，写深度：yes）；
  - 在 BasePass 中被排除（不写颜色）；
  - 在 AfterTranslucency Pass 中再画一次（写颜色：yes，写深度：no，但深度测试可以正确遮挡早期半透明像素的颜色，因为半透明本身就不写深度）。
  - 后续依赖 SceneDepth 的效果可以识别它们。
- **方案 B**：拆成独立的 RenderPass（类似 `AddMobileSeparateTranslucencyPass`，但取消 `SubpassHint = DepthReadSubpass`，改用 `DepthWrite_StencilRead`），PSO 用 `<true, CF_DepthNearOrEqual>`。代价：失去 tile-memory 单 RenderPass 性能，需要 store/load SceneColor & SceneDepth。
- **方案 C（与 Plan 当前一致）**：接受这些物体不写深度。只适合“后处理对这些物体不敏感”的场景。

---

## 8. 关键文件 & 行号速查

- `Engine/Source/Runtime/RHI/Public/RHIResources.h:3688` — `ESubpassHint` 枚举（`DepthReadSubpass` 注释“Render pass has depth reading subpass”）。
- `Engine/Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp:1586` — 设置 `PassParameters->RenderTargets.SubpassHint = ESubpassHint::DepthReadSubpass`。
- `Engine/Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp:1614` — `RHICmdList.NextSubpass();` // scene depth is read only and can be fetched
- `Engine/Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp:1623` — `RenderTranslucency(RHICmdList, View);`（Plan 将在其后插入 `RenderMobileAfterTranslucencyPass`）
- `Engine/Source/Runtime/Renderer/Private/DepthRendering.cpp:753` — `SetMobileDepthPassRenderState`（MobileDepthPass 用 `<true, CF_DepthNearOrEqual>` + Stencil Replace）
- `Engine/Source/Runtime/Renderer/Private/DepthRendering.cpp:1243` — `REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR(MobileDepthPass, CreateDepthPassProcessor, ..., EMeshPass::DepthPass, ...)`。
- `Engine/Source/Runtime/Renderer/Private/MobileBasePass.cpp:1151` — `CreateMobileBasePassProcessor`，`<true, CF_DepthNearOrEqual>` + `CanUseDepthStencil | CanReceiveCSM`。
- `Engine/Source/Runtime/Renderer/Private/MobileBasePass.cpp:1184/1196/1207` — 三个 Translucency Processor，全部 `<false, CF_DepthNearOrEqual>` + `DepthRead_StencilRead` + 仅 `CanUseDepthStencil`。
- `Engine/Source/Runtime/Renderer/Private/MobileSeparateTranslucencyPass.cpp:50` — `SubpassHint = ESubpassHint::DepthReadSubpass`；line 67 `RHICmdList.NextSubpass();`。

---

## 9. 一句话总结

`TStaticDepthStencilState<false, CF_DepthNearOrEqual>::GetRHI()` = “**做 ZTest（反向 Z）但不写深度**”。在你这个 `RenderMobileAfterTranslucencyPass` 里，由于它位于 `DepthReadSubpass` 之后（line 1614 NextSubpass 之后），DepthStencil attachment 已是只读，**无论 PSO 怎么配，都不会写入硬件深度**。所以你在该 Pass 读取的 SceneDepth 里**不包含 AfterTranslucency 物体本身**。要让它们出现在 SceneDepth 中，必须让它们在更早的阶段（PrePass / MobileDepthPass）就把深度写好，而不是寄希望于在这个 Pass 里改 `true`。

---

# 追加分析（移动 Forward 路径下 bIsFullDepthPrepassEnabled 的开关链与"AfterTranslucency 想写深度"的可行方案）

## A. `bIsFullDepthPrepassEnabled` 在哪里被赋值？谁在控制它？

唯一赋值点：`Engine/Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp:302`

```cpp
bIsFullDepthPrepassEnabled    = Scene->EarlyZPassMode == DDM_AllOpaque;
bIsMaskedOnlyDepthPrepassEnabled = Scene->EarlyZPassMode == DDM_MaskedOnly;
```

也就是说，它是从 `Scene->EarlyZPassMode` 这个枚举换算来的，**不是单独的 CVar**。所以问题变成：移动端的 `Scene->EarlyZPassMode` 在什么条件下会是 `DDM_AllOpaque`？

### `Scene->EarlyZPassMode` 的计算入口

`Engine/Source/Runtime/Renderer/Private/RendererScene.cpp:4694-4708`：

```cpp
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
        OutZPassMode = DDM_AllOpaque;   // <-- 这里才会把 bIsFullDepthPrepassEnabled 打开
    }
}
```

所以**只有 `MobileUsesFullDepthPrepass(Platform) == true`，`bIsFullDepthPrepassEnabled` 才会是 true**。

### `MobileUsesFullDepthPrepass` 的真正开关

`Engine/Source/Runtime/RenderCore/Private/RenderUtils.cpp:616-619`：

```cpp
RENDERCORE_API bool MobileUsesFullDepthPrepass(const FStaticShaderPlatform Platform)
{
    return MobileUsesShadowMaskTexture(Platform)
        || IsMobileAmbientOcclusionEnabled(Platform)
        || IsUsingDBuffers(Platform)
        || FReadOnlyCVARCache::MobileEarlyZPass(Platform) == 1;
}
```

满足以下**任一条件**才会启用 Full PrePass：

1. `MobileUsesShadowMaskTexture(Platform)` —— 使用 ShadowMask 纹理（移动端某些阴影方案）；
2. `IsMobileAmbientOcclusionEnabled(Platform)` —— 移动 SSAO；
3. `IsUsingDBuffers(Platform)` —— DBuffer Decal；
4. **`r.Mobile.EarlyZPass == 1`** —— 用户主动开启（`Engine/Source/Runtime/Renderer/Private/RendererScene.cpp:126-134`）。

> CVar 默认值是 `0`（`RendererScene.cpp:128`），说明文档：
> - 0: off（默认）
> - 1: all opaque  → 触发 Full PrePass / `DDM_AllOpaque` / `bIsFullDepthPrepassEnabled = true`
> - 2: masked primitives only → `DDM_MaskedOnly`，只走 MaskedPrePass

### 移动端"默认 Forward + 无 SSAO/DBuffer/ShadowMask"的实际表现

如果项目没有显式 `r.Mobile.EarlyZPass=1`，并且没有上述三个 feature 触发：
- `MobileUsesFullDepthPrepass = false`
- `Scene->EarlyZPassMode = DDM_None`（或在 `r.Mobile.EarlyZPass=2` 下 `DDM_MaskedOnly`）
- `bIsFullDepthPrepassEnabled = false`
- `bIsMaskedOnlyDepthPrepassEnabled = (CVar == 2)`

**这就是"正常的 Forward 移动路径"的默认状态**：没有 Full PrePass。

### 关键确认：哪些代码会因 `bIsFullDepthPrepassEnabled = false` 而被跳过

在 `MobileShadingRenderer.cpp` 中以下条件**全部走 else 分支**：

| 行 | 含义 |
|---|---|
| `:899  if (!bIsFullDepthPrepassEnabled) { ... }` | 在 BasePass 之前不补建 DepthPass RenderingCommands |
| `:1246 if (bIsFullDepthPrepassEnabled) { RenderFullDepthPrepass(...) }` | **跳过** `RenderFullDepthPrepass`（line 796 那个独立 RDG Pass）|
| `:1322 if (!bIsFullDepthPrepassEnabled) { FenceOcclusionTests(...) }` | 走 else，遮挡查询在 base pass 之后 fence |
| `:1437 if (!bIsFullDepthPrepassEnabled) { DepthPass BuildRenderingCommands ... }` | BuildInstanceCullingDrawParams 里仍然为 DepthPass 构造命令（用于 Masked PrePass）|
| `:1494 BasePassRenderTargets.DepthStencil = ... ? DepthRead_StencilWrite : DepthWrite_StencilWrite` | **走 else** = `DepthWrite_StencilWrite`，即 BasePass 整个阶段深度都是可写的 |
| `:1656/1691 if (!bIsFullDepthPrepassEnabled) AddResolveSceneDepthPass(...)` | 在 BasePass 之后做 MSAA Depth Resolve |

最关键的是 line 1494：当 Full PrePass 关闭时，**BasePass 的 DepthStencil Access = `DepthWrite_StencilWrite`**，意味着深度是 BasePass 自己写的，没有所谓"先验深度图"。

### "深度是否和不透明物体一起渲染？"——直接证据

`Engine/Source/Runtime/Renderer/Private/MobileBasePass.cpp:952-960`：

```cpp
else if ((MeshBatch.bUseForDepthPass && Scene->EarlyZPassMode == DDM_AllOpaque) || bMaskedInEarlyPass)
{
    // 已经在 PrePass 写过深度 → BasePass 只做 Z-Equal 测试，不再写
    DrawRenderState.SetDepthStencilState(TStaticDepthStencilState<false, CF_Equal>::GetRHI());
}
else
{
    const bool bEnableReceiveDecalOutput = ((Flags & EFlags::CanUseDepthStencil) == EFlags::CanUseDepthStencil);
    MobileBasePass::SetOpaqueRenderState(DrawRenderState, ..., bEnableReceiveDecalOutput && IsMobileHDR(), bPassUsesDeferredShading);
}
```

`SetOpaqueRenderState`（`MobileBasePass.cpp:549-557`）在 `bEnableReceiveDecalOutput || bUsesDeferredShading` 时使用：

```cpp
DrawRenderState.SetDepthStencilState(TStaticDepthStencilState<
    true, CF_DepthNearOrEqual,                     // <-- DepthWrite = true
    true, CF_Always, SO_Keep, SO_Keep, SO_Replace,
    false, CF_Always, SO_Keep, SO_Keep, SO_Keep,
    0x00, 0xff>::GetRHI());
```

否则（line 558-561 注释 "default depth state should be already set"）使用 `CreateMobileBasePassProcessor` 设置的默认 `TStaticDepthStencilState<true, CF_DepthNearOrEqual>`（`MobileBasePass.cpp:1157`）。

**结论（验证完成）**：在移动 Forward 默认配置下：
- 没有 `EarlyZPass` / Full PrePass；
- Opaque 物体的**深度是在 BasePass 中和颜色一起渲染的**；
- BasePass DepthStencilState 永远是 `<true, CF_DepthNearOrEqual>`（带或不带 stencil 操作）；
- BasePass RenderTarget Access = `DepthWrite_StencilWrite`；
- 之后 `RHICmdList.NextSubpass()` 切到 Subpass 1，DepthStencil 变只读。

## B. "AfterTranslucency 在 RenderTranslucency 之后渲染、又需要写入深度"——可行方案

> 前提：你的物体必须在 `RenderTranslucency(RHICmdList, View);`（`MobileShadingRenderer.cpp:1623`）**之后**绘制。

由于 `RHICmdList.NextSubpass()`（line 1614）之后 Subpass 1 内 DepthStencil 是 **InputAttachment(read-only)**，**Subpass 1 内部任何 PSO 都无法写入硬件深度**。所以"在原位置插入并写深度"这个组合**物理上不可能**。要写深度，必须**结束当前 RenderPass**或者**拆分子 Pass 结构**。下面给出三种实际可行做法，按推荐度排序：

---

### 方案 1：把 AfterTranslucency 放到 SinglePass 之外（独立 RenderPass）✅ 最干净

让 `RenderForwardSinglePass` 内部的 RDG Pass 自然结束（它在 line 1675 之后会通过 `GraphBuilder.AddPass` lambda 返回让 RDG 关闭 RenderPass），然后在 **它后面再添加一个独立的 RDG Pass** 来画 AfterTranslucency，DepthStencil 绑定为 `DepthWrite_StencilWrite`。

实现要点：

1. **不要**把 `RenderMobileAfterTranslucencyPass(...)` 放到 SinglePass 的 lambda 内部（Plan 第 398 行那样会落在 Subpass 1 里）。
2. 把它从 `RenderForwardSinglePass` lambda 中移除，改成在 `RenderForward(...)` 里独立 `AddPass`。
3. 该 RenderPass 的 `FRenderTargetBindingSlots`：
   - `[0] = SceneColor`（`ELoad`，目标是它的颜色继续叠加在半透明之上）；
   - `DepthStencil = FDepthStencilBinding(SceneDepth, ELoad, ELoad, FExclusiveDepthStencil::DepthWrite_StencilWrite)`；
   - `SubpassHint = ESubpassHint::None`（不再分 subpass）。
4. Processor 改为：

```cpp
PassDrawRenderState.SetDepthStencilState(
    TStaticDepthStencilState<true, CF_DepthNearOrEqual>::GetRHI());
PassDrawRenderState.SetDepthStencilAccess(
    FExclusiveDepthStencil::DepthWrite_StencilWrite);
```

5. 移动 TBR 代价分析：
   - **会触发 tile flush**：上一个 RenderPass（SinglePass）必须把 SceneColor / SceneDepth `Store` 到 system memory；新 RenderPass `Load` 回 tile。
   - 在 1080p 上一次 store/load 的带宽与延迟代价通常在 0.3–1.5 ms（取决于 GPU），可接受。
   - 与 `AddMobileSeparateTranslucencyPass`（`MobileSeparateTranslucencyPass.cpp:37`）的模式类似，但那个用 `DepthReadSubpass` 来避免 flush；你需要写深度，就必须放弃 subpass merge。

> **这是 UE 移动管线里"AfterTranslucency 既能在半透明之后画、又能写深度"的标准做法。** 后续依赖深度的效果（DOF/SSR/软粒子/MotionBlur）能识别到这些物体。

---

### 方案 2：开启 `r.Mobile.EarlyZPass=1`（项目级），物体仍走 SinglePass 内 AfterTranslucency

如果你的目标只是"让 SceneDepth 包含这些物体，给后处理用"，且**不要求**它们的深度比"BasePass 之后 + 半透明已写位置"更晚——可以让它们在 PrePass 里就写深度：

1. 项目设置打开 `r.Mobile.EarlyZPass=1`（或 DefaultEngine.ini）。
2. `Scene->EarlyZPassMode = DDM_AllOpaque`，`bIsFullDepthPrepassEnabled = true`。
3. 你的物体 PrimitiveSceneProxy `bUseForDepthPass = true` 且 `ShouldRenderInDepthPass() = true`（这两者在 UE 中默认就是 true 对 Opaque）。
4. 你在 MeshPass 过滤逻辑里**只**把这些物体从 BasePass 中移除，**不要**从 `EMeshPass::DepthPass` 中移除。
5. AfterTranslucency Pass 继续用 Plan 当前的 `<false, CF_DepthNearOrEqual>` + `DepthRead_StencilRead`，跑在 Subpass 1 里。

效果：
- 深度在 **DepthPass / Full PrePass** 阶段就被写入（DepthStencilState `<true, CF_DepthNearOrEqual, ..., SO_Replace>`，参见 `DepthRendering.cpp:755`）；
- BasePass 因为 `bIsFullDepthPrepassEnabled = true` → `BasePassRenderTargets.DepthStencil = DepthRead_StencilWrite`（`MobileShadingRenderer.cpp:1494`）→ 只读深度做 ZTest；
- 你的物体在 BasePass 中不画（你已经从 BasePass 排除），但**深度已经在那里**；
- 半透明绘制后，AfterTranslucency Pass 再画一次颜色，正常遮挡半透明；
- SceneDepth 里**包含**这些物体。

> 注意：开启 Full PrePass 会让 BasePass 的 MSAA / DepthAux 路径发生变化（line 1494, 1540），并且会失去 MSAA 兼容（`SceneUtils.cpp:212-218`：`bRendererSupportMSAA = bRHISupportsMSAA && !bMobilePixelProjectedReflection && !bIsFullDepthPrepassEnabled`）。需要项目能接受这个成本。

---

### 方案 3：拆掉 SinglePass，走 MultiPass（`RenderForwardMultiPass`）

`MobileShadingRenderer.cpp:1567-1574`：

```cpp
if (bRequiresMultiPass)
{
    RenderForwardMultiPass(GraphBuilder, PassParameters, ViewContext, SceneTextures);
}
else
{
    RenderForwardSinglePass(GraphBuilder, PassParameters, ViewContext, SceneTextures);
}
```

MultiPass 模式下半透明本身就是独立 RenderPass（不再依赖 `NextSubpass`），所以在它之后直接加你的 AfterTranslucency RenderPass 也行，配置同方案 1。

- 切换条件由 `bRequiresMultiPass` 决定（功能：自定义场景捕获、某些后处理 fetch 不支持等情况会强制 MultiPass）。
- 性能代价比 SinglePass 大（同等于把 SceneColor/Depth flush 一次），但兼容性最高。

---

## C. 直接对你 Plan 的修订建议

| 项 | 当前 Plan | 建议改法 |
|---|---|---|
| `SetDepthStencilState<false, ...>` 的取值 | `false`（不写深度） | 取决于走哪个方案：方案 1/3 → 改 `true`；方案 2 → 保持 `false` |
| `SetDepthStencilAccess` | `DepthRead_StencilRead` | 同上：方案 1/3 → `DepthWrite_StencilWrite`；方案 2 → 保持 |
| `SetBlendState(CW_RGBA)` | 已注释，问"是否需要" | **不需要显式设置**，让材质 BlendState 生效（与三个 Translucency Processor 风格一致）|
| Flags `CanUseDepthStencil` | 仅这一个 | 可以保持。若需要接收 CSM/阴影则补 `CanReceiveCSM` |
| 在 `RenderForwardSinglePass` lambda 内 `RenderTranslucency` 之后调用 | Plan 第 398 行 | **方案 1**：移出 lambda，作为独立 `GraphBuilder.AddPass` 在 SinglePass 之后；**方案 2**：保持位置；**方案 3**：放在 MultiPass 半透明之后 |

> 直接选哪个？
> - 如果你**确实需要后处理（DOF/SSR/FX 软粒子）识别这些物体**，方案 1 或 2 二选一。  
>   - 项目可以接受 `r.Mobile.EarlyZPass=1` 的全局开销（含失去 MSAA） → **方案 2 最省事**；  
>   - 项目不能改全局设置 / 仍要 MSAA → **方案 1**（独立 RenderPass，仅这一个 Pass 触发 flush）。  
> - 如果只是**视觉上覆盖半透明，不需要写深度**，Plan 当前的写法已经够用，不必改（即 §1–§9 的结论）。

## D. 证据速查表（本节追加部分）

- `MobileShadingRenderer.cpp:302` — `bIsFullDepthPrepassEnabled = Scene->EarlyZPassMode == DDM_AllOpaque;`
- `MobileShadingRenderer.cpp:849-855` — `RenderMaskedPrePass` 仅在 `bIsMaskedOnlyDepthPrepassEnabled` 时执行（仅 `r.Mobile.EarlyZPass=2`）。
- `MobileShadingRenderer.cpp:1246-1248` — `if (bIsFullDepthPrepassEnabled) { RenderFullDepthPrepass(...); }` 是 Full PrePass 的唯一调用点。
- `MobileShadingRenderer.cpp:1494-1496` — BasePass 的 DepthStencilAccess 根据 `bIsFullDepthPrepassEnabled` 切换。
- `RendererScene.cpp:126-134` — `r.Mobile.EarlyZPass` CVar 定义，默认 `0`，`ECVF_ReadOnly`。
- `RendererScene.cpp:4694-4708` — `FScene::GetEarlyZPassMode` 移动端分支。
- `RenderUtils.cpp:616-619` — `MobileUsesFullDepthPrepass` 真正开关。
- `MobileBasePass.cpp:952-960`、`531-561`、`1157` — Mobile BasePass 深度状态：默认 `<true, CF_DepthNearOrEqual>`；若已有 PrePass 则 `<false, CF_Equal>`。
- `DepthRendering.cpp:753-784` — Mobile DepthPass 始终 `<true, CF_DepthNearOrEqual, ..., SO_Replace>` 写深度+stencil。
- `DepthRendering.cpp:1021-1023` — `FDepthPassMeshProcessor::AddMeshBatch` 用 `MeshBatch.bUseForDepthPass` 决定收录。
- `MobileSeparateTranslucencyPass.cpp:37-72` — 独立 RenderPass 写半透明的范本（你方案 1 可以参考它，但要把 `SubpassHint::DepthReadSubpass` 改成 `None`，把 Access 改成 `DepthWrite_StencilWrite`）。

---

# 追加分析 2 — `RHICmdList.NextSubpass()` 内部行为、Subpass 原理、能否再加一个写深度的 Subpass

## E. `RHICmdList.NextSubpass()` 到底做了什么？

### E.1 调用链（RHI 层）

`Engine/Source/Runtime/RHI/Public/RHICommandList.h:3888-3897`：

```cpp
FORCEINLINE_DEBUGGABLE void NextSubpass()
{
    if (Bypass())
    {
        GetContext().RHINextSubpass();
    }
    else
    {
        ALLOC_COMMAND(FRHICommandNextSubpass)();
    }
}
```

它要么直接调用平台 Context 的虚函数 `RHINextSubpass()`，要么排队一条 `FRHICommandNextSubpass` 命令稍后由 RHI 线程执行（`RHICommandList.h:1653`）。

### E.2 Vulkan 后端实际行为

`Engine/Source/Runtime/VulkanRHI/Private/VulkanRenderTarget.cpp:721-727`：

```cpp
void FVulkanCommandListContext::RHINextSubpass()
{
    check(CurrentRenderPass);
    FVulkanCmdBuffer* CmdBuffer = CommandBufferManager->GetActiveCmdBuffer();
    VkCommandBuffer Cmd = CmdBuffer->GetHandle();
    VulkanRHI::vkCmdNextSubpass(Cmd, VK_SUBPASS_CONTENTS_INLINE);
}
```

它就是包了一层 **Vulkan 原生命令 `vkCmdNextSubpass`**：在**同一个 `VkRenderPass`**对象内推进到下一个 Subpass。注意它**没有**：
- 没有 `vkCmdEndRenderPass` / `vkCmdBeginRenderPass`；
- 没有切换 attachment、没有重新 transition layout；
- 没有 Tile flush，整个过程发生在 GPU 的 on-chip tile memory 内（移动 TBR / TBDR）。

Metal/OpenGL 等其他后端类似（`MetalRHIContext.h:174`, `OpenGLDrv.h:426`）：要么实现成 tile shader-stage 切换，要么是空操作（如果不支持 subpass）。

### E.3 它推进到的"下一个 Subpass"长什么样？

`VkRenderPass` 是在 `BeginRenderPass` 之前由 `FVulkanRenderTargetLayout` 的 `GetSubpassHint()` 一次性构造的。结构由 `Engine/Source/Runtime/VulkanRHI/Private/VulkanRenderpass.h:417-674` 决定，对 `ESubpassHint::DepthReadSubpass` 的具体描述：

- **Subpass 0（主子 Pass）**：行 490-506
  - ColorAttachments：所有 SceneColor + SceneDepthAux 等；
  - DepthStencilAttachment：可写（`DepthStencilAttachmentReference`，layout = `DEPTH_STENCIL_ATTACHMENT_OPTIMAL`）；
  - 这一段执行 BasePass，可以正常 ZTest + ZWrite。

- **Subpass 1（DepthRead 子 Pass）**：行 509-539
  - ColorAttachments：只有 SceneColor（行 513 `SubpassDesc.SetColorAttachments(ColorAttachmentReferences, 1);`）；
  - **DepthStencilAttachment 被替换成 `DepthStencilAttachment`**（行 523 `SubpassDesc.SetDepthStencilAttachment(&DepthStencilAttachment);`）；
  - 这个 `DepthStencilAttachment` 在行 482 设置 layout 为 `VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL`（**深度只读、stencil 可写**）；
  - 同一份深度还被加为 **InputAttachment（InputAttachments1[0]）**（行 518-521），让 PS 可以通过 `SubpassLoad()` 在 tile 内读取硬件深度（这就是注释里的 "scene depth is read only and can be fetched"）；
  - 行 531-538 添加了 `VkSubpassDependency`：源 Stage = `EARLY_FRAGMENT_TESTS | LATE_FRAGMENT_TESTS`、源 Access = `DEPTH_STENCIL_ATTACHMENT_WRITE`；目标 Stage = `FRAGMENT_SHADER`、目标 Access = `INPUT_ATTACHMENT_READ`；`dependencyFlags = BY_REGION_BIT`（按 tile 处理）。

所以 `RHICmdList.NextSubpass()` 切到的就是这个"深度只读 + Color 可写 + Depth 作为 InputAttachment"的 Subpass。**只要你还在这个 RenderPass 内，DepthStencil 的 layout 就被 RenderPass 描述符锁定为 read-only**——这是创建 `VkRenderPass` 时通过 `pAttachments` / `pSubpasses` 写死的。

### E.4 PSO 校验

`VulkanPipeline.cpp:1802-1808` 也验证了这一点：

```cpp
if (RemappingInfo.InputAttachmentData.Num())
{
    // input attachements can't exist in a first sub-pass
    check(PSOInitializer.SubpassHint != ESubpassHint::None); 
    check(PSOInitializer.SubpassIndex != 0);
}
```

- 用到 InputAttachment 的 shader（半透明、Decal、Fog 那批）必须把 PSO 的 `SubpassIndex >= 1`；
- `MobileBasePass.cpp:1036`：`uint8 SubpassIndex = bTranslucentBasePass ? (bDeferredShading ? 2 : 1) : 0;` 直接证实"半透明走 Subpass 1，BasePass 走 Subpass 0"。

### E.5 "你为什么说 Subpass 1 的 DepthStencil 是 InputAttachment(read-only)？"

直接证据：
1. `VulkanRenderpass.h:482` 把 layout 设为 `VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL`；
2. `VulkanRenderpass.h:518-521` 把同一份深度同时挂为 `InputAttachments1[0]`；
3. `VulkanRenderpass.h:523` Subpass1 的 DepthStencilAttachment 指向只读的 `DepthStencilAttachment`；
4. 这一切是在 **`VkRenderPass` 创建时就被烙进描述符**的，运行时不能改。Vulkan 规范明确：subpass 内 attachment 的访问类型由 `VkAttachmentReference::layout` 决定，深度 `READ_ONLY` 时写入会被驱动拒绝（validation error），即使 PSO `bDepthWrite=true`。

> 这就是 §2 / §6 的结论"无论 PSO 怎么配，都不会写入硬件深度"的硬件级原因。

---

## F. `RHICmdList.SetCurrentStat(GET_STATID(STAT_CLMM_Translucency))` 的作用

### F.1 是干什么的？

`Engine/Source/Runtime/RHI/Private/RHICommandList.cpp:311-317`：

```cpp
void FRHICommandListBase::SetCurrentStat(TStatId Stat)
{
    if (!Bypass())
    {
        ALLOC_COMMAND(FRHICommandStat)(Stat);
    }
}
```

它向命令流插入一条 `FRHICommandStat`，其 `Execute()` 做的事情只有一件：**`GCurrentExecuteStat = CurrentExecuteStat;`**（同文件 line 305-308）。

`GCurrentExecuteStat` 是 RHI 线程用来对**接下来执行的命令**统一计时/归类的全局 stat ID（受 STATGROUP_CommandListMarkers 管理）。

### F.2 STAT_CLMM_Translucency 在哪里声明

`MobileShadingRenderer.cpp:125`：

```cpp
DECLARE_CYCLE_STAT(TEXT("Translucency"), STAT_CLMM_Translucency, STATGROUP_CommandListMarkers);
```

`CLMM` = "CommandList MMarker"（移动端的 RHI 线程时间桶）。同组里还有 `STAT_CLMM_Opaque`（line 122）、`STAT_CLMM_Occlusion`（line 123）、`STAT_CLMM_Post`（line 124）、`STAT_CLMM_Shadows`（line 126）等。

### F.3 用途

- 用 `stat CommandListMarkers` 命令在控制台能看到一个分组报告："Opaque/Translucency/Occlusion/Post/..."各占多少 RHI 线程时间；
- `unrealinsights` / `profilegpu` 等工具也会按这些 marker 把 GPU 工作切片显示；
- 它**完全是性能统计**用途，**对管线/渲染结果毫无影响**，不发任何 GPU 命令。
- 删除它不会改变任何行为，只会让你在 stats 报告里看到一段时间被归到上一个标记里。

---

## G. Subpass 原理：移动 GPU 为什么要这么做？

### G.1 Tile-based 架构回顾

- 移动 GPU（Mali / Adreno / PowerVR / Apple GPU）是 **TBR/TBDR**：把 framebuffer 切成 16×16 ~ 64×64 像素的 tile，每个 tile 单独渲染。
- 每个 tile 有一段 **on-chip tile memory（SRAM）**，BasePass / 半透明 / Decal 等在 tile 内进行的 color/depth 读写**完全不走 DRAM 带宽**。
- 但只要 RenderPass `End` —— 当前 tile memory 的内容必须 `Store` 回 DRAM 中的 framebuffer attachment；新 RenderPass `Begin` 又得 `Load` 回 tile。这一次 Store + Load 在 1080p 上就是几兆带宽和 ~0.5–1.5 ms 的"墙时间"。
- 因此移动管线设计的核心目标是：**把所有能放进同一 RenderPass 的工作全塞进去**，用 Subpass 串起来，避免反复 store/load。

### G.2 Vulkan Subpass 的精确语义

一个 `VkRenderPass` 内部可有 N 个 `VkSubpassDescription`：
- 每个 Subpass 申明它**读哪些 attachment**（Input/Color/Depth/Resolve）、**它们的 layout 是什么**；
- Subpass 之间通过 `VkSubpassDependency` 表达内存可见性/执行顺序（典型是 `BY_REGION_BIT`，即每个 tile 内"前一个 subpass 写完，下一个 subpass 在同 tile 上读"）；
- 因此前一个 subpass 写到 tile memory 的颜色/深度可以在下一个 subpass **以 `SubpassLoad` 在 PS 中按像素读到**，**仍然不出 tile**。

### G.3 UE 怎么把 BasePass + 半透明 塞进同一 RenderPass

`MobileShadingRenderer.cpp:1586`：

```cpp
PassParameters->RenderTargets.SubpassHint = bTonemapSubpassInline
    ? ESubpassHint::CustomResolveSubpass
    : ESubpassHint::DepthReadSubpass;
```

这把"我希望这个 RDG Pass 内部有 2~3 个 Vulkan Subpass"的提示交给 Vulkan 后端。
后端按 `GetSubpassHint()`（`VulkanRenderpass.h:417`）构造 `VkRenderPass`：

| SubpassHint | Subpass 总数 | Subpass 布局 |
|---|---|---|
| `None` | 1 | Color RW + Depth RW（普通 RenderPass）|
| `DepthReadSubpass` | 2 | S0: Color/Depth RW；**S1: Color RW + Depth Read-Only (InputAttachment)** |
| `DeferredShadingSubpass` | 3 | S0: SceneColor + GBuffer + Depth RW；S1: 输入 Depth；S2: 输入 GBuffer + Depth，输出 SceneColor |
| `CustomResolveSubpass` | 3 | 包含 DepthRead + 自定义 resolve（移动 MSAA in-tile resolve）|

> Vulkan 规范要求：`VkRenderPass` 的所有 subpass 必须在 RenderPass 创建时**全部一次性描述**。运行时不能添加、修改、关闭、跳过任何 subpass。  
> 这就是为什么"我想在 RenderTranslucency 之后再插入一个写深度的 Subpass"在 RenderPass 已经 Begin 之后**无法做到** —— 那个 `VkRenderPass` 对象里**根本没有这个 subpass 的位置**。

---

## H. "我能不能再开一个 Subpass 让它写深度？"

逐情况讨论：

### H.1 在 **已经 Begin 的同一 RenderPass 内**临时插入新 subpass

**不可以**。原因：
- `VkRenderPass` 是不可变描述符；
- `RHICmdList.NextSubpass()` 只能前进到**已声明**的下一个 subpass，不能"创建"；
- 你没法在 `BeginRenderPass` 之前就把这个新 subpass 描述进去，除非你扩展 `ESubpassHint` 枚举并改 `VulkanRenderpass.h:417` 这段代码（见 H.3）。

### H.2 在 RenderTranslucency 之后**结束**当前 RenderPass，再开一个新的（**普通 RenderPass**）写深度

**可以**。这就是上面"方案 1"。技术上等于：
1. 让 RDG 当前 Pass 的 lambda 自然结束 → RHI 调用 `vkCmdEndRenderPass`；
2. 新建一个 RDG `AddPass`，绑定相同的 SceneColor / SceneDepth，但 `SubpassHint = None`、`DepthStencilAccess = DepthWrite_StencilWrite`；
3. RHI 调用 `vkCmdBeginRenderPass`，此时 SceneDepth 的 layout 由 RDG 自动 transition 回 `DEPTH_STENCIL_ATTACHMENT_OPTIMAL`，深度可写。

成本：**1 次 tile flush**（Store SceneColor + SceneDepth），新 RenderPass `LoadAction=ELoad` 再 Load 回 tile。带宽与延迟见 §H.5。

### H.3 给 `ESubpassHint` 加一个**自定义** SubpassHint（如 `DepthReadThenDepthWriteSubpass`），让 VkRenderPass 包含一个第三 subpass，DepthStencil 可写

**理论可以，工程不推荐**。要做这些事情：
1. `RHIResources.h:3688` 在 `ESubpassHint` 加一个新枚举值；
2. `VulkanRenderpass.h:417` 的 `BuildSubpasses` 处理新的 hint：在 Subpass 1（DepthRead）之后再加一个 Subpass 2，DepthStencil 用 `DEPTH_STENCIL_ATTACHMENT_OPTIMAL`（可写），不挂 InputAttachment；
3. 为 Subpass 1→Subpass 2 添加 `VkSubpassDependency`：srcStage = `FRAGMENT_SHADER`、srcAccess = `INPUT_ATTACHMENT_READ`；dstStage = `EARLY_FRAGMENT_TESTS | LATE_FRAGMENT_TESTS`、dstAccess = `DEPTH_STENCIL_ATTACHMENT_WRITE`；
4. PSO 校验：在 `VulkanPipeline.cpp:1825` 那段加分支，允许 `SubpassIndex >= 2` 时再次进入写深度状态；
5. 你的 `CreateMobileAfterTranslucencyPassProcessor` 标记 PSO 的 `SubpassIndex = 2`、`SubpassHint = <你的新 hint>`、DepthStencilAccess = `DepthWrite_StencilWrite`、PSO `<true, CF_DepthNearOrEqual>`；
6. 修改 `MobileShadingRenderer.cpp:1586` 让 SinglePass 用你的新 hint；
7. 在 SinglePass 的 lambda 里在 `RenderTranslucency` 后再调一次 `RHICmdList.NextSubpass()` 进入 Subpass 2，然后执行 `RenderMobileAfterTranslucencyPass`；
8. 仅对 Vulkan 后端有效；OpenGL ES / Metal 后端要分别实现/降级（OpenGL ES 上 subpass 通常是空操作，等价于"什么都不分"——这恰好可能让你的"Subpass 2"在 OpenGL 上跟方案 1 一样落到独立 RenderPass）；
9. 不同 GPU 驱动对多 subpass 的 tile residency 行为各异，需要现场测试 Mali / Adreno / PowerVR / Apple 上的稳定性和性能。

**好处**：理论上能保留 in-tile 性能（同一个 `VkRenderPass`，避免 store/load）。
**坏处**：
- 修改 RHI 层，影响范围远超你这一个 Pass；
- 要维护新 Hint 的所有后端实现；
- 多 subpass 共享 tile memory，Subpass 2 想写 Depth → Vulkan 实现里 SceneDepth attachment 的 `storeOp` 要从 `DontCare` 改成 `Store`（否则深度仍丢），这又侵入 `VulkanRenderTarget.cpp` 的 attachment 配置；
- 跟 MSAA Custom Resolve、Tonemap Subpass 等其他既有 SubpassHint 互斥/混合复杂度高；
- 即便实现成功，"避免一次 store/load"在大多数场景节省 0.3–1.0ms 左右，**改 RHI 的开发与回归测试成本远大于这点收益**。

### H.4 直接的可行办法（务实）

- **强烈推荐方案 1**（独立 RenderPass）。仅这一个 RDG Pass 触发一次 store/load，工程改动只在 Renderer 模块内，跨平台没有兼容问题。
- 如果你的物体数量小且材质简单（典型情况：UI、击中标记、武器投影），那一次 store/load 的代价对总帧时间影响微乎其微。

### H.5 性能成本估算（仅供参考）

| 选项 | 额外 DRAM 带宽 / 帧 | 额外延迟 | 兼容性 |
|---|---|---|---|
| H.2 方案 1（独立 RenderPass） | SceneColor + SceneDepth Store + Load ≈ 1080p × 4B + 1080p × 4B × 2（来回）≈ 16–20 MB | 0.3–1.5 ms | 全后端通用 |
| H.3 自定义 Subpass | 0（tile-internal） | ~0 | 仅 Vulkan，需手改 RHI |
| H.2 方案 2 (`r.Mobile.EarlyZPass=1`) | PrePass 已强制启用，所有 Opaque 多走一次 vs / 0.5–1.5 MB tile 写出 | 0.5–2 ms（取决于场景三角形数） + 失去 MSAA | 全后端 |

> 实测建议：**先用方案 1 实现并跑 GPU Profiler**（`profilegpu`、`stat unit`、Mali Streamline / Snapdragon Profiler）。如果 store/load 真的成为瓶颈，再考虑 H.3。

---

## I. 总结回答

1. **`RHICmdList.NextSubpass()` 做了什么**：调用平台后端 `RHINextSubpass()`，在 Vulkan 上就是 `vkCmdNextSubpass(VK_SUBPASS_CONTENTS_INLINE)`，在同一个 `VkRenderPass` 内推进到下一个**预先声明**的 subpass。它不会切换 RenderPass、不触发 tile flush。

2. **`SetCurrentStat(GET_STATID(STAT_CLMM_Translucency))`**：仅是**性能 stat marker**，往 RHI 命令流插入 `FRHICommandStat`，在执行时把 `GCurrentExecuteStat` 设为 "Translucency"，让 RHI 线程耗时归到这一桶里。**不影响任何 GPU 工作**。

3. **为什么 Subpass 1 的 DepthStencil 是 read-only**：因为创建 `VkRenderPass` 时（`VulkanRenderpass.h:482`），第二个 Subpass 的 `DepthStencilAttachment.layout` 被写死为 `VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL`，同时把同一份深度挂为 InputAttachment（`:518-523`）。这是 RenderPass 描述符层面的硬约束，PSO 里再怎么开 `DepthWrite=true` 也写不进去。

4. **能不能再开一个写深度的 Subpass**：
   - 在**当前已 Begin 的 RenderPass 内** → 不可以（`VkRenderPass` 不可变）；
   - 在 RenderTranslucency 之后**结束当前 RenderPass，新开一个** → 完全可以，就是方案 1；
   - 给 `ESubpassHint` 新增枚举值并改 RHI，让 `VkRenderPass` 多一个可写深度的第三 Subpass → 理论可以，工程成本远大于收益，**不推荐**。

5. **性能损耗**：
   - 方案 1（新开 RenderPass）：~1 次 SceneColor/SceneDepth Store+Load = 16–20 MB 带宽 / 0.3–1.5 ms（1080p 估算）；
   - 方案 H.3（改 RHI 加 Subpass）：理论上几乎为 0；
   - 在绝大多数项目里方案 1 的成本可以接受，且不破坏跨平台一致性。

## J. 证据速查表（本节追加部分）

- `Engine/Source/Runtime/RHI/Public/RHICommandList.h:3888-3897` — `FRHICommandList::NextSubpass()` 入口
- `Engine/Source/Runtime/RHI/Public/RHICommandList.h:1653` — `FRHICommandNextSubpass`（命令对象）
- `Engine/Source/Runtime/RHI/Public/RHIContext.h:757` — `RHINextSubpass()` 虚函数
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanRenderTarget.cpp:721-727` — Vulkan 实现：`vkCmdNextSubpass`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanRenderpass.h:417-674` — `VkRenderPass` 构建逻辑（按 `ESubpassHint` 分支构造 Subpass 0/1/2）
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanRenderpass.h:482` — Subpass 1 的深度 layout = `VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanRenderpass.h:518-523` — Subpass 1 把深度同时挂为 InputAttachment + DepthStencilAttachment
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanRenderpass.h:531-538` — Subpass 0→1 的 `VkSubpassDependency`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanPipeline.cpp:1802-1808` — PSO 校验：InputAttachment 必须在 SubpassIndex>=1
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanPipeline.cpp:1825-1829` — DepthReadSubpass 在 SubpassIndex>=1 时强制 NumRenderTargets=1
- `Engine/Source/Runtime/Renderer/Private/MobileBasePass.cpp:1036-1037` — 半透明 PSO `SubpassIndex = bDeferredShading ? 2 : 1`
- `Engine/Source/Runtime/RHI/Private/RHICommandList.cpp:305-317` — `SetCurrentStat` 实现（仅排队 `FRHICommandStat`，设全局 stat ID）
- `Engine/Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp:118-130` — `STAT_CLMM_*` 全部声明
- `Engine/Source/Runtime/RHI/Public/RHIResources.h:3688-3701` — `ESubpassHint` 枚举（None / DepthReadSubpass / DeferredShadingSubpass / CustomResolveSubpass）

---

# 追加分析 3 — 为什么 UE 在 DepthReadSubpass（Subpass 1）里把 Depth 写入关闭？

这是 **移动 TBR/TBDR 硬件 + Vulkan Subpass 语义 + 半透明渲染语义 + 性能** 四方共同决定的工程取舍，不是 UE 的随意选择。下面分五层讲清楚。

## K.1 硬件层：移动 GPU 想让深度"留在 tile 内被 PS 读"

移动 GPU（Mali / Adreno / PowerVR / Apple GPU）想让 PS 在**同一个 RenderPass 内**通过 `SubpassLoad()` 直接读 tile memory 里的深度（framebuffer fetch / pixel local storage），这样：

- 半透明做软粒子、Decal 做深度对比、Fog 做体积深度衰减——全部不用采样独立的 SceneDepth 纹理；
- **不出 tile、不走 DRAM 带宽**；
- 等价于一次"免费"的深度读取。

但 Vulkan/硬件有一个强制规则：

> 当一个 attachment 在同一个 Subpass 内**同时**被声明为 InputAttachment（被 PS 通过 `SubpassLoad` 读）和 DepthStencilAttachment（潜在被写），它的 layout 必须是下面这类**只读**之一：
>
> - `VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL`
> - `VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL`（UE 用这个，depth 只读 / stencil 可写）
> - `VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_STENCIL_READ_ONLY_OPTIMAL`
> - `VK_IMAGE_LAYOUT_GENERAL`（带性能损失，不推荐）
>
> **没有任何 layout 能"同时支持作为 InputAttachment 被采样 + 深度可写"**。如果硬把 layout 设为 `DEPTH_STENCIL_ATTACHMENT_OPTIMAL`（可写）且同时挂 InputAttachment，validation layer 会直接报 VUID 错误，驱动行为未定义。这就是"feedback loop"被禁止的根因——硬件在 tile memory 内通过 framebuffer fetch 读取深度时，需要 attachment 是稳定的（read-only），否则同一 tile 内的多个 PS 实例之间会出现写后读 hazard。

### 证据：`VulkanRenderpass.h:467-487`

```cpp
if (bDepthReadSubpass || bDeferredShadingSubpass)
{
    DepthStencilAttachment.attachment = RTLayout.GetDepthAttachmentReference()->attachment;
    DepthStencilAttachment.SetAspect(VK_IMAGE_ASPECT_DEPTH_BIT);

    // FIXME: checking a Depth layout is not correct in all cases
    // PSO cache can create a PSO for subpass 1 or 2 first, where depth is read-only but that does not mean depth pre-pass is enabled
    if (false && RTLayout.GetDepthAttachmentReference()->layout == VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL)
    {
        // Depth is read only and is expected to be sampled as a regular texture
        DepthStencilAttachment.layout = VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL;
    }
    else
    {
        // lights write to stencil for culling, so stencil is expected to be writebale while depth is read-only
        DepthStencilAttachment.layout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL;
        DepthInputAttachment        = DepthStencilAttachment.attachment;
        DepthInputAttachmentLayout  = DepthStencilAttachment.layout;
        DepthInputAspectMask        = VK_IMAGE_ASPECT_DEPTH_BIT;
    }
}
```

### 关于 `if (false && ...)` 这一段（重要澄清）

仔细看这条 `if` —— 第一项条件直接是 `false`，由于 `&&` 短路求值，整条分支**在任何运行时条件下都不会进入**，编译器会直接当作 `if (false)` 优化掉。**这是 Epic 主动用 `false` 禁掉的一条 future-优化分支，不是误写**。它对应的注释 `FIXME: checking a Depth layout is not correct in all cases` 把意图讲清楚了：

- 原本的想法：如果传入的 Depth attachment layout 已经是 `READ_ONLY_OPTIMAL`（说明外层做过 FullDepthPrepass，深度在 RenderPass 开始前就已经 ready 并且整张图都是只读的），Subpass 1 可以走另一种 layout（`VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL`，**深度+stencil 都只读**），让深度按"普通只读纹理"对待，避免写 stencil 的某些路径。
- 发现的问题：**Vulkan PSO Cache 在编译 PSO 时不知道当前 RenderPass 是哪种"宏观情况"**——它只看 PSO 自己声明的 `SubpassIndex`。同一个 `VkRenderPass` 可能被不同代码路径（带 prepass / 不带 prepass）拿来用，导致 PSO 的 RenderPass 兼容性出问题。
- 解决方案：先用 `false` 把这条分支关掉，等于"软删除 + 备忘"（UE 代码库常见的 FIXME 写法），保留意图等以后修。

**所以当前线上行为永远走 else 分支：`Subpass 1` 的 DepthStencilAttachment layout = `DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL`，同时这一份深度被加成 InputAttachment。这就是 PS 能 fetch 深度、但不能写深度的实际原因。**

### "两者不可兼得"结论的来源

这个结论**不是从被禁用的 `if (false && ...)` 分支得出的**——那条分支只是被关闭的未来优化。结论的真正依据是：

1. **else 分支自身**：同一个 `DepthStencilAttachment` 既被 `SubpassDesc.SetDepthStencilAttachment(...)` 挂为深度模板附件（`VulkanRenderpass.h:523`），又通过 `InputAttachments1[0]` 被 `SubpassDesc.SetInputAttachments(...)` 挂为输入附件（`VulkanRenderpass.h:518-521`），而这一个 attachment 只能有**唯一**的 layout。
2. **Vulkan 规范**：这个 layout 必须是上面那 4 种"包含只读"的之一，没有一种能让深度同时可读（作为 InputAttachment）+ 可写。
3. **else 分支选择的是 `DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL`**：明确表示"深度只读、stencil 可写"——后者保留 stencil 写权限是为了 Mobile Forward 的 light culling 之类逻辑（注释 "lights write to stencil for culling"）。

> 结论 1（更精确表述）：在 Subpass 1 内，由于深度同时被声明为 DepthStencilAttachment + InputAttachment，Vulkan 规范要求 layout 必须为只读形式之一；UE 选择了 `DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL`，因此深度在 Subpass 1 内无法写入。被禁用的 `if (false && ...)` 分支是 Epic 留下的优化想法（针对外层已 prepass 完成的情形改成全只读 layout），但因 PSO Cache 兼容性问题被暂时关闭，**不影响"Subpass 1 内不能写深度"这一既成事实**。

## K.2 渲染语义层：Subpass 1 里跑的东西本来就不该写深度

`MobileShadingRenderer.cpp:1614-1623` 之后跑的依次是：

```cpp
RHICmdList.NextSubpass();
RenderDecals(...);                    // Decal：写在已有表面上的贴花
RenderModulatedShadowProjections(...);// 调制阴影：暗色叠加
RenderFog(...);                       // 全屏雾：颜色调制
RenderTranslucency(...);              // 半透明物体（玻璃、火焰、粒子）
```

这些 Pass 的共同点：**它们的颜色叠加/混合到已有 SceneColor 上，但它们的"存在"不应改变 Opaque 已定下的深度可见性**。

- **半透明物体不写深度**是 OIT / over operator 的基础规则；
- **Decal 不写深度**：它只是在 Opaque 表面"涂"颜色；
- **Fog 不写深度**：全屏后处理；
- **Modulated Shadow 不写深度**：阴影只是暗色叠加。

证据（所有 Mobile Translucency Processor 都用 `<false, ...>`）：
- `MobileBasePass.cpp:1187`（`CreateMobileTranslucencyStandardPassProcessor`）
- `MobileBasePass.cpp:1199`（`CreateMobileTranslucencyAfterDOFProcessor`）
- `MobileBasePass.cpp:1210`（`CreateMobileTranslucencyAllPassProcessor`）

> 结论 2：即使没有 Subpass 限制，UE 的设计本来就不希望 Subpass 1 里的物体写深度。Subpass read-only 把这件事强制化，避免有人误配 PSO 写深度而破坏视觉。

## K.3 性能层：Read-Only Depth 启用 Early-Z 与 Hierarchical-Z

如果一个 Pass 同时读 + 写深度，GPU 不能在 ROP 之前预知最终的 Z 值，**Early-Z 与 HiZ 会被部分禁用**（驱动 fallback 到 Late-Z）。

把 Subpass 1 标为 Depth Read-Only，等于明确告诉驱动："这一段内我只做 ZTest、永远不会写 Z"。驱动据此可以：

- 全程用 Early-Z 提前剔除（半透明、Decal、Fog 都享受到 HiZ 加速）；
- Depth Buffer 永远不需要在 tile 内反复 RMW，**省 ROP 带宽**；
- 开启 PowerVR 的 Hidden Surface Removal、Adreno 的 Forward Pixel Killing 等架构级优化。

对比：如果 Subpass 1 允许写深度，驱动必须按 "Depth RMW" 模式工作，半透明 overdraw 多的场景性能掉得很明显。

> 结论 3：read-only 不只是为了让 PS 能 fetch 深度，也让深度比较与剔除走最快的硬件路径。

## K.4 工程一致性：避免半透明顺序错乱与 Decal 自咬合

如果 Subpass 1 允许写深度：

- 先画的玻璃 A 写了深度，后画的玻璃 B 通过 ZTest 又写一次 → B 的颜色盖过 A、但 A 的颜色仍在 SceneColor 里 → **半透明顺序结果错乱**；
- Decal 写深度 → 与原始表面深度产生 z-fighting（自咬合）；
- 软粒子从"距 SceneDepth 越近越淡"变成"距 SceneDepth_被前一个粒子改过 越近越淡" → 互相影响、闪烁。

UE 把这一段的深度写直接禁掉，**杜绝以上所有 bug 的源头**，让管线行为可预测。

## K.5 历史权衡：DepthReadSubpass 的设计初衷

- 早期移动 UE 是 MultiPass（半透明独立 RenderPass，会 store/load SceneColor）；
- 后来 Vulkan/Metal 普及后引入 SinglePass + `ESubpassHint::DepthReadSubpass`（`RHIResources.h:3688`），**主要目的就是省一次 SceneColor/SceneDepth 的 store-load**；
- 代价是"半透明阶段深度只读"——这个代价**正好与 UE 渲染语义匹配**（半透明本来就不写深度），无任何视觉损失；
- 反过来，如果 UE 让 Subpass 1 可写深度 → 必须把深度 attachment layout 改回 `ATTACHMENT_OPTIMAL` → 失去 PS 在 tile 内 fetch SceneDepth 的能力 → 软粒子/Decal/Fog 全要走"采样独立 SceneDepth 纹理"的慢路径 → SceneDepth 需要 Resolve/Copy → 又得一次 store-load。

**等于回到了 MultiPass 的开销，却没有任何收益。**

## K.6 一句话总结

| 维度 | 为什么 Read-Only |
|---|---|
| 硬件 | Vulkan/移动 GPU 规则：InputAttachment fetch 必须 read-only，否则是 feedback loop |
| 语义 | 半透明、Decal、Fog 本来就不该写深度 |
| 性能 | Read-only 让 Early-Z / HiZ / HSR 全部生效，省 ROP 带宽 |
| 工程 | 杜绝半透明顺序错乱、Decal 自咬合 |
| 历史 | DepthReadSubpass 的设计初衷就是用"放弃写深度"换取"in-tile 读深度"，比 MultiPass 省一次 store/load |

UE 把 Subpass 1 的深度禁写**不是限制**，而是**为了拿到 in-tile depth fetch 这个大优化所必须付出的、且在渲染语义上正好不亏的代价**。AfterTranslucency 想"既在这段时间画，又写深度"，是逆着这个权衡走——所以必须用方案 1（独立 RenderPass）跳出来。

## K.7 证据速查表（本节追加部分）

- `Engine/Source/Runtime/VulkanRHI/Private/VulkanRenderpass.h:474-486` — Subpass 1 深度 layout 设为 `DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL` 的代码与注释
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanRenderpass.h:518-523` — 同一份深度同时挂为 InputAttachment + DepthStencilAttachment
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanRenderpass.h:531-538` — Subpass 0→1 的 `VkSubpassDependency`：srcAccess=DEPTH_WRITE → dstAccess=INPUT_ATTACHMENT_READ
- `Engine/Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp:1614-1623` — Subpass 1 内顺序：Decal / ModulatedShadow / Fog / Translucency 全部不写深度
- `Engine/Source/Runtime/Renderer/Private/MobileBasePass.cpp:1184-1216` — 三个 Mobile Translucency Processor 全部 `<false, CF_DepthNearOrEqual>` + `DepthRead_StencilRead`
- `Engine/Source/Runtime/RHI/Public/RHIResources.h:3688-3701` — `ESubpassHint` 枚举注释（"Render pass has depth reading subpass"）

---

# 追加分析 4 — RenderMobileBasePass 是否写入深度？能否"BasePass 只写我物体的深度，AfterTranslucency 再写颜色"？

## L.1 RenderMobileBasePass 到底写不写深度？

**会。** `RenderMobileBasePass` 是移动 Forward 路径下**绝大多数 opaque/masked 物体写入硬件深度的唯一阶段**。逐步证明：

### L.1.1 Pass 入口

`Engine/Source/Runtime/Renderer/Private/MobileBasePassRendering.cpp:470-491`：

```cpp
void FMobileSceneRenderer::RenderMobileBasePass(FRHICommandList& RHICmdList,
        const FViewInfo& View, const FInstanceCullingDrawParams* InstanceCullingDrawParams)
{
    CSV_SCOPED_TIMING_STAT_EXCLUSIVE(RenderBasePass);
    SCOPED_DRAW_EVENT(RHICmdList, MobileBasePass);
    ...
    RHICmdList.SetViewport(...);
    View.ParallelMeshDrawCommandPasses[EMeshPass::BasePass]
        .DispatchDraw(nullptr, RHICmdList, InstanceCullingDrawParams);

    if (View.Family->EngineShowFlags.Atmosphere)
    {
        View.ParallelMeshDrawCommandPasses[EMeshPass::SkyPass]
            .DispatchDraw(nullptr, RHICmdList, &SkyPassInstanceCullingDrawParams);
    }

    // editor primitives
    FMeshPassProcessorRenderState DrawRenderState;
    DrawRenderState.SetBlendState(TStaticBlendStateWriteMask<CW_RGBA>::GetRHI());
    DrawRenderState.SetDepthStencilAccess(Scene->DefaultBasePassDepthStencilAccess);
    DrawRenderState.SetDepthStencilState(
        TStaticDepthStencilState<true, CF_DepthNearOrEqual>::GetRHI());  // <-- 深度可写
    RenderMobileEditorPrimitives(RHICmdList, View, DrawRenderState, InstanceCullingDrawParams);
}
```

它干 3 件事：
1. 派发 `EMeshPass::BasePass` 的所有 MeshDrawCommand；
2. 派发 `EMeshPass::SkyPass`（如果开了大气）；
3. 渲染 EditorPrimitives（DepthWrite = true）。

### L.1.2 BasePass 用的 RenderTarget Access（关键）

`MobileShadingRenderer.cpp:1494-1496`：

```cpp
BasePassRenderTargets.DepthStencil = bIsFullDepthPrepassEnabled ?
    FDepthStencilBinding(SceneDepth, ELoad, ELoad, FExclusiveDepthStencil::DepthRead_StencilWrite) :
    FDepthStencilBinding(SceneDepth, EClear, EClear, FExclusiveDepthStencil::DepthWrite_StencilWrite);
```

- 默认（`r.Mobile.EarlyZPass=0`，没 SSAO/DBuffer/ShadowMask）→ 走 else → **`DepthWrite_StencilWrite`**；
- BasePass 在 Subpass 0 执行（`MobileBasePass.cpp:1036` `SubpassIndex=0`），attachment layout 是 `DEPTH_STENCIL_ATTACHMENT_OPTIMAL`，**深度可写**。

### L.1.3 BasePass 的 PSO（Processor 层）

`MobileBasePass.cpp:1151-1162`（`CreateMobileBasePassProcessor`）：

```cpp
PassDrawRenderState.SetBlendState(TStaticBlendStateWriteMask<CW_RGBA>::GetRHI()); // 颜色全写
PassDrawRenderState.SetDepthStencilAccess(DefaultBasePassDepthStencilAccess);
PassDrawRenderState.SetDepthStencilState(
    TStaticDepthStencilState<true, CF_DepthNearOrEqual>::GetRHI());               // 深度写=true
```

到了 `Process` 阶段，`MobileBasePass.cpp:946-960` 会根据情况调整：
- 无 PrePass 且非 masked-in-early → `MobileBasePass::SetOpaqueRenderState(...)`，里面用：
  ```cpp
  TStaticDepthStencilState<true, CF_DepthNearOrEqual,
      true, CF_Always, SO_Keep, SO_Keep, SO_Replace, ...>::GetRHI();  // 深度 + Stencil 都写
  ```
  （`MobileBasePass.cpp:549-557`）
- 已有 PrePass 写过深度 → `TStaticDepthStencilState<false, CF_Equal>` 只做 ZTest，不再重复写。

### L.1.4 一句话

> **在默认移动 Forward 路径下，`RenderMobileBasePass` 中每个不透明物体都用 `<true, CF_DepthNearOrEqual>` 写颜色 + 写深度（必要时也写 stencil）。这就是 SceneDepth 的来源。**

## L.2 移动端整体深度渲染链路速查

按 SinglePass 路径（默认）：

```
[CPU 端：FScene::GetEarlyZPassMode]
    ├─ MobileUsesFullDepthPrepass = false（默认）
    └─ EarlyZPassMode = DDM_None / DDM_MaskedOnly
            ↓
[FMobileSceneRenderer::Render]
    ├─ if (bIsMaskedOnlyDepthPrepassEnabled)
    │     └─ RenderMaskedPrePass()              ← 仅 Masked 物体写深度（独立 RenderPass）
    │
    └─ RenderForward()
          └─ RenderForwardSinglePass()           ← 一个大 RenderPass (Vulkan VkRenderPass)
                ├─ [VkRenderPass.Subpass 0]      ← DepthStencil Access = DepthWrite_StencilWrite
                │     ├─ RenderMaskedPrePass()   ← 若 DDM_MaskedOnly，在这里仍画一遍（可能重复）
                │     ├─ RenderMobileBasePass()  ← ★ 所有 opaque 物体在这里写 SceneDepth
                │     ├─ SkyPass
                │     └─ EditorPrimitives
                │     PostRenderBasePass()
                │
                ├─ RHICmdList.NextSubpass()      ← 切到 Subpass 1，深度变 read-only InputAttachment
                │
                └─ [VkRenderPass.Subpass 1]      ← DepthStencil layout = DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL
                      ├─ RenderDecals()
                      ├─ RenderModulatedShadowProjections()
                      ├─ RenderFog()
                      ├─ RenderTranslucency()
                      └─ (你想插入的 RenderMobileAfterTranslucencyPass)  ← 这里不能写深度
```

启用 `r.Mobile.EarlyZPass=1`（Full PrePass）时：

```
[FMobileSceneRenderer::Render]
    └─ if (bIsFullDepthPrepassEnabled)
          └─ RenderFullDepthPrepass()            ← 独立 RenderPass，DepthStencil=DepthWrite_StencilWrite
                                                   所有 EMeshPass::DepthPass 的物体写深度
            ↓
    └─ RenderForward() / RenderForwardSinglePass()
          └─ [VkRenderPass.Subpass 0]            ← DepthStencil Access = DepthRead_StencilWrite
                ├─ RenderMobileBasePass()        ← BasePass 用 <false, CF_Equal>，只做 ZTest 不写深度
                │   （因 MobileBasePass.cpp:952，PrePass 已写过）
                ...
          [Subpass 1 同上，仍然 read-only]
```

**关键对比**：

| 模式 | 谁写深度 | 写在哪个 RenderPass |
|---|---|---|
| 默认 SinglePass | RenderMobileBasePass | 同一 RenderPass 的 Subpass 0 |
| Masked PrePass | RenderMaskedPrePass（仅 masked）+ RenderMobileBasePass（其它） | 两个不同 RenderPass |
| Full PrePass | RenderFullDepthPrepass（全部） | 独立 RenderPass；BasePass 内不再写 |

## L.3 "BasePass 只写我物体的深度（不写颜色），AfterTranslucency 再写颜色" —— 可行性 & 实现方案

### L.3.1 直接结论

**可行，且这是在 SinglePass 架构下最高效的解法之一**：

- BasePass 阶段（Subpass 0）深度 attachment 是 `DepthWrite`，**你的物体可以放心写深度**；
- AfterTranslucency 阶段（Subpass 1）深度是 read-only InputAttachment，**你的物体只写颜色**，并且可以在 PS 中 fetch 自己已写入的深度做 soft particle / depth-test；
- 由于深度已经在 BasePass 写过，AfterTranslucency 的 PS 用 `CF_Equal` 做 ZTest，可以完美对齐像素位置，且 Early-Z 全开；
- **不需要新增独立 RenderPass、不触发 tile flush、性能几乎免费**。

这相当于 UE 自己在 `MobileBasePass.cpp:952` 处对"已经走过 PrePass"的物体做的优化 —— 你只是把这个机制套到一小撮自定义物体上。

### L.3.2 实现核心：在 BasePass 内画一个"depth-only ghost"版本

不要直接修改 BasePass 让它"不画颜色"——会破坏全局逻辑。**正确做法是新建一个独立的 MeshPass**，专门收集你的 AfterTranslucency 物体的 **"depth-only" 副本**，让它**也在 Subpass 0 内派发**，但 BlendState = `CW_NONE`（颜色全屏蔽）、DepthStencilState = `<true, CF_DepthNearOrEqual>`。

#### 步骤 1：新建 `EMeshPass::MobileAfterTranslucencyDepthPass`

```cpp
// SceneRendering.h - EMeshPass 枚举
enum class EMeshPass : uint8 {
    ...
    MobileAfterTranslucencyPass,             // 你已加 - 颜色 Pass，跑在 Subpass 1
    MobileAfterTranslucencyDepthPass,        // 新加 - 深度 Pass，跑在 Subpass 0
    ...
};
```

#### 步骤 2：新建对应的 Processor（关键：CW_NONE + DepthWrite）

```cpp
// MobileBasePass.cpp（或新文件）
FMeshPassProcessor* CreateMobileAfterTranslucencyDepthPassProcessor(
    ERHIFeatureLevel::Type FeatureLevel,
    const FScene* Scene,
    const FSceneView* InViewIfDynamicMeshCommand,
    FMeshPassDrawListContext* InDrawListContext)
{
    FMeshPassProcessorRenderState PassDrawRenderState;
    // 关键 1：颜色全屏蔽（8 个 MRT 都关）
    PassDrawRenderState.SetBlendState(TStaticBlendStateWriteMask<
        CW_NONE, CW_NONE, CW_NONE, CW_NONE,
        CW_NONE, CW_NONE, CW_NONE, CW_NONE>::GetRHI());
    // 关键 2：深度写开，stencil 也可写（匹配 BasePass Access）
    PassDrawRenderState.SetDepthStencilAccess(
        FExclusiveDepthStencil::DepthWrite_StencilWrite);
    PassDrawRenderState.SetDepthStencilState(TStaticDepthStencilState<
        true, CF_DepthNearOrEqual>::GetRHI());

    const FMobileBasePassMeshProcessor::EFlags Flags =
        FMobileBasePassMeshProcessor::EFlags::CanUseDepthStencil
        | FMobileBasePassMeshProcessor::EFlags::ForcePassDrawRenderState;
        // ForcePassDrawRenderState：阻止 Process 内再被 SetOpaqueRenderState 覆盖
        // 参考 MobileBasePass.cpp:943-961

    return new FMobileBasePassMeshProcessor(
        EMeshPass::MobileAfterTranslucencyDepthPass,
        Scene, InViewIfDynamicMeshCommand,
        PassDrawRenderState, InDrawListContext, Flags);
}

REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR(
    MobileAfterTranslucencyDepthPass,
    CreateMobileAfterTranslucencyDepthPassProcessor,
    EShadingPath::Mobile, EMeshPass::MobileAfterTranslucencyDepthPass,
    EMeshPassFlags::CachedMeshCommands | EMeshPassFlags::MainView);
```

> 注意 `EFlags::ForcePassDrawRenderState`（`MobileBasePassRendering.h:474`）的作用：`FMobileBasePassMeshProcessor::Process`（`MobileBasePass.cpp:943-961`）默认会根据物体属性重新调用 `SetOpaqueRenderState` 覆盖 DepthStencilState（参考 Renderer.cpp:522 的用法）。加这个 Flag 后 Processor 不会覆盖，**你设的 `<true, CF_DepthNearOrEqual>` + `CW_NONE` 会保留到最终 PSO**。

#### 步骤 3：让 AfterTranslucency 的颜色 Pass 改用 CF_Equal

由于深度已经在 Subpass 0 写好，颜色 Pass 用 `CF_Equal` 最优（HiZ + Early-Z 100% 命中）：

```cpp
// CreateMobileAfterTranslucencyPassProcessor 修改
PassDrawRenderState.SetDepthStencilAccess(
    FExclusiveDepthStencil::DepthRead_StencilRead);
PassDrawRenderState.SetDepthStencilState(TStaticDepthStencilState<
    false, CF_Equal>::GetRHI());     // 改成 CF_Equal，最快
```

#### 步骤 4：MeshDrawCommand 收集时把同一物体加到两个 Pass

在 PrimitiveSceneInfo 添加 mesh 收集逻辑里，对你想做 AfterTranslucency 的 primitive 同时 enqueue 到：
- `EMeshPass::MobileAfterTranslucencyDepthPass`（深度副本）
- `EMeshPass::MobileAfterTranslucencyPass`（颜色副本）

且**不要**加进 `EMeshPass::BasePass`。

#### 步骤 5：在 SinglePass lambda 的 Subpass 0 里派发 Depth Pass

```cpp
// MobileShadingRenderer.cpp RenderForwardSinglePass lambda
RHICmdList.SetCurrentStat(GET_STATID(STAT_CLMM_Opaque));
RenderMobileBasePass(RHICmdList, View, &PassParameters->InstanceCullingDrawParams);

// ★ 新加：在 Subpass 0 末尾派发深度副本（仍可写深度）
View.ParallelMeshDrawCommandPasses[EMeshPass::MobileAfterTranslucencyDepthPass]
    .DispatchDraw(nullptr, RHICmdList, &AfterTranslucencyDepthInstanceCullingDrawParams);

RenderMobileDebugView(RHICmdList, View);
RHICmdList.PollOcclusionQueries();
PostRenderBasePass(RHICmdList, View);

// scene depth is read only and can be fetched
RHICmdList.NextSubpass();
...
RenderTranslucency(RHICmdList, View);

// 颜色副本（深度已写过，CF_Equal 配对）
RenderMobileAfterTranslucencyPass(RHICmdList, View,
    &AfterTranslucencyInstanceCullingDrawParams);
```

#### 步骤 6：BuildInstanceCullingDrawParams 补一行

```cpp
View.ParallelMeshDrawCommandPasses[EMeshPass::MobileAfterTranslucencyDepthPass]
    .BuildRenderingCommands(GraphBuilder, Scene->GPUScene,
        AfterTranslucencyDepthInstanceCullingDrawParams);
```

### L.3.3 效果

| 阶段 | 操作 | 深度 | 颜色 |
|---|---|---|---|
| Subpass 0 — RenderMobileBasePass | 其他 opaque 正常画 | 写 | 写 |
| Subpass 0 — MobileAfterTranslucencyDepthPass | **你的物体 depth-only** | **写** | **屏蔽（CW_NONE）** |
| NextSubpass → Subpass 1 | 切只读 | read-only | - |
| Subpass 1 — RenderTranslucency 等 | 半透明叠加 | 不写 | 写 |
| Subpass 1 — MobileAfterTranslucencyPass | **你的物体 color-only** | **不写（CF_Equal 测试通过）** | **写** |

**最终 SceneDepth 里有你物体的深度，最终 SceneColor 里你的颜色在半透明之上**。

### L.3.4 为什么这样不会破坏视觉？

- 深度副本只贡献 SceneDepth，不写 SceneColor，所以 BasePass 阶段半透明物体之前的 SceneColor 不会被你的颜色污染；
- 深度副本写完后，**任何在 Subpass 0 之后才画的半透明（Decal/Fog/Translucency）都会被你的深度遮挡**（因为它们做 ZTest），正好符合"我的物体应该挡住半透明"的语义；

  - 如果你**不希望**深度阶段就遮挡半透明（例如希望半透明先全画完再被你覆盖），那就**不能用这个方案**，因为深度遮挡发生在 Translucency 之前；
  - 你最初的诉求似乎是"半透明在前、我画在后覆盖颜色"——那就需要明确：要不要让半透明被你的物体遮挡。如果**两个都要**（半透明被遮挡 + 颜色压在半透明之上），方案 L.3 完美；如果半透明**不应**被你的物体遮挡（半透明应该可见，你的颜色再压上来），那这个方案不对。
- 颜色副本在 Subpass 1 用 `CF_Equal`，只有"恰好在自己之前写过深度的像素"才通过 → 准确画在自己深度位置上 → 颜色压在半透明之上。

### L.3.5 性能成本

- **零额外 RenderPass、零 tile flush**；
- 多一次 vertex shader 执行（深度副本通常用 DepthOnly VS，开销 ~10%–30% 比 BasePass VS 低）；
- PS 几乎为零（无颜色写、material 早返回，硬件通常跑深度专用 fast-path）；
- 颜色 Pass 用 CF_Equal 反而比 CF_DepthNearOrEqual 更快（HiZ 100% 命中）；
- 总体几乎免费。**比方案 1（独立 RenderPass）省 16–20 MB 带宽 / 0.3–1.5 ms**。

### L.3.6 注意事项 & 陷阱

1. **必须使用 `ForcePassDrawRenderState` Flag**，否则 `FMobileBasePassMeshProcessor::Process` 会用 `SetOpaqueRenderState` 把你的 `<true, ...>` + `CW_NONE` 覆盖掉。证据：`MobileBasePass.cpp:946 `if (!bForcePassDrawRenderState) { ... SetOpaqueRenderState ... }`。
2. 深度副本最好用 **DepthOnly Shader**（参考 `DepthRendering.cpp:786 FDepthPassMeshProcessor::Process`），可以让 PS 完全不跑材质，性能最佳。但工程上更简单的做法是复用 BasePass shader + `CW_NONE`，驱动也能识别"PS 输出被屏蔽 → 可跳过"。
3. **Stencil**：如果你的物体需要写 Stencil（DBuffer Decal 接收标记等），在 Subpass 1 仍能写 stencil（layout `DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL`），但**深度阶段就写好通常更省**。
4. **MSAA**：如果开了 MSAA，Subpass 0 写的是 MSAA depth，Subpass 1 fetch 的是同一份。CF_Equal 配对在 sample-rate shading 下需要测试稳定性（部分 GPU 在 MSAA 下深度精度有抖动，可能改成 `CF_DepthNearOrEqual` 更保险）。
5. **半透明遮挡问题**：如 L.3.4 所述，需要明确你的语义。

## L.4 与之前 3 个方案对比

| 方案 | 改动量 | 性能 | 深度可见 | 半透明被遮挡 |
|---|---|---|---|---|
| 原 Plan（无深度写） | 最小 | 最快 | ✘ | ✘（颜色覆盖） |
| **L.3 BasePass 深度副本（新推荐）** | **中等** | **几乎免费** | **✓** | **✓** |
| 方案 1（独立 RenderPass） | 中等 | -0.3~1.5ms / 16-20MB | ✓ | ✓ |
| 方案 2（`r.Mobile.EarlyZPass=1`） | 全局开销 + 失去 MSAA | 中等 | ✓ | ✓ |
| 方案 3（MultiPass） | 大 | 最差 | ✓ | ✓ |

**结论：L.3"BasePass 内 depth-only 副本 + Subpass 1 color-only 副本"是 SinglePass 架构下**几乎免费**的最优解，唯一额外成本是一次深度阶段的 VS 重新执行。前提是你接受"半透明可以被你的物体遮挡"这个语义。**

## L.5 证据速查表（本节追加部分）

- `Engine/Source/Runtime/Renderer/Private/MobileBasePassRendering.cpp:470-491` — `RenderMobileBasePass` 主体
- `Engine/Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp:1494-1496` — BasePass DepthStencil Access：默认 `DepthWrite_StencilWrite`
- `Engine/Source/Runtime/Renderer/Private/MobileBasePass.cpp:1151-1162` — `CreateMobileBasePassProcessor` 默认 `<true, CF_DepthNearOrEqual>`
- `Engine/Source/Runtime/Renderer/Private/MobileBasePass.cpp:549-557` — `SetOpaqueRenderState` 写 `<true, ..., SO_Replace>` 深度 + stencil
- `Engine/Source/Runtime/Renderer/Private/MobileBasePass.cpp:946-961` — Process 阶段的 DepthStencilState 覆盖逻辑（`bForcePassDrawRenderState` 控制是否覆盖）
- `Engine/Source/Runtime/Renderer/Private/MobileBasePassRendering.h:474` — `EFlags::ForcePassDrawRenderState`
- `Engine/Source/Runtime/Renderer/Private/Renderer.cpp:522` — `ForcePassDrawRenderState` 已有的使用示例（CSM）
- `Engine/Source/Runtime/Renderer/Private/MobileBasePass.cpp:1036` — BasePass `SubpassIndex = 0`
- `Engine/Source/Runtime/Renderer/Private/MobileDeferredShadingPass.cpp:674` — `TStaticBlendStateWriteMask<CW_NONE, ..., CW_NONE>` 用法范例（关所有颜色写）
- `Engine/Source/Runtime/Renderer/Private/DepthRendering.cpp:786 起` — DepthOnly shader 路径，可作为更激进的深度副本实现参考



