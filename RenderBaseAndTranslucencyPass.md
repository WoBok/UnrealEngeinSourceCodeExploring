# UE5.4 移动端：让指定不透明物体在透明渲染之后再绘制

> 目标平台：UE5.4 / 移动端 / Android
> 目标：让被标记的不透明物体在最初不渲染，等所有透明物体渲染完之后再渲染，并能遮挡任何透明物体。
> 实现方式：直接修改引擎源码，新增 `EMeshPass` + 组件开关 + 在移动端透明渲染之后插入新 Pass。

---

## 一、UE 是怎么区分"透明物体"和"不透明物体"的

UE 的"透明/不透明"分类**完全由材质的 Blend Mode 决定**，而不是由组件或者 Mesh 决定。整个链路如下：

### 1.1 材质层：`EBlendMode`

在 `Engine/Source/Runtime/Engine/Public/SceneTypes.h` 里定义：

```cpp
enum EBlendMode
{
    BLEND_Opaque,           // 不透明（写深度、写颜色）
    BLEND_Masked,           // Alpha Test 裁切（仍当作不透明渲染）
    BLEND_Translucent,      // 半透明（不写深度、按 alpha 混合）
    BLEND_Additive,         // 加色混合（透明类）
    BLEND_Modulate,         // 调制混合（透明类）
    BLEND_AlphaComposite,   // 预乘 alpha
    BLEND_AlphaHoldout,
    BLEND_TranslucentColoredTransmittance, // Substrate 新增
    BLEND_MAX,
};
```

只有 `BLEND_Opaque` 和 `BLEND_Masked` 走"不透明"流程，其余都走"透明 (Translucent)"流程。

### 1.2 材质相关性：`FMaterialRelevance`

`Engine/Source/Runtime/Engine/Public/MaterialShared.h`：

```cpp
struct FMaterialRelevance
{
    uint32 bOpaque : 1;
    uint32 bMasked : 1;
    uint32 bNormalTranslucency : 1;
    uint32 bSeparateTranslucency : 1;  // After DOF
    uint32 bTranslucencyModulate : 1;
    uint32 bPostMotionBlurTranslucency : 1;
    ...
};
```

每个 Primitive 在 `GetViewRelevance()` 里会基于材质 BlendMode 设置这些位，渲染器据此判断它要参与哪一个 Pass。

### 1.3 Mesh Pass 系统：`EMeshPass`

`Engine/Source/Runtime/Renderer/Public/MeshPassProcessor.h`：

```cpp
namespace EMeshPass
{
    enum Type : uint8
    {
        DepthPass,
        BasePass,                           // ←★ 不透明 + Masked 在这里
        AnisotropyPass,
        SkyPass,
        SingleLayerWaterPass,
        ...
        TranslucencyStandard,               // ←★ 透明在这里 (及其它 Translucency*)
        TranslucencyStandardModulate,
        TranslucencyAfterDOF,
        TranslucencyAfterDOFModulate,
        TranslucencyAfterMotionBlur,
        TranslucencyAll,
        ...
        CustomDepth,
        MobileInverseOpacity,
        ...
        Num,
        NumBits = 5,
    };
}
```

每个 EMeshPass 都有一个对应的 `FMeshPassProcessor` 子类负责"把一个 FMeshBatch + 材质 + Primitive，编译成一组 `FMeshDrawCommand`（绘制命令）"。

### 1.4 分类入口：`FMobileBasePassMeshProcessor::AddMeshBatch`

`Engine/Source/Runtime/Renderer/Private/MobileBasePassRendering.cpp`：

```cpp
void FMobileBasePassMeshProcessor::AddMeshBatch(const FMeshBatch& MeshBatch, ...)
{
    // 取材质和 BlendMode
    const FMaterial& Material = ...;
    const EBlendMode BlendMode = Material.GetBlendMode();

    // 这里实际上有判断：
    // - 如果是 Translucent/Additive/Modulate 等，TranslucencyPassType != TPT_AllTranslucency 时跳过
    // - 如果是 Opaque/Masked，才继续在 BasePass 处理
    bool bShouldDraw = (TranslucencyPassType == ETranslucencyPass::TPT_AllTranslucency)
                    ? IsTranslucentBlendMode(BlendMode)
                    : !IsTranslucentBlendMode(BlendMode);
    if (!bShouldDraw) return;
    ...
}
```

