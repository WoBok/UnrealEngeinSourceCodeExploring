# 方案分析:UE5.4 移动端"透明后渲染不透明"Pass 改造

> 分析对象:`Docs/Plan.md`(v1,作者:CX)
> 分析日期:2026-06-22
> 分析范围:基于本仓库 `Source/Runtime/Renderer/...` 与 `Source/Runtime/Engine/...` 的当前代码逐条核验

## 1. 方案思路与核心问题还原

目标可以抽象成下面这张管线图(移动端 Forward 路径):

```
Depth PrePass (MaskedOnly)
  └─ MaskedOpaque
Opaque Subpass (BasePass)        ← 这里要"剔除" bRenderAfterTranslucency == true 的物体
Translucency Subpass             ← 沿用现有 RenderTranslucency
  └─ Translucent (depth-read, no depth write)
After-Translucency Subpass       ← 新加,只渲染被标记的物体,深度测试 Cf_DepthNearOrEqual,深度不写
Tone-map / Custom Resolve
```

**作者核心依赖的事实**:在移动端,Translucency 默认走 `FExclusiveDepthStencil::DepthRead_StencilRead`(只读深度),所以新加的 pass 把 `TStaticDepthStencilState<true, CF_DepthNearOrEqual>`(与 BasePass 一致)套上去,既可以与已写好的不透明深度做比较,又不会被自身写深度扰乱。这条逻辑**是成立的**,所以设计主线 OK。但落地时,作者只覆盖了 Forward 路径,其它 3 条相关路径都漏了(见下表)。

> 备注:用户文档里的 `Engine/Source/...` 路径写错了,实际仓库里的相对路径是 `Source/...`(根目录就是工程根,本身就是 Source)。下文中所有"实际文件"路径按 `Source/...` 给出。

## 2. 现有代码与作者方案逐项核验

### 2.1 EMeshPass 新增条目 — `Source/Runtime/Renderer/Public/MeshPassProcessor.h:32`

- 作者方案:新增 `MobileAfterTranslucencyPass`,把底部 `static_assert` 改成 `Num == 33` / `Num == 33 + 4`。
- 实际代码:`EMeshPass::Type` 在 `WaterInfoTexturePass` 之后是 32 项(无 editor);加上 `MobileAfterTranslucencyPass` 后 `Num` 变 33,editor 下变 37。作者的目标值**正确**。
- **隐藏坑**:`static_assert` 里的 GUID `{674D7D62-CFD8-4971-9A8D-CD91E5612CD8}` 必须原样保留,它是 Epic 用来阻止 GUID 自动合并的锚点,删了/改了会触发 LWC auto-resolve 误判。
- `NumBits = 6` (支持 64 项)够用,不需要扩大。

### 2.2 PrimitiveComponent 字段 — `Source/Runtime/Engine/Classes/Components/PrimitiveComponent.h:408`

- 实际位置:在 `bRenderInMainPass:1` 之后直接追加 `bRenderAfterTranslucency:1` 即可(行号 ±2)。
- `SetRenderAfterTranslucency` 写到 `PrimitiveComponent.cpp:4457` 附近,**要和 `bRenderInMainPass` 的 setter 行为完全对齐**(`MarkRenderStateDirty()`),作者给出的实现一致。
- **缺失**:构造函数 `UPrimitiveComponent::UPrimitiveComponent()` 里要追加 `bRenderAfterTranslucency = false;`(作者补了 `:333`),同时需要检查是否有子类的构造函数显式初始化 `bRenderInMainPass` 把这块也覆盖了 —— 例如 `UInstancedStaticMeshComponent`、`UHISMComponent` 等,如果有就要一起加。建议用 `rg "\.cpp:.*bRenderInMainPass\s*=\s*"` 扫一遍,基本只有 `PrimitiveComponent.cpp:333` 一处。

### 2.3 PrimitiveSceneProxy / Desc — `PrimitiveSceneProxy.h:1200` / `PrimitiveSceneProxyDesc.h:93`

- 行号核实:`PrimitiveSceneProxy.h:1200` 实际是 `bRenderInDepthPass`,`bRenderInMainPass` 紧跟其后在 1202 行;`PrimitiveSceneProxyDesc.h:93` 是 `bRenderInDepthPass`,`bRenderInMainPass` 在 94 行;`PrimitiveSceneProxy.cpp:277`(`InitializeFrom`)与 426(`FPrimitiveSceneProxy(InProxyDesc,…)` 构造)实际行号都对得上。
- 字段类型都用 `uint8 : 1` / `uint32 : 1`,保持一致即可。
- **`ShouldRenderAfterTranslucency()`** 不要复制 `ShouldRenderInMainPass()` 的注释,要在 doc-comment 里写清楚:**该标志控制 pass 分流,默认 false,仅移动端有效,Deferred 路径不生效**(下面 §4.2 解释原因)。

### 2.4 PrimitiveViewRelevance — `PrimitiveViewRelevance.h:54`

- 该结构是 bitfield 且 `operator|=` 走 memcmp-like 内存按位或;`bRenderAfterTranslucency : 1` 加在 `bRenderInMainPass` 之后是安全的。
- **构造默认值 = false** 必须显式设,否则 `FPrimitiveViewRelevance()` 里那段 `memset(this, 0, sizeof(*this))` 会让 32-bit `bRenderInMainPass = true` 那段也"OK",但**新加的字段是 false 也要显式声明**,这是代码风格而非必须(很多字段就不显式写)。为与 `bRenderInMainPass` 风格对齐,显式 `= false` 即可。
- **关键风险**:这结构是很多地方按位或出来的,`AddMeshBatch` 拿到的 `ViewRelevance` 也用 |= 累加;如果新加字段在 |= 期间有未初始化内存,后果是**脏读**。本结构 `memset(0) + 显式 bRenderInMainPass=true` 写得很保守,沿用即可。

