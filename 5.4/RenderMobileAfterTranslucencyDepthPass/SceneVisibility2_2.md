# 新增 AllwayFrontPass：是否需要修改 ComputeRelevance / ComputeDynamicMeshRelevance？

> 设计目标：为指定 primitive 实现"不透明渲染时不画，等透明物体渲染完再画"的 AllwayFrontPass
> 涉及函数：
> - `SceneVisibility.cpp:1299` `FRelevancePacket::ComputeRelevance`（静态网格相关性）
> - `SceneVisibility.cpp:2186` `ComputeDynamicMeshRelevance`（动态网格相关性）
>
> 文档版本：v1.0 ｜ 分析对象：大闹天宫 MR01 主引擎 UE5 移动端分支
> 配套文档：
> - [SceneVisibility2.md](./SceneVisibility2.md) — SceneVisibility.cpp 解析
> - [SceneVisibility2_1.md](./SceneVisibility2_1.md) — MobileShadingRenderer 中 SceneVisibility.h 引用清单

---

## 用户的设计思路（前置）

```
            ┌──────────── UPrimitiveComponent.bRenderInAllwayFrontPass = true
            │                  (用户设置)
            ▼
FPrimitiveSceneProxy::GetViewRelevance
            │  返回 bRenderInMainPass=false, bRenderInAllwayFrontPass=true
            ▼
SceneVisibility.cpp:2228 (新增)
            │  PassMask.Set(EMeshPass::AllwayFrontPass)
            ▼
MobileAllwayFrontMeshProcessor::ShouldDraw
            │  验证 bRenderInAllwayFrontPass && IsOpaque
            │  使用与 BasePass 共享的 Shader (复用 PSO)
            ▼
MobileShadingRenderer::RenderAllwayFrontPass (新增)
            │  在 RenderTranslucency 之后调用
            │  Single Pass: subpass 1 末尾 (next subpass 后)
            │  Multi Pass:  新建第 3 个 RDG Pass
            ▼
        SceneColor
```

> 不透明渲染时不进行渲染，等到透明物体渲染完成之后再渲染指定的物体。

---

# 是否需要修改 `ComputeRelevance` 和 `ComputeDynamicMeshRelevance`？

## 直接回答：**两个函数都必须改，但不是核心改动点**

这两个函数是 **Visibility 阶段的"路由器"**，负责把每个 primitive 的 mesh batch 投递到对应的 `EMeshPass::xxx` 桶里。如果不改它们，你的 `bRenderInAllwayFrontPass=true` 物体会**完全消失** —— 因为用户设计里 `bRenderInMainPass=false`，所有现有路由分支都不会接住它。

但它们只是 9 个改动点中的两个，下面给完整清单。

---

# 1. 为什么必须改这两个函数

## 1.1 ComputeRelevance（SceneVisibility.cpp:1299，静态网格）

**关键拦截点（1556 行）**：

```cpp
if (StaticMeshRelevance.bUseForMaterial && (ViewRelevance.bRenderInMainPass || ViewRelevance.bRenderCustomDepth))
{
    // ↑ 你的物体 bRenderInMainPass=false, bRenderCustomDepth=false → 整个 if 体被跳过
    if (ShadingPath == EShadingPath::Mobile) {
        if (!StaticMeshRelevance.bUseSkyMaterial) {
            DrawCommandPacket.AddCommandsForMesh(..., EMeshPass::BasePass);
            // ...
        }
        MarkMask |= EMarkMaskBits::StaticMeshVisibilityMapMask;  // ← 这一句很关键
    }
}
```

**两个直接后果**：
1. 你的静态 mesh 不会被加到 `EMeshPass::AllwayFrontPass`（自然，因为分支没进）
2. `MarkMask` 不会写入 `StaticMeshVisibilityMapMask` → `View.StaticMeshVisibilityMap` 对应位永远 0 → **后续 `MobileAllwayFrontMeshProcessor::ShouldDraw` 即使返回 true 也不会执行**，因为上层 `FParallelMeshDrawCommandPass` 会先用这个 bitmap 过滤

## 1.2 ComputeDynamicMeshRelevance（SceneVisibility.cpp:2186-2430，动态网格）

**关键拦截点（2198 行）**：

