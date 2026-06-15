# `FMobileBasePassMeshProcessor::AddMeshBatch` 调用链梳理

> 适用源码:`Source/Runtime/Renderer/Private/MobileBasePass.cpp`
> 整理日期:2026-06-13

---

## 一、函数本体

**定义位置**:`Source/Runtime/Renderer/Private/MobileBasePass.cpp:867`

```cpp
void FMobileBasePassMeshProcessor::AddMeshBatch(
    const FMeshBatch& MeshBatch, uint64 BatchElementMask,
    const FPrimitiveSceneProxy* PrimitiveSceneProxy, int32 StaticMeshId)
```

它是 `FMeshPassProcessor::AddMeshBatch`(声明在 `MeshPassProcessor.h:2068`)的虚函数 `override final`(声明在 `MobileBasePassRendering.h:489`)。函数内部会循环遍历 `MaterialRenderProxy` 的 fallback 链,依次调用私有的 `TryAddMeshBatch → Process → BuildMeshDrawCommands`,最终把 `FMeshDrawCommand` 写入 DrawListContext。

---

## 二、Processor 是如何被注册并实例化的

### 1. 注册(静态阶段,模块加载时)

`MobileBasePass.cpp:1218-1222` 用 `REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR` 宏把多个 Create 函数注册进全局跳转表:

| Pass 类型 | Create 函数 | Pass 枚举 |
|---|---|---|
| MobileBasePass | `CreateMobileBasePassProcessor` | `EMeshPass::BasePass` |
| MobileBasePassCSM | `CreateMobileBasePassCSMProcessor` | `EMeshPass::MobileBasePassCSM` |
| MobileTranslucencyAllPass | `CreateMobileTranslucencyAllPassProcessor` | `EMeshPass::TranslucencyAll` |
| MobileTranslucencyStandardPass | `CreateMobileTranslucencyStandardPassProcessor` | `EMeshPass::TranslucencyStandard` |
| MobileTranslucencyAfterDOFPass | `CreateMobileTranslucencyAfterDOFProcessor` | `EMeshPass::TranslucencyAfterDOF` |

### 2. 宏展开 / 跳转表

- 宏定义:`Source/Runtime/Renderer/Public/MeshPassProcessor.h:2265`
- 静态注册类 `FRegisterPassProcessorCreateFunction`:`MeshPassProcessor.h:2239`,构造函数把函数指针写进 `FPassProcessorManager::JumpTable[ShadingPath][PassType]`
- 跳转表所在类 `FPassProcessorManager`:`MeshPassProcessor.h:2190`
- 运行时取出用 `FPassProcessorManager::CreateMeshPassProcessor(EShadingPath, EMeshPass, ...)`

---

## 三、`AddMeshBatch` 的直接调用方

`AddMeshBatch` 是通过 `FMeshPassProcessor*` 基类指针虚函数调用进入的。Mobile BasePass 进入此函数的路径**全部在 `Source/Runtime/Renderer/Private/MeshDrawCommands.cpp` 里**:

### A. 移动端 BasePass 专用路径(关键)

**`GenerateMobileBasePassDynamicMeshDrawCommands`**(`MeshDrawCommands.cpp:674`)

这个函数为移动端 BasePass 单独写了一份。它持有两个 Processor:`PassMeshProcessor`(非 CSM)和 `MobilePassCSMPassMeshProcessor`(CSM),根据 `View.MobileCSMVisibilityInfo` 决定走哪一个:

- `:702-703` 给两个 Processor 都 `SetDrawListContext(&DynamicPassMeshDrawListContext)`
- `:724 / :728` —— **动态网格分支**:CSM 可见用 CSM Processor,否则用普通 Processor
- `:764 / :769` —— **静态网格 batch 分支**:同样按 CSM 可见性分流

### B. 通用路径(其它 mesh pass 走这里)

**`GenerateDynamicMeshDrawCommands`**(`MeshDrawCommands.cpp:581`)
- `:621` 动态 mesh elements
- `:644 / :648` 静态 mesh batch