### 2.5 StaticMeshRender.cpp / SkeletalMesh.cpp 写入 `ViewRelevance`

- 实际行号:`StaticMeshRender.cpp:2062`(`FStaticMeshSceneProxy::GetViewRelevance`)和 `SkeletalMesh.cpp:7115`(`FSkeletalMeshSceneProxy::GetViewRelevance`)。作者的方案准确。
- **额外遗漏**:
  - `FStaticMeshSceneProxy::GetMeshElement`(`StaticMeshRender.cpp` 多处)不涉及 relevance,不用改。
  - `LandscapeRender.cpp` 等使用自定义 SceneProxy 的组件,会通过自己的 `GetViewRelevance` 设置 bRenderAfterTranslucency;**默认 false 即可**,但 `FLandscapeSceneProxy::GetViewRelevance` 里需要看一下是否漏掉。`rg "Result\.bRenderInMainPass\s*=" Source/Runtime` 检查一下:仅 StaticMesh/SkeletalMesh 显式赋值,其它 Proxy 是继承 / 拷贝 `bRenderInMainPass = MaterialRelevance.bOpaque && ...`,这种自动走"= false"路径的不要紧,但**InstancedStaticMesh / HISM / Niagara / GeometryCollection** 都有自己的 `GetViewRelevance`,需要 grep 一遍手动补:
  ```
  rg "FPrimitiveViewRelevance.*::GetViewRelevance" Source/Runtime
  ```
  重点关注 `InstancedStaticMesh.cpp`、`SkeletalMesh.cpp`、`GeometryCollection.cpp`、`CableComponent`、`Foliage`、`PaperSprite` 等。**任何一个漏掉都会导致 SetRenderAfterTranslucency 视觉上不生效**。

### 2.6 MobileBasePassMeshProcessor 构造改造 — `Source/Runtime/Renderer/Private/MobileBasePass.cpp:810`

- 实际行号:构造函数在 810 行附近,正确。新增 `bool IsAfterTranslucencyBasePass` 参数(默认值 false)合理。
- 关键派生关系:`bTranslucentBasePass = (InTranslucencyPassType != TPT_MAX)`。新加 `bAfterTranslucencyBasePass` 之后,新 pass 的 `bTranslucentBasePass = false`(`TPT_MAX` 是默认值),`bPassUsesDeferredShading = bDeferredShading`,`bDeferredShading` 在移动端 deferred 路径上为 true,这就是**§4.2 deferred 路径问题的根源**。
- `ShouldDraw(Material)` 在 `bTranslucentBasePass == false` 分支返回 `!bIsTranslucent`,即只画不透明材质;translucent 材质直接被丢弃,无需在 `AddMeshBatch` 顶层的 filter 额外加材质判断,作者只判断 `bShouldRenderAfterTranslucency` 是够用的。

### 2.7 `AddMeshBatch` 顶部分流 — `MobileBasePass.cpp:867`

- 作者写法:
  ```cpp
  bool bShouldRenderAfterTranslucency = PrimitiveSceneProxy->ShouldRenderAfterTranslucency();
  if (bAfterTranslucencyBasePass) {
      if (!bShouldRenderAfterTranslucency) return;
  } else {
      if (bShouldRenderAfterTranslucency) return;
  }
  ```
- **问题 1(逻辑)**:这个检查发生在 `MeshBatch.bUseForMaterial`、`!PrimitiveSceneProxy->ShouldRenderInMainPass()` 之前;顺序上,只要 material 失效或者组件 `bRenderInMainPass=false`,就应该直接 return 而不是再判 after-translucency。建议放在原 return 之后,语义上更对称。
- **问题 2(性能)**:对于一个全是 bRenderAfterTranslucency 物体的场景,**BasePass processor 仍然会被注册、创建并对所有 StaticMesh 调用 `AddMeshBatch` 然后被立刻 reject**,等价于"白做工"。最干净的做法是把"分流"挪到 `SceneVisibility.cpp` 的 `AddCommandsForMesh` 上(见 §2.9),这里只保留保险的 `bRenderInMainPass == false → return`。
- **问题 3(InstancedStaticMesh 边界)**:`AddMeshBatch` 的 `PrimitiveSceneProxy` 在 instanced static mesh 路径下可能被替换为 `HISMProxy` / `ISMCProxy`,**`ShouldRenderAfterTranslucency` 必须**在父组件的 `FInstancedStaticMeshSceneProxy` 上转发到 Component,而不是仅靠基类。检查 `InstancedStaticMesh.cpp` 里的 `FInstancedStaticMeshSceneProxy::ShouldRenderInMainPass()` 实现(它本来就是从 `FStaticMeshSceneProxy` 继承并 override 的),`ShouldRenderAfterTranslucency` 不需要 override,默认会读取 Component 的值,**OK**。

### 2.8 CreateMobileAfterTranslucencyPassProcessor + REGISTER — `MobileBasePass.cpp:1123/1151`