```cpp
if (ViewRelevance.bDrawRelevance && (ViewRelevance.bRenderInMainPass || ViewRelevance.bRenderCustomDepth || ViewRelevance.bRenderInDepthPass))
{
    // ↑ 你的物体 3 个 flag 都是 false → 跳过
    if (ViewRelevance.bRenderInMainPass || ViewRelevance.bRenderCustomDepth) {
        PassMask.Set(EMeshPass::BasePass);
        // ...
        if (ShadingPath == EShadingPath::Mobile) {
            PassMask.Set(EMeshPass::MobileBasePassCSM);
        }
    }
}
```

**直接后果**：动态 mesh 没有任何 PassMask bit 被 set → `DispatchPassSetup` 阶段不会处理它 → 无 draw command 生成。

## 1.3 改动模板

### ComputeRelevance（1559 行附近的 Mobile 分支）

```cpp
// === 在最外层 if 之外（即 1556 行之前），新增独立分支 ===
if (StaticMeshRelevance.bUseForMaterial && ViewRelevance.bRenderInAllwayFrontPass)
{
    DrawCommandPacket.AddCommandsForMesh(
        PrimitiveIndex, PrimitiveSceneInfo, StaticMeshRelevance, StaticMesh,
        CullingPayloadFlags, Scene, bCanCache,
        EMeshPass::AllwayFrontPass);
    MarkMask |= EMarkMaskBits::StaticMeshVisibilityMapMask;
    ++NumVisibleStaticMeshElements;
}
```

> **设计要点**：单独一个 `if`，**不要**把 `bRenderInAllwayFrontPass` 塞进 1556 行的现有条件 —— 否则 BasePass 路由分支会重新激活，物体被画两次（在 BasePass 和 AllwayFrontPass）。

### ComputeDynamicMeshRelevance（2186-2430）

```cpp
// === 在函数体末尾、Editor / HairStrands 分支之前，新增 ===
if (ViewRelevance.bDrawRelevance && ViewRelevance.bRenderInAllwayFrontPass)
{
    PassMask.Set(EMeshPass::AllwayFrontPass);
    View.NumVisibleDynamicMeshElements[EMeshPass::AllwayFrontPass] += NumElements;
}
```

---

# 2. 完整改动清单（共 9 个区域，~13 个文件）

按依赖顺序排列（前面的不改后面的会编译失败）：

## ① 类型系统（必改 · 3 处）

### 1.1 `Engine/Source/Runtime/Renderer/Public/MeshPassProcessor.h:32-78`

```cpp
namespace EMeshPass
{
    enum Type : uint8
    {
        DepthPass,
        // ... 既有 32 项 ...
        WaterInfoTexturePass,
        AllwayFrontPass,        // ← 新增（放最末尾、Num 之前）
#if WITH_EDITOR
        HitProxy,
        // ...
#endif
        Num,
        NumBits = 6,
    };
}
```

⚠️ `NumBits = 6` 支持最多 64 个枚举，目前 32 + 4(Editor) = 36，安全。

### 1.2 `MeshPassProcessor.h:83-135` 的 `GetMeshPassName`

```cpp
case EMeshPass::AllwayFrontPass:    return TEXT("AllwayFrontPass");
```

⚠️ 末尾的 `static_assert(EMeshPass::Num == 32 + ...)` 行也要同步加 1。

### 1.3 `Engine/Source/Runtime/Engine/Public/PrimitiveViewRelevance.h:70` 之后

```cpp
/** Whether the primitive should be rendered in the second stage depth only pass. */
uint32 bRenderInSecondStageDepthPass : 1;

/** [MR01] Whether the primitive should be rendered in the always-front pass after translucency. */
uint32 bRenderInAllwayFrontPass : 1;     // ← 新增
```

⚠️ `FPrimitiveViewRelevance` 是 memzero 友好的位域结构（构造函数靠 `memset` 清零），**默认值是 0** 正好符合"默认不进 AllwayFrontPass"的语义，无需在构造函数特别处理。

## ② 组件层（必改 · 2-3 处）

### 2.1 `Engine/Source/Runtime/Engine/Classes/Components/PrimitiveComponent.h`

```cpp
UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Rendering, AdvancedDisplay)
uint8 bRenderInAllwayFrontPass : 1;
```

### 2.2 `FPrimitiveSceneProxy`（`PrimitiveSceneProxy.h/cpp`）