注:移动端的 BasePass/MobileBasePassCSM 走 A,Translucency 三个 Pass 走 B。

### C. 任务包装层

上面两个 Generate 函数都从 **`FMeshDrawCommandPassSetupTask::AnyThreadTask`**(`MeshDrawCommands.cpp:1006`)分发:
- `:1019-1044` BasePass 分支(`MergeMobileBasePassMeshDrawCommands` 合并 cached static commands + `GenerateMobileBasePassDynamicMeshDrawCommands`)
- `:1046-1063` 其它 pass 分支(`GenerateDynamicMeshDrawCommands`)

调度入口是 **`FParallelMeshDrawCommandPass::DispatchPassSetup`**(`MeshDrawCommands.cpp:1334`),它把 `MeshPassProcessor` / `MobileBasePassCSMMeshPassProcessor` 存入 TaskContext(`:1358-1359`),然后并行启动 SetupTask(`:1440-1441`)或内联执行(`:1447-1453`)。

---

## 四、源头追溯:`FMobileSceneRenderer::Render` → `AddMeshBatch`

### Step 1. Render 入口
`Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp:910`
`void FMobileSceneRenderer::Render(FRDGBuilder& GraphBuilder)`

### Step 2. InitViews
`MobileShadingRenderer.cpp:433` `FMobileSceneRenderer::InitViews(...)`

在 `:726` 调用:
```cpp
SetupMobileBasePassAfterShadowInit(BasePassDepthStencilAccess,
    TaskDatas.VisibilityTaskData->GetViewCommandsPerView(), InstanceCullingManager);
```

### Step 3. SetupMobileBasePassAfterShadowInit
`MobileShadingRenderer.cpp:377`

- `:388` 创建 BasePass Processor:`FPassProcessorManager::CreateMeshPassProcessor(EShadingPath::Mobile, EMeshPass::BasePass, ...)` —— **会通过跳转表落到 `CreateMobileBasePassProcessor`,然后 `new FMobileBasePassMeshProcessor(EMeshPass::BasePass, ...)`(MobileBasePass.cpp:1162)**
- `:390` 同样手法创建 CSM Processor —— 落到 `CreateMobileBasePassCSMProcessor`(`MobileBasePass.cpp:1181`)
- `:403` 取得 `FParallelMeshDrawCommandPass& Pass = View.ParallelMeshDrawCommandPasses[EMeshPass::BasePass];`
- `:410-425` 调用 `Pass.DispatchPassSetup(..., MeshPassProcessor, ..., BasePassCSMMeshPassProcessor, ...)`

### Step 4-6:并行任务、动态命令生成、虚函数调用

进入第三节描述的 C → A 路径,最终走到 `FMobileBasePassMeshProcessor::AddMeshBatch`。

**(对于 Translucency 三个 pass)**:它们走 `FSceneRenderer::SetupMeshPass`(`SceneRendering.cpp:4196`),在 `:4233` 用同一个 `FPassProcessorManager::CreateMeshPassProcessor` 通用循环创建,然后 `:4257` 调 `DispatchPassSetup`,再进入 `GenerateDynamicMeshDrawCommands`。

---

## 五、整体链路图

```
FMobileSceneRenderer::Render                        MobileShadingRenderer.cpp:910
  └─ InitViews                                       MobileShadingRenderer.cpp:433
       └─ SetupMobileBasePassAfterShadowInit         MobileShadingRenderer.cpp:377
            ├─ FPassProcessorManager::CreateMeshPassProcessor
            │     ↓ (跳转表 JumpTable[Mobile][BasePass])
            │     CreateMobileBasePassProcessor      MobileBasePass.cpp:1151
            │         └─ new FMobileBasePassMeshProcessor(EMeshPass::BasePass, ...) 
            │                                        MobileBasePass.cpp:1162
            │     CreateMobileBasePassCSMProcessor   MobileBasePass.cpp:1165
            │         └─ new FMobileBasePassMeshProcessor(EMeshPass::MobileBasePassCSM, ...) 
            │                                        MobileBasePass.cpp:1181
            └─ FParallelMeshDrawCommandPass::DispatchPassSetup
                                                     MeshDrawCommands.cpp:1334
                 └─ FMeshDrawCommandPassSetupTask::AnyThreadTask (并行任务)
                                                     MeshDrawCommands.cpp:1006
                      └─ GenerateMobileBasePassDynamicMeshDrawCommands
                                                     MeshDrawCommands.cpp:674
                           ├─ FDynamicPassMeshDrawListContext 构造        :696
                           ├─ Processor->SetDrawListContext()             :702-703
                           ├─ MobilePassCSMPassMeshProcessor->AddMeshBatch :724 / :764
                           └─ PassMeshProcessor->AddMeshBatch             :728 / :769
                                └──→ ★ FMobileBasePassMeshProcessor::AddMeshBatch
                                                     MobileBasePass.cpp:867
                                     └─ TryAddMeshBatch (:882)
                                          └─ Process (:892)
                                               └─ BuildMeshDrawCommands → FMeshDrawCommand
```