- 实际行号:`CreateMobileBasePassProcessor` 在 1148 行;`REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR` 在 1218 行(注意 plan 文档里写的 1123 是 5.3 早期版本,当前已偏移)。
- **PassFlag 选错的风险**:作者选了 `EMeshPassFlags::CachedMeshCommands | EMeshPassFlags::MainView`。
  - `CachedMeshCommands`:开启 PSO 缓存与命令缓存(后续同 material + 同 mesh 不重新编译,这是默认 mobile 必备)。
  - `MainView`:这个 pass 只走主 view,不参与 SceneCapture / Reflection / PlanarReflection。
  - SceneCapture 不走这个新 pass 没问题,但 **VR 中通常会做 Mirror/SceneCapture 调试面板**——它们的 SceneRenderer 不是 FMobileSceneRenderer 而是 FSceneRenderer,本改动不影响;但若未来加 `FOpenColorIODisplayExtension` 之类 hook 走 SceneCapture,它们就拍不到被标记的物体。这是**可接受的取舍**。
- **遗漏**:这个新 pass 应当在 `BuildInstanceCullingDrawParams()`(`MobileShadingRenderer.cpp:1436`)里追加一次 `BuildRenderingCommands`,否则 `DispatchDraw` 时 `ParallelMeshDrawCommandPasses[EMeshPass::MobileAfterTranslucencyPass]` 的 culling 结果是空的。**作者 plan 里完全没提**,下面 §3.1 单独列出。

### 2.9 SceneVisibility.cpp — 静态路径 (`:1564` 附近) 与 动态路径 (`:2211` 附近)

- 实际行号:静态 1556,动态 2211(plan 写 1564 / 2211 附近,微偏)。
- **静态路径问题**:
  ```cpp
  if (StaticMeshRelevance.bUseForMaterial && (ViewRelevance.bRenderInMainPass || ViewRelevance.bRenderCustomDepth))
  {
      if (ShadingPath == EShadingPath::Mobile) {
          if (!StaticMeshRelevance.bUseSkyMaterial) {
              if (ViewRelevance.bRenderAfterTranslucency) {
                  DrawCommandPacket.AddCommandsForMesh(..., EMeshPass::MobileAfterTranslucencyPass);
              }
              DrawCommandPacket.AddCommandsForMesh(..., EMeshPass::BasePass);   // ← 仍走 BasePass
              if (!bMobileBasePassAlwaysUsesCSM) { /* CSM */ }
          }
      }
  }
  ```
  - **问题 A:重复入队**。当 `bRenderAfterTranslucency == true` 时,被标记的 mesh 仍然会进 BasePass 命令包;`AddMeshBatch` 顶部的 filter 才会拒掉,等于每个被标记物体都付两次 culling 成本。改成 `if/else` 更合理:
    ```cpp
    if (ViewRelevance.bRenderAfterTranslucency) {
        DrawCommandPacket.AddCommandsForMesh(..., EMeshPass::MobileAfterTranslucencyPass);
    } else {
        DrawCommandPacket.AddCommandsForMesh(..., EMeshPass::BasePass);
        if (!bMobileBasePassAlwaysUsesCSM) {
            DrawCommandPacket.AddCommandsForMesh(..., EMeshPass::MobileBasePassCSM);
        }
    }
    ```
  - **问题 B:DepthPass / Velocity / 静态深度准备 路径没考虑**。上面这段之上还有一段 depth pre-pass:
    ```cpp
    if (StaticMeshRelevance.bUseForDepthPass && (bDrawDepthOnly || (bMobileMaskedInEarlyPass && ViewRelevance.bMasked)))
    ```
    `bDrawDepthOnly` 在 mobile 上**只有 `bIsFullDepthPrepassEnabled` 时才为 true**(`SceneVisibility.cpp:1427`),普通 forward + 不开 MaskedOnly prepass 的工程,这里根本进不去。**所以普通 mobile forward 路径下,被标记物体的深度**从来不会通过 DepthPrePass 写入(它只走 Opaque BasePass 时写)。
    - 含义:BasePass 写深度 → 透明只读 → after-translucency 用 `CF_DepthNearOrEqual` 测,**作者的核心假设成立**。
    - 风险:如果工程开启了 `r.EarlyZPass = 4 (DDM_AllOpaque)`,**被标记物体也会进 DepthPrePass**,提前写自己的深度,后文 §4.1 会讲这个 case 出现的视觉不一致。
  - **问题 C:Velocity 路径**。Static 路径上方有:
    ```cpp
    if (StaticMeshRelevance.bUseForMaterial && ViewRelevance.bRenderInMainPass) {
        if (ViewRelevance.HasVelocity()) {
            // OpaqueVelocityMeshProcessor / TranslucentVelocityMeshProcessor
        }
    }
    ```
    作者加的 `bRenderAfterTranslucency` 判断在外层,所以被标记物体**不会**进 Velocity pass。在 `bRenderAfterTranslucency == true && bRenderInMainPass == true` 的设计下(plan 默认这个语义),本来也不需要——因为没进 BasePass 自然没 Opaque velocity 输出,运动模糊就丢了。**建议:把 `|| ViewRelevance.bRenderAfterTranslucency` 也补上**,让被标记的移动物体会进 TranslucentVelocity(因为它们对 motion blur 的反应是"晚于 translucency 但仍要被采样"),否则 VR 里 TAA / DLSS / Motion Blur 全部失效。