- 类成员添加 `uint8 bRenderInAllwayFrontPass : 1;`
- 构造函数从 `UPrimitiveComponent` 拷贝
- 基类 `GetViewRelevance` 设置：
  ```cpp
  Result.bRenderInAllwayFrontPass = bRenderInAllwayFrontPass;
  if (bRenderInAllwayFrontPass) {
      Result.bRenderInMainPass = false;     // 互斥：只走 AllwayFrontPass
      Result.bRenderInDepthPass = false;    // 不进早 Z
      // bShadowRelevance 是否保留？看你需求，建议保留以保持阴影一致
  }
  ```

### 2.3 派生 SceneProxy（如果它们覆盖了 GetViewRelevance）
- `FStaticMeshSceneProxy::GetViewRelevance`
- `FSkeletalMeshSceneProxy::GetViewRelevance`
- 等等。**只要派生类没动 `Result.bRenderInAllwayFrontPass`，都会继承基类的赋值**。

## ③ Visibility 路由（**用户问的核心** · 必改 · 2 处）

如 §1.3 所述。

## ④ MeshProcessor（必改 · 1 个新文件 + 1 处注册）

### 4.1 新建 `MobileAllwayFrontPass.cpp/h`

骨架（仿照 `MobileBasePass.cpp`）：

```cpp
class FMobileAllwayFrontMeshProcessor : public FMeshPassProcessor
{
public:
    FMobileAllwayFrontMeshProcessor(
        const FScene* Scene,
        ERHIFeatureLevel::Type FeatureLevel,
        const FSceneView* InViewIfDynamicMeshCommand,
        const FMeshPassProcessorRenderState& InPassDrawRenderState,
        FMeshPassDrawListContext* InDrawListContext);

    virtual void AddMeshBatch(
        const FMeshBatch& RESTRICT MeshBatch,
        uint64 BatchElementMask,
        const FPrimitiveSceneProxy* RESTRICT PrimitiveSceneProxy,
        int32 StaticMeshId = -1) override final;

    virtual void CollectPSOInitializers(...) override final;

private:
    bool TryAddMeshBatch(...);
    bool Process(...);

    FMeshPassProcessorRenderState PassDrawRenderState;
};

// 关键：渲染状态——"始终在前"
FMobileAllwayFrontMeshProcessor::FMobileAllwayFrontMeshProcessor(...)
    : FMeshPassProcessor(EMeshPass::AllwayFrontPass, Scene, FeatureLevel, ...)
    , PassDrawRenderState(InPassDrawRenderState)
{
    // 选项 A：永远在最前（不读不写深度）
    PassDrawRenderState.SetDepthStencilState(
        TStaticDepthStencilState<false, CF_Always>::GetRHI());
    
    // 选项 B：读深度但不写（Z 测试用 SceneDepth fetch，但允许被透明遮挡时仍画在透明之上）
    // PassDrawRenderState.SetDepthStencilState(
    //     TStaticDepthStencilState<false, CF_DepthNearOrEqual>::GetRHI());
    
    // 选项 C：完全忽略深度（典型 UI / Outline 风格）
    // PassDrawRenderState.SetDepthStencilState(
    //     TStaticDepthStencilState<false, CF_Always>::GetRHI());
    
    // 一般要 disable depth write，否则会污染后续 Pass 的深度
    PassDrawRenderState.SetBlendState(
        TStaticBlendState<>::GetRHI());     // 默认 Opaque blend
}

bool FMobileAllwayFrontMeshProcessor::TryAddMeshBatch(...)
{
    // 关键过滤：只接受不透明材质
    const FMaterial& Material = ...;
    if (!IsOpaqueOrMaskedBlendMode(Material.GetBlendMode())) {
        return true;  // 跳过透明材质
    }
    
    // 复用 BasePass 的 shader（核心 PSO 复用逻辑）
    // 直接 include "MobileBasePassRendering.h" 然后用 GetMobileBasePassShaders<...>
    return Process<...>(...);
}
```

### 4.2 注册（`MobileAllwayFrontPass.cpp` 文件末尾）

```cpp
FMeshPassProcessor* CreateMobileAllwayFrontPassProcessor(
    ERHIFeatureLevel::Type FeatureLevel, const FScene* Scene,
    const FSceneView* InViewIfDynamicMeshCommand, FMeshPassDrawListContext* InDrawListContext)
{
    FMeshPassProcessorRenderState DrawRenderState;
    return new FMobileAllwayFrontMeshProcessor(Scene, FeatureLevel,
        InViewIfDynamicMeshCommand, DrawRenderState, InDrawListContext);
}

REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR(
    MobileAllwayFrontPass,
    CreateMobileAllwayFrontPassProcessor,
    EShadingPath::Mobile,
    EMeshPass::AllwayFrontPass,
    EMeshPassFlags::CachedMeshCommands | EMeshPassFlags::MainView);
```

