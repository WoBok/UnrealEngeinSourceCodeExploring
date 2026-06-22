# Plan.md 修改方案分析（UE5.4 移动端渲染：在透明物体之后绘制不透明物体）

> 评审日期：2026-06-22  
> 评审基线：本地 `Engine/Source/...` UE5.4 引擎代码  
> 目标平台：Android VR、Mobile Forward（前向渲染）

---

## 0. 总评

整体思路是正确的：**新增一个移动端 MeshPass，复用 `FMobileBasePassMeshProcessor`（不透明 BasePass 的处理器），将其插入到 `RenderTranslucency` 之后调用**，借助"BasePass 写深度，Translucency 不写深度"这个事实让后绘制的不透明物体能够覆盖前面的透明像素。这条路径在引擎里是走得通的。

但当前 Plan 里至少**5 处对不上 UE5.4 实际源码**（行号/前置代码不一致），并且漏掉了**3 处必须修改才能跑通**的位置：

| 严重等级 | 问题 | 影响 |
|---|---|---|
| 🔴 致命 | `FMobileSceneRenderer::BuildInstanceCullingDrawParams` 漏建新 Pass 的 `InstanceCullingDrawParams` | 新 Pass `DispatchDraw` 时实例数据为空 / 命令未构建，渲染不出来或崩溃 |
| 🔴 致命 | `RenderMobileAfterTranslucencyPass` 复用了 `PassParameters->InstanceCullingDrawParams`（这是 BasePass 的） | 新 Pass 拿到的是 BasePass 的实例 culling 数据，指令对应错位 |
| 🔴 致命 | `FMobileBasePassMeshProcessor::AddMeshBatch` 的早 return 顺序问题 | `bRenderInMainPass = false` 的物体会被新 Pass 漏掉 |
| 🟠 重要 | static_assert 数字写错（实际 `Num == 32`，不是 `33`） | 编译期断言失败 |
| 🟠 重要 | `Result.bRenderInMainPass = ShouldRenderInMainPass();` 这条路径存在于**9 个文件**，只改 StaticMesh / SkeletalMesh 不够 | Niagara/粒子/文本/Billboard/Model/Nanite/HeterogeneousVolume 等都不会进新 Pass |
| 🟡 中等 | `SceneVisibility.cpp` 中静态网格分支：把"after translucency 走新 Pass、其它走 BasePass"二选一，会破坏 `CustomDepth` 的写入 | CustomDepth 失效 |
| 🟡 中等 | `bDeferredShading` 仍出现在构造函数里——Mobile Deferred 路径下新 Pass 行为未验证 | 切到 Deferred 必然出问题；本项目用 Forward 可暂不管 |
| 🟢 提示 | PSO Precache 路径在 `bRenderInMainPass = false` 时 early return | 与 Plan 设计意图保持一致，无需 hash 改动 |

下面逐项展开。

---

## 1. 行号/代码事实校正（Plan 中与源码不符的地方）

### 1.1 `MeshPassProcessor.h` —— `Num == 32`，不是 `33`

Plan 里写的是 `EMeshPass::Num == 33 + 4` / `== 33`。**实际 UE5.4 当前枚举到 `WaterInfoTexturePass` 为止刚好 32 项**，新增后应为 33（非编辑器）/ 37（编辑器）：

`Engine/Source/Runtime/Renderer/Public/MeshPassProcessor.h:127-131`：
```cpp
#if WITH_EDITOR
    static_assert(EMeshPass::Num == 32 + 4, "Need to update switch(MeshPass) after changing EMeshPass");
#else
    static_assert(EMeshPass::Num == 32, "Need to update switch(MeshPass) after changing EMeshPass");
#endif
```

加新枚举值后应改为：
```cpp
#if WITH_EDITOR
    static_assert(EMeshPass::Num == 33 + 4, ...);
#else
    static_assert(EMeshPass::Num == 33, ...);
#endif
```

并且 Plan 中说"在底部添加 case"是对的，但**也必须更新这两条 static_assert**（Plan 段落里没明确写出新数字）。

> 同样，Plan 里"GUID"那段注释如果做改动也要按引擎规范换 GUID（这是 Epic 防止合并误自动解析的机制）。

---

### 1.2 `MobileBasePassRendering.h:480` —— 构造函数参数顺序的真实形态

实际位置正确（`E:\...\MobileBasePassRendering.h:460-534`），但 Plan 里贴的构造函数末尾参数 `bAfterTranslucencyBasePass = false` 没问题。**注意类是定义在 `MobileBasePassRendering.h`，实现却在 `MobileBasePass.cpp`（不是 `MobileBasePassRendering.cpp`）**。

实际定义 `MobileBasePass.cpp:810-826`：
```cpp
FMobileBasePassMeshProcessor::FMobileBasePassMeshProcessor(
    EMeshPass::Type InMeshPassType,
    ...
    EFlags InFlags,
    ETranslucencyPass::Type InTranslucencyPassType)
    : FMeshPassProcessor(InMeshPassType, ...)
    , PassDrawRenderState(InDrawRenderState)
    , TranslucencyPassType(InTranslucencyPassType)
    , Flags(InFlags)
    , bTranslucentBasePass(InTranslucencyPassType != ETranslucencyPass::TPT_MAX)
    , bDeferredShading(IsMobileDeferredShadingEnabled(...))
    , bPassUsesDeferredShading(bDeferredShading && !bTranslucentBasePass)
{
}
```

Plan 里贴的实现末尾 `, bAfterTranslucencyBasePass(IsAfterTranslucencyBasePass))` **多了一个右括号**——这是笔误，应是：
```cpp
, bAfterTranslucencyBasePass(IsAfterTranslucencyBasePass)
)
```
括号位置在外层 `)` 之前，删掉那一个多余 `)`。

---

