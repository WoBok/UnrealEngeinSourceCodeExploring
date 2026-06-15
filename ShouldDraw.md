# 移动端 ShouldDraw 分析：BasePass 处理器为什么会处理透明物体

> 目标：解释 `FMobileBasePassMeshProcessor::ShouldDraw` 里为什么会出现"接收透明物体"的分支；并把移动端透明物体的完整渲染逻辑串起来。
> 适用版本：UE5.4 / 移动端 (Forward + Deferred 都覆盖)。

---

## 一、最关键的事实（颠覆直觉）

> **`FMobileBasePassMeshProcessor` 这个名字是误导性的 —— 它不只是"BasePass 处理器"，而是移动端"前向着色 Mesh 处理器"。**
> **同一个类被实例化了 5 次，分别挂到 5 个不同的 `EMeshPass` 槽位上：**
> **`BasePass / MobileBasePassCSM / TranslucencyStandard / TranslucencyAfterDOF / TranslucencyAll`。**

所以你看到的 `ShouldDraw()` 里 "处理透明物体" 的那一支代码，**根本不是 BasePass 在处理透明物体**，而是这个类被注册成"透明 Pass 的处理器"时跑的那一支。`bTranslucentBasePass` 这个成员就是用来区分当前实例属于哪种用途的。

下面用源码逐步还原。

---

## 二、一个类被注册了 5 次

`Source/Runtime/Renderer/Private/MobileBasePass.cpp:1218`：

```cpp
REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR(MobileBasePass,                CreateMobileBasePassProcessor,                EShadingPath::Mobile, EMeshPass::BasePass,             EMeshPassFlags::CachedMeshCommands | EMeshPassFlags::MainView);
REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR(MobileBasePassCSM,             CreateMobileBasePassCSMProcessor,             EShadingPath::Mobile, EMeshPass::MobileBasePassCSM,    EMeshPassFlags::CachedMeshCommands | EMeshPassFlags::MainView);
REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR(MobileTranslucencyAllPass,     CreateMobileTranslucencyAllPassProcessor,     EShadingPath::Mobile, EMeshPass::TranslucencyAll,      EMeshPassFlags::MainView);
REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR(MobileTranslucencyStandardPass,CreateMobileTranslucencyStandardPassProcessor,EShadingPath::Mobile, EMeshPass::TranslucencyStandard, EMeshPassFlags::MainView);
REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR(MobileTranslucencyAfterDOFPass,CreateMobileTranslucencyAfterDOFProcessor,    EShadingPath::Mobile, EMeshPass::TranslucencyAfterDOF, EMeshPassFlags::MainView);
// Skipping EMeshPass::TranslucencyAfterDOFModulate because dual blending is not supported on mobile
```

每条 `REGISTER_*` 都把一个工厂函数绑到一个 `EMeshPass` 上。注意：**这 5 个工厂返回的类型都是同一个 `FMobileBasePassMeshProcessor`**。

来看这 5 个工厂的差别（`MobileBasePass.cpp:1151~1216`）：

```cpp
// ── 1. EMeshPass::BasePass（不透明） ─────────────────────────
FMeshPassProcessor* CreateMobileBasePassProcessor(...)
{
    FMeshPassProcessorRenderState PassDrawRenderState;
    PassDrawRenderState.SetBlendState(TStaticBlendStateWriteMask<CW_RGBA>::GetRHI());                // 不透明：直接写颜色
    PassDrawRenderState.SetDepthStencilState(TStaticDepthStencilState<true, CF_DepthNearOrEqual>::GetRHI()); // 写深度 + 正常深度测试

    const auto Flags = FMobileBasePassMeshProcessor::EFlags::CanUseDepthStencil | (...CSM...);

    // ★ 不传 TranslucencyPassType，默认 TPT_MAX
    return new FMobileBasePassMeshProcessor(EMeshPass::BasePass, Scene, ..., Flags);
}

// ── 2. EMeshPass::TranslucencyStandard（普通半透明） ─────────
FMeshPassProcessor* CreateMobileTranslucencyStandardPassProcessor(...)
{
    FMeshPassProcessorRenderState PassDrawRenderState;
    PassDrawRenderState.SetDepthStencilState(TStaticDepthStencilState<false, CF_DepthNearOrEqual>::GetRHI()); // ★ 不写深度
    PassDrawRenderState.SetDepthStencilAccess(FExclusiveDepthStencil::DepthRead_StencilRead);

    return new FMobileBasePassMeshProcessor(
        EMeshPass::TranslucencyStandard, Scene, ..., Flags,
        ETranslucencyPass::TPT_TranslucencyStandard);                                                // ★ 关键
}

// ── 3. EMeshPass::TranslucencyAfterDOF（景深后半透明） ───────
FMeshPassProcessor* CreateMobileTranslucencyAfterDOFProcessor(...)
{
    ...同上深度只读...
    return new FMobileBasePassMeshProcessor(
        EMeshPass::TranslucencyAfterDOF, Scene, ..., Flags,
        ETranslucencyPass::TPT_TranslucencyAfterDOF);                                                // ★ 关键
}

// ── 4. EMeshPass::TranslucencyAll（全部半透明，作 PSO 预编译/兜底） ─
FMeshPassProcessor* CreateMobileTranslucencyAllPassProcessor(...)
{
    ...同上深度只读...
    return new FMobileBasePassMeshProcessor(
        EMeshPass::TranslucencyAll, Scene, ..., Flags,
        ETranslucencyPass::TPT_AllTranslucency);                                                     // ★ 关键
}
```