⚠️ **`CachedMeshCommands` flag 决定能不能命中 `Scene.CachedDrawLists` 缓存** —— 由于 ComputeRelevance 路径里 `StaticMeshRelevance.GetStaticMeshCommandInfoIndex(PassType)` 依赖这个 flag，必须开启。

## ⑤ View 数据结构（自动扩容 · 0 改动）

`FViewInfo::ParallelMeshDrawCommandPasses[EMeshPass::Num]` 和 `NumVisibleDynamicMeshElements[EMeshPass::Num]` 都是基于 `EMeshPass::Num` 的静态数组，改了枚举自动扩容。**无需手动修改**。

## ⑥ Setup MeshPasses（必改 · 1 处）

`SceneVisibility.cpp:4401 SetupMeshPasses()` 内会对每个 view 调用 `SceneRenderer.SetupMeshPass(View, BasePassDepthStencilAccess, ViewCommands, InstanceCullingManager)`。

`SetupMeshPass` 内部会循环 `EMeshPass::Num` 个枚举值并调用对应的 `FParallelMeshDrawCommandPass::DispatchPassSetup`。**只要新枚举注册了 PassProcessor，这一步会自动处理**。

⚠️ 但由于移动端 BasePass 是延后到 `SetupMobileBasePassAfterShadowInit` 处理的，AllwayFrontPass 也建议放在那里 —— 否则会出现**两次 DispatchPassSetup**：一次在通用 SetupMeshPass，一次（如果你不小心）在 AfterShadowInit。

**推荐方案**：在 `MobileShadingRenderer.cpp:377` 的 `SetupMobileBasePassAfterShadowInit` 末尾追加：

```cpp
// === 新增 ===
FMeshPassProcessor* AllwayFrontProcessor = FPassProcessorManager::CreateMeshPassProcessor(
    EShadingPath::Mobile, EMeshPass::AllwayFrontPass, Scene->GetFeatureLevel(),
    Scene, &View, nullptr);

FParallelMeshDrawCommandPass& AllwayFrontPass = View.ParallelMeshDrawCommandPasses[EMeshPass::AllwayFrontPass];
AllwayFrontPass.DispatchPassSetup(
    Scene, View,
    FInstanceCullingContext(TEXT("AllwayFrontPass"), ShaderPlatform, &InstanceCullingManager, ViewIds, nullptr, InstanceCullingMode),
    EMeshPass::AllwayFrontPass,
    BasePassDepthStencilAccess,
    AllwayFrontProcessor,
    View.DynamicMeshElements,
    &View.DynamicMeshElementsPassRelevance,
    View.NumVisibleDynamicMeshElements[EMeshPass::AllwayFrontPass],
    ViewCommands.DynamicMeshCommandBuildRequests[EMeshPass::AllwayFrontPass],
    ViewCommands.DynamicMeshCommandBuildFlags[EMeshPass::AllwayFrontPass],
    ViewCommands.NumDynamicMeshCommandBuildRequestElements[EMeshPass::AllwayFrontPass],
    ViewCommands.MeshCommands[EMeshPass::AllwayFrontPass]);
```

⚠️ 如果你不希望延后到 AfterShadowInit，可以让通用 `SetupMeshPass` 处理（什么都不需要做），但那样 AllwayFrontPass 的 `DispatchPassSetup` 会和 BasePass setup **同时**进行。这没问题，因为 AllwayFrontPass 不依赖 Shadow。**两种方案都可行**，看你对 RT 时间的偏好。

## ⑦ Render 执行（必改 · 4 个分支 + 1 个新函数）

### 7.1 新增 `RenderAllwayFrontPass`

```cpp
// 在 MobileShadingRenderer.h 添加声明，.cpp 添加实现
void FMobileSceneRenderer::RenderAllwayFrontPass(FRHICommandList& RHICmdList, FViewInfo& View, const FInstanceCullingDrawParams* InstanceCullingDrawParams)
{
    QUICK_SCOPE_CYCLE_COUNTER(STAT_RenderAllwayFrontPass);
    SCOPED_DRAW_EVENT(RHICmdList, AllwayFrontPass);
    
    View.ParallelMeshDrawCommandPasses[EMeshPass::AllwayFrontPass].Draw(RHICmdList, InstanceCullingDrawParams);
}
```