> 一句话总结：**UE 的"透明物体"和"不透明物体"是同一个 Mesh，根据它绑定的材质 BlendMode 被分别送入 BasePass 还是 Translucency Pass，分类完全在 MeshProcessor 阶段做。**

---

## 二、UE 在移动端是怎么把它们分到不同渲染阶段的

移动端 (Forward) 主流程在 `Engine/Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp` 的 `FMobileSceneRenderer::Render()` 里，简化后顺序：

```
FMobileSceneRenderer::Render(FRDGBuilder& GraphBuilder, ...)
├── InitViews()                          // 可见性裁剪，按 Pass 收集 MeshDrawCommand
├── RenderPrePass()                      // 可选深度预 Pass
├── RenderForward() / RenderDeferred()
│   ├── RenderMobileBasePass()           // ★ 不透明 + Masked（EMeshPass::BasePass）
│   ├── RenderDecals()
│   ├── RenderFog()
│   ├── RenderTranslucency()             // ★ 透明（EMeshPass::TranslucencyStandard 等）
│   └── RenderPostProcessing()
```

每个 `RenderXxx` 内部都是：

```cpp
View.ParallelMeshDrawCommandPasses[EMeshPass::BasePass]
    .DispatchDraw(nullptr, RHICmdList, &View.InstanceCullingDrawParams);
```

**关键点**：每个 View 上有一个 `ParallelMeshDrawCommandPasses[EMeshPass::Num]` 数组，`InitViews` 阶段把 Mesh 按"是否符合这个 Pass"分类塞进去；`RenderXxx` 时只是把对应 Pass 的命令派发出去。

> 所以，要让你的物体"在透明渲染完之后再画"，本质做两件事：
> 1. **不让它进入原本的 BasePass**；
> 2. **新建一个 EMeshPass，在 Translucency 之后调用它的 DispatchDraw**。

---

## 三、完整实现方案（直接改源码）

总览要做的事情：

| # | 改动 | 文件 |
|---|------|------|
| 1 | 给 `UPrimitiveComponent` 加一个 `bRenderAfterTranslucent` 开关 | `PrimitiveComponent.h/.cpp` |
| 2 | 把这个开关同步到 `FPrimitiveSceneProxy` | `PrimitiveSceneProxy.h/.cpp` |
| 3 | 给 `EMeshPass` 加一个枚举 `MobileAfterTranslucent` | `MeshPassProcessor.h/.cpp` |
| 4 | 让 `FMobileBasePassMeshProcessor` 跳过这些 Primitive | `MobileBasePassRendering.cpp` |
| 5 | 让同一个 MeshProcessor 在新增 Flag 下只处理这些 Primitive | `MobileBasePassRendering.h/.cpp` |
| 6 | 注册新 Pass 的 CreateFunction | `MobileBasePassRendering.cpp` |
| 7 | 在移动端 Renderer 里于 `RenderTranslucency` 之后派发新 Pass | `MobileShadingRenderer.cpp` |

下面逐步给出具体改动。**所有改动都给出文件路径 + 在哪里加 + 加什么。**

---

### 步骤 1：在 `UPrimitiveComponent` 上增加开关

#### 1.1 `Engine/Source/Runtime/Engine/Classes/Components/PrimitiveComponent.h`

找到一段已有的 `uint8 bXxx : 1;` 渲染开关聚集区（比如 `bRenderInMainPass`, `bRenderInDepthPass` 附近），加一行：

```cpp
public:
    /** 若为 true，则跳过 BasePass，最后在透明渲染完成后再渲染，且渲染在所有透明物体之上。仅移动端生效。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Rendering|Advanced", AdvancedDisplay)
    uint8 bRenderAfterTranslucent : 1;
```

