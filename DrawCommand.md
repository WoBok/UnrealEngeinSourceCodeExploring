# FinalizeCommand 与 DispatchDraw 的关系

> 本文档回答两个问题：
> 1. `MeshPassProcessor.inl` 中的 `DrawListContext->FinalizeCommand(...)` 与 `MobileBasePassRendering.cpp` 中的 `View.ParallelMeshDrawCommandPasses[EMeshPass::BasePass].DispatchDraw(nullptr, RHICmdList, InstanceCullingDrawParams)` 是否有关系？
> 2. `DispatchDraw` 中有没有拿到 `FinalizeCommand` 产生的相关数据？
>
> 简短答案：**有强耦合关系**。`FinalizeCommand` 是写入端，`DispatchDraw` 是读取端；中间由 `FParallelMeshDrawCommandPass` 的 `TaskContext.MeshDrawCommands` 数组串联，并由 `FInstanceCullingDrawParams` 提供 GPU 端 Instance Culling 的产出。

---

## 1. 涉及的两个调用点

### 1.1 `FinalizeCommand` —— 写入端

`Engine/Source/Runtime/Renderer/Public/MeshPassProcessor.inl:202`

```cpp
FMeshDrawCommandPrimitiveIdInfo IdInfo = GetDrawCommandPrimitiveId(PrimitiveSceneInfo, BatchElement);
DrawListContext->FinalizeCommand(
    MeshBatch, BatchElementIndex, IdInfo, MeshFillMode, MeshCullMode,
    SortKey, Flags, PipelineState, &MeshProcessorShaders, MeshDrawCommand);
```

这是模板 `FMeshPassProcessor::BuildMeshDrawCommands` 内部循环的最后一步。**每个 `FMeshBatchElement` 都会调用一次**。

### 1.2 `DispatchDraw` —— 读取端

`Engine/Source/Runtime/Renderer/Private/MobileBasePassRendering.cpp:478`

```cpp
void FMobileSceneRenderer::RenderMobileBasePass(FRHICommandList& RHICmdList, const FViewInfo& View, const FInstanceCullingDrawParams* InstanceCullingDrawParams)
{
    // ...
    RHICmdList.SetViewport(...);
    View.ParallelMeshDrawCommandPasses[EMeshPass::BasePass].DispatchDraw(
        nullptr, RHICmdList, InstanceCullingDrawParams);
    // ...
}
```

实现在 `MeshDrawCommands.cpp:1640`(`FParallelMeshDrawCommandPass::DispatchDraw`)。

---

## 2. 二者之间的连接件:`FParallelMeshDrawCommandPass`

`FViewInfo` 内部为每个 `EMeshPass::Type` 持有一个 `FParallelMeshDrawCommandPass`:

```cpp
// SceneRendering.h:1362
TStaticArray<FParallelMeshDrawCommandPass, EMeshPass::Num> ParallelMeshDrawCommandPasses;
```

`FParallelMeshDrawCommandPass` 的整个生命周期分为四个阶段:

| 阶段 | 调用方 | 作用 |
|------|--------|------|
| ① AddMeshBatch | `FMobileBasePassMeshProcessor::AddMeshBatch` | 产生 `FMeshDrawCommand` 并调用 `FinalizeCommand` |
| ② DispatchPassSetup | 异步任务(`TaskContext` 填充) | 排序、合并、构建 `FVisibleMeshDrawCommand` 数组 |
| ③ BuildRenderingCommands | 渲染线程 | GPU 端 Instance Culling,产出 `FInstanceCullingDrawParams` |
| ④ **DispatchDraw** | 渲染线程(本文件要分析的) | 把上述所有数据组装成 RHI Draw Call 提交 |

**关键对象 `FParallelMeshDrawCommandPass::TaskContext`**(定义于 `MeshDrawCommands.h:81`):