### 7.2 在 4 个渲染路径里的插入位置

| 路径 | 行号 | 调用位置 |
|---|---|---|
| `RenderForwardSinglePass` | **1623 之后** | `RenderTranslucency` 完成、Tonemap NextSubpass 之前 |
| `RenderForwardMultiPass` | **1735 之后** | `RenderTranslucency` 之后，仍在第二个 RDG Pass 内 |
| `RenderDeferredSinglePass` | **1985 之后** | `RenderTranslucency` 之后 |
| `RenderDeferredMultiPass` | **2068 之后** | 同 Multi Pass |

例如 `RenderForwardSinglePass`（1623 行附近）：

```cpp
// Draw translucency.
RenderTranslucency(RHICmdList, View);

// === 新增 AllwayFrontPass ===
RenderAllwayFrontPass(RHICmdList, View, &PassParameters->InstanceCullingDrawParams);

#if UE_ENABLE_DEBUG_DRAWING
// ...
```

⚠️ **关于"Single Pass 在 subpass 1 末尾（next subpass 后）"的理解**：

你的描述可能有歧义。从 1614 / 1650 行来看：
- `subpass 0` = Opaque BasePass（深度可写，颜色可写）
- `subpass 1` = Translucency / Decals / Fog（**深度只读**，可读 SceneDepth fetch）
- `subpass 2`（仅 `bTonemapSubpassInline`）= CustomResolve Tonemap

`AllwayFrontPass` 应该插在 **subpass 1 的末尾**（即 `RenderTranslucency` 之后、`NextSubpass` 之前）。这意味着：

- ✅ 可以读 SceneDepth（fetch）
- ❌ 不能写 SceneDepth（subpass 1 是 DepthRead）
- ✅ 可以写 SceneColor（覆盖之前所有 BasePass+Translucency 结果）

这正好符合你 "always front" 的语义，但意味着 `MobileAllwayFrontMeshProcessor` 的 DepthStencilState **必须** disable depth write，否则会触发 RHI validation 错误。

## ⑧ Shader 选择（关键 · 复用 PSO）

由于你说"使用与 BasePass 共享的 Shader (复用 PSO)"，关键点：

```cpp
// 在 MobileAllwayFrontPass.cpp 里 #include "MobileBasePassRendering.h"
// 然后在 Process 函数里用：

TMobileBasePassPSPolicyParamType<...> PixelShader;
TMobileBasePassVSPolicyParamType<...> VertexShader;
GetMobileBasePassShaders<...>(...);
```

但有一个坑：`GetMobileBasePassShaders` 内部会用 `MeshPassType` 做 shader permutation 区分（`EMeshPass::BasePass` vs `EMeshPass::MobileBasePassCSM`）。如果你想**完全复用** BasePass 的 PSO，你的 MeshProcessor 内部应该 **以 `EMeshPass::BasePass` 的身份** 选 shader，但 **以 `EMeshPass::AllwayFrontPass` 的身份** 提交 draw command。

**简化方案**：直接派生自 `FMobileBasePassMeshProcessor`，在 `TryAddMeshBatch` 里只过滤 opaque + bRenderInAllwayFrontPass。

## ⑨ 收尾（建议 · 不必改）

- **CSM 阴影**：`MobileShadingRenderer::PrepareViewVisibilityLists`（359 行）只关心 `MobileBasePassCSM`，AllwayFrontPass 默认不接收 CSM。如果需要，得自己再分一份 `MobileAllwayFrontCSMVisibilityMap`，但通常 always-front 的物体不需要阴影。
- **TileBased Renderer 兼容性**：在 OpenGL ES 的某些驱动上，subpass 1 内不允许新增 draw command 类型 → 需要在 `RHISubmitCommandsAndFlushGPU` 之后再画 → 这种平台可能要回退到 multi-pass。

---

# 3. 一图总览：改动点 vs 数据流