#### 1.2 `Engine/Source/Runtime/Engine/Private/Components/PrimitiveComponent.cpp`

在 `UPrimitiveComponent::UPrimitiveComponent(const FObjectInitializer& ObjectInitializer)` 构造函数里初始化（找到其他 `bRenderInXxx = true;` 那一片）：

```cpp
bRenderAfterTranslucent = false;
```

---

### 步骤 2：把开关同步到 `FPrimitiveSceneProxy`

#### 2.1 `Engine/Source/Runtime/Renderer/Public/PrimitiveSceneProxy.h`

找到 `class FPrimitiveSceneProxy` 内部的一堆 `uint8 bXxx : 1;` 位字段（比如 `bRenderInMainPass`, `bRenderInDepthPass` 附近），加：

```cpp
public:
    /** 移动端：是否在透明渲染之后再渲染（参见 UPrimitiveComponent::bRenderAfterTranslucent）。 */
    uint8 bRenderAfterTranslucent : 1;
```

#### 2.2 `Engine/Source/Runtime/Renderer/Private/PrimitiveSceneProxy.cpp`

在 `FPrimitiveSceneProxy::FPrimitiveSceneProxy(const UPrimitiveComponent* InComponent, FName InResourceName)` 的初始化列表里加一项（找到一堆 `bRenderInXxx(InComponent->bRenderInXxx)` 旁边）：

```cpp
, bRenderAfterTranslucent(InComponent->bRenderAfterTranslucent)
```

> 这两步做完，引擎渲染线程上的 `FPrimitiveSceneProxy` 就拿到了游戏线程组件的开关。

---

### 步骤 3：新增 `EMeshPass` 枚举值

#### 3.1 `Engine/Source/Runtime/Renderer/Public/MeshPassProcessor.h`

找到 `namespace EMeshPass { enum Type : uint8 {...} }`，在 `Num` 之前、`#if WITH_EDITOR` 之前加一行：

```cpp
namespace EMeshPass
{
    enum Type : uint8
    {
        ...原有项...
        DitheredLODFadingOutMaskPass,

        // ★ 新增：移动端在 Translucency 之后再画的不透明 Pass
        MobileAfterTranslucent,

#if WITH_EDITOR
        EditorSelection,
        EditorLevelInstance,
#endif
        Num,
        NumBits = 5,  // 5 bits = 最多 32 个 pass，已有约 26~28 个，没问题
    };
}
```

> 如果你打开计算后 EMeshPass 项数超过 32，需要把 `NumBits` 改成 6，并搜全代码看哪里用了 5 这个常量（实际情况 5.4 不会超）。

#### 3.2 `Engine/Source/Runtime/Renderer/Private/MeshPassProcessor.cpp`

找到 `const TCHAR* GetMeshPassName(EMeshPass::Type MeshPass)` 函数（或类似 switch），加一个 case：

```cpp
case EMeshPass::MobileAfterTranslucent: return TEXT("MobileAfterTranslucent");
```

如果文件里还有 `EMeshPassName` 数组，对应位置补一行 `TEXT("MobileAfterTranslucent"),`。

---

### 步骤 4：让原 BasePass 跳过这些 Primitive

#### 4.1 `Engine/Source/Runtime/Renderer/Private/MobileBasePassRendering.h`

找到：

```cpp
class FMobileBasePassMeshProcessor : public FMeshPassProcessor
{
public:
    enum class EFlags
    {
        None = 0,
        CanUseDepthStencil  = (1 << 0),
        ForcePassDrawRenderState = (1 << 1),
    };
    ...
```

在 `EFlags` 里加一项：

```cpp
    enum class EFlags
    {
        None = 0,
        CanUseDepthStencil       = (1 << 0),
        ForcePassDrawRenderState = (1 << 1),

        // ★ 新增：本 MeshProcessor 实例用于"透明后不透明 Pass"，
        //   开启时只收集 bRenderAfterTranslucent 的 Primitive；关闭时跳过这些 Primitive。
        AfterTranslucentPass     = (1 << 2),
    };
```