### 1.3 `MobileBasePass.cpp:1218` —— 注册行真实位置 / 顺序

Plan 写在 `:1123` 注册 `REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR`，实际真实位置是 **`1218-1222`**（共 5 条）：

```
1218 REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR(MobileBasePass, ..., EMeshPass::BasePass, EMeshPassFlags::CachedMeshCommands | EMeshPassFlags::MainView);
1219 REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR(MobileBasePassCSM, ..., EMeshPassFlags::CachedMeshCommands | EMeshPassFlags::MainView);
1220 REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR(MobileTranslucencyAllPass, ..., EMeshPassFlags::MainView);
1221 REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR(MobileTranslucencyStandardPass, ..., EMeshPassFlags::MainView);
1222 REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR(MobileTranslucencyAfterDOFPass, ..., EMeshPassFlags::MainView);
```

Plan 里给的 flags `EMeshPassFlags::CachedMeshCommands | EMeshPassFlags::MainView` 是**对的**——和 `MobileBasePass` 一致；这是想缓存静态网格 Draw Commands 的正确选择。

---

### 1.4 `MobileBasePassRendering.cpp:492` —— `RenderMobileBasePass` 真实位置

实际 `RenderMobileBasePass` 定义在 **`MobileBasePassRendering.cpp:470`**，Plan 给的 492 行号偏了一点点。把 `RenderMobileAfterTranslucencyPass` 放到这个文件里没有问题，只是注意紧跟在 `RenderMobileBasePass` 之后。

并且 Plan 中 `SCOPE_CYCLE_COUNTER(STAT_AfterTranslucencyDrawTime)` 和 `SCOPED_GPU_STAT(RHICmdList, AfterTranslucency)` 引用的 STAT id 在引擎里**并没有定义**，需要先在某个 .cpp 顶部用 `DECLARE_CYCLE_STAT` 与 `DECLARE_GPU_STAT_NAMED` 声明，否则不能编译：

```cpp
DECLARE_CYCLE_STAT(TEXT("AfterTranslucencyDrawTime"), STAT_AfterTranslucencyDrawTime, STATGROUP_SceneRendering);
DECLARE_GPU_STAT_NAMED(AfterTranslucency, TEXT("AfterTranslucency"));
```

（参考 `BasePass` 在 `MobileBasePassRendering.cpp` 顶部的写法。）

---

### 1.5 `SceneVisibility.cpp:1564` 真实代码与 Plan 不同

Plan 中提到的位置 `:1564` 是对的，但 Plan 把"添加新 Pass 命令"写成了 **"if(bRenderAfterTranslucency) 走新 Pass，else 走 BasePass"** 的 if/else 结构：

```cpp
if(ViewRelevance.bRenderAfterTranslucency)
{
    DrawCommandPacket.AddCommandsForMesh(... EMeshPass::MobileAfterTranslucencyPass);
}
DrawCommandPacket.AddCommandsForMesh(... EMeshPass::BasePass);  // 注意这里没有 else
```

这段代码**没有 else 分支**——所以被标记的物体既会进 BasePass 又会进 AfterTranslucencyPass。但 `AddMeshBatch` 内部 Plan 的代码已经会用 `bAfterTranslucencyBasePass` 与 `ShouldRenderAfterTranslucency()` 做二次过滤剔除其中一个，所以这部分**虽然在表面上看像 bug，但功能上是对的**。

不过会带来：
1. **每个被标记物体在 SceneVisibility 时会被插入两个 Pass 的 DrawCommandPacket**，最终缓存 / 哈希查找走两次（轻微浪费，无功能问题）。
2. **未被标记物体只走 BasePass**，也是对的。
3. **建议**改成 if/else 形式更干净：
   ```cpp
   if (ViewRelevance.bRenderAfterTranslucency)
   {
       DrawCommandPacket.AddCommandsForMesh(... EMeshPass::MobileAfterTranslucencyPass);
   }
   else
   {
       DrawCommandPacket.AddCommandsForMesh(... EMeshPass::BasePass);
   }
   if (!bMobileBasePassAlwaysUsesCSM) { ... }  // CSM 始终走 BasePassCSM 逻辑
   ```
   配合 `AddMeshBatch` 中只保留必要分流即可。

⚠ **但更严重的问题**（见 §3.5）是：Plan 中**动态网格分支**也用了 if/else，**这会破坏 `bRenderCustomDepth` 物体进入 BasePass 的能力**——一个 `bRenderAfterTranslucency = true` 且 `bRenderCustomDepth = true` 的物体会不进 BasePass，但 CustomDepth 仍依赖 BasePass MarkMask？请见下文。

---

### 1.6 `PrimitiveComponent.h:407` 行号偏移、`Components/PrimitiveComponent.cpp:4457` 行号偏移

实际：
- `PrimitiveComponent.h:408` 是 `uint8 bRenderInMainPass : 1;`
- `PrimitiveComponent.h:1918` 是 `ENGINE_API void SetRenderInMainPass(bool bValue);` Plan 里写的 `:1917` 差 1 行，无大碍。

---

## 2. 三处必须修补但 Plan 漏掉的关键改动

### 2.1 🔴 致命：必须为新 Pass 调用 `BuildRenderingCommands`

`Engine/Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp:1433-1446`：