- **动态路径问题**:`ComputeDynamicMeshRelevance` 的写法同 §2.9 问题 A,会重复入队,同样改成 `if/else`。
  - **更大的坑**:动态路径还有一个外层 gate 是 `if (ViewRelevance.bRenderInMainPass || ViewRelevance.bRenderCustomDepth)`(`SceneVisibility.cpp:2211`),作者把新分支写在它**内部**。这等于强制要求 `bRenderAfterTranslucency == true` 的物体 `bRenderInMainPass` 必须为 true 才能进新 pass——但用户从语义上想表达的恰恰是"不要进 main pass"。两种选择都行,但**请二选一并在代码注释里写清楚默认**(推荐:`bRenderInMainPass` 走传统 main pass;`bRenderAfterTranslucency` 走 after-translucent pass;两者互斥但允许同时为 true 然后用 OR 优先级由 SceneVisibility 决定)。
  - **更更更大的坑**:动态路径上面这一段还会给 `MobileBasePassCSM`、`SkyPass`、`AnisotropyPass` 排队。如果 `bRenderAfterTranslucency == true` 物体**只在 new pass 出现**,它就**不会**被 CSM 采样,**不会**投到 sky 反射、不会**进 AnisotropyPass**。考虑场景:VR 手柄模型高亮标记 → 在金属反射 / Roughness 反射里看不到自己。需要权衡:
    - 不进 CSM:正确,新 pass 渲染时它本来就不该投阴影(避免"半透明投射的硬阴影"破缺)。
    - 不进反射 / anisotropy:**需要**额外决策,建议在 `ViewRelevance::bRenderInMainPass == false && bRenderAfterTranslucency == true` 的物体上,显式排到 `ReflectionCapture` 队列(查 `SceneVisibility.cpp` 找一下 `bRenderInMainPass` 的反射分支,通常在 `ComputeStaticMeshRelevance`/`ComputeDynamicMeshRelevance` 之外)。

## 3. 必须额外补的修改(plan 没列)

### 3.1 `BuildInstanceCullingDrawParams` 漏掉

`Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp:1436`:

```cpp
void FMobileSceneRenderer::BuildInstanceCullingDrawParams(FRDGBuilder& GraphBuilder, FViewInfo& View, FMobileRenderPassParameters* PassParameters)
{
    if (Scene->GPUScene.IsEnabled())
    {
        if (!bIsFullDepthPrepassEnabled) { ... DepthPass ... }
        View.ParallelMeshDrawCommandPasses[EMeshPass::BasePass].BuildRenderingCommands(...);
        View.ParallelMeshDrawCommandPasses[EMeshPass::SkyPass].BuildRenderingCommands(...);
        View.ParallelMeshDrawCommandPasses[StandardTranslucencyMeshPass].BuildRenderingCommands(...);
        View.ParallelMeshDrawCommandPasses[EMeshPass::DebugViewMode].BuildRenderingCommands(...);
        //  ← 这里要追加 ↓
        View.ParallelMeshDrawCommandPasses[EMeshPass::MobileAfterTranslucencyPass].BuildRenderingCommands(
            GraphBuilder, Scene->GPUScene, ???);
    }
}
```

参数需要一个 `FInstanceCullingDrawParams`(每 pass 一份),参考其他 pass 在 `SceneRendering.h:2792` 附近的私有成员声明,加一个 `FInstanceCullingDrawParams AfterTranslucencyInstanceCullingDrawParams;`,然后在 `BuildInstanceCullingDrawParams` 里调用。

**作者 plan 把 `RenderMobileAfterTranslucencyPass(RHICmdList, View, &PassParameters->InstanceCullingDrawParams)` 写成了"复用 PassParameters->InstanceCullingDrawParams"**——这是错的,这个 `InstanceCullingDrawParams` 是给 BasePass 的 GPU culling 输出用的,直接给新 pass 等于**根本没 build 新 pass 的 culling 结果**,DispatchDraw 时画不出东西。**必须改**。

### 3.2 RenderForward / RenderForwardSinglePass / RenderForwardMultiPass 之外的 3 个调用点

`RenderMobileBasePass` 在 `MobileShadingRenderer.cpp` 共被 4 处调用(889/896/1609/1682/1968/2011,实际 4 个调用点):

| 调用点 | 函数 | 涉及 translucency? | 是否需要补 After-Translucency? |
|---|---|---|---|
| 889/896 | `RenderCustomRenderPassBasePass` | 否(无 translucent) | **否** |
| 1609 | `RenderForwardSinglePass` | 是 | **是**(plan 已列) |
| 1682 | `RenderForwardMultiPass` | 是 | **是**(plan 已列) |
| 1968 | `RenderDeferredSinglePass` | 是 | **是 + 需要额外处理(见 §4.2)** |
| 2011 | `RenderDeferredMultiPass` | 是 | **是 + 需要额外处理(见 §4.2)** |

**`MobileShadingRenderer.cpp:1623` / `1735` / `1985` / `2068` 处的 `RenderTranslucency` 后面都要接 `RenderMobileAfterTranslucencyPass`**。Deferred 路径还有 G-Buffer 问题(下面 §4.2)。

### 3.3 `MobileShadingRenderer.cpp:1623` 与 `1735` 的参数错

- `RenderForwardSinglePass`(`:1578`)里 `PassParameters` 只有一个,作者写 `&PassParameters->InstanceCullingDrawParams` 正确。
- `RenderForwardMultiPass`(`:1662`)里第二个 pass 用的是 `SecondPassParameters`,作者写 `&PassParameters->InstanceCullingDrawParams` **引用错了**——`PassParameters` 不在 lambda 捕获列表里,得用 `&SecondPassParameters->InstanceCullingDrawParams`(或 §3.1 单独声明的 `AfterTranslucencyInstanceCullingDrawParams`)。
- Multi-pass 第二个 pass 的 `RenderTargets` 是 `DepthRead_StencilWrite` 状态,scene color 已经绑为 load,可以直接写,**适合**新 pass。