`ENUM_CLASS_FLAGS(FMobileBasePassMeshProcessor::EFlags)` 已经存在，不用改。

#### 4.2 `Engine/Source/Runtime/Renderer/Private/MobileBasePassRendering.cpp`

找到 `void FMobileBasePassMeshProcessor::AddMeshBatch(const FMeshBatch& RESTRICT MeshBatch, uint64 BatchElementMask, const FPrimitiveSceneProxy* RESTRICT PrimitiveSceneProxy, int32 StaticMeshId)` 的**最开头**（在所有早退判断之前），加：

```cpp
void FMobileBasePassMeshProcessor::AddMeshBatch(
    const FMeshBatch& RESTRICT MeshBatch,
    uint64 BatchElementMask,
    const FPrimitiveSceneProxy* RESTRICT PrimitiveSceneProxy,
    int32 StaticMeshId)
{
    // ===== ★ 新增：按 bRenderAfterTranslucent 进行 Pass 路由 =====
    const bool bIsAfterTranslucentPass =
        EnumHasAnyFlags(Flags, EFlags::AfterTranslucentPass);

    if (PrimitiveSceneProxy)
    {
        if (bIsAfterTranslucentPass)
        {
            // 这个 MeshProcessor 实例是为"透明后不透明 Pass"准备的：
            // 只收集 bRenderAfterTranslucent 的 Primitive。
            if (!PrimitiveSceneProxy->bRenderAfterTranslucent)
            {
                return;
            }
        }
        else
        {
            // 普通 BasePass：跳过被标记为"透明后渲染"的 Primitive。
            if (PrimitiveSceneProxy->bRenderAfterTranslucent)
            {
                return;
            }
        }
    }
    // ===== ★ 新增结束 =====

    // ...原有代码继续...
}
```

> 这样：被标记的物体**不再进入原本的 BasePass**，它的 MeshDrawCommand 会留给我们新注册的那个 Pass 去收。

---

### 步骤 5：为新 Pass 注册 CreateFunction

#### 5.1 仍在 `Engine/Source/Runtime/Renderer/Private/MobileBasePassRendering.cpp`

找到现有的 `CreateMobileBasePassProcessor`（移动端 BasePass 注册函数），仿写一个新的，放在它附近：

```cpp
// ★ 新增：移动端"透明后不透明 Pass"的创建函数
FMeshPassProcessor* CreateMobileAfterTranslucentPassProcessor(
    ERHIFeatureLevel::Type FeatureLevel,
    const FScene* Scene,
    const FSceneView* InViewIfDynamicMeshCommand,
    FMeshPassDrawListContext* InDrawListContext)
{
    FMeshPassProcessorRenderState DrawRenderState;

    // 关键：深度测试 ALWAYS，且写深度 —— 保证"始终画在最前面"。
    // 颜色用不透明 BlendState（不混合，直接覆盖）。
    DrawRenderState.SetDepthStencilState(
        TStaticDepthStencilState<true /*bDepthWrite*/, CF_Always>::GetRHI());
    DrawRenderState.SetBlendState(TStaticBlendState<>::GetRHI());

    // 复用 BasePass 的 MeshProcessor，只通过 Flag 切到"透明后"模式。
    // 第二个 bool 参数（如果存在）请与原 CreateMobileBasePassProcessor 保持一致；
    // 这里以最常见的 5.4 签名为例：
    return new FMobileBasePassMeshProcessor(
        EMeshPass::MobileAfterTranslucent,
        FeatureLevel,
        Scene,
        InViewIfDynamicMeshCommand,
        DrawRenderState,
        InDrawListContext,
        ETranslucencyPass::TPT_MAX,                                // 不透明流程
        FMobileBasePassMeshProcessor::EFlags::CanUseDepthStencil
        | FMobileBasePassMeshProcessor::EFlags::ForcePassDrawRenderState   // 强制使用我们设置的 DrawRenderState
        | FMobileBasePassMeshProcessor::EFlags::AfterTranslucentPass);
}

// ★ 注册：把新枚举绑到上面这个工厂
REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR(
    MobileAfterTranslucentPass,
    CreateMobileAfterTranslucentPassProcessor,
    EShadingPath::Mobile,
    EMeshPass::MobileAfterTranslucent,
    EMeshPassFlags::CachedMeshCommands | EMeshPassFlags::MainView);
```