```cpp
void FMobileSceneRenderer::BuildInstanceCullingDrawParams(FRDGBuilder& GraphBuilder, FViewInfo& View, FMobileRenderPassParameters* PassParameters)
{
    if (Scene->GPUScene.IsEnabled())
    {
        if (!bIsFullDepthPrepassEnabled)
        {
            View.ParallelMeshDrawCommandPasses[EMeshPass::DepthPass].BuildRenderingCommands(GraphBuilder, Scene->GPUScene, DepthPassInstanceCullingDrawParams);
        }
        View.ParallelMeshDrawCommandPasses[EMeshPass::BasePass].BuildRenderingCommands(GraphBuilder, Scene->GPUScene, PassParameters->InstanceCullingDrawParams);
        View.ParallelMeshDrawCommandPasses[EMeshPass::SkyPass].BuildRenderingCommands(GraphBuilder, Scene->GPUScene, SkyPassInstanceCullingDrawParams);
        View.ParallelMeshDrawCommandPasses[StandardTranslucencyMeshPass].BuildRenderingCommands(GraphBuilder, Scene->GPUScene, TranslucencyInstanceCullingDrawParams);
        View.ParallelMeshDrawCommandPasses[EMeshPass::DebugViewMode].BuildRenderingCommands(GraphBuilder, Scene->GPUScene, DebugViewModeInstanceCullingDrawParams);
    }
}
```

每个 Mesh Pass 必须先 `BuildRenderingCommands` 才能 `DispatchDraw`。Plan **完全没有处理**这步，新 Pass 在调度时拿不到 InstanceCullingContext / 命令列表。

**必须添加**：
1. 在 `FMobileSceneRenderer` 类（声明在 `MobileShadingRenderer.h` 或父类 `SceneRendering.h` 的 mobile 相关位置）里添加一个新成员：`FInstanceCullingDrawParams AfterTranslucencyInstanceCullingDrawParams;`（参照 `TranslucencyInstanceCullingDrawParams` 怎么声明）。
2. 在 `BuildInstanceCullingDrawParams` 里加：
   ```cpp
   View.ParallelMeshDrawCommandPasses[EMeshPass::MobileAfterTranslucencyPass].BuildRenderingCommands(GraphBuilder, Scene->GPUScene, AfterTranslucencyInstanceCullingDrawParams);
   ```

> 说明：`FViewInfo::ParallelMeshDrawCommandPasses` 是 `TStaticArray<FParallelMeshDrawCommandPass, EMeshPass::Num>`（`SceneRendering.h:1362`），数组按 `EMeshPass::Num` 自动扩容，所以加新枚举值不会越界。`NumVisibleDynamicMeshElements[EMeshPass::Num]`（同文件 :1273）也一样。

### 2.2 🔴 致命：`RenderMobileAfterTranslucencyPass` 调用 `DispatchDraw` 时传错的 `InstanceCullingDrawParams`

Plan 中：
```cpp
RenderMobileAfterTranslucencyPass(RHICmdList, View, &PassParameters->InstanceCullingDrawParams);
```

`PassParameters->InstanceCullingDrawParams` 在 `MobileShadingRenderer.cpp:1441` 已绑定到 **BasePass** 的 commands。复用它会让新 Pass 拿到错误的命令偏移、错误的 instance 数量。`RenderTranslucency` 在 `MobileTranslucentRendering.cpp:18` 用的是**独立**变量 `TranslucencyInstanceCullingDrawParams`：

```cpp
View.ParallelMeshDrawCommandPasses[StandardTranslucencyMeshPass].DispatchDraw(nullptr, RHICmdList, &TranslucencyInstanceCullingDrawParams);
```

**必须改为传 `&AfterTranslucencyInstanceCullingDrawParams`**（即 §2.1 新增的成员），并在 `RenderForwardSinglePass` / `RenderForwardMultiPass` 中以这个变量调用：

```cpp
RenderTranslucency(RHICmdList, View);
RenderMobileAfterTranslucencyPass(RHICmdList, View, &AfterTranslucencyInstanceCullingDrawParams);
```

或者：让 `RenderMobileAfterTranslucencyPass` 内部直接引用成员，函数签名就不要传 `FInstanceCullingDrawParams*` 了，仿照 `RenderTranslucency` 的写法。

### 2.3 🔴 致命：`AddMeshBatch` 早 return 顺序

Plan 中 `AddMeshBatch` 的实现：
```cpp
void FMobileBasePassMeshProcessor::AddMeshBatch(...)
{
    //RenderAfterTranslucency Added
    bool bShouldRenderAfterTranslucency = PrimitiveSceneProxy->ShouldRenderAfterTranslucency();
    if (bAfterTranslucencyBasePass) { if (!bShouldRenderAfterTranslucency) return; }
    else                            { if ( bShouldRenderAfterTranslucency) return; }

    if (!MeshBatch.bUseForMaterial || ... || !PrimitiveSceneProxy->ShouldRenderInMainPass())
        return;
    ...
}
```

注意问题：
1. `PrimitiveSceneProxy` 在第一行直接被解引用，但下面有 `PrimitiveSceneProxy && ...` 的判断说明引擎不保证它非空——**应先判空**，否则崩溃：
   ```cpp
   const bool bShouldRenderAfterTranslucency = PrimitiveSceneProxy && PrimitiveSceneProxy->ShouldRenderAfterTranslucency();
   ```
