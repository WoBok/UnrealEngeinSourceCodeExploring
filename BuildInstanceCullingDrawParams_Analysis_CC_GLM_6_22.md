# `FMobileSceneRenderer::BuildInstanceCullingDrawParams` 详解

> 分析日期：2026-06-22
> 引擎版本：UE5.4（本地 `Engine/Source/...` 快照）
> 上下文：分析 Plan.md 时由 `BasePass` 与其它 Pass 的 `FInstanceCullingDrawParams` 声明方式不一致引出

---

## 1. 函数作用（一句话）

**为本帧 Mobile 渲染中每个使用 GPU Scene 的 Mesh Pass 提前做一次"GPU Culling + 间接绘制参数生成"**，把每个 Pass 用到的 `DrawIndirectArgsBuffer` / `InstanceIdOffsetBuffer` / 偏移等填进对应的 `FInstanceCullingDrawParams`，供之后 `DispatchDraw` 调用 `RHICmdList.DrawIndexedPrimitiveIndirect` 时使用。

`Engine/Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp:1433-1446`：

```cpp
void FMobileSceneRenderer::BuildInstanceCullingDrawParams(FRDGBuilder& GraphBuilder, FViewInfo& View, FMobileRenderPassParameters* PassParameters)
{
    if (Scene->GPUScene.IsEnabled())   // 关键：只有 GPUScene 开启才走 GPU Culling
    {
        if (!bIsFullDepthPrepassEnabled)
            View.ParallelMeshDrawCommandPasses[EMeshPass::DepthPass].BuildRenderingCommands(GraphBuilder, Scene->GPUScene, DepthPassInstanceCullingDrawParams);

        View.ParallelMeshDrawCommandPasses[EMeshPass::BasePass].BuildRenderingCommands(GraphBuilder, Scene->GPUScene, PassParameters->InstanceCullingDrawParams);  // ← 嵌入 PassParameters
        View.ParallelMeshDrawCommandPasses[EMeshPass::SkyPass].BuildRenderingCommands(GraphBuilder, Scene->GPUScene, SkyPassInstanceCullingDrawParams);
        View.ParallelMeshDrawCommandPasses[StandardTranslucencyMeshPass].BuildRenderingCommands(GraphBuilder, Scene->GPUScene, TranslucencyInstanceCullingDrawParams);
        View.ParallelMeshDrawCommandPasses[EMeshPass::DebugViewMode].BuildRenderingCommands(GraphBuilder, Scene->GPUScene, DebugViewModeInstanceCullingDrawParams);
    }
}
```

调用时机：在所有 `RenderForwardSinglePass / MultiPass / DeferredSinglePass / DeferredMultiPass` 的 RDG Pass **添加之前**（`MobileShadingRenderer.cpp:1564 / 1934`），即 GPU Culling 和实际绘制是分两个步骤的。

---

## 2. `FInstanceCullingDrawParams` 本质是什么

`Engine/Source/Runtime/Renderer/Public/InstanceCulling/InstanceCullingContext.h:32-40`：

```cpp
BEGIN_SHADER_PARAMETER_STRUCT(FInstanceCullingDrawParams, )
    RDG_BUFFER_ACCESS(DrawIndirectArgsBuffer, ERHIAccess::IndirectArgs)
    RDG_BUFFER_ACCESS(InstanceIdOffsetBuffer, ERHIAccess::VertexOrIndexBuffer)
    SHADER_PARAMETER(uint32, InstanceDataByteOffset)  // 进每实例 buffer 的字节偏移
    SHADER_PARAMETER(uint32, IndirectArgsByteOffset)  // 进 indirect 参数 buffer 的字节偏移
    SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FInstanceCullingGlobalUniforms, InstanceCulling)
    SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FSceneUniformParameters, Scene)
    SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FBatchedPrimitiveParameters, BatchedPrimitive)
END_SHADER_PARAMETER_STRUCT()
```

**它本身就是一个 RDG Shader Parameter Struct**，可以两种方式被使用：
1. **嵌入到某个 RDG Pass 的 PassParameters 里** —— RDG 会自动跟踪 `DrawIndirectArgsBuffer / InstanceIdOffsetBuffer` 的资源依赖与状态转换。
2. **作为独立的"裸" struct 成员**保存在某处 —— 不参与 RDG 自动依赖管理，但内容仍然有效，可在合适的 RDG Pass 上下文里使用。