> 三个透明 Pass 共享同一个 MeshProcessor 类，只是**构造参数 `ETranslucencyPass::Type` 不同 + 渲染状态不写深度**。

---

## 三、`bTranslucentBasePass` 是怎么被打开的

`MobileBasePass.cpp:810` 构造函数：

```cpp
FMobileBasePassMeshProcessor::FMobileBasePassMeshProcessor(
    EMeshPass::Type InMeshPassType, ...,
    EFlags InFlags,
    ETranslucencyPass::Type InTranslucencyPassType)  // 默认 TPT_MAX
    : FMeshPassProcessor(InMeshPassType, Scene, ERHIFeatureLevel::ES3_1, ...)
    , PassDrawRenderState(InDrawRenderState)
    , TranslucencyPassType(InTranslucencyPassType)
    , Flags(InFlags)
    , bTranslucentBasePass(InTranslucencyPassType != ETranslucencyPass::TPT_MAX)   // ★★★
    , bDeferredShading(IsMobileDeferredShadingEnabled(...))
    , bPassUsesDeferredShading(bDeferredShading && !bTranslucentBasePass)
{
}
```

**逻辑非常简单：**

| 工厂 | 传入的 `InTranslucencyPassType` | `bTranslucentBasePass` |
|------|--------------------------------|------------------------|
| `CreateMobileBasePassProcessor` | （没传）→ `TPT_MAX` | **false**（不透明 BasePass） |
| `CreateMobileBasePassCSMProcessor` | （没传）→ `TPT_MAX` | **false**（不透明 BasePass + CSM） |
| `CreateMobileTranslucencyStandardPassProcessor` | `TPT_TranslucencyStandard` | **true** |
| `CreateMobileTranslucencyAfterDOFProcessor` | `TPT_TranslucencyAfterDOF` | **true** |
| `CreateMobileTranslucencyAllPassProcessor` | `TPT_AllTranslucency` | **true** |

---

## 四、回到 `ShouldDraw()` —— 它在做什么

```cpp
bool FMobileBasePassMeshProcessor::ShouldDraw(const FMaterial& Material) const
{
    const auto ShadingModels = Material.GetShadingModels();
    const bool bIsTranslucent =
           IsTranslucentBlendMode(Material.GetBlendMode())
        || ShadingModels.HasShadingModel(MSM_SingleLayerWater); // 单层水也走透明 Pass
    const bool bCanReceiveCSM = ...;

    if (bTranslucentBasePass)
    {
        // ★ 这一支专门给"透明 Pass 实例"用
        bool bShouldDraw =
            bIsTranslucent                                                                  // 必须是透明材质
            && !Material.IsDeferredDecal()                                                  // 排除延迟贴花
            && (   TranslucencyPassType == ETranslucencyPass::TPT_AllTranslucency           //   3) 兜底 All 都收
                || (TranslucencyPassType == ETranslucencyPass::TPT_TranslucencyStandard
                        && !Material.IsMobileSeparateTranslucencyEnabled())                 //   1) 普通：未启用"独立透明"
                || (TranslucencyPassType == ETranslucencyPass::TPT_TranslucencyAfterDOF
                        &&  Material.IsMobileSeparateTranslucencyEnabled()));               //   2) 后处理后：启用了"独立透明"
        check(!bShouldDraw || bCanReceiveCSM == false);
        return bShouldDraw;
    }
    else
    {
        // ★ 这一支给"BasePass 实例"用：只收非透明（即不透明 + Masked）
        return !bIsTranslucent;
    }
}
```