2. 用户的"复用 BasePass"意图是：被标记物体**应当从 BasePass 移出，进 AfterTranslucency Pass**。但 Plan 把判断放在 `ShouldRenderInMainPass()` early return **之前** —— 如果用户同时勾掉 `bRenderInMainPass` 并勾上 `bRenderAfterTranslucency`，物体会**仍然进 AfterTranslucencyPass**（因为 `ShouldRenderInMainPass()` 的 early return 在后面）。

   这是好事还是坏事取决于语义。引擎里 `bRenderInMainPass` 字段的本意是"不在 BasePass 主路径渲染"，那么"在 AfterTranslucency 渲染"就是它的一种特例。**保持当前顺序是合理的**——用户可以让一个物体 `bRenderInMainPass=false` 且 `bRenderAfterTranslucency=true`，让它**只在透明后**绘制。

   但同时 `PSOPrecache` 那条路径（`CollectPSOInitializers`）会因为 `if (!PreCacheParams.bRenderInMainPass) return;` 而**不为这些物体预缓存 PSO**——这意味着第一次出现该物体的 AfterTranslucencyPass 绘制会触发 PSO 编译卡帧。

   👉 **建议方案二**：用户给 `bRenderAfterTranslucency` 做语义是"BasePass 不渲染此物体，改在 AfterTranslucency 渲染"，那么应该是：
   - SceneProxy 在 `bRenderAfterTranslucency = true` 时，让 `bRenderInMainPass` 仍然为 true（这样 PSO precache 仍正常工作），但通过 `bAfterTranslucencyBasePass` 路径分流即可。**也就是当前 Plan 的默认行为**——只要用户不要主动设 `bRenderInMainPass = false` 就行。
   - 但更稳妥的写法是在 `CollectPSOInitializers` 也加一条：`if (MeshPassType == EMeshPass::MobileAfterTranslucencyPass) {...}` 分支按需 precache。最简单：让新 Pass 的 PSO precache **沿用 BasePass** 的同一份 PSO（它们的 RenderState 完全一致），不需额外条目。

   👉 **建议方案一（最小改动）**：保持当前 Plan，但**把分流判断移到 `bRenderInMainPass` 检查之后**：
   ```cpp
   if (!MeshBatch.bUseForMaterial || ... || !PrimitiveSceneProxy->ShouldRenderInMainPass())
       return;

   const bool bShouldRenderAfterTranslucency = PrimitiveSceneProxy->ShouldRenderAfterTranslucency();
   if (bAfterTranslucencyBasePass != bShouldRenderAfterTranslucency)
       return;
   ```
   这样语义清晰："必须先满足主 Pass 条件，再做分流"。**推荐方案一**。

---

## 3. 其它需要补充或修改的地方

### 3.1 还有 7 个文件需要同步修改 `Result.bRenderInMainPass` 路径

`Plan` 中只修改了 `StaticMeshRender.cpp:2062` 和 `SkeletalMesh.cpp:7115`。但 `Result.bRenderInMainPass = ShouldRenderInMainPass();` 这条语句在以下 9 个文件中都出现了：

```
Source/Runtime/Engine/Private/StaticMeshRender.cpp           (已改)
Source/Runtime/Engine/Private/SkeletalMesh.cpp               (已改)
Source/Runtime/Engine/Private/Rendering/NaniteResources.cpp  (需改)
Source/Runtime/Engine/Private/Particles/ParticleSystemRender.cpp  (需改)
Source/Runtime/Engine/Private/Components/TextRenderComponent.cpp  (需改)
Source/Runtime/Engine/Private/Components/MaterialBillboardComponent.cpp (需改)
Source/Runtime/Engine/Private/Components/HeterogeneousVolumeComponent.cpp (需改)
Source/Runtime/Engine/Private/Components/ArrowComponent.cpp  (需改)
Source/Runtime/Engine/Private/ModelRender.cpp                (需改)
```

如果项目里**只**用静态/骨骼网格作为标记对象，可不改其它；但 BSP（ModelRender）、粒子、文本、Billboard 等需要时要补上。**建议在 `PrimitiveSceneProxy::GetViewRelevance` 的基类逻辑里把 `Result.bRenderAfterTranslucency = ShouldRenderAfterTranslucency();` 写一遍就够**——但 UE 是 SceneProxy 各自实现 `GetViewRelevance`，所以必须按上面文件清单逐个改。

---

### 3.2 SceneVisibility 动态网格分支：CustomDepth 会失效

Plan 中动态网格分支：
```cpp
if (ViewRelevance.bRenderInMainPass || ViewRelevance.bRenderCustomDepth)
{
    if (ViewRelevance.bRenderAfterTranslucency)
    {
        PassMask.Set(EMeshPass::MobileAfterTranslucencyPass);
        View.NumVisibleDynamicMeshElements[EMeshPass::MobileAfterTranslucencyPass] += NumElements;
    }
    else
    {
        PassMask.Set(EMeshPass::BasePass);
        View.NumVisibleDynamicMeshElements[EMeshPass::BasePass] += NumElements;
    }
    ...
}
```

实际位置：`SceneVisibility.cpp:2211-2214`。

问题：紧跟 `BasePass` 后面是：
- `MobileBasePassCSM`（2228-2232）
- `CustomDepth`（2234-2238）

被标记物体仅进 `MobileAfterTranslucencyPass`，**不进 BasePass，也意味着 `MobileBasePassCSM` 不会被加**——若该物体接收阴影，会渲染不正确。`CustomDepth` 自身是独立分支，没问题。

修复方式之一：让被标记物体**同时进**两个 Pass，借由 `AddMeshBatch` 内的分流过滤一个：
```cpp
if (ViewRelevance.bRenderInMainPass || ViewRelevance.bRenderCustomDepth)
{
    PassMask.Set(EMeshPass::BasePass);
    View.NumVisibleDynamicMeshElements[EMeshPass::BasePass] += NumElements;

    if (ViewRelevance.bRenderAfterTranslucency)
    {
        PassMask.Set(EMeshPass::MobileAfterTranslucencyPass);
        View.NumVisibleDynamicMeshElements[EMeshPass::MobileAfterTranslucencyPass] += NumElements;
    }
    ...
}
```
和 `AddMeshBatch` 中的分流保持一致。**静态网格分支也按这个原则统一**（即 §1.5 提到的不要变成 if/else）。

> 备注：CSM 路径在 mobile 上是否对你 VR 项目有用要看 `r.Mobile.AllowDistanceFieldShadows` 和阴影模式。如果项目里不接收 CSM，无所谓。

---

### 3.3 PSO Precache 与新 Pass 的关系