`RDG_BUFFER_ACCESS(DrawIndirectArgsBuffer, ERHIAccess::IndirectArgs)` 这一行告诉 RDG：**任何把它当作 PassParameters 一部分的 raster pass 在开始之前，必须把这个 buffer 转换到 `IndirectArgs` 访问状态**。`InstanceIdOffsetBuffer` 同理转换到 `VertexOrIndexBuffer` 状态。

---

## 3. 两种声明方式的根本原因

### 3.1 BasePass 用嵌入式（`PassParameters->InstanceCullingDrawParams`）

`MobileShadingRenderer.cpp:208-221`：

```cpp
BEGIN_SHADER_PARAMETER_STRUCT(FMobileRenderPassParameters, RENDERER_API)
    SHADER_PARAMETER_STRUCT_INCLUDE(FViewShaderParameters, View)
    SHADER_PARAMETER_STRUCT_INCLUDE(FInstanceCullingDrawParams, InstanceCullingDrawParams)  // ← 嵌入！
    SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FMobileBasePassUniformParameters, MobileBasePass)
    ...
    RENDER_TARGET_BINDING_SLOTS()
END_SHADER_PARAMETER_STRUCT()
```

每帧 `GraphBuilder.AllocParameters<FMobileRenderPassParameters>()` 分配的实例就是后面 `GraphBuilder.AddPass("SceneColorRendering", PassParameters, ERDGPassFlags::Raster, [lambda] ...)` 这个**主 raster pass 的参数包**。BasePass 是这个主 raster pass 的"主角"，所以它的 `FInstanceCullingDrawParams` 嵌入在这里，**RDG 在这个 pass 开始前自动把两个 buffer 转换到 `IndirectArgs / VertexOrIndexBuffer` 状态**。

### 3.2 其他 Pass 用独立成员

`SceneRendering.h:2791-2796`：

```cpp
// All mesh passes that can be fused into single render-pass
// Base mesh pass gets its culling parameters from a render-pass struct
FInstanceCullingDrawParams DepthPassInstanceCullingDrawParams;
FInstanceCullingDrawParams SkyPassInstanceCullingDrawParams;
FInstanceCullingDrawParams DebugViewModeInstanceCullingDrawParams;
FInstanceCullingDrawParams TranslucencyInstanceCullingDrawParams;
```

**Epic 的注释直接说明了**：这些 Pass "**可以被融合（fuse）到同一个 render-pass 里**"——意思是它们的 `DispatchDraw` 是在 BasePass 主 lambda **内部**调用的，**和 BasePass 共享同一个 RDG raster pass 的作用域**，因此不需要再单独声明 PassParameters。

证据看 `RenderForwardSinglePass`（`MobileShadingRenderer.cpp:1590-1653`）：

```cpp
GraphBuilder.AddPass(
    RDG_EVENT_NAME("SceneColorRendering"),
    PassParameters,                    // 只这一份 PassParameters，是 BasePass 的
    ERDGPassFlags::Raster | ERDGPassFlags::NeverMerge,
    [this, PassParameters, ...](FRHICommandList& RHICmdList)
{
    FViewInfo& View = *ViewContext.ViewInfo;
    ...
    RenderMaskedPrePass(RHICmdList, View);                           // ← 用 DepthPassInstanceCullingDrawParams
    RenderMobileBasePass(RHICmdList, View, &PassParameters->InstanceCullingDrawParams);  // ← 用嵌入的
    ...
    RenderDecals(RHICmdList, View, &PassParameters->InstanceCullingDrawParams);
    RenderFog(RHICmdList, View);
    RenderTranslucency(RHICmdList, View);                            // ← 用 TranslucencyInstanceCullingDrawParams
});
```

`RenderMobileBasePass` 内部调用 `DispatchDraw(BasePass)` 和 `DispatchDraw(SkyPass)`（`MobileBasePassRendering.cpp:482` 用的就是 `SkyPassInstanceCullingDrawParams`）。

**关键**：这些 `DispatchDraw` 都发生在同一个 RDG raster pass 的 lambda 里。它们调用的 RHI 命令（`RHICmdList.DrawIndexedPrimitiveIndirect`）所引用的 buffer 资源，是 `BuildRenderingCommands` 时通过 `GraphBuilder` 注册的 RDG buffer，**这些 buffer 的 access transition 由 BuildRenderingCommands 内部添加的 Compute Pass 完成**（GPU culling 那个 compute pass 把结果写出来，输出 access 自然是 `IndirectArgs` / `VertexOrIndexBuffer`）。所以即便独立保存也没问题，只要它们的使用点确实在 raster pass 的 lambda 内即可。

### 3.3 为什么 BasePass 非要嵌入？