---

## 六、关键认知点

1. **两条创建分流**:移动端 BasePass / MobileBasePassCSM 在 `SetupMobileBasePassAfterShadowInit` 里**手动创建**;Translucency 三个 pass 走 `FSceneRenderer::SetupMeshPass` 通用循环。两者都最终落到同一个 `FPassProcessorManager` 跳转表。

2. **CSM 双 Processor**:移动端 BasePass 比较特殊——会同时构造 `BasePass` 和 `MobileBasePassCSM` 两个 Processor,在 `GenerateMobileBasePassDynamicMeshDrawCommands` 中按 `View.MobileCSMVisibilityInfo` 逐 primitive 二选一调用 `AddMeshBatch`,避免在每帧光照状态切换时反复重建 cached MDC。

3. **Cached vs Dynamic**:`EMeshPassFlags::CachedMeshCommands` 标志(`MobileBasePass.cpp:1218-1219`)意味着大部分静态 mesh 的 MDC 在 `FPrimitiveSceneInfo::AddToScene` 阶段就已缓存(`PrimitiveSceneInfo.cpp:485,500` 也会走 `AddMeshBatch`,但只在场景注册时跑一次);上面追溯的 Render 路径每帧处理的是**动态部分**和**需要根据 View 状态(如 CSM 可见性)重新生成的部分**。

4. **触发 Render 的上游**:`FMobileSceneRenderer::Render` 由引擎主渲染线程通过 `FRendererModule::BeginRenderingViewFamily` → `FRendererModule::RenderViewFamilies` → `FSceneRenderer::RenderViewFamily_RenderThread` 等机制驱动,顶层是每个 Tick 提交的 `FSceneViewFamily` 渲染请求。

---

## 七、关键文件速查

| 文件 | 关键内容 |
|---|---|
| `Runtime/Renderer/Private/MobileBasePass.cpp` | Processor 定义、`AddMeshBatch` 实现、5 个 Create 函数、注册宏 |
| `Runtime/Renderer/Private/MobileBasePassRendering.h` | `FMobileBasePassMeshProcessor` 类声明(`:460`)、`AddMeshBatch` 虚函数声明(`:489`) |
| `Runtime/Renderer/Public/MeshPassProcessor.h` | 基类 `FMeshPassProcessor`、`FPassProcessorManager`、`REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR` 宏 |
| `Runtime/Renderer/Private/MeshDrawCommands.cpp` | `DispatchPassSetup`、`FMeshDrawCommandPassSetupTask`、`GenerateMobileBasePassDynamicMeshDrawCommands`、`GenerateDynamicMeshDrawCommands` |
| `Runtime/Renderer/Private/MobileShadingRenderer.cpp` | 移动端 Render 入口、`InitViews`、`SetupMobileBasePassAfterShadowInit` |
| `Runtime/Renderer/Private/SceneRendering.cpp` | 通用 `FSceneRenderer::SetupMeshPass`(Translucency 等走这里) |
| `Runtime/Renderer/Private/PrimitiveSceneInfo.cpp` | 场景注册阶段缓存静态 MDC 的路径(`:485 / :500`) |