### 3.4 PSO Precache 路径

- `Source/Runtime/Runtime/Engine/Public/PSOPrecache.h:34` 的 `FPSOPrecacheParams::bRenderInMainPass` 是 1 bit,目前共 64-1=21 个 `Unused` 位(`PSOPrecache.h:130`),够塞 1 bit 新字段 `bRenderAfterTranslucency`。
- `UPrimitiveComponent::SetupPrecachePSOParams`(`PrimitiveComponent.cpp:4620`)需要把 `bRenderAfterTranslucency` 写进 `Params`。
- `FMobileBasePassMeshProcessor::CollectPSOInitializers`(`MobileBasePass.cpp:1056`)的 early-out:
  ```cpp
  if (!PreCacheParams.bRenderInMainPass || !ShouldDraw(Material)) return;
  ```
  这里**同时**会拒绝 `bRenderAfterTranslucency == true` 的物体(因为 `bRenderInMainPass` 也得是 true)。**结论:用户的 `bRenderAfterTranslucency` 与 `bRenderInMainPass` 共用同一份 PSO 即可,新 pass 与 BasePass 共享 PSO 集合**。但因为新 pass 注册了独立的 `REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR(MobileAfterTranslucencyPass, ...)`,preacher 系统会把它当成**另一个 PSO 集合**对待,内存上 PSO 数量会**翻倍**(对被标记物体),这是浪费但不是 bug。
- **优化选项**:如果想 PSO 共享,把 `FPSOPrecacheParams` 也加一个 `bRenderAfterTranslucency` 字段,在 `CollectPSOInitializers` 里加一行:
  ```cpp
  if (MeshPassType == EMeshPass::MobileAfterTranslucencyPass && !PreCacheParams.bRenderAfterTranslucency) return;
  if (!PreCacheParams.bRenderInMainPass || !ShouldDraw(Material)) return;
  ```
  用户的回答"是否需要修改 PSO"——**严格按最小变更**可以不改(PSO 数量翻倍),**按正确做法**应该改 FPSOPrecacheParams。

### 3.5 PrimitiveViewRelevance 的 `bStaticRelevance` / `bDynamicRelevance` / `bRenderInDepthPass` 是否要改

**答:都不需要改,理由如下。**

- `bStaticRelevance` / `bDynamicRelevance` 是 ComputeStaticMeshRelevance / ComputeDynamicMeshRelevance 在"是否要按静态/动态命令包分发"用的标签,与具体 pass 无关,新加 pass 仍然会走其中一种(由 `StaticMeshRelevance` 决定,而 StaticMeshRelevance 是 mesh batch 级别)。
- `bRenderInDepthPass` 控制"即使不在主 pass 中也要画到深度 prepass",主要给 occlusion / velocity-with-depth 场景用。如果 `bRenderAfterTranslucency == true && bRenderInDepthPass == true`,被标记物体**会**进 `RenderPrePass`,在 forward 路径上 `bIsFullDepthPrepassEnabled = (Scene->EarlyZPassMode == DDM_AllOpaque)`,默认 false,所以**默认不会**触发。但用户如果开 DDM_AllOpaque,被标记物体的深度会先于 translucency 写,这是**§4.1** 讨论的边界情况。
- 结论:**保持这三个字段不动**,依赖默认行为即可。

## 4. 关键正确性 / 视觉风险

### 4.1 深度写入时序

作者的方案假设:**Translucency Subpass 的深度是"只读且来自 Opaque BasePass 写下的"**。这条假设在以下条件**同时成立**时是正确的:
- `bIsFullDepthPrepassEnabled == false`(默认 mobile forward)
- `bIsMaskedOnlyDepthPrepassEnabled == false` 或被标记物体材质不是 Masked
- 没有 Velocity pass 把 depth 也写了 (`bVelocityPassWritesDepth == false`)

如果用户开了 DDM_AllOpaque + mobile,被标记物体(若 `bRenderInDepthPass=true`)会在 RenderPrePass 里写自己的深度,后于被标记物体的 translucent 物体就会被错误遮挡(因为 translucency 测的深度是"被标记物体"的而非"Opaque"的)。**建议默认把 `bRenderAfterTranslucency` 物体的 `bRenderInDepthPass` 强制设 false**,或者在 `UPrimitiveComponent::PostEditChangeProperty` 里加一句 `if (bRenderAfterTranslucency) bRenderInDepthPass = false;`。

### 4.2 Mobile Deferred 路径的 G-Buffer 不一致(本方案最大问题)

`MobileShadingRenderer.cpp:1885` `RenderDeferredSinglePass` 与 `:1996` `RenderDeferredMultiPass` 都是**G-Buffer + Lighting + Translucency** 结构:
1. BasePass 写 G-Buffer + SceneDepth + SceneColor
2. Lighting pass 读 G-Buffer 写 SceneColor(此时 G-Buffer 包含场景所有 opaque 信息)
3. Translucency 写 SceneColor(不影响 G-Buffer)