> 说明：
> - `FRegisterPassProcessorCreateFunction` / `REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR` 是 UE 用来把 EMeshPass 和工厂函数挂钩的注册机制，**注册以后**，`InitViews` 阶段会自动让所有被标记 `bRenderAfterTranslucent` 的 Primitive 走进我们这个 MeshProcessor。
> - 如果你 IDE 里看到 `CreateMobileBasePassProcessor` 的签名与上面略有不同（5.4 里有过细调），**完全照搬旁边那一个原函数的签名**就行，只改 EFlags 和 EMeshPass。

#### 5.2 注意：深度处理可选增强

上面的 `CF_Always` 表示**永远通过深度测试**，因此你的物体一定盖在所有东西之上；但**同一个 Pass 内部多个被标记物体之间不再按深度排序**，而是按提交顺序覆盖。如果你只需要**单个**物体或者它们彼此之间不重叠，这就够了。

如果你需要它们彼此之间也按深度正确排序，可以把这一 Pass 拆成两步（DepthPrepass + ColorPass），都用 `CF_Always` 写深度做"清零"，再用 `CF_DepthNearOrEqual` 画颜色 —— 步骤 7 那里会预留这个位置。

---

### 步骤 6：在移动端 Renderer 里派发新 Pass

#### 6.1 `Engine/Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp`

找到 `FMobileSceneRenderer::Render(...)`（或 `RenderForward()` —— 5.4 移动端主流程在这里），里面会看到类似：

```cpp
RenderMobileBasePass(GraphBuilder, ...);
...
RenderTranslucency(GraphBuilder, ...);
// 在这之后是后处理
RenderPostProcessing(...);
```

在 `RenderTranslucency(...)` 之后、`RenderPostProcessing(...)` 之前，加一行：

```cpp
RenderTranslucency(GraphBuilder, ...);   // 原有

// ★ 新增：在所有透明渲染完成之后，把被标记的不透明物体画到最前
RenderMobileAfterTranslucentPass(GraphBuilder, SceneTextures, Views);

RenderPostProcessing(...);               // 原有
```

`Views` 替换成实际可用的视图集合（5.4 里通常是 `Views`、`AllViews` 或 `View` —— 跟前后行保持一致即可）。

#### 6.2 在 `MobileShadingRenderer.cpp` 文件末尾（或 anonymous namespace 里）增加函数实现：