`Engine/Source/Runtime/Engine/Private/Components/PrimitiveComponent.cpp:4620-4647`：
```cpp
void UPrimitiveComponent::SetupPrecachePSOParams(FPSOPrecacheParams& Params)
{
    Params.bRenderInMainPass = bRenderInMainPass;
    Params.bRenderInDepthPass = bRenderInDepthPass;
    Params.bStaticLighting = HasStaticLighting();
    Params.bAffectDynamicIndirectLighting = bAffectDynamicIndirectLighting;
    Params.bCastShadow = CastShadow;
    Params.bRenderCustomDepth = bRenderCustomDepth;
    Params.bCastShadowAsTwoSided = bCastShadowAsTwoSided;
    Params.SetMobility(Mobility);
    Params.SetStencilWriteMask(...);
    ...
}
```

`FPSOPrecacheParams` 内部是 `union { struct{...}; uint64 Data; }`，hash/operator== 都基于 `Data`（`PSOPrecache.h:51-64`）。**这里不需要为 `bRenderAfterTranslucency` 添加新字段到 `FPSOPrecacheParams`**，理由如下：

- 你的新 Pass 直接复用了 BasePass 的 `FMobileBasePassMeshProcessor`、复用了同样的 BlendState/DepthStencilState，**生成的 PSO 跟 BasePass 完全相同**。所以原 BasePass 的 precache 结果就能复用。
- 由于注册时给了 `EMeshPassFlags::CachedMeshCommands | EMeshPassFlags::MainView`，新 Pass 会**自动**被 PSO Precache 调度（`REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR` 宏会让它走 `CollectPSOInitializers`）。
- `FMobileBasePassMeshProcessor::CollectPSOInitializers`（`MobileBasePass.cpp:1056`）会被新 Pass 的 PSO 收集器再次调用一次——**等价于 BasePass 预缓存了两遍同一组 PSO**，浪费极小，但**功能正确**。

**结论**：PSO 这一块**不必动**，只要 `bRenderInMainPass` 保持 true（默认就是 true），precache 即可命中。Plan 中的两个 PSO 问题答案：
> **问题 1：改动是否影响 PSO？**  
> 不影响。只要不主动把 `bRenderInMainPass` 设为 false，PSO 仍会被 BasePass 路径预缓存且能被新 Pass 复用。

如果想"洁癖"地避免重复 precache（一组 PSO 被收集两次），可以在 `FMobileBasePassMeshProcessor::CollectPSOInitializers` 顶部加一条 early return：
```cpp
if (MeshPassType == EMeshPass::MobileAfterTranslucencyPass)
{
    return; // reuse BasePass PSOs, no need to collect again
}
```
非必需。

---

### 3.4 `bStaticRelevance` / `bDynamicRelevance` / `bRenderInDepthPass` 是否需要改？

`Engine/Source/Runtime/Engine/Public/PrimitiveViewRelevance.h:40-52`：

```cpp
/** The primitive's static elements are rendered for the view. */
uint32 bStaticRelevance : 1;
/** The primitive's dynamic elements are rendered for the view. */
uint32 bDynamicRelevance : 1;
...
/** The primitive should render to the depth prepass even if it's not rendered in the main pass. */
uint32 bRenderInDepthPass : 1;
```

- **`bStaticRelevance` / `bDynamicRelevance`**：分别由各 SceneProxy 在 `GetViewRelevance` 中按自己的网格类型自动设置（静态网格通常 `bStaticRelevance = true`，骨骼网格通常 `bDynamicRelevance = true`，粒子 always dynamic）。**不需要修改**。
- **`bRenderInDepthPass`**：是"即便 bRenderInMainPass=false 也要渲染到 depth prepass"。对于你的方案：
  - 如果让物体先在 BasePass 渲染，再在 AfterTranslucency 渲染——它已经在 BasePass 写过深度，这一项无所谓。
  - 如果让物体**不进 BasePass、只进 AfterTranslucency**（即 `bRenderInMainPass = false` 的特殊用法），它的深度信息会丢失，无法被 `RenderMaskedPrePass`、CSM Shadow 等用到。

**结论**：你的需求是"在透明物体之后再画一遍同样的不透明物体"，**不应该**把 `bRenderInMainPass = false`。`bRenderInDepthPass` 与 `bStatic/DynamicRelevance` 都**保留默认行为即可**，无需改动。

Plan 问题答案：
> **问题 2：bStaticRelevance、bDynamicRelevance、bRenderInDepthPass 需要修改吗？**  
> **不需要**。它们由各 SceneProxy 自行决定，与新增 Pass 正交。

---

### 3.5 Mobile Deferred 路径

构造函数中：
```cpp
, bDeferredShading(IsMobileDeferredShadingEnabled(GetFeatureLevelShaderPlatform(ERHIFeatureLevel::ES3_1)))
, bPassUsesDeferredShading(bDeferredShading && !bTranslucentBasePass)
```

新 Pass 的 `bTranslucentBasePass = false`（因为 `InTranslucencyPassType = TPT_MAX`），故 `bPassUsesDeferredShading = bDeferredShading`。

在 Mobile Deferred 路径下（`RenderDeferredSinglePass` / `RenderDeferredMultiPass`），透明物体绘制在 Deferred Shading 之后；此时 RenderTarget 已不再是 GBuffer，只有 SceneColor。**若强行让新 Pass 写 GBuffer，渲染目标不匹配，会出错**。

**项目当前是 Mobile Forward（你说的），所以这部分先不管**。但如果将来切换到 Mobile Deferred，需要新写一个 Deferred 版本的工厂函数 + RenderState，或者在 Forward 才注册 / 启用新 Pass。

**建议**：在 `CreateMobileAfterTranslucencyPassProcessor` 顶部加一条早返回：
```cpp
if (IsMobileDeferredShadingEnabled(GetFeatureLevelShaderPlatform(...)))
{
    return nullptr; // not supported in mobile deferred yet
}
```
（如果 framework 允许返回 nullptr 不注册 processor。否则做一个空 processor。）