如果把"After-Translucency Pass"插到步骤 3 之后:
- **G-Buffer 已经定型**,被标记物体的 G-Buffer 数据(Albedo / Normal / Metallic / Roughness / 自发光)**没有被写入**,只能用不透明背景的 G-Buffer 做 lighting 计算 → 视觉上**和它旁边的 opaque 物体一样亮、一样 normal**。
- 进一步:被标记物体的 G-Buffer 没写,**BasePass Pixel Shader 的某些 pass(比如 Substrate、Sub-pixel 折射)会读错 G-Buffer 产生全局瑕疵**。
- **结论**:
  - **不建议**在 Mobile Deferred 路径上启用 `bRenderAfterTranslucency`。
  - plan 的 `RenderMobileAfterTranslucencyPass(RHICmdList, View, &PassParameters->InstanceCullingDrawParams);` 在 `RenderDeferredSinglePass` / `RenderDeferredMultiPass` 里**不该**无条件调用。建议加一个判断:
    ```cpp
    if (!bDeferredShading) {
        RenderMobileAfterTranslucencyPass(RHICmdList, View, &AfterTranslucencyInstanceCullingDrawParams);
    }
    ```
  - 或者从 `SceneVisibility.cpp` 端禁止把 `bRenderAfterTranslucency == true && bDeferredShading` 的物体加到 new pass(更稳)。这要求把 `bDeferredShading` 传进 SceneVisibility,目前是 ViewInfo/Scene 级别的,可以通过 `View.FeatureLevel` + `IsMobileDeferredShadingEnabled(ShaderPlatform)` 在 SceneVisibility 里算。

### 4.3 Subpass / 移动 GPU API 限制

- `RenderForwardSinglePass` 把 base + translucency 放在同一 Vulkan / Metal subpass 中(用 `ESubpassHint::DepthReadSubpass`)。这意味着 scene color attachment 在子通道之间**不会**做 store/load,直接留在 tile memory。如果 After-Translucency 也想 tile 友好,需要它和 RenderTranslucency 跑在**同一个**子通道里。
- 放在 `RenderTranslucency(RHICmdList, View);` 之后、`bTonemapSubpassInline` 之前的 `RHICmdList.NextSubpass();` 之前的**同一 subpass**内——作者 plan 写的位置是**对的**。
- 但需要注意:Subpass 之间的输入附件(VkSubpassDescription::pInputAttachments)在移动端 GPU 上对**深度附件**有性能要求(必须 `VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT` 且 layout = DEPTH_READ_ONLY)。UE 在 mobile 路径上 `RenderTargets.DepthStencil.SetDepthStencilAccess(FExclusiveDepthStencil::DepthRead_StencilRead)` 已经处理了这一点。**新 pass 保持 DepthRead_StencilRead 即可,不要改成 DepthWrite**——这与"我想让深度测试"的需求一致,且**不**与作者 plan 冲突(plan 也用 `TStaticDepthStencilState<true, CF_DepthNearOrEqual>` 且 `PassDrawRenderState.SetDepthStencilAccess(DefaultBasePassDepthStencilAccess)` 走 DepthWrite)。**这有问题**——`DefaultBasePassDepthStencilAccess` 在 translucency subpass 里被改写为 `DepthRead_StencilRead`,**如果新 pass 沿用作者在 CreateMobileAfterTranslucencyPassProcessor 里的 `SetDepthStencilAccess(DefaultBasePassDepthStencilAccess)`,它会拿到 DepthWrite_StencilWrite,在子通道里做深度写入会导致 Vulkan validation error**。
  - 修正:新 pass 应当显式 `PassDrawRenderState.SetDepthStencilAccess(FExclusiveDepthStencil::DepthRead_StencilRead);` 而不是 `DefaultBasePassDepthStencilAccess`,`TStaticDepthStencilState<true, CF_DepthNearOrEqual>` 用 `<false, CF_Always>` 的话(无深度写)更安全。
  - 或者:`TStaticDepthStencilState<true, CF_DepthNearOrEqual>` 与 DepthRead_StencilRead 配对是合法的(Vulkan 允许 depth test 但不写),这点 OK,只是要把 SetDepthStencilAccess 修对。

### 4.4 多视图 / VR 立体声

- 移动端 VR 通常用 `bIsMobileMultiViewEnabled = true`,scene color / depth attachment 都是 multi-view,新 pass 也要在 multi-view 下工作。`DispatchDraw` / `ParallelMeshDrawCommandPasses` 已经把 multi-view 处理掉了,只要 `FViewInfo::ViewRect` / `SetViewport` 与其它 pass 写法一致即可——**作者写法对**。
- 立体声的 `bIsInstancedStereoPass` 不影响 mobile forward 路径,跳过。

### 4.5 `bVelocity` / `bRenderCustomDepth` / `bShadowRelevance` 的副作用

被标记物体若 `bRenderInMainPass = true` 同时 `bRenderAfterTranslucency = true`,需要考虑:
- **CustomDepth**:作者新分支在静态路径里**没**改 `ViewRelevance.bRenderCustomDepth` 那段(`SceneVisibility.cpp:1601` 附近),所以被标记物体仍然会进 CustomDepth pass,正确(用户大概率想要自定义深度上有这个物体)。
- **Shadow**:被标记物体若 `CastShadow == true`,它会**仍然**进 shadow depth pass,在主光阴影贴图里有它的投影。**这通常是想要的**(VR 里手柄之类的阴影该有);但**阴影会先于 after-translucent pass 渲染**,所以玩家会看到"标记物体的影子 → 标记物体本体"的顺序,符合直觉。

### 4.6 SceneCapture / Reflection Capture