```cpp
// ★ 新增：派发 EMeshPass::MobileAfterTranslucent 的 MeshDrawCommands
void FMobileSceneRenderer::RenderMobileAfterTranslucentPass(
    FRDGBuilder& GraphBuilder,
    FSceneTextures& SceneTextures,
    TArrayView<FViewInfo> InViews)
{
    for (FViewInfo& View : InViews)
    {
        FParallelMeshDrawCommandPass& Pass =
            View.ParallelMeshDrawCommandPasses[EMeshPass::MobileAfterTranslucent];

        if (!Pass.HasAnyDraw())
        {
            continue;
        }

        Pass.BuildRenderingCommands(GraphBuilder, Scene->GPUScene, View.DynamicPrimitiveCollector.GetInstanceSceneDataOffset(), View.DynamicPrimitiveCollector.NumInstances(), View.InstanceCullingDrawParams);

        auto* PassParameters = GraphBuilder.AllocParameters<FMobileBasePassParameters>();
        PassParameters->View = View.GetShaderParameters();
        PassParameters->MobileBasePass = CreateMobileBasePassUniformBuffer(GraphBuilder, View, EMobileBasePass::Opaque, EMobileSceneTextureSetupMode::All);
        PassParameters->InstanceCullingDrawParams = View.InstanceCullingDrawParams;
        PassParameters->RenderTargets[0] = FRenderTargetBinding(
            SceneTextures.Color.Target, ERenderTargetLoadAction::ELoad);
        PassParameters->RenderTargets.DepthStencil = FDepthStencilBinding(
            SceneTextures.Depth.Target,
            ERenderTargetLoadAction::ELoad,
            ERenderTargetLoadAction::ELoad,
            FExclusiveDepthStencil::DepthWrite_StencilWrite);

        GraphBuilder.AddPass(
            RDG_EVENT_NAME("MobileAfterTranslucent"),
            PassParameters,
            ERDGPassFlags::Raster,
            [&View, PassParameters](FRHICommandList& RHICmdList)
            {
                PassParameters->InstanceCullingDrawParams = View.InstanceCullingDrawParams;
                View.ParallelMeshDrawCommandPasses[EMeshPass::MobileAfterTranslucent]
                    .DispatchDraw(nullptr, RHICmdList, &PassParameters->InstanceCullingDrawParams);
            });
    }
}
```

并在 `MobileShadingRenderer.h` 里加声明（在 `class FMobileSceneRenderer` 的 `private:` 区域）：

```cpp
private:
    // ★ 新增
    void RenderMobileAfterTranslucentPass(
        FRDGBuilder& GraphBuilder,
        FSceneTextures& SceneTextures,
        TArrayView<FViewInfo> InViews);
```

> 上面的 `FMobileBasePassParameters` / `CreateMobileBasePassUniformBuffer` / `FMobileSceneTextureSetupMode` 名字在 5.4 里就是这些；如果你的 5.4 里它们叫 `FMobileBasePassUniformParameters` / `EMobileSceneTextureSetupMode::All`，**直接照搬同文件里 `RenderMobileBasePass` 里那一段构造 PassParameters 的代码即可**——你只需要确保：
> - RenderTargets[0] 绑定 SceneColor (Load)
> - RenderTargets.DepthStencil 绑定 SceneDepth (Load, DepthWrite)
> - 调用 `View.ParallelMeshDrawCommandPasses[EMeshPass::MobileAfterTranslucent].DispatchDraw(...)`

---

## 四、使用方式

源码编译完之后，引擎里就有了一个新的开关。使用步骤：

1. 选中你想要"最后渲染、压在所有透明物体之上"的那个 Actor 的 `StaticMeshComponent` / `SkeletalMeshComponent`。
2. 在 Details 面板 → `Rendering → Advanced` 里勾上 **Render After Translucent**。
3. 这个物体的材质本身保持不透明（`BLEND_Opaque` 或 `BLEND_Masked`）即可 —— 它会被我们这条新链路接管。

蓝图里也可以直接：

```
SetRenderAfterTranslucent(true)
```

（因为我们加了 `BlueprintReadWrite`。如果你想用 setter 而不是直接赋值，自己加一个 `UFUNCTION` 包装即可。）

---

## 五、构建与验证

1. **编译目标**：`Development Editor` + Android target。
2. **关键校验点**：
   - 打开 RenderDoc / Mali Graphics Debugger / Snapdragon Profiler 抓帧；
   - 看到事件序列大致是：`MobileBasePass` → ... → `Translucency` → `MobileAfterTranslucent`；
   - 在 `MobileAfterTranslucent` 这一步，被标记物体被绘制；
   - 之前已经画好的半透明物体被它的像素覆盖。