---

### 3.6 RenderTargets / DepthStencil 状态：是否能正确"遮挡透明物体"

核心成立性验证（**这是你方案的核心**）：

1. `MobileShadingRenderer.cpp:1700-1716`（`RenderForwardMultiPass` 的第二阶段）显示透明物体绘制时：
   ```cpp
   SecondPassParameters->RenderTargets.DepthStencil.SetDepthLoadAction(ERenderTargetLoadAction::ELoad);
   SecondPassParameters->RenderTargets.DepthStencil.SetDepthStencilAccess(ExclusiveDepthStencil);  // DepthRead_StencilRead
   ```
   —— 透明阶段以**只读**方式 Load 之前 BasePass 写的深度。`MobileTranslucentRendering.cpp` 中的 `RenderTranslucency` 也是用 `DepthRead_StencilRead`。

2. 你的新 Pass 用 `TStaticDepthStencilState<true, CF_DepthNearOrEqual>::GetRHI()`（**写深度**），同时 `DepthStencilAccess = DefaultBasePassDepthStencilAccess`（通常 `DepthWrite_StencilWrite`）。

⚠ **冲突点**：
- **单 Pass（`RenderForwardSinglePass`）路径**：所有绘制都在同一个 `GraphBuilder.AddPass`、同一个 `ERDGPassFlags::Raster` 范围内、同一组 `RenderTargets`。Pass 的 DepthStencilAccess 在 `InitRenderTargetBindings_Forward` 里设置（`MobileShadingRenderer.cpp:1494-1496`）为 `DepthWrite_StencilWrite`。所以单 Pass 路径下深度读写**没问题**。
- **多 Pass（`RenderForwardMultiPass`）路径**：透明物体绘制是单独的 `AddPass`，DepthStencilAccess 是 `DepthRead_StencilRead`（`MobileShadingRenderer.cpp:1716`），**整个 SecondPass 不允许深度写入**。你的新 Pass 想"沿用 BasePass 的深度写入"行不通，**RHI 会拒绝**。

✅ **修复方案**：
- 方案 A：把 `RenderMobileAfterTranslucencyPass` 放在 SecondPass 内，但**改成 `DepthRead_StencilRead`、深度状态为 `<false, CF_DepthNearOrEqual>`（不写深度，只测试）**。
  - 后果：新画的物体能遮挡透明物体（深度 ≤ 当前深度则通过），但**不更新深度**——后续绘制（如 Fog）若依赖最新深度，结果稍有偏差。VR 项目此时已没什么"后续"了，可接受。
- 方案 B：在 `RenderForwardMultiPass` 中**额外** `AddPass`，参数复制自 `SecondPassParameters` 但 DepthStencilAccess 改成 `DepthWrite_StencilWrite`。代价：多一个 RDG Pass、可能多一次 LoadStore。
- 方案 C：把 `bDepthStencilWrite` 改为 `false`，DepthStencilState 用 `<false, CF_DepthNearOrEqual>`。**这是最稳妥的**，不会破坏现有 RT 状态机。

`RenderForwardSinglePass` 路径下方案 C 也可以工作，因为 RenderTarget 是 `DepthRead_StencilWrite` (取决于 subpass，但深度子 pass hint 是 `DepthReadSubpass`，所以是只读)——同样不能写。**所以方案 C 是统一的、跨路径正确的**。

**结论**：**Plan 中"沿用 BasePass 的 DepthStencilState (true, CF_DepthNearOrEqual)"是错的**，必须改成：
```cpp
PassDrawRenderState.SetDepthStencilState(TStaticDepthStencilState<false, CF_DepthNearOrEqual>::GetRHI());
PassDrawRenderState.SetDepthStencilAccess(FExclusiveDepthStencil::DepthRead_StencilRead);
```
（仿照 `CreateMobileTranslucencyAfterDOFProcessor`，`MobileBasePass.cpp:1196-1205`。这正是它能在透明阶段绘制的原因。）

---

### 3.7 `bUseSkyMaterial` / `bUseSingleLayerWaterMaterial` / `MeshDecal` 等特殊材质

`SceneVisibility.cpp:1561-1573`：
```cpp
if (!StaticMeshRelevance.bUseSkyMaterial)
{
    DrawCommandPacket.AddCommandsForMesh(... EMeshPass::BasePass);
    if (!bMobileBasePassAlwaysUsesCSM) { ... CSM ... }
}
else
{
    DrawCommandPacket.AddCommandsForMesh(... EMeshPass::SkyPass);
}
```

天空盒材质走 `SkyPass`，不应进入 AfterTranslucency。若用户错误地把天空盒标记成 `bRenderAfterTranslucency`，应当在 `AddMeshBatch` 里通过 `ShouldDraw()` 把它过滤掉（`ShouldDraw` 已经基于 BlendMode 过滤；天空材质的 BlendMode 通常是 Opaque，所以会被尝试加入但 SkyPass 不走这条路）。**保持默认即可，无须改动**。

`SingleLayerWater` 在 mobile 上是直接走 BasePass 处理（`SceneVisibility.cpp:1574` 注释）。同理，标记水材质会进 AfterTranslucency，但水材质本身在 `ShouldDraw` 里被视为透明（`HasShadingModel(MSM_SingleLayerWater)`），会被过滤。**无须特别处理**。

`MeshDecal` 是另一条专门的 EMeshPass，与 BasePass 解耦，**互不影响**。

---

### 3.8 静态网格缓存（`CacheMeshDrawCommands`）

`PrimitiveSceneInfo::CacheMeshDrawCommands` 按 `EMeshPass::Num` 索引缓存，新枚举值会自动包含。注册时 `EMeshPassFlags::CachedMeshCommands` 是关键——必须包含，否则每帧重新构建命令（性能差）。Plan 中已包含。✅