```
                ┌──────────────────────────────────────────┐
                │ ① UPrimitiveComponent.bRenderInAllwayFrontPass = true │
                │   (用户编辑器 / 蓝图)                       │
                └────────────────┬─────────────────────────┘
                                 ↓
                ┌──────────────────────────────────────────┐
                │ ② FPrimitiveSceneProxy::GetViewRelevance  │
                │   Result.bRenderInMainPass = false         │
                │   Result.bRenderInAllwayFrontPass = true   │
                └────────────────┬─────────────────────────┘
                                 ↓
        ┌─── SceneVisibility.cpp ────────────────────────────────┐
        │                                                        │
        │  ③.A ComputeRelevance:1556-1577 (静态网格)              │
        │       新增分支 → DrawCommandPacket.AddCommandsForMesh    │
        │                  (..., EMeshPass::AllwayFrontPass)      │
        │       MarkMask |= StaticMeshVisibilityMapMask            │
        │                                                        │
        │  ③.B ComputeDynamicMeshRelevance:2186 (动态网格)         │
        │       PassMask.Set(EMeshPass::AllwayFrontPass)          │
        │       NumVisibleDynamicMeshElements++                   │
        │                                                        │
        │  ④ SetupMeshPasses (4401)                                │
        │     ↓ 通过 SceneRenderer.SetupMeshPass 自动处理            │
        │     ↓（或在 SetupMobileBasePassAfterShadowInit 手动追加）   │
        └────────────────────┬───────────────────────────────────┘
                             ↓
        ┌─────────────────────────────────────────────────────────┐
        │ ⑤ FParallelMeshDrawCommandPass::DispatchPassSetup        │
        │   ↓ 调用                                                │
        │ ⑥ FMobileAllwayFrontMeshProcessor::AddMeshBatch (新建)   │
        │   - TryAddMeshBatch: 过滤 opaque only                   │
        │   - 复用 GetMobileBasePassShaders 选 shader              │
        │   - DepthStencil = (false, CF_Always) 不写深度            │
        │ ⑦ REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR (新建)      │
        │   ShadingPath = Mobile, MeshPass = AllwayFrontPass       │
        │   PassFlags = CachedMeshCommands | MainView              │
        └─────────────────────┬───────────────────────────────────┘
                              ↓
        ┌─────────────────────────────────────────────────────────┐
        │ ⑧ MobileShadingRenderer::Render*                         │
        │   - RenderForwardSinglePass:1623 后 ← 在 subpass 1 内      │
        │   - RenderForwardMultiPass:1735  后 ← 第二个 RDG Pass      │
        │   - RenderDeferredSinglePass:1985 后                     │
        │   - RenderDeferredMultiPass:2068 后                      │
        │     RenderAllwayFrontPass(RHICmdList, View, ...)         │
        └─────────────────────────────────────────────────────────┘
```

---

# 4. 简化清单（按工作量排序）

| 优先级 | 区域 | 文件数 | 工作量 |
|---|---|---|---|
| **P0** | EMeshPass 枚举 + GetMeshPassName + bRenderInAllwayFrontPass | 2 | 5 min |
| **P0** | UPrimitiveComponent UPROPERTY + Proxy 透传 | 2 | 30 min |
| **P0** | ComputeRelevance + ComputeDynamicMeshRelevance 路由 | 1 | 30 min |
| **P0** | MobileAllwayFrontMeshProcessor + 注册 | 1 (新建) | 2-3 h |
| **P0** | RenderAllwayFrontPass + 4 个分支插入 | 1 | 1 h |
| **P1** | SetupMobileBasePassAfterShadowInit Dispatch（可选优化） | 1 | 30 min |
| **P1** | 派生 SceneProxy 的 GetViewRelevance 检查 | N | 1-2 h |
| **P2** | SubpassHint 在不同平台/驱动的兼容测试 | - | 实测 |
| **P2** | PSO 缓存验证（确保确实复用 BasePass PSO） | - | 实测 |

**总计**：约 **6-9 个核心文件**修改 + **1 个新文件**，工作量约 **1-2 个工作日**。

---

# 5. 你必须做出的设计决策（影响实现细节）