3. **可能踩的坑**：
   - **物体完全不显示**：先确认 `bRenderAfterTranslucent` 在 SceneProxy 上是 true（可在 `FPrimitiveSceneProxy` 构造里加一个 `UE_LOG`）；其次确认它的材质 BlendMode 是 Opaque/Masked，不要是 Translucent。
   - **物体仍然被场景里的不透明物体遮挡**：说明深度测试没生效成 `CF_Always`。检查 `EFlags::ForcePassDrawRenderState` 有没有被 `FMobileBasePassMeshProcessor` 内部覆盖；必要时在 `Process()` 里直接强写 `DrawRenderState.SetDepthStencilState(...)`。
   - **VR / 多 View 场景**：上面循环用 `InViews`，多 View 没问题；但记得各 View 自己的 `InstanceCullingDrawParams`。
   - **Vulkan 的 RenderPass 合并**：移动端 Vulkan 上，若上一帧 Translucency 在一个 RenderPass 内、你这个 Pass 又用 `ELoad` 进同一附件，Vulkan 后端可能会自动合并 RenderPass —— 性能更好，没坏处。
   - **EMeshPass 位宽**：如步骤 3.1 提到的，确认 `NumBits = 5` 足够；5.4 中是足够的，但如果你的引擎里还合并过其它修改导致 EMeshPass 数 > 32，改为 6 并搜全代码 `5` 这个魔数。

---

## 六、原理小结（一句话回顾整条链路）

> UE 在 `InitViews` 阶段，根据每个 Primitive 的材质 BlendMode + 我们新加的 `bRenderAfterTranslucent`，把它的 MeshBatch 分发给不同的 `FMeshPassProcessor`，编译成 `FMeshDrawCommand` 放到对应的 `EMeshPass` 槽位上；移动端渲染时按 BasePass → Translucency → **(我们新增) MobileAfterTranslucent** → PostProcessing 的顺序依次 DispatchDraw。我们做的事情就是：**多挖一个 EMeshPass 槽、按开关把物体转进去、在 RenderTranslucency 之后多调一次 DispatchDraw，并强制深度测试 ALWAYS + 写深度**。

按这套改完，你就有了一个**只动引擎源码、不依赖任何材质或蓝图 trick**的最直接方案：被你勾选的不透明物体，会在所有透明物体渲染完之后再画一次，并且永远在最前。

---

## 七、文件改动清单（速查）

| 文件 | 改动类型 | 关键内容 |
|------|----------|---------|
| `Engine/Source/Runtime/Engine/Classes/Components/PrimitiveComponent.h` | 新增字段 | `uint8 bRenderAfterTranslucent : 1;` (UPROPERTY) |
| `Engine/Source/Runtime/Engine/Private/Components/PrimitiveComponent.cpp` | 构造初始化 | `bRenderAfterTranslucent = false;` |
| `Engine/Source/Runtime/Renderer/Public/PrimitiveSceneProxy.h` | 新增字段 | `uint8 bRenderAfterTranslucent : 1;` |
| `Engine/Source/Runtime/Renderer/Private/PrimitiveSceneProxy.cpp` | 构造初始化列表 | `, bRenderAfterTranslucent(InComponent->bRenderAfterTranslucent)` |
| `Engine/Source/Runtime/Renderer/Public/MeshPassProcessor.h` | 新增枚举 | `MobileAfterTranslucent,` |
| `Engine/Source/Runtime/Renderer/Private/MeshPassProcessor.cpp` | 名字映射 | `case EMeshPass::MobileAfterTranslucent: return TEXT("MobileAfterTranslucent");` |
| `Engine/Source/Runtime/Renderer/Private/MobileBasePassRendering.h` | 新增 EFlag | `AfterTranslucentPass = (1 << 2),` |
| `Engine/Source/Runtime/Renderer/Private/MobileBasePassRendering.cpp` | AddMeshBatch 路由 + 注册新 Pass 工厂 | 详见步骤 4.2 / 5.1 |
| `Engine/Source/Runtime/Renderer/Private/MobileShadingRenderer.h` | 新增函数声明 | `void RenderMobileAfterTranslucentPass(...)` |
| `Engine/Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp` | 实现 + 在 RenderTranslucency 之后调用 | 详见步骤 6.1 / 6.2 |