- 反射捕获 (Reflection Capture) 不走 `FViewInfo::RenderTranslucency` 也不走 SceneRenderer 全部 subpass,所以新 pass 不会出现在反射里——这与"VR 物体不进入金属反射"的取舍一致,**可以接受**。
- 如果用户希望"金属反射里也能看到被标记物体",需要在 `ComputeRelevanceForReflectionCapture`(`Engine/Private/...`)里额外把它强制标 `bRenderInMainPass = true`,或者 `FLumenScene` 相关路径里加对应 pass(本方案不涉及 Lumen,跳过)。

### 4.7 `FVelocityMeshProcessor` 的 velocity 缺失

§2.9 已提:**被标记物体不会进 Opaque velocity**,所以:
- VR 头显移动导致 TAA 抖动:**被标记物体会闪烁**(因为历史帧没匹配过它的速度向量)。
- DLSS / XeSS Motion Vector:同样缺失。

修复:在 `SceneVisibility.cpp:1510` 的 velocity 分支里把外层 `&& ViewRelevance.bRenderInMainPass` 改成 `&& (ViewRelevance.bRenderInMainPass || ViewRelevance.bRenderAfterTranslucency)`,且在 `bRenderAfterTranslucency == true` 时排到 `EMeshPass::TranslucentVelocity` 而不是 `EMeshPass::Velocity`(因为它"晚于 opaque"渲染,语义上属于 translucency 阶段写入 motion vector)。**这条 plan 完全没提,务必补**。

## 5. 用户两个具体问题的回答

### 5.1 `bRenderInMainPass` / PSO 路径是否要改

- **答:严格最小变更不需要改**。原因是:
  - `FPSOPrecacheParams::bRenderInMainPass` 是 1 bit,`bRenderAfterTranslucency == true` 物体**默认** `bRenderInMainPass = true`(plan 的隐含语义),所以 `CollectPSOInitializers` 的 early-out 不会过滤掉它,PSO 会被收集。
  - 同一个 `FMobileBasePassMeshProcessor` 实例同时被 `BasePass` 和 `MobileAfterTranslucencyPass` 注册,PSO 集合在这两个 pass 上各自生成一份,被标记物体的 PSO 数量翻倍——**浪费但正确**。
- **如果要节省 PSO 内存**,在 `FPSOPrecacheParams` 加 `bRenderAfterTranslucency` 1 bit(`PSOPrecache.h:115` 附近),在 `PrimitiveComponent.cpp:4620` 写入,在 `MobileBasePass.cpp:1060` 加一行:
  ```cpp
  if (MeshPassType == EMeshPass::MobileAfterTranslucencyPass && !PreCacheParams.bRenderAfterTranslucency) return;
  ```
  让两个 pass 的 PSO 集合**互斥**。
- **额外提醒**:`UPrimitiveComponent::SetupPrecachePSOParams` 是组件级,所有 mesh material 共享这一份 `PreCacheParams`。如果同一组件有多个 material,这套字段**只能决定整体是否参与 PSO 收集**——`CollectPSOInitializers` 还会按 material shader 单独调一次。

### 5.2 `bStaticRelevance` / `bDynamicRelevance` / `bRenderInDepthPass` 哪里要改

- **答:这三处都不需要改**。理由:
  - `bStaticRelevance` / `bDynamicRelevance` 与具体 mesh pass 无关,只决定"命令走静态包还是动态包",新加的 pass 由 `SceneVisibility.cpp` 的 `AddCommandsForMesh` 决策走静态还是动态,这两个标签是 ViewRelevance 层面已经算好的。
  - `bRenderInDepthPass` 控制"是否进 DepthPass",`SceneVisibility.cpp:1531` 的判断是 `bUseForDepthPass && (bDrawDepthOnly || (bMobileMaskedInEarlyPass && ViewRelevance.bMasked))`,与 `bRenderAfterTranslucency` 正交。如果工程开 DDM_AllOpaque,被标记物体会进 DepthPass,**作者需要决定是否接受**(详见 §4.1)。最简单的妥协:让 `bRenderAfterTranslucency == true` 自动把 `bRenderInDepthPass = false`(`PostEditChangeProperty` 或 setter 内部),这样无需在 SceneVisibility 改任何东西。
- **`bRenderInMainPass` 也没必要动**——它现在是个 "in main pass AND not editor-only" 的总开关,新加的 `bRenderAfterTranslucency` 是正交维度。

## 6. 改动清单(汇总)

按风险/必要性排序,**P0 是必须改、P1 是强烈建议改、P2 是优化项**。