**所以 BasePass 实例从来不会去渲染透明物体**：

- 当 `bTranslucentBasePass == false`（即 `EMeshPass::BasePass`），走 `else` 分支：`return !bIsTranslucent;` —— 透明物体直接被拒收。
- 你看到的 "处理透明物体" 那一段是 `if (bTranslucentBasePass)` 内部，**那是这个类被注册成 `EMeshPass::TranslucencyStandard / TranslucencyAfterDOF / TranslucencyAll` 时跑的代码**，不是 BasePass。

> 进一步地：当某个透明材质勾了 `MobileSeparateTranslucency`，它在 `TranslucencyStandard` 实例的 `ShouldDraw` 里被拒收、在 `TranslucencyAfterDOF` 实例里被接收 —— 它的 MeshDrawCommand 就只会存进 `EMeshPass::TranslucencyAfterDOF` 槽里。这就是"独立透明（DOF 后透明）"的分发原理。

---

## 五、AddMeshBatch 的入口（场景里每个 Mesh 都会被这 5 个实例各喂一次）

`MobileBasePass.cpp:867`：

```cpp
void FMobileBasePassMeshProcessor::AddMeshBatch(
    const FMeshBatch& MeshBatch, ..., const FPrimitiveSceneProxy* PrimitiveSceneProxy, int32 StaticMeshId)
{
    if (!MeshBatch.bUseForMaterial ||
        (Flags & EFlags::DoNotCache) == EFlags::DoNotCache ||
        (PrimitiveSceneProxy && !PrimitiveSceneProxy->ShouldRenderInMainPass()))
    {
        return;
    }
    ...
    while (MaterialRenderProxy)
    {
        const FMaterial* Material = MaterialRenderProxy->GetMaterialNoFallback(...);
        ...
        TryAddMeshBatch(..., *Material);            // → 里面调用 ShouldDraw
        ...
    }
}

bool FMobileBasePassMeshProcessor::TryAddMeshBatch(...)
{
    if (ShouldDraw(Material))                        // ← 这里把"该不该收"问 ShouldDraw
    {
        ...
        return Process(...);                         // 真正生成 FMeshDrawCommand 并塞进对应 EMeshPass 槽
    }
    return true;
}
```

**调用方（不是这个类内部）**：UE 在 `InitViews` 阶段会枚举所有 EMeshPass，把同一个 `FMeshBatch` 分别发给每个 Pass 的 MeshProcessor。所以：

- 一个不透明的桌子 → 5 个实例都被问到一次，只有 `BasePass` 实例的 `ShouldDraw` 通过 → 命令进 `ParallelMeshDrawCommandPasses[EMeshPass::BasePass]`。
- 一个普通半透明玻璃 → 只有 `TranslucencyStandard` 和 `TranslucencyAll` 实例通过 → 命令进对应两个槽。
- 一个 `MobileSeparateTranslucency` 玻璃 → `TranslucencyAfterDOF` 和 `TranslucencyAll` 实例通过。

---

## 六、Render 阶段：谁去 DispatchDraw 哪个槽

### 6.1 不透明 BasePass 的派发

`MobileBasePass.cpp:470`：

```cpp
void FMobileSceneRenderer::RenderMobileBasePass(FRHICommandList& RHICmdList, const FViewInfo& View, ...)
{
    ...
    View.ParallelMeshDrawCommandPasses[EMeshPass::BasePass].DispatchDraw(nullptr, RHICmdList, InstanceCullingDrawParams);

    if (View.Family->EngineShowFlags.Atmosphere)
    {
        View.ParallelMeshDrawCommandPasses[EMeshPass::SkyPass].DispatchDraw(...);
    }
    ...
}
```

### 6.2 透明 Pass 的派发（**真正"透明物体的渲染逻辑"**）

`Source/Runtime/Renderer/Private/MobileTranslucentRendering.cpp:7`（整个文件只有 20 行）：