---

### 3.9 `PrimitiveSceneProxyDesc.h` 的 bit field

Plan 中：
```cpp
uint32 bRenderInMainPass : 1;
uint32 bRenderAfterTranslucency : 1;
```

实际 `PrimitiveSceneProxyDesc.h:93` 的字段就是 `uint32 bRenderInMainPass : 1;`，Plan 写的对。`:25` 处的默认值赋值也正确。

---

## 4. 推荐的最终修改清单

把以上分析合并，**最小、安全、能 work** 的最终改动如下（与 Plan 的差异）：

### 4.1 修正 Plan 已有内容

| Plan 中 | 修正为 |
|---|---|
| `MeshPassProcessor.h` static_assert 数字 `33 + 4` / `33` | `33 + 4` / `33`（**新 Num 值**，即原 `32`+1） |
| `MobileBasePass.cpp` 构造函数末尾 `, bAfterTranslucencyBasePass(IsAfterTranslucencyBasePass))` | 删多余的右括号：`, bAfterTranslucencyBasePass(IsAfterTranslucencyBasePass)` |
| `CreateMobileAfterTranslucencyPassProcessor` 的 DepthStencilState 用 `<true, CF_DepthNearOrEqual>` 和 `DefaultBasePassDepthStencilAccess` | **改为** `<false, CF_DepthNearOrEqual>` 和 `FExclusiveDepthStencil::DepthRead_StencilRead`（§3.6）|
| `AddMeshBatch` 中 `PrimitiveSceneProxy->ShouldRenderAfterTranslucency()` 直接解引用 | 先 `PrimitiveSceneProxy &&` 判空（§2.3）|
| `AddMeshBatch` 中分流判断放在最前面 | **建议**移到 `ShouldRenderInMainPass()` 早返回之后（§2.3 方案一）|
| `SceneVisibility.cpp` 动态网格里 if/else 二选一 | 改成"标记物体同时进 BasePass 和 AfterTranslucencyPass"（§3.2）|
| `SceneVisibility.cpp` 静态网格里**保留 BasePass**，只**额外**加 AfterTranslucencyPass | 已是 Plan 写法（§1.5），无需改动 |

### 4.2 Plan 漏掉、必须新增的改动

1. **`MobileShadingRenderer.cpp` BuildInstanceCullingDrawParams** —— 加 `BuildRenderingCommands` 调用，配套一个新的 `FInstanceCullingDrawParams AfterTranslucencyInstanceCullingDrawParams;` 成员（§2.1）。
2. **`RenderForwardSinglePass` / `RenderForwardMultiPass`** —— 用新成员而不是 `&PassParameters->InstanceCullingDrawParams`（§2.2）。
3. **STAT 声明** —— `DECLARE_CYCLE_STAT` + `DECLARE_GPU_STAT_NAMED`，否则编不过（§1.4）。
4. **如果项目用了 Niagara / 粒子 / Text / Billboard / BSP / Nanite / HeterogeneousVolume**，对应文件里也加 `Result.bRenderAfterTranslucency = ShouldRenderAfterTranslucency();`（§3.1）。

### 4.3 可选优化

- `CollectPSOInitializers` 顶部对 `MeshPassType == EMeshPass::MobileAfterTranslucencyPass` 做 early return，避免重复 precache 同一份 PSO（§3.3）。
- 若计划支持 Mobile Deferred，要为新 Pass 写独立工厂或在 `CreateMobileAfterTranslucencyPassProcessor` 中检测 deferred shading 决定是否启用（§3.5）。

---

## 5. 用户问题直接回答

> **Q1：改动是否会影响 PSO？`SetupPrecachePSOParams` 这一块需要修改吗？**

不影响、不需要改。新 Pass 复用了 `FMobileBasePassMeshProcessor` 与完全一致的 RenderState，生成的 PSO Initializer 与 BasePass 完全相同，BasePass 已经预缓存的 PSO 在新 Pass 中直接命中。`FPSOPrecacheParams::bRenderInMainPass` 字段不需要扩展，`SetupPrecachePSOParams` 不需要新增赋值。

唯一注意：**不要把目标物体的 `bRenderInMainPass` 设为 false**，否则 `CollectPSOInitializers` 顶部的 `if (!PreCacheParams.bRenderInMainPass) return;` 会让物体跳过 precache，运行时出现 PSO 编译卡帧。

> **Q2：`bStaticRelevance`、`bDynamicRelevance`、`bRenderInDepthPass` 需要修改吗？**

都**不需要**修改。
- `bStaticRelevance` / `bDynamicRelevance` 由 SceneProxy 根据自身网格类型自动设置，与 MeshPass 类型解耦。
- `bRenderInDepthPass` 的含义是"`bRenderInMainPass=false` 时仍写深度"——你的新 Pass 物体仍会进 BasePass，深度已经写入，所以保持默认即可。

---

## 6. 修改步骤验证 Checklist（建议落地时挨条对照）