```cpp
struct FDrawCommandPassSetupTaskContext
{
    FMeshCommandOneFrameArray MeshDrawCommands;   // ② 阶段填充,④ DispatchDraw 读取
    FGraphicsMinimalPipelineStatePassSet MinimalPipelineStatePassSet;
    FInstanceCullingContext InstanceCullingContext;
    bool bUseGPUScene;
    bool bDynamicInstancing;
    int32 InstanceFactor;
    int32 MaxNumDraws;
    FGraphEventRef TaskEventRef;
    // ...
};
```

`MeshDrawCommands`(类型是 `TArray<FVisibleMeshDrawCommand, SceneRenderingAllocator>`)就是连接 ① 与 ④ 的桥梁。

---

## 3. 详细数据流(以 Mobile BasePass 为例)

### 阶段 ① —— `FinalizeCommand` 把数据写入存储

调用链:

```
FMobileBasePassMeshProcessor::AddMeshBatch
  └─> TryAddMeshBatch
        └─> BuildMeshDrawCommands        (MeshPassProcessor.inl:47-205)
              ├─ 构造 SharedMeshDrawCommand(包含 shaders、vertex streams、PSO initializer)
              ├─ InitializeShaderBindings
              ├─ DrawListContext->AddCommand(SharedMeshDrawCommand, NumElements)   // line 161
              │      └─> 申请/复用 FMeshDrawCommand 槽位(在 TChunkedArray<FMeshDrawCommand> 中)
              └─ DrawListContext->FinalizeCommand(...)                            // line 202
                     └─> FCachedPassMeshDrawListContextImmediate::FinalizeCommand
                            └─> FinalizeCommandCommon (MeshPassProcessor.cpp:2053)
                                  ├─ MeshDrawCommand.SetDrawParametersAndFinalize(...)
                                  │     // 写入:IndexBuffer, FirstIndex, NumPrimitives,
                                  │     //       NumInstances, BaseVertexIndex, NumVertices
                                  │     //       或 IndirectArgs.{Buffer,Offset}
                                  │     // 锁定:PipelineId, ShaderBindings
                                  └─ 构造 CommandInfo(FCachedMeshDrawCommandInfo) 存入缓存
                                        // 内容:SortKey, FillMode, CullMode, Flags,
                                        //       CullingPayload(LodIndex + 屏幕大小)
```

经过阶段 ①,以下数据被"封存":

- **`FMeshDrawCommand` 本身**(在 `FDynamicMeshDrawCommandStorage::MeshDrawCommands` TChunkedArray 里):
  - `ShaderBindings`、`VertexStreams`、`IndexBuffer`
  - `CachedPipelineId`(`FGraphicsMinimalPipelineStateId`)
  - `FirstIndex`、`NumPrimitives`、`NumInstances`
  - `VertexParams` 或 `IndirectArgs`(由 `SetDrawParametersAndFinalize` 写入)
  - `StencilRef`、`PrimitiveIdStreamIndex`、`PrimitiveType`
- **`FVisibleMeshDrawCommand` / `FCachedMeshDrawCommandInfo`** 元数据:
  - `SortKey`、`StateBucketId`、`MeshFillMode`、`MeshCullMode`
  - `Flags`(`EFVisibleMeshDrawCommandFlags`,含 `ForceInstanceCulling`、`PreserveInstanceOrder`、`FetchInstanceCountFromScene`、`HasPrimitiveIdStreamIndex` 等)
  - `CullingPayload`(`FMeshDrawCommandCullingPayload`:LodIndex + 屏幕大小范围)
  - `PrimitiveIdInfo`(只有 dynamic 路径直接存;cached 路径间接由 PrimitiveSceneInfo 还原)
  - `RunArray` / `NumRuns`(instance run 列表)

### 阶段 ② —— Setup Task 排序合并

`DispatchPassSetup` 异步执行 `SortAndMergeDynamicPassMeshDrawCommands`,把缓存的 `FCachedMeshDrawCommandInfo` 排序并按 state bucket 合并:

- 排序键 = `SortKey` + `StateBucketId`
- 动态 instancing 合并:同一个 PSO 桶的多 instance 共享一个 `FMeshDrawCommand`,每条 `FVisibleMeshDrawCommand` 通过 `MeshDrawCommand` **指针** 引用
- 写入 `TaskContext.MeshDrawCommands`(类型为 `FMeshCommandOneFrameArray = TArray<FVisibleMeshDrawCommand>`)
- 设置 `MaxNumDraws = TaskContext.MeshDrawCommands.Num()`
- 发出 `TaskEventRef` 作为完成信号

### 阶段 ③ —— `BuildRenderingCommands` 跑 GPU Culling

调用方: `MobileShadingRenderer.cpp:885`、`1441-1444`,传入 `PassParameters->InstanceCullingDrawParams` 引用。

```cpp
PassParameters->InstanceCullingDrawParams = FInstanceCullingDrawParams();
p->BuildRenderingCommands(GraphBuilder, GPUScene, PassParameters->InstanceCullingDrawParams);
```

内部:
- 等待阶段 ② 的 `TaskEventRef`
- `TaskContext.InstanceCullingContext.BuildRenderingCommands(...)` 向 RDG 注册 compute pass
- Compute pass 读取 `TaskContext.MeshDrawCommands`,遍历 `FVisibleMeshDrawCommand`/`FCachedMeshDrawCommandInfo` 中的 `CullingPayload`、`PrimitiveIdInfo`、`RunArray`,在 GPU 上做视锥/LOD/instance culling
- 写出两个 RDG 缓冲:
  - `DrawIndirectArgsBuffer`(每个被 cull 通过的 draw 5 个 uint32: indexCountPerInstance, instanceCount, startIndex, baseVertex, startInstance)
  - `InstanceIdOffsetBuffer`(每个被 cull 通过的 instance 一个 uint32,指向 GPU scene 中 instance 数据)
- 把这些 buffer + offset 写进 `FInstanceCullingDrawParams`

`FInstanceCullingDrawParams` 定义(`InstanceCulling/InstanceCullingContext.h:32-40`):

```cpp
BEGIN_SHADER_PARAMETER_STRUCT(FInstanceCullingDrawParams, )
    RDG_BUFFER_ACCESS(DrawIndirectArgsBuffer,  ERHIAccess::IndirectArgs)
    RDG_BUFFER_ACCESS(InstanceIdOffsetBuffer,  ERHIAccess::VertexOrIndexBuffer)
    SHADER_PARAMETER(uint32, InstanceDataByteOffset)   // per-pass 偏移
    SHADER_PARAMETER(uint32, IndirectArgsByteOffset)   // per-pass 偏移
    SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FInstanceCullingGlobalUniforms, InstanceCulling)
    SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FSceneUniformParameters,        Scene)
    SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FBatchedPrimitiveParameters,   BatchedPrimitive)
END_SHADER_PARAMETER_STRUCT()
```

### 阶段 ④ —— `DispatchDraw` 读取并提交

`MeshDrawCommands.cpp:1640-1717` 简化版:

```cpp
void FParallelMeshDrawCommandPass::DispatchDraw(
    FParallelCommandListSet* ParallelCommandListSet,
    FRHICommandList& RHICmdList,
    const FInstanceCullingDrawParams* InstanceCullingDrawParams) const
{
    if (MaxNumDraws <= 0) return;

    // 1. 把 RDG 参数转成 RHI 友好的 OverrideArgs
    FMeshDrawCommandOverrideArgs OverrideArgs;
    if (InstanceCullingDrawParams)
    {
        OverrideArgs = GetMeshDrawCommandOverrideArgs(*InstanceCullingDrawParams);
    }

    if (ParallelCommandListSet)
    {
        // 1a. 并行路径:把 TaskContext.MeshDrawCommands + OverrideArgs
        //     打包成 FDrawVisibleMeshCommandsAnyThreadTask 派发到 worker thread
        //     → 每个 task 调 TaskContext.InstanceCullingContext.SubmitDrawCommands(...)
    }
    else
    {
        // 1b. 串行路径(Mobile BasePass 走的):
        WaitForMeshPassSetupTask();   // 等阶段 ② 完成

        if (TaskContext.bUseGPUScene)
        {
            TaskContext.InstanceCullingContext.SubmitDrawCommands(
                TaskContext.MeshDrawCommands,                  // ① 写入 → ② 排序合并 → ④ 读取
                TaskContext.MinimalPipelineStatePassSet,
                OverrideArgs,                                  // ③ GPU culling 产出
                0, TaskContext.MeshDrawCommands.Num(),
                TaskContext.InstanceFactor,
                RHICmdList);
        }
        else
        {
            FMeshDrawCommandSceneArgs SceneArgs;
            SubmitMeshDrawCommandsRange(TaskContext.MeshDrawCommands, ..., SceneArgs, ...);
        }
    }
}
```