理论上 BasePass 也可以做成"独立成员"，但 Epic 这样做有**两个工程上的好处**：

1. **PassParameters 是 RDG raster pass 的"标识符"**：BasePass 是 RDG raster pass 的核心，把它的 culling params 直接做成参数包的一部分，**所有 `RDG_BUFFER_ACCESS` 标记会被 RDG 验证器（debug 构建）严格检查**——可以提前发现 buffer 状态错误。
2. **多视图复制**：`MobileShadingRenderer.cpp:1708-1709` 的 `*SecondPassParameters = *PassParameters;`，第二阶段 RenderPass 复制 PassParameters 时，BasePass 的 culling 信息自动被携带（虽然第二阶段不用，但保留无害）；如果是独立成员，没有这种"附着"关系。

特殊情形（`MobileShadingRenderer.cpp:836-853`）：当启用 Full Depth Prepass 时，DepthPass 单独作为一个 RDG pass 跑：
```cpp
View.ParallelMeshDrawCommandPasses[EMeshPass::DepthPass].BuildRenderingCommands(
    GraphBuilder, Scene->GPUScene, PassParameters->InstanceCullingDrawParams);  // 此时复用 PassParameters 的嵌入字段
GraphBuilder.AddPass("FullDepthPrepass", PassParameters, ERDGPassFlags::Raster,
    [...](FRHICommandList& RHICmdList) {
        RenderPrePass(RHICmdList, View, &PassParameters->InstanceCullingDrawParams);
    });
```
在这条路径下 DepthPass 也成了"raster pass 的主角"，所以同样要嵌入。

---

## 4. 数据如何被填充：`BuildRenderingCommands`

`Engine/Source/Runtime/Renderer/Private/MeshDrawCommands.cpp:1605-1631`：

```cpp
void FParallelMeshDrawCommandPass::BuildRenderingCommands(
    FRDGBuilder& GraphBuilder,
    const FGPUScene& GPUScene,
    FInstanceCullingDrawParams& OutInstanceCullingDrawParams)  // ← 输出参数（引用）
{
    if (TaskContext.InstanceCullingContext.IsEnabled())
    {
        check(!bHasInstanceCullingDrawParameters);                // 每个 pass 只能填充一次

        if (CVarDeferredMeshPassSetupTaskSync.GetValueOnRenderThread() != 0)
        {
            TaskContext.InstanceCullingContext.BuildRenderingCommands(GraphBuilder, GPUScene, &OutInstanceCullingDrawParams);
        }
        else
        {
            TaskContext.InstanceCullingContext.WaitForSetupTask();
            TaskContext.InstanceCullingContext.BuildRenderingCommands(GraphBuilder, GPUScene, &OutInstanceCullingDrawParams);
        }

        bHasInstanceCullingDrawParameters = true;
        return;
    }
    // 不启用 GPU Culling 时退化为空
    OutInstanceCullingDrawParams.DrawIndirectArgsBuffer = nullptr;
    OutInstanceCullingDrawParams.InstanceIdOffsetBuffer = nullptr;
    OutInstanceCullingDrawParams.InstanceDataByteOffset = 0U;
    OutInstanceCullingDrawParams.IndirectArgsByteOffset = 0U;
}
```

填充流程（高层概括）：
1. 等 SetupTask 完成（前一帧/同帧异步收集到所有 `FMeshDrawCommand` 完成）。
2. `FInstanceCullingContext::BuildRenderingCommands` 内部向 `GraphBuilder` **添加一个 GPU Culling Compute Pass**：
   - 输入：所有 instance ID、GPUScene 数据、view 信息
   - 输出：`DrawIndirectArgsBuffer`（每个 draw call 的 instance count 等参数）、`InstanceIdOffsetBuffer`（每个 instance 在最终 buffer 里的偏移）
3. 把生成的两个 RDG buffer 引用、字节偏移、关联的 uniform buffer 写到 `OutInstanceCullingDrawParams`。

调用一次后 `bHasInstanceCullingDrawParameters` 置为 true，**同一个 ParallelMeshDrawCommandPass 不可重复调用**——这就是为什么你新增的 `EMeshPass::MobileAfterTranslucencyPass` 需要**单独一个** `FInstanceCullingDrawParams` 成员，不能复用 BasePass 的。

---

## 5. 数据如何被消费：`DispatchDraw`

`MeshDrawCommands.cpp:1640-1652`：