- [ ] `MeshPassProcessor.h`：枚举加一项 + `GetMeshPassName` 加 case + static_assert 数字改为 `33 + 4` / `33`
- [ ] `PrimitiveComponent.h/.cpp`：新增 `bRenderAfterTranslucency` 字段、Setter、构造初始化
- [ ] `PrimitiveSceneProxy.h/.cpp`：新增 `bRenderAfterTranslucency`、`ShouldRenderAfterTranslucency()`、两个构造函数的初始化
- [ ] `PrimitiveSceneProxyDesc.h`：新增 `bRenderAfterTranslucency` 字段及默认值
- [ ] `PrimitiveViewRelevance.h`：新增 `bRenderAfterTranslucency` 字段及默认值
- [ ] `StaticMeshRender.cpp:2062`：`Result.bRenderAfterTranslucency = ShouldRenderAfterTranslucency();`
- [ ] `SkeletalMesh.cpp:7115`：同上
- [ ] **其余 7 个 SceneProxy 文件**（按需）：同上（§3.1）
- [ ] `MobileBasePassRendering.h:460-534`：构造函数加 `bAfterTranslucencyBasePass` 参数与成员
- [ ] `MobileBasePass.cpp:810`：构造函数实现（**修正多余括号**）
- [ ] `MobileBasePass.cpp:867`：`AddMeshBatch` 加分流（**先判空、放在 `bRenderInMainPass` 检查之后**）
- [ ] `MobileBasePass.cpp` 末尾：新增 `CreateMobileAfterTranslucencyPassProcessor`（**改用 DepthRead_StencilRead 与 `<false, CF_DepthNearOrEqual>`**）
- [ ] `MobileBasePass.cpp:1218`：新增 `REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR`
- [ ] `MobileBasePassRendering.cpp:470`：新增 `RenderMobileAfterTranslucencyPass`（**用新的 `AfterTranslucencyInstanceCullingDrawParams`** 而不是 `PassParameters->InstanceCullingDrawParams`）；**声明 STAT**
- [ ] `MobileShadingRenderer.h`（或 `FMobileSceneRenderer` 头）：新增 `FInstanceCullingDrawParams AfterTranslucencyInstanceCullingDrawParams;` 成员
- [ ] `MobileShadingRenderer.cpp:1433` `BuildInstanceCullingDrawParams`：新增 `BuildRenderingCommands` 调用
- [ ] `MobileShadingRenderer.cpp:1623` / `:1735`：`RenderTranslucency` 之后调用 `RenderMobileAfterTranslucencyPass`
- [ ] `SceneVisibility.cpp:1556` 静态网格：在 BasePass 后**额外**添加 AfterTranslucencyPass 命令
- [ ] `SceneVisibility.cpp:2211` 动态网格：**在 BasePass 旁边并列**加 AfterTranslucencyPass（不是 if/else 替换）

---

## 7. 参考索引（事实依据，便于复核）

| 文件:行 | 内容 |
|---|---|
| `MeshPassProcessor.h:34-78` | `EMeshPass::Type` 枚举（共 32 项 + 4 编辑器项）|
| `MeshPassProcessor.h:127-131` | static_assert（`Num == 32` / `Num == 32 + 4`）|
| `SceneRendering.h:1273` | `NumVisibleDynamicMeshElements[EMeshPass::Num]` |
| `SceneRendering.h:1362` | `TStaticArray<FParallelMeshDrawCommandPass, EMeshPass::Num> ParallelMeshDrawCommandPasses` |
| `PrimitiveSceneProxy.h:700-701` | `ShouldRenderInMainPass()` / `ShouldRenderInDepthPass()` |
| `PrimitiveSceneProxy.h:1197-1200` | `bRenderInDepthPass` / `bRenderInMainPass` |
| `PrimitiveSceneProxy.cpp:277` | `bRenderInMainPass = InComponent->bRenderInMainPass;` |
| `PrimitiveSceneProxy.cpp:428` | `bRenderInMainPass(InProxyDesc.bRenderInMainPass)` |
| `PrimitiveSceneProxyDesc.h:25` | `bRenderInMainPass = true;`（默认值）|
| `PrimitiveSceneProxyDesc.h:93` | `uint32 bRenderInMainPass : 1;` |
| `PrimitiveViewRelevance.h:40,42,52,54,103` | bit field 定义 + 默认值 |
| `PrimitiveComponent.h:408,1918` | 字段与 Setter 声明 |
| `PrimitiveComponent.cpp:4620-4647` | `SetupPrecachePSOParams` 实现 |
| `PSOPrecache.h:27-131` | `FPSOPrecacheParams` 定义（union + `Data` hash）|
| `MobileBasePassRendering.h:460-534` | `FMobileBasePassMeshProcessor` 类定义 |
| `MobileBasePass.cpp:810-826` | `FMobileBasePassMeshProcessor` 构造函数 |
| `MobileBasePass.cpp:867-890` | `AddMeshBatch` 实现 |
| `MobileBasePass.cpp:1056-1149` | `CollectPSOInitializers` 实现 |
| `MobileBasePass.cpp:1151-1216` | `CreateMobile*Processor` 工厂函数 |
| `MobileBasePass.cpp:1218-1222` | 注册行 |
| `MobileBasePassRendering.cpp:470-491` | `RenderMobileBasePass` |
| `MobileShadingRenderer.cpp:1433-1446` | `BuildInstanceCullingDrawParams` |
| `MobileShadingRenderer.cpp:1578-1660` | `RenderForwardSinglePass` |
| `MobileShadingRenderer.cpp:1662-1749` | `RenderForwardMultiPass`（两阶段，第二阶段 `DepthRead_StencilRead`）|
| `MobileShadingRenderer.cpp:1947-2074` | `RenderDeferredSinglePass` / `RenderDeferredMultiPass` |
| `MobileTranslucentRendering.cpp:7-20` | `FMobileSceneRenderer::RenderTranslucency`（用 `TranslucencyInstanceCullingDrawParams`）|
| `SceneVisibility.cpp:1556-1593` | 静态网格 mobile 添加 BasePass / SkyPass |
| `SceneVisibility.cpp:2198-2238` | 动态网格 mobile 添加 BasePass / CSM / CustomDepth |
| `StaticMeshRender.cpp:2062` | `Result.bRenderInMainPass = ShouldRenderInMainPass();` |
| `SkeletalMesh.cpp:7115` | 同上 |

---

*— 分析完成，所有事实均基于本地 UE5.4 源码当前快照。Plan 可在以上修正之后直接落地。*