`GetMeshDrawCommandOverrideArgs` 把 RDG 句柄转换为 RHI buffer:

```cpp
// InstanceCulling/InstanceCullingContext.cpp:88
FMeshDrawCommandOverrideArgs GetMeshDrawCommandOverrideArgs(const FInstanceCullingDrawParams& P)
{
    Result.InstanceBuffer       = P.InstanceIdOffsetBuffer.GetBuffer()->GetRHI();
    Result.IndirectArgsBuffer   = P.DrawIndirectArgsBuffer.GetBuffer()->GetRHI();
    Result.InstanceDataByteOffset = P.InstanceDataByteOffset;
    Result.IndirectArgsByteOffset = P.IndirectArgsByteOffset;
    return Result;
}
```

`FMeshDrawCommandOverrideArgs`(`MeshPassProcessor.h:1035-1049`):

```cpp
struct FMeshDrawCommandOverrideArgs
{
    FRHIBuffer* InstanceBuffer;        // = culling 后的 InstanceIdOffsetBuffer
    FRHIBuffer* IndirectArgsBuffer;    // = culling 后的 DrawIndirectArgsBuffer
    uint32 InstanceDataByteOffset;
    uint32 IndirectArgsByteOffset;
    // ...
};
```

`SubmitDrawCommands` 内部:遍历 `TaskContext.MeshDrawCommands` 中的每条 `FVisibleMeshDrawCommand`,取其持有的 `FMeshDrawCommand` 指针(由阶段 ① 写入),套上 `OverrideArgs`,最后调 `RHICmdList.DrawIndexedPrimitiveIndirect(IndirectArgsBuffer, IndirectArgsByteOffset)` 或 `DrawPrimitiveIndirect`。

---

## 4. 二者关系总结表

| 维度 | `FinalizeCommand` | `DispatchDraw` |
|------|-------------------|----------------|
| 文件 | `MeshPassProcessor.inl:202` | `MobileBasePassRendering.cpp:478` (声明于 `MeshDrawCommands.h`,实现在 `MeshDrawCommands.cpp:1640`) |
| 作用 | **写入** — 把 `FMeshBatch` 元素固化为 `FMeshDrawCommand` + 元数据 | **读取** — 把 `TaskContext.MeshDrawCommands` + culling 产出转为 RHI Draw Call |
| 触发时机 | `AddMeshBatch` 期间,对每个 batch 元素同步调用 | `RenderMobileBasePass` 期间,所有 pass 设置完成后才调用 |
| 直接数据 | `FMeshBatch`, `IdInfo`, `SortKey`, `Flags`, `PipelineState`, `MeshDrawCommand`(in-out) | `TaskContext.MeshDrawCommands`(`FMeshCommandOneFrameArray`), `InstanceCullingDrawParams` |
| 产出的数据 | 填充 `FMeshDrawCommand`(在 TChunkedArray 中);构造 `FCachedMeshDrawCommandInfo`(SortKey/CullingPayload/Flags/...) | 不产出新数据,只读取并提交 RHI 调用 |
| 是否使用对方产出 | — | **是,直接使用 `TaskContext.MeshDrawCommands`(其 `MeshDrawCommand` 指针就是 `FinalizeCommand` 阶段写入的 `FMeshDrawCommand` 对象)**,还使用阶段 ③ 的 `FInstanceCullingDrawParams` |