| 优先级 | 文件 | 位置 | 修改 |
|---|---|---|---|
| P0 | `Source/Runtime/Renderer/Private/SceneRendering.h` | 私有成员区,约 2792 | 新增 `FInstanceCullingDrawParams AfterTranslucencyInstanceCullingDrawParams;` |
| P0 | `Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp` | 1436 `BuildInstanceCullingDrawParams` | 追加 `View.ParallelMeshDrawCommandPasses[EMeshPass::MobileAfterTranslucencyPass].BuildRenderingCommands(GraphBuilder, Scene->GPUScene, AfterTranslucencyInstanceCullingDrawParams);` |
| P0 | `Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp` | 1985 / 2068 `RenderDeferred*` | 条件性插入 `if (!bDeferredShading) RenderMobileAfterTranslucencyPass(...)`,且把 `RenderTranslucency` 后的 `RenderMobileAfterTranslucencyPass` 加上 `&& !bDeferredShading` 保护(参见 §4.2) |
| P0 | `Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp` | 1735 多 pass 路径 | 修引用:`&SecondPassParameters->InstanceCullingDrawParams` 改为 §3.1 新增的 `AfterTranslucencyInstanceCullingDrawParams` 引用 |
| P0 | `Source/Runtime/Renderer/Private/MobileBasePassRendering.cpp` | 533 附近 `bPassUsesDeferredShading` 计算 | 新增 `bAfterTranslucencyBasePass` 字段,以及对应构造函数参数(plan 已列,行号对) |
| P0 | `Source/Runtime/Renderer/Private/MobileBasePass.cpp` | 1133 / 1151 区域 | `CreateMobileAfterTranslucencyPassProcessor` 里 `SetDepthStencilAccess(DefaultBasePassDepthStencilAccess)` 改为 `SetDepthStencilAccess(FExclusiveDepthStencil::DepthRead_StencilRead)`(单 pass 路径上 §4.3) |
| P0 | `Source/Runtime/Renderer/Private/MobileBasePass.cpp` | 867 `AddMeshBatch` 顶层 filter | 位置调整到 `ShouldRenderInMainPass` 检查之后(避免白做工);语义上"if/else 互斥"在 SceneVisibility 已经分流,这里可只保留 fallback: `if (PrimitiveSceneProxy && !PrimitiveSceneProxy->ShouldRenderInMainPass()) return;` 之上对 marked 直接 return |
| P1 | `Source/Runtime/Engine/Private/Components/PrimitiveComponent.cpp` | Setter 区域 | `SetRenderAfterTranslucency` 在赋值前同步 `bRenderInDepthPass = false`(避免 §4.1 风险) |
| P1 | `Source/Runtime/Renderer/Private/SceneVisibility.cpp` | 1500 附近 velocity 分支 | `&& ViewRelevance.bRenderInMainPass` 改为 `&& (ViewRelevance.bRenderInMainPass || ViewRelevance.bRenderAfterTranslucency)`,并把 `bRenderAfterTranslucency == true` 的物体排到 `EMeshPass::TranslucentVelocity` |
| P1 | `Source/Runtime/Engine/Private/InstancedStaticMesh.cpp` 等 | `GetViewRelevance` 实现 | grep 后逐一补 `Result.bRenderAfterTranslucency = ShouldRenderAfterTranslucency();`(基类已经处理,InstancedStaticMesh 走静态 mesh 路径,**应当**自动通过 `FStaticMeshSceneProxy::GetViewRelevance` 拉到,但要确认) |
| P1 | `Source/Runtime/Engine/Private/SceneVisibility.cpp` 静态路径 | 1556 | 改成 if/else,不再重复入队 BasePass(见 §2.9 问题 A) |
| P1 | `Source/Runtime/Renderer/Private/SceneVisibility.cpp` 动态路径 | 2211 | 同上,且把外层 gate 改成 `bRenderInMainPass || bRenderCustomDepth || bRenderAfterTranslucency`,并把 bRenderAfterTranslucency 物体的 CSM / Sky / Anisotropy 处理显式注释(默认跳过 CSM 与 Anisotropy,允许 Sky 反射) |
| P1 | `Source/Runtime/Renderer/Private/SceneVisibility.cpp` | 多处 | 确认被标记物体不参与 **Reflection capture** / **Planar reflection** —— 默认不会,因为 reflection capture 路径用的是另一套 view,直接核对 `ComputeReflectionCaptureRelevance`(`Engine/Private/...` 里) |
| P2 | `Source/Runtime/Engine/Public/PSOPrecache.h` | 115 附近 | `FPSOPrecacheParams` 加 `bRenderAfterTranslucency : 1`,Unused -1 |
| P2 | `Source/Runtime/Engine/Private/Components/PrimitiveComponent.cpp` | 4620 | `Params.bRenderAfterTranslucency = bRenderAfterTranslucency;` |
| P2 | `Source/Runtime/Renderer/Private/MobileBasePass.cpp` | 1060 | `if (MeshPassType == EMeshPass::MobileAfterTranslucencyPass && !PreCacheParams.bRenderAfterTranslucency) return;` |
| P2 | `Source/Runtime/Renderer/Private/SceneVisibility.cpp` | 2208 附近 | 暴露 `IsMobileDeferredShadingEnabled` 到 SceneVisibility,允许 `bDeferredShading` 时把 `bRenderAfterTranslucency` 物体**降级**走 BasePass(并打 warning) |

## 7. 总结

**方案主线成立,但落地有 5 处必须修、5 处强烈建议改,以及 Mobile Deferred 路径下 G-Buffer 不一致这个根本性约束。**

最低可工作的最小 patch:
1. 补 `AfterTranslucencyInstanceCullingDrawParams` 与 `BuildRenderingCommands`(§3.1)
2. 修 `CreateMobileAfterTranslucencyPassProcessor` 的 `SetDepthStencilAccess` 为 `DepthRead_StencilRead`(§4.3)
3. Mobile Deferred 路径下条件性跳过(§4.2)
4. 修多 pass 路径的 `PassParameters` 引用(§3.3)
5. 修 `SceneVisibility.cpp` 静态 / 动态路径的 if/else 重复入队(§2.9)
6. Setter 同步 `bRenderInDepthPass = false`(§4.1)

完成这 6 条后,功能在 Mobile Forward 路径上可以正确工作;Mobile Deferred 路径需要在工程侧用 `bRenderAfterTranslucency = false`(默认行为)规避 G-Buffer 问题。