```cpp
void FParallelMeshDrawCommandPass::DispatchDraw(
    FParallelCommandListSet* ParallelCommandListSet,
    FRHICommandList& RHICmdList,
    const FInstanceCullingDrawParams* InstanceCullingDrawParams) const
{
    if (MaxNumDraws <= 0) return;

    FMeshDrawCommandOverrideArgs OverrideArgs;
    if (InstanceCullingDrawParams)
    {
        OverrideArgs = GetMeshDrawCommandOverrideArgs(*InstanceCullingDrawParams);  // 把 RDG buffer 解引用成真实 RHIBuffer
    }
    ...
    // 最终触发 RHICmdList.DrawIndexedPrimitiveIndirect(VertexBuffer, OverrideArgs.IndirectArgsBuffer, OverrideArgs.IndirectArgsByteOffset)
}
```

DispatchDraw 把 `FInstanceCullingDrawParams`（RDG 句柄）转成 `FMeshDrawCommandOverrideArgs`（实际 RHI buffer），覆盖每个 `FMeshDrawCommand` 中的间接参数，最终通过 `DrawIndexedPrimitiveIndirect` 提交。

---

## 6. 四种 InstanceCullingDrawParams 的使用对照表

| Pass | 声明位置 | 填充位置 | 消费位置（DispatchDraw） | 所在 RDG Raster Pass |
|---|---|---|---|---|
| **BasePass** | `FMobileRenderPassParameters::InstanceCullingDrawParams`（嵌入式） | `BuildInstanceCullingDrawParams:1441` | `RenderMobileBasePass`（`MobileBasePassRendering.cpp:480`）传 `&PassParameters->InstanceCullingDrawParams` | 主 raster pass "SceneColorRendering" |
| **DepthPass** | `FMobileSceneRenderer::DepthPassInstanceCullingDrawParams`（成员） | `BuildInstanceCullingDrawParams:1439` 或 `RenderFullDepthPrepass`（`MobileShadingRenderer.cpp:824` 复用 PassParameters） | `RenderMaskedPrePass:853` 在主 raster pass 的 lambda 内 | 嵌入主 raster pass lambda，或独立 "FullDepthPrepass" |
| **SkyPass** | `FMobileSceneRenderer::SkyPassInstanceCullingDrawParams`（成员） | `BuildInstanceCullingDrawParams:1442` | `RenderMobileBasePass`（`MobileBasePassRendering.cpp:482`）传 `&SkyPassInstanceCullingDrawParams` | 嵌入主 raster pass lambda |
| **Translucency** | `FMobileSceneRenderer::TranslucencyInstanceCullingDrawParams`（成员） | `BuildInstanceCullingDrawParams:1443` | `RenderTranslucency`（`MobileTranslucentRendering.cpp:18`）传 `&TranslucencyInstanceCullingDrawParams` | 单 pass 路径：主 raster pass lambda；多 pass 路径：第二 raster pass lambda |
| **DebugViewMode** | `FMobileSceneRenderer::DebugViewModeInstanceCullingDrawParams`（成员） | `BuildInstanceCullingDrawParams:1444` | `MobileShadingRenderer.cpp:2106`（deferred 路径独立 lambda） | 独立 raster pass |
| **MobileAfterTranslucencyPass（新加）** | `FMobileSceneRenderer::AfterTranslucencyInstanceCullingDrawParams`（成员） | `BuildInstanceCullingDrawParams`（要补） | `RenderMobileAfterTranslucencyPass`（要补）传 `&AfterTranslucencyInstanceCullingDrawParams` | 紧跟 Translucency 之后，复用同一个 raster pass lambda |

---

## 7. 三种声明方式的本质区别总结

| 维度 | 嵌入式（BasePass） | 独立成员（其他 4 个） |
|---|---|---|
| **生命周期** | 与 `FMobileRenderPassParameters` 实例绑定（GraphBuilder 分配，帧结束时释放） | 与 `FMobileSceneRenderer` 对象绑定（整个渲染器存活期） |
| **RDG 依赖追踪** | 自动：作为 raster pass 的 PassParameters 一部分，`DrawIndirectArgsBuffer` / `InstanceIdOffsetBuffer` 的状态转换由 RDG 在 pass 开始前自动添加 | 不自动；依赖 `BuildRenderingCommands` 内部添加的 culling compute pass 的输出 access 设置（compute pass 输出本就是 `IndirectArgs` / `VertexOrIndexBuffer`，可以直接被后续 raster pass 消费） |
| **调试与验证** | RDG validator 会检查 buffer 是否处于正确 access 状态，错误会触发 check | 不进入 RDG validator 的 PassParameters 检查范围 |
| **典型使用场景** | 当前 Pass **就是** RDG raster pass 的主角 | 当前 Pass 是被"挂载"到别人 raster pass lambda 里执行的副 Pass |
| **复制语义** | `*SecondPassParameters = *PassParameters;` 会一并复制 culling 信息 | 没有这种行为；多个 lambda 访问同一份成员变量 |