```cpp
void FMobileSceneRenderer::RenderTranslucency(FRHICommandList& RHICmdList, const FViewInfo& View)
{
    const bool bShouldRenderTranslucency =
        ShouldRenderTranslucency(StandardTranslucencyPass) && ViewFamily.EngineShowFlags.Translucency;

    if (bShouldRenderTranslucency)
    {
        CSV_SCOPED_TIMING_STAT_EXCLUSIVE(RenderTranslucency);
        SCOPE_CYCLE_COUNTER(STAT_TranslucencyDrawTime);
        SCOPED_DRAW_EVENT(RHICmdList, Translucency);
        SCOPED_GPU_STAT(RHICmdList, Translucency);

        RHICmdList.SetViewport(...);
        // ★★★ 真正"画透明"就这一句
        View.ParallelMeshDrawCommandPasses[StandardTranslucencyMeshPass]
            .DispatchDraw(nullptr, RHICmdList, &TranslucencyInstanceCullingDrawParams);
    }
}
```

`StandardTranslucencyMeshPass` 这个枚举值在哪里被定下来？`MobileShadingRenderer.cpp:307`：

```cpp
StandardTranslucencyPass = ViewFamily.AllowTranslucencyAfterDOF()
                           ? ETranslucencyPass::TPT_TranslucencyStandard
                           : ETranslucencyPass::TPT_AllTranslucency;
StandardTranslucencyMeshPass = TranslucencyPassToMeshPass(StandardTranslucencyPass);
```

也就是：
- 视图允许 "DOF 后透明" → 走 `EMeshPass::TranslucencyStandard`（不带 SeparateTranslucency 标记的透明物体）。带 SeparateTranslucency 标记的另由 DOF 后处理流程去画 `TranslucencyAfterDOF`。
- 否则 → 直接走 `EMeshPass::TranslucencyAll` 兜底。

`MobileShadingRenderer.cpp` 里 `RenderMobileBasePass` 和 `RenderTranslucency` 的串联 —— **总共出现在 4 处**（行号：1609/1623、1682/1735、1968/1985、2011/2068），对应 4 条移动端渲染路径，结构都长这样：

```cpp
// MobileShadingRenderer.cpp（4 种渲染路径都长这个样子）
RenderMobileBasePass(RHICmdList, View, &PassParameters->InstanceCullingDrawParams);   // 不透明
...                                                                                   // Decal / Fog / Occlusion / ...
RenderTranslucency(RHICmdList, View);                                                 // 透明
```

---

## 七、全链路总结一张图

```
                              FMobileBasePassMeshProcessor 
                         ┌───────────────┬──────────────────────────┐
                         │  bTranslucentBasePass = false (BasePass) │  bTranslucentBasePass = true (Translucent)
─────────────────────────┼──────────────────────────────────────────┼──────────────────────────────────────────
注册槽 (EMeshPass)        │ BasePass / MobileBasePassCSM             │ TranslucencyStandard / AfterDOF / All
渲染状态                  │ 写深度 + CF_DepthNearOrEqual + OpaqueBlend │ 不写深度 + CF_DepthNearOrEqual + TransBlend
ShouldDraw 接受           │ !bIsTranslucent                          │ bIsTranslucent && TranslucencyPassType 匹配
被谁 DispatchDraw         │ RenderMobileBasePass                     │ RenderTranslucency
                         │ ParallelMeshDrawCommandPasses[BasePass]  │ ParallelMeshDrawCommandPasses[StandardTranslucencyMeshPass]
顺序                      │ 第 1 步                                  │ 第 2 步（在 BasePass 之后）
```

**核心结论：**

1. **"`ShouldDraw` 里有些透明物体也会返回到可以渲染中吗？"**
   会，但只在**该实例被注册为透明 Pass**（即 `bTranslucentBasePass == true`）时才会通过。同一段函数在 BasePass 实例上跑时走 `else` 分支，明确 `return !bIsTranslucent`，透明物体一定被拒。

2. **"这个不是 BasePass 吗？BasePass 中不应该不渲染透明物体吗？"**
   类名叫 `FMobileBasePassMeshProcessor` 但**实际上是"移动端前向着色 Mesh 处理器"的复用**，它在 BasePass / TranslucencyStandard / TranslucencyAfterDOF / TranslucencyAll 四个槽位各活了一次。被注册为 BasePass 那个实例确实不会渲染透明物体，是被注册为透明 Pass 的那几个实例在处理透明物体的命令收集；最终在 `RenderTranslucency`（`MobileTranslucentRendering.cpp`）里被 `DispatchDraw` 出去。