| 问题 | 选项 | 推荐 |
|---|---|---|
| 深度测试行为？ | A. 完全 Always（覆盖所有）/ B. CF_DepthNearOrEqual（仍受深度遮挡）/ C. 自定义 fetch 测试 | **A**，符合 "always front" 语义 |
| 深度写入？ | A. 不写 / B. 写 | **不写**（subpass 1 是 DepthRead，写会报错） |
| 是否接受 Masked 材质？ | A. 仅 Opaque / B. Opaque+Masked | **B**，复用 BasePass 行为 |
| 是否参与阴影投射？ | A. 不投 / B. 投阴影 | **A**（always front 物体多用于 UI 元素，不需要阴影） |
| 是否参与 GBuffer / Lighting（仅 Deferred Mobile）？ | A. 跳过（写最终 SceneColor）/ B. 写 GBuffer | **A**，Translucency 之后 GBuffer 已不可写 |
| 排序方式？ | A. FrontToBack / B. BackToFront / C. 状态聚合 | **C**（StateBucket，最快） |
| 是否支持 ISR？ | A. 支持 / B. 不支持 | **A**，复用 BasePass 的 InstanceCullingMode |

---

# 6. 验证步骤建议

实现完之后，按这个顺序验证：

1. **编译通过**：`static_assert(EMeshPass::Num == ...)` 这种 ass 没炸 → 枚举改对了
2. **路由验证**：用 `r.RDGImmediateMode 1` + `r.ProfileGPU` 看到 `AllwayFrontPass` 事件
3. **物体可见**：放一个普通 cube + 设 `bRenderInAllwayFrontPass=true`，应该看不见（透明渲染前）→ 看见在透明物体之上
4. **PSO 复用**：用 `r.PSOPrecaching.LogStats 1` 确认没有新增大量 PSO（说明确实复用了 BasePass shader）
5. **Subpass 兼容**：移动设备实测 Mali / Adreno / Apple GPU，特别是 OpenGL ES 后端

---

# 7. 总结：回答你的核心问题

> **"需要牵扯这两个函数吗？"**

**是的，必须。** 但只是**两行 if + 几行 AddCommandsForMesh / PassMask.Set**，不是难点。

> **"或者需要修改其他部分吗？"**

**需要，至少 9 个区域** —— 但其中 6 个是**机械式扩展**（枚举 + UPROPERTY + ViewRelevance + 注册宏）。

**真正的难点在 2 个地方**：

1. **`FMobileAllwayFrontMeshProcessor` 的 shader 选择和 RenderState** —— 要正确复用 BasePass 的 shader template，同时 disable depth write
2. **Subpass 兼容性** —— Single Pass 模式下 AllwayFrontPass 必须能在 `DepthReadSubpass` 内运行，这需要在不同移动 GPU 上实测验证

如果需要现在就实施其中某个部分，建议从 `MeshPassProcessor.h` + `PrimitiveViewRelevance.h` 这两个最基础的开始，改动最小、影响最深。

---

## 附录：关键代码位置索引

| 文件 | 行号 | 内容 |
|---|---|---|
| `Engine/Source/Runtime/Renderer/Public/MeshPassProcessor.h` | 32-78 | EMeshPass 枚举定义 |
| `Engine/Source/Runtime/Renderer/Public/MeshPassProcessor.h` | 83-135 | GetMeshPassName switch |
| `Engine/Source/Runtime/Renderer/Public/MeshPassProcessor.h` | 2209-2249 | FPassProcessorManager + 注册宏 |
| `Engine/Source/Runtime/Renderer/Private/MobileBasePass.cpp` | 1218-1222 | MobileBasePass 注册示例 |
| `Engine/Source/Runtime/Engine/Public/PrimitiveViewRelevance.h` | 39-77 | FPrimitiveViewRelevance 位域 |
| `Engine/Source/Runtime/Renderer/Private/SceneVisibility.cpp` | 1299 | FRelevancePacket::ComputeRelevance |
| `Engine/Source/Runtime/Renderer/Private/SceneVisibility.cpp` | 1556-1577 | 静态网格 Mobile 路由分支 |
| `Engine/Source/Runtime/Renderer/Private/SceneVisibility.cpp` | 2186-2430 | ComputeDynamicMeshRelevance |
| `Engine/Source/Runtime/Renderer/Private/SceneVisibility.cpp` | 2228-2232 | 动态网格 Mobile 路由分支 |
| `Engine/Source/Runtime/Renderer/Private/SceneVisibility.cpp` | 4401-4470 | SetupMeshPasses |
| `Engine/Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp` | 377-427 | SetupMobileBasePassAfterShadowInit |
| `Engine/Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp` | 1578-1660 | RenderForwardSinglePass |
| `Engine/Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp` | 1662-1749 | RenderForwardMultiPass |
| `Engine/Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp` | 1947-1994 | RenderDeferredSinglePass |
| `Engine/Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp` | 1996-2068+ | RenderDeferredMultiPass |