---

## 8. 对你 RenderAfterTranslucency 方案的实操结论

新加的 `MobileAfterTranslucencyPass` 是"在透明 lambda 之后顺手再调用一个 DispatchDraw"，**与 Translucency 完全同构**——所以：

1. **必须**用独立成员声明：在 `SceneRendering.h:2796` 附近 `FInstanceCullingDrawParams TranslucencyInstanceCullingDrawParams;` 下方加一行 `FInstanceCullingDrawParams AfterTranslucencyInstanceCullingDrawParams;`。
2. **不能**复用 `PassParameters->InstanceCullingDrawParams`，否则 `BuildRenderingCommands` 第二次调用时会触发 `check(!bHasInstanceCullingDrawParameters)` 断言失败——同一个 `FParallelMeshDrawCommandPass` 不能填充两次，但这一点更主要的是 BasePass 与新 Pass 是**两个不同的 `FParallelMeshDrawCommandPass`**，所以即使断言不触发，**绑定的 culling buffer 也是另一回事，复用必然画错**。
3. 在 `BuildInstanceCullingDrawParams:1444` 之后补一行：
   ```cpp
   View.ParallelMeshDrawCommandPasses[EMeshPass::MobileAfterTranslucencyPass].BuildRenderingCommands(GraphBuilder, Scene->GPUScene, AfterTranslucencyInstanceCullingDrawParams);
   ```
4. 在 `RenderForwardSinglePass:1623` 和 `RenderForwardMultiPass:1735` 的 `RenderTranslucency(RHICmdList, View);` 之后调用：
   ```cpp
   RenderMobileAfterTranslucencyPass(RHICmdList, View, &AfterTranslucencyInstanceCullingDrawParams);
   ```
   或者把 `RenderMobileAfterTranslucencyPass` 写得跟 `RenderTranslucency` 一样不传参数、内部直接引用成员变量。

---

## 9. 参考索引（事实依据）

| 文件:行 | 内容 |
|---|---|
| `InstanceCullingContext.h:32-40` | `FInstanceCullingDrawParams` 结构定义（RDG shader parameter struct）|
| `MeshDrawCommands.cpp:1605-1631` | `FParallelMeshDrawCommandPass::BuildRenderingCommands` 实现 |
| `MeshDrawCommands.cpp:1640-1652` | `FParallelMeshDrawCommandPass::DispatchDraw` 实现 |
| `SceneRendering.h:2791-2796` | 四个独立 `FInstanceCullingDrawParams` 成员声明 + Epic 注释 |
| `MobileShadingRenderer.cpp:208-221` | `FMobileRenderPassParameters` 定义（含嵌入式 `InstanceCullingDrawParams`）|
| `MobileShadingRenderer.cpp:824-836` | Full Depth Prepass 路径独立 raster pass（DepthPass 复用 PassParameters）|
| `MobileShadingRenderer.cpp:1433-1446` | `BuildInstanceCullingDrawParams` 本体 |
| `MobileShadingRenderer.cpp:1564 / 1934` | `BuildInstanceCullingDrawParams` 的调用点 |
| `MobileShadingRenderer.cpp:1578-1653` | `RenderForwardSinglePass`（包含所有 DispatchDraw 调用上下文）|
| `MobileShadingRenderer.cpp:1662-1746` | `RenderForwardMultiPass`（透明在第二 raster pass）|
| `MobileShadingRenderer.cpp:1708-1716` | `*SecondPassParameters = *PassParameters;`（嵌入式的复制语义）|
| `MobileShadingRenderer.cpp:2106` | DebugViewMode 在 deferred 路径独立 lambda 内 DispatchDraw |
| `MobileBasePassRendering.cpp:470-491` | `RenderMobileBasePass` 实现（含 BasePass + SkyPass 的 DispatchDraw）|
| `MobileTranslucentRendering.cpp:7-20` | `RenderTranslucency` 实现（用 `TranslucencyInstanceCullingDrawParams`）|

---

*— 本文档作为 `Plan_Analysis_CC_GLM_6_22.md` 的补充说明，专门解释 BasePass 与其它 Pass 的 InstanceCullingDrawParams 声明差异。*