3. **"透明物体的渲染逻辑在哪？"**
   - 命令收集端：`MobileBasePass.cpp` 里 `CreateMobileTranslucency*PassProcessor` + `FMobileBasePassMeshProcessor::ShouldDraw` 的 `if (bTranslucentBasePass)` 分支。
   - 命令派发端：`MobileTranslucentRendering.cpp::RenderTranslucency` 的 `View.ParallelMeshDrawCommandPasses[StandardTranslucencyMeshPass].DispatchDraw(...)`。
   - 串联位置：`MobileShadingRenderer.cpp` 的 `RenderMobileBasePass(...)` → ... → `RenderTranslucency(...)`，**共 4 处**。

---

## 八、对"透明后再渲染不透明 Pass"改造方案的连带影响

这意味着把 `EMeshPass::MobileAfterTranslucent` 的命令收集**复用 `FMobileBasePassMeshProcessor`** 是合理的，只需要：

- 加一个新的 `CreateMobileAfterTranslucentPassProcessor`：
  - 渲染状态：`SetBlendState(TStaticBlendStateWriteMask<CW_RGBA>::GetRHI())` + `SetDepthStencilState(TStaticDepthStencilState<true, CF_Always>::GetRHI())`（写深度 + 始终通过）。
  - 关键决定：**不要传 `ETranslucencyPass::Type`（保留默认 `TPT_MAX`）**，让 `bTranslucentBasePass == false`，这样 `ShouldDraw` 会自动走 "只接收非透明材质" 那一支，符合我们对"不透明物体最后画"的需求。
  - 但需要在 `AddMeshBatch` 入口处加 `bRenderAfterTranslucent` 路由：只有勾上这个标志的 Primitive 才会被收（同时原 BasePass 实例要排除它）。具体改法参见 `Docs/RenderBaseAndTranslucencyPass.md` 步骤 4 ~ 5。

- 在 `MobileShadingRenderer.cpp` 里仿照 `RenderTranslucency` 写一个 `RenderMobileAfterTranslucent`：

  ```cpp
  void FMobileSceneRenderer::RenderMobileAfterTranslucent(FRHICommandList& RHICmdList, const FViewInfo& View)
  {
      SCOPED_DRAW_EVENT(RHICmdList, MobileAfterTranslucent);
      RHICmdList.SetViewport(View.ViewRect.Min.X, View.ViewRect.Min.Y, 0.0f,
                             View.ViewRect.Max.X, View.ViewRect.Max.Y, 1.0f);
      View.ParallelMeshDrawCommandPasses[EMeshPass::MobileAfterTranslucent]
          .DispatchDraw(nullptr, RHICmdList, &AfterTranslucentInstanceCullingDrawParams);
  }
  ```

- **在 `MobileShadingRenderer.cpp` 的 1623、1735、1985、2068 这 4 处 `RenderTranslucency(RHICmdList, View);` 后面都补一行 `RenderMobileAfterTranslucent(RHICmdList, View);`** —— 这样 4 条移动端渲染路径都覆盖到了（Forward / Deferred / 各种变体）。

> 之前 `Docs/RenderBaseAndTranslucencyPass.md` 的"步骤 6.1"里只提到"在 RenderTranslucency 之后加一行"，实际需要修正成"4 处都加一行"，否则有些渲染路径下你的新 Pass 不会被调用。

---

## 九、相关文件速查

| 关注点 | 文件 |
|--------|------|
| `FMobileBasePassMeshProcessor` 类声明 + EFlags | `Source/Runtime/Renderer/Private/MobileBasePassRendering.h:460` |
| 构造函数 + `ShouldDraw` + `TryAddMeshBatch` + `AddMeshBatch` | `Source/Runtime/Renderer/Private/MobileBasePass.cpp:810~890` |
| 5 个 `Create*Processor` 工厂 + `REGISTER_*` | `Source/Runtime/Renderer/Private/MobileBasePass.cpp:1151~1223` |
| `RenderMobileBasePass`（不透明派发） | `Source/Runtime/Renderer/Private/MobileBasePass.cpp:470` |
| `RenderTranslucency`（透明派发） | `Source/Runtime/Renderer/Private/MobileTranslucentRendering.cpp:7` |
| `StandardTranslucencyMeshPass` 的初始化 | `Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp:307` |
| 4 个 `RenderMobileBasePass → RenderTranslucency` 串联点 | `Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp:1609/1623、1682/1735、1968/1985、2011/2068` |