### 完整调用链一览

```
[阶段 ①] FMobileBasePassMeshProcessor::AddMeshBatch
   └─> BuildMeshDrawCommands (MeshPassProcessor.inl)
         ├─ 分配 FMeshDrawCommand 槽位
         ├─ 写入 ShaderBindings / VertexStreams / PSO initializer
         └─> DrawListContext->FinalizeCommand(...)         ← 写入端
               └─> SetDrawParametersAndFinalize(...)
               └─> 记录 FCachedMeshDrawCommandInfo
                            │
                            ▼
[阶段 ②] FParallelMeshDrawCommandPass::DispatchPassSetup  (异步)
   └─> SortAndMergeDynamicPassMeshDrawCommands
         └─> TaskContext.MeshDrawCommands (FVisibleMeshDrawCommand[])
                            │
                            ▼
[阶段 ③] FParallelMeshDrawCommandPass::BuildRenderingCommands
   └─> TaskContext.InstanceCullingContext.BuildRenderingCommands(...)
         └─> 产出 FInstanceCullingDrawParams (DrawIndirectArgsBuffer, InstanceIdOffsetBuffer, ...)
                            │
                            ▼
[阶段 ④] FParallelMeshDrawCommandPass::DispatchDraw         ← 读取端
   └─> OverrideArgs = GetMeshDrawCommandOverrideArgs(InstanceCullingDrawParams)
   └─> SubmitDrawCommands(TaskContext.MeshDrawCommands, ..., OverrideArgs, ...)
         └─> 对每条 FVisibleMeshDrawCommand:
                ├─ 取 MeshDrawCommand 指针(就是阶段 ① 写入的 FMeshDrawCommand)
                ├─ 套上 OverrideArgs(rebind InstanceBuffer / IndirectArgsBuffer)
                └─> RHICmdList.DrawIndexedPrimitiveIndirect(...) / DrawPrimitiveIndirect(...)
```

---

## 5. 直接回答你的问题

**Q1: `FinalizeCommand` 与 `DispatchDraw` 有关系吗?**

**有,它们是同一份数据(`FMeshDrawCommand` + `FVisibleMeshDrawCommand`)的生产者—消费者。** 二者并不在同一处代码直接互调,而是通过 `FParallelMeshDrawCommandPass::TaskContext.MeshDrawCommands` 这个中间结构串联。`FinalizeCommand` 在阶段 ① 把 `FMeshDrawCommand` 写进 TChunkedArray 并登记 `FCachedMeshDrawCommandInfo`;`DispatchDraw` 在阶段 ④ 读取同一批对象(`TaskContext.MeshDrawCommands` 中的 `FVisibleMeshDrawCommand::MeshDrawCommand` 指针指向阶段 ① 写入的那个 `FMeshDrawCommand`)。

**Q2: `DispatchDraw` 中有没有拿到其相关数据?**

**有,并且是完整地拿**。具体而言:

1. **直接拿到** `FinalizeCommand` 产出的 `FMeshDrawCommand` —— 通过 `TaskContext.MeshDrawCommands` 数组(由 Setup 阶段排序合并后放入),`SubmitDrawCommands`/`SubmitMeshDrawCommandsRange` 会逐个遍历这些 `FVisibleMeshDrawCommand` 并解引用其 `MeshDrawCommand` 指针,这个指针指向的就是 `FinalizeCommand` 期间被 `SetDrawParametersAndFinalize` 填充好的同一个 `FMeshDrawCommand` 对象。
   - 拿到的内容包括:已锁定的 `CachedPipelineId`、`ShaderBindings`、`VertexStreams`、`IndexBuffer`、`FirstIndex`/`NumPrimitives`/`NumInstances`、`StencilRef`、`PrimitiveIdStreamIndex`、`PrimitiveType` 等。
   - 对于 GPU Scene 路径(`bUseGPUScene=true`,Mobile 默认走这条),`SetDrawParametersAndFinalize` 写入的 `IndirectArgs` 字段会被阶段 ③ 的 GPU Culling 覆盖 — 这部分数据由 `FInstanceCullingDrawParams` 提供。
2. **间接拿到** `FinalizeCommand` 期间记录的元数据 —— `SortKey`、`Flags`(影响排序与 instance 处理)、`CullingPayload`(LodIndex + 屏幕大小,供阶段 ③ GPU culling 使用)、`StateBucketId`(排序次级键)、`MeshFillMode`/`MeshCullMode`(可见性 override 备用)。
3. **通过 `FInstanceCullingDrawParams` 拿到** GPU 端的 instance culling 产出 —— `DrawIndirectArgsBuffer`、`InstanceIdOffsetBuffer`、两个 byte offset,以及 `FInstanceCullingGlobalUniforms` / `FSceneUniformParameters` / `FBatchedPrimitiveParameters` 三个 uniform buffer。这些 buffer 在阶段 ③ 由 GPU compute pass 用阶段 ①/② 提供的 `PrimitiveIdInfo` / `CullingPayload` / `RunArray` 计算得到。

> 一句话总结:`FinalizeCommand` 决定了"画什么、怎么画",`DispatchDraw` 决定了"画到哪、用哪个 indirect buffer";`FParallelMeshDrawCommandPass` 是它们的"生产线",`FInstanceCullingDrawParams` 是 GPU culling 的"分流阀"。两边在 `TaskContext.MeshDrawCommands` 和 `OverrideArgs` 这两个点上的数据完全对齐。

---

## 6. 关键源文件定位(便于进一步阅读)

| 内容 | 路径 | 行号 |
|------|------|------|
| `FinalizeCommand` 调用 | `Engine/Source/Runtime/Renderer/Public/MeshPassProcessor.inl` | 202 |
| `BuildMeshDrawCommands` 模板主体 | `Engine/Source/Runtime/Renderer/Public/MeshPassProcessor.inl` | 47-205 |
| `SetDrawParametersAndFinalize` | `Engine/Source/Runtime/Renderer/Private/MeshPassProcessor.cpp` | 1048-1085 |
| `FinalizeCommandCommon` | `Engine/Source/Runtime/Renderer/Private/MeshPassProcessor.cpp` | 2053-2111 |
| `FCachedPassMeshDrawListContextImmediate::FinalizeCommand` | `Engine/Source/Runtime/Renderer/Private/MeshPassProcessor.cpp` | 2113-2160 |
| `DispatchDraw` 调用 | `Engine/Source/Runtime/Renderer/Private/MobileBasePassRendering.cpp` | 478 |
| `FMobileBasePassMeshProcessor` 类 | `Engine/Source/Runtime/Renderer/Private/MobileBasePassRendering.h` | 460-534 |
| `FParallelMeshDrawCommandPass` 类 | `Engine/Source/Runtime/Renderer/Private/MeshDrawCommands.h` | 115-199 |
| `DispatchDraw` 实现 | `Engine/Source/Runtime/Renderer/Private/MeshDrawCommands.cpp` | 1640-1717 |
| `GetMeshDrawCommandOverrideArgs` | `Engine/Source/Runtime/Renderer/Private/InstanceCulling/InstanceCullingContext.cpp` | 88 |
| `FInstanceCullingDrawParams` 定义 | `Engine/Source/Runtime/Renderer/Public/InstanceCulling/InstanceCullingContext.h` | 32-40 |
| `FMeshDrawCommandOverrideArgs` 定义 | `Engine/Source/Runtime/Renderer/Public/MeshPassProcessor.h` | 1035-1049 |
| `FVisibleMeshDrawCommand` 定义 | `Engine/Source/Runtime/Renderer/Public/MeshPassProcessor.h` | 1559-1621 |
| `ParallelMeshDrawCommandPasses` 字段 | `Engine/Source/Runtime/Renderer/Private/SceneRendering.h` | 1362 |
| `BuildRenderingCommands` 调用(Mobile) | `Engine/Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp` | 885, 1441-1444 |
