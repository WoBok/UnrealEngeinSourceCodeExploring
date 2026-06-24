# PSO Precache 行为分析（MobileAfterTranslucencyPass 新增 Mesh Pass）

> 针对 `Plan.md` 第 4 节新增 `REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR(MobileAfterTranslucencyPass, ...)` 之后，PSO Precache 数量是否会"翻倍"的端到端验证与建议。仅聚焦 PSO Precache，其他风险点不在本文范围。

---

## 一、问题陈述

> 新增 `REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR(MobileAfterTranslucencyPass, ...)` 后，PSO 数量是否真的会"翻倍"？根源在哪？影响范围多大？

---

## 二、调用链验证（前因）

### 1. 注册宏会同时挂一个 PSO 收集器

`Source/Runtime/Renderer/Public/MeshPassProcessor.h:2266`

```cpp
#define REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR(Name, MeshPassProcessorCreateFunction, ShadingPath, MeshPass, MeshPassFlags) \
    IPSOCollector* CreatePSOCollector##Name(ERHIFeatureLevel::Type FeatureLevel) \
    { \
        return MeshPassProcessorCreateFunction(FeatureLevel, nullptr, nullptr, nullptr); \   // ★ 直接复用 MeshProcessor 当 PSOCollector
    } \
    FRegisterPSOCollectorCreateFunction RegisterPSOCollector##Name(&CreatePSOCollector##Name, ShadingPath, GetMeshPassName(MeshPass)); \
    FRegisterPassProcessorCreateFunction RegisterMeshPassProcesser##Name(...);
```

关键点：

- `CreateMobileAfterTranslucencyPassProcessor(FeatureLevel, nullptr, nullptr, nullptr)` 会被静态地实例化成一个 `FMobileBasePassMeshProcessor` 用作 PSO Collector。
- `FRegisterPSOCollectorCreateFunction` 把它登记到全局表 `FPSOCollectorCreateManager::PSOCollectors[EShadingPath::Mobile][N]`（`Engine/Public/PSOPrecacheMaterial.h:94-108`）。

> 也就是说：**只要按 Plan 第 4 节注册了该宏，Mobile ShadingPath 的 PSO Collector 数量就 +1**，无论场景里有没有任何 `bRenderAfterTranslucency = true` 的物体。

### 2. 材质 PSO 预缓存会逐个 Collector 跑一遍

`Source/Runtime/Engine/Private/Materials/MaterialShader.cpp:2746`

```cpp
for (int32 Index = 0; Index < FPSOCollectorCreateManager::GetPSOCollectorCount(ShadingPath); ++Index)
{
    PSOCollectorCreateFunction CreateFunction = FPSOCollectorCreateManager::GetCreateFunction(ShadingPath, Index);
    if (CreateFunction)
    {
        IPSOCollector* PSOCollector = CreateFunction(PrecacheParams.FeatureLevel);
        ...
        PSOCollector->CollectPSOInitializers(SceneTexturesConfig, *PrecacheParams.Material,
            PrecacheParams.VertexFactoryData, PrecacheParams.PrecachePSOParams, PSOInitializers);
        delete PSOCollector;
    }
}
```

对**每个被预缓存的材质**（运行时由 Component 触发 / Cook 时也有路径），引擎会**完整遍历所有 Collector**。这里没有任何按 PassType 过滤的快捷返回。

### 3. `FMobileBasePassMeshProcessor::CollectPSOInitializers` 几乎没有按 MeshPassType 区分

`Source/Runtime/Renderer/Private/MobileBasePass.cpp:1056-1149`

```cpp
void FMobileBasePassMeshProcessor::CollectPSOInitializers(...)
{
    // 只对 TranslucencyAll 这一种 PassType 做了快速返回
    if (MeshPassType == EMeshPass::TranslucencyAll && PSOPrecacheTranslucencyAllPass->GetInt() == 0)
        return;

    if (!PreCacheParams.bRenderInMainPass || !ShouldDraw(Material))
        return;

    // ...
    FMobileLightMapPolicyTypeList UniformLightMapPolicyTypes =
        GetUniformLightMapPolicyTypeForPSOCollection(bLitMaterial, bTranslucentBasePass,
                                                    bPassUsesDeferredShading, bCanReceiveCSM, bMovable);

    for (ELightMapPolicyType LightMapPolicyType : UniformLightMapPolicyTypes)
    {
        // SkyLight OFF
        CollectPSOInitializersForLMPolicy(... bEnableSkyLight=false, LOCAL_LIGHTS_DISABLED ...);
        if (bUseLocalLightPermutation)
            CollectPSOInitializersForLMPolicy(... bEnableSkyLight=false, LocalLightSetting ...);

        // SkyLight ON
        CollectPSOInitializersForLMPolicy(... bEnableSkyLight=true, LOCAL_LIGHTS_DISABLED ...);
        if (bUseLocalLightPermutation)
            CollectPSOInitializersForLMPolicy(... bEnableSkyLight=true, LocalLightSetting ...);
    }
}
```

由于新的 Collector 实例的构造路径是：

- `TranslucencyPassType = TPT_MAX` → `bTranslucentBasePass = false`
- `bPassUsesDeferredShading = bDeferredShading`（与原 BasePass 完全相同）
- `Flags = CanUseDepthStencil`（**未带 CanReceiveCSM**）→ `bCanReceiveCSM = false`

所以它走的是与原 `BasePass` Collector **几乎相同**的"非透明 + 非延迟透明 BasePass"分支，会**重复展开同一套 LightMapPolicy × SkyLight × LocalLight 的所有排列**。

---

## 三、PSO 是否真的会"重复"？（后果验证）

PSO 是否会被去重，取决于 `FGraphicsMinimalPipelineStateInitializer` 的哈希。对比 `CreateMobileBasePassProcessor` 与 `CreateMobileAfterTranslucencyPassProcessor` 在 PassDrawRenderState 上的差异：

| 状态 | BasePass (`MobileBasePass.cpp:1153-1162`) | 新 AfterTranslucencyPass (Plan L319-329) |
|---|---|---|
| BlendState | `TStaticBlendStateWriteMask<CW_RGBA>` | **未设置（注释掉了）** |
| DepthStencilState | `TStaticDepthStencilState<true, CF_DepthNearOrEqual>`（**写深度**）| `TStaticDepthStencilState<false, ...>`（**不写深度**）|
| DepthStencilAccess | `DefaultBasePassDepthStencilAccess`（Write）| `DepthRead_StencilRead` |
| Flags / `bCanReceiveCSM` | 视 `MobileBasePassAlwaysUsesCSM` 可能为 true | **始终 false** |

结论：

1. **DepthStencilState 不同** → PSO 的 `DepthStencilHash` 不同 → **管线缓存不会去重**，新建一份独立 PSO。
2. **bCanReceiveCSM=false** 意味着 `GetUniformLightMapPolicyTypeForPSOCollection` 不会返回 `LMP_MOBILE_DIRECTIONAL_LIGHT_CSM` / `LMP_MOBILE_DIRECTIONAL_LIGHT_CSM_AND_LIGHTMAP` 等 CSM 相关排列（参考 `MobileBasePass.cpp:448-560`），所以新 Collector 的"非 CSM"那一组排列会和 BasePass 完全重叠（DepthState 不同 → 不去重），而 BasePass 多出来的"CSM"那一组排列在新 Pass 中不会再发。
3. **BlendState 未显式 SetBlendState** → 沿用 `FMeshPassProcessorRenderState` 默认值（通常等价于 BasePass 的 `CW_RGBA`，但你自己注释里也在犹豫），即便相同也无法让两者归一（DepthState 已经不同了）。
4. **SubpassIndex/SubpassHint** 由 `CollectPSOInitializersForLMPolicy` 计算（`MobileBasePass.cpp:1036`）：`bTranslucentBasePass=false` → `SubpassIndex = 0`、`SubpassHint = DepthReadSubpass`。这与"AfterTranslucency 实际是在 translucency 后绘制（同一个 RenderPass 的 *后段*）"不一致 — 后段实际 SubpassIndex 应为 1（forward 单 RenderPass 情况下，参考 `MobileBasePass.cpp:1036`）。
   - 副作用：要么 PSO 验证 miss（运行时被迫现编），要么标错 subpass。

---

## 四、影响量化（PSO 数量"翻倍"是不是夸张？）

设单个材质在原 BasePass Collector 中产生 N 份 PSO（受 `UniformLightMapPolicyTypes × SkyLight × LocalLight` 排列影响）。

新 Pass 的产出 ≈ N − (CSM 相关排列数)。在 `MobileBasePassAlwaysUsesCSM == false` 的工程里，CSM 排列只是 N 的一部分（通常 ~30%）。

**结论：** 不是严格 2.0×，但**每个不透明可绘制材质的 PSO 数量会增加 ≈1.5×~1.9×**（取决于工程配置：是否启用 LQ Lightmap、Movable Dir Light、Local Lights、SkyLight 等开关）。

并且，这些新 PSO **对所有材质都会生成**（哪怕该材质从未在 `bRenderAfterTranslucency` 物体上使用），因为 `CollectPSOs` 流水是按"材质 × Collector"枚举，与运行时实际用法无关。

---

## 五、建议（仅围绕 PSO 问题，不展开其他修复）

按"最小代价"排序：

1. **最简单**：在 `FMobileBasePassMeshProcessor::CollectPSOInitializers` 入口加一道按 MeshPassType 的快速返回，结合 PSO precache CVar 控制：

   ```cpp
   // MobileBasePass.cpp:1057 附近
   if (MeshPassType == EMeshPass::MobileAfterTranslucencyPass)
   {
       static IConsoleVariable* CVarPSOPrecacheMobileAfterTranslucency =
           IConsoleManager::Get().FindConsoleVariable(TEXT("r.PSOPrecache.MobileAfterTranslucencyPass"));
       if (!CVarPSOPrecacheMobileAfterTranslucency || CVarPSOPrecacheMobileAfterTranslucency->GetInt() == 0)
       {
           return;
       }
   }
   ```

   并在某处 `IConsoleManager::Get().RegisterConsoleVariable(...)` 注册它，默认 0（关掉预缓存）。运行时 PSO 现编一份即可，毕竟只有你打了标的少数物体会走这条路径。

2. **更激进**：注册宏改成只注册 PassProcessor，不注册 PSO Collector（手写一个 `FRegisterPassProcessorCreateFunction`，跳过 `FRegisterPSOCollectorCreateFunction`），这样它根本不参与材质 PSO 预缓存。代价：首帧/首绘制时会有 PSO hitch，但你的"标记物体"通常数量很少，影响可控。

3. **彻底**：在 `CollectPSOInitializers` 里只发与"opaque after translucency"对应的最小集合（例如限定 `LMP_NO_LIGHTMAP` + 关闭 SkyLight + 关闭 LocalLight），同时把 `DepthStencilState` 改成与 BasePass 一致（`<true, CF_DepthNearOrEqual>`）以便和 BasePass PSO 复用。但这与"不写深度"的设计意图冲突，需要先决策深度写入策略。

---

## 六、补充澄清：`TranslucencyAll` 短路 与 `AddMeshBatch` 过滤 是否影响 PSO 数量

### 6.1 `TranslucencyAll` 的快速返回只对 `TranslucencyAll` 生效

`MobileBasePass.cpp:1058-1063`

```cpp
static IConsoleVariable* PSOPrecacheTranslucencyAllPass =
    IConsoleManager::Get().FindConsoleVariable(TEXT("r.PSOPrecache.TranslucencyAllPass"));
if (MeshPassType == EMeshPass::TranslucencyAll && PSOPrecacheTranslucencyAllPass->GetInt() == 0)
{
    return;
}
```

- 条件是 `MeshPassType == EMeshPass::TranslucencyAll` **并且** CVar `r.PSOPrecache.TranslucencyAllPass == 0`，二者同时成立才 `return`。
- 它**只对 `TranslucencyAll` 这一个 PassType 有效**，对新增的 `MobileAfterTranslucencyPass` 没有任何短路作用。所以前文给出的 PSO 翻倍结论保持成立。
- 第五节建议 1 的修复就是在这一段旁边再加一组 `MeshPassType == EMeshPass::MobileAfterTranslucencyPass` 的对照短路，逻辑形态与现有 `TranslucencyAll` 的写法一致。

### 6.2 `AddMeshBatch` 中的 `bAfterTranslucencyBasePass` 过滤 与 PSO Precache 无关

PSO Precache 与运行时 DrawCommand 收集是**两条完全独立的链路**：

**A. PSO Precache 链（只调用 `CollectPSOInitializers`）**

```
PrecacheMaterialPSOs(...)
└─ FMaterialShaderMap 内部预缓存
   └─ MaterialShader.cpp:2746 遍历所有 PSOCollector
      └─ CreateFunction(FeatureLevel)                ← 宏内 CreatePSOCollector##Name
         └─ CreateMobileAfterTranslucencyPassProcessor(FeatureLevel, nullptr, nullptr, nullptr)
            └─ new FMobileBasePassMeshProcessor(...)  // Scene/View/DrawListContext 均为 nullptr
      └─ PSOCollector->CollectPSOInitializers(...)   ★ 仅调用这一个虚函数
      └─ delete PSOCollector
```

- 该路径在 `REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR` 宏内固定调用形态为 `MeshPassProcessorCreateFunction(FeatureLevel, nullptr, nullptr, nullptr)`（`MeshPassProcessor.h:2269`），临时构造的 Processor 实例**永远不会调用 `AddMeshBatch`**。
- `CollectPSOInitializers` 的入参只有 `FMaterial` + `FPSOPrecacheVertexFactoryData` + `FPSOPrecacheParams`，**没有 `FPrimitiveSceneProxy`**，因此 `ShouldRenderAfterTranslucency()` 在这条链上根本读不到。

**B. 运行时绘制链（调用 `AddMeshBatch`）**

代表性的调用方（节选自仓库内）：

- `MeshDrawCommands.cpp:621/644/648/724/728/764/769` — 每帧/缓存生成 MeshDrawCommand
- `PrimitiveSceneInfo.cpp:500` — Primitive 加入场景时缓存静态 mesh 的 DrawCommand
- `MobileBasePassRendering.cpp:519/542` — 编辑器图元等动态收集

典型形态：

```cpp
PassMeshProcessor->AddMeshBatch(*MeshAndRelevance.Mesh, BatchElementMask,
                                MeshAndRelevance.PrimitiveSceneProxy);  // ★ 这里才有 Proxy
```

只有 B 链才有真实的 `FPrimitiveSceneProxy`，`AddMeshBatch` 中 `PrimitiveSceneProxy->ShouldRenderAfterTranslucency()` 的过滤逻辑才能生效。这条链路只负责**生成 DrawCommand**，**不参与 PSO precache 的枚举**。

### 6.3 总结对照表

| 代码位置 | 影响链路 | 是否影响 PSO Precache 数量 |
|---|---|---|
| `AddMeshBatch` 内 `bAfterTranslucencyBasePass` / `ShouldRenderAfterTranslucency` 过滤 | B（运行时绘制） | **不影响** |
| `CollectPSOInitializers` 入口对 `MeshPassType == MobileAfterTranslucencyPass` 短路返回 | A（PSO Precache） | **直接决定**新 Pass 是否参与 PSO 预缓存 |
| `TranslucencyAll` + `r.PSOPrecache.TranslucencyAllPass` CVar 短路 | A（仅对 TranslucencyAll） | 对新 Pass **没有影响** |

因此要消除 PSO 翻倍，**必须**改在 `CollectPSOInitializers` 入口（或者改 `REGISTER_*` 宏的注册写法不挂 Collector），`AddMeshBatch` 的分流逻辑无法解决这个问题。

---

## 七、补充：`r.PSOPrecache.TranslucencyAllPass` 默认值 & 其他 Pass Collector 是否也会"翻倍"

### 7.1 `r.PSOPrecache.TranslucencyAllPass` 默认值 = 0

`BasePassRendering.cpp:106-113`

```cpp
static TAutoConsoleVariable<int32> CVarPSOPrecacheTranslucencyAllPass(
    TEXT("r.PSOPrecache.TranslucencyAllPass"),
    0,                                          // ★ 默认 0
    TEXT("Precache PSOs for TranslucencyAll pass.\n") \
    TEXT(" 0: No PSOs are compiled for this pass (default).\n") \
    TEXT(" 1: PSOs are compiled for all primitives which render to a translucency pass.\n"),
    ECVF_ReadOnly
);
```

- 默认 0 = 不参与 PSO 预缓存；用户必须显式 `r.PSOPrecache.TranslucencyAllPass=1` 才会启用。
- `MobileBasePass.cpp:1058` 读这个值用的是 `FindConsoleVariable`，与上面是同一个 CVar，所以 Mobile 端也是默认 0。

### 7.2 其他 Mobile Pass Collector 是否也会"PSO 翻倍"？答：不会

把 Mobile ShadingPath 上所有 `REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR` 注册的 Collector 都核对一遍，结论：它们在 `CollectPSOInitializers` 入口都有**按材质功能位**的硬性提前返回，普通不透明材质根本不会落到展开逻辑里。

| Pass Collector | 入口提前返回条件 | 行号 | 普通 opaque 材质是否会被展开 |
|---|---|---|---|
| `FSkyPassMeshProcessor::CollectPSOInitializers` | `if (!Material.IsSky()) return;` | `SkyPassRendering.cpp:164` | 否（仅 Sky 材质）|
| `FMeshDecalMeshProcessor::CollectPSOInitializers` | `if (!Material.IsDeferredDecal()) return;` | `PostProcessMeshDecals.cpp:406` | 否（仅 Decal 材质）|
| `FAnisotropyMeshProcessor::CollectPSOInitializers` | `if (ShouldDraw(Material, Material.MaterialUsesAnisotropy_GameThread()) && SupportsAnisotropicMaterials(...))` | `AnisotropyRendering.cpp:243` | 否（仅各向异性材质，Mobile 通常未注册）|
| `FDistortionMeshProcessor::CollectPSOInitializers` | `if (!ShouldDraw(Material)) return;`（要求材质支持折射）| `DistortionRendering.cpp:993` | 否（仅折射材质）|
| `FCustomDepthPassMeshProcessor::CollectPSOInitializers` | 受 `r.PSOPrecache.CustomDepth`（默认 1）与 `PreCacheParams.bRenderCustomDepth` 控制 | `CustomDepthRendering.cpp:36-43, 660` | 仅请求 CustomDepth 的物体 |
| `FDepthPassMeshProcessor`（含 Mobile）| 受 `PreCacheParams.bRenderInDepthPass` / `bUsesPixelDepthOffset` 等 PreCache 标志门控 | `DepthRendering.cpp:1096+` | 仅深度参与的物体 |
| `FMobileBasePassMeshProcessor::CollectPSOInitializers`（本次新增 Pass 复用的就是它）| `ShouldDraw(Material)` opaque 分支为 `!bIsTranslucent`，叠加 `PreCacheParams.bRenderInMainPass` | `MobileBasePass.cpp:847, 1066` | **是** — 任何主 Pass 不透明材质都被整套展开 |

### 7.3 关键差异：为什么唯独你新增的 Pass 会"翻倍"

`FMobileBasePassMeshProcessor::ShouldDraw(opaque 分支)` 仅判断 `!bIsTranslucent`（`MobileBasePass.cpp:828-848`）：

```cpp
bool FMobileBasePassMeshProcessor::ShouldDraw(const FMaterial& Material) const
{
    ...
    if (bTranslucentBasePass) { /* translucency 走另一条路 */ }
    else
    {
        // opaque materials.
        return !bIsTranslucent;
    }
}
```

而你"标记后渲染"的依据 `bRenderAfterTranslucency` 是放在 **PrimitiveComponent / SceneProxy** 上的，**不是材质属性**。PSO Precache 链（A 链，见 §6.2）只看 `FMaterial` 与 `FPSOPrecacheParams`，**根本拿不到 SceneProxy**，因此没法像 Sky/Decal/Anisotropy/Distortion 那样在 Collector 入口用材质特征 "天然过滤" 掉绝大多数材质。

> 结论：**Sky/Decals/Anisotropy/Distortion 这些 Pass 不会让普通材质 PSO 翻倍**，它们只为对应类别的特殊材质产出 PSO；而本次新增的 `MobileAfterTranslucencyPass` 由于复用 BasePass MeshProcessor 且没有"AfterTranslucency 专属"的材质标识，**对所有 opaque 主 Pass 材质都会再走一遍**。这正是必须给它加一道 CVar 短路（默认关）或不挂 PSO Collector 的根本原因。

### 7.4 旁证：UE 其他 PSOPrecache CVar 默认值一览

| CVar | 默认 | 文件 |
|---|---|---|
| `r.PSOPrecache.LightMapPolicyMode` | 1（只预缓 LMP_NO_LIGHTMAP）| `BasePassRendering.cpp:98` |
| `r.PSOPrecache.TranslucencyAllPass` | **0** | `BasePassRendering.cpp:107` |
| `r.PSOPrecache.PrecacheAlphaColorChannel` | 见同文件 :116 | `BasePassRendering.cpp:116` |
| `r.PSOPrecache.CustomDepth` | 1 | `CustomDepthRendering.cpp:36` |
| `r.PSOPrecache.DitheredLODFadingOutMaskPass` | **0** | `DepthRendering.cpp:77` |
| `r.PSOPrecache.ProjectedShadows` | 1 | `DepthRendering.cpp:86` |

UE 对"非主线、可能爆 PSO 数"的 Pass 默认就是关掉 PSO 预缓存（如 `TranslucencyAllPass`、`DitheredLODFadingOutMaskPass`）。新增的 `MobileAfterTranslucencyPass` 性质完全符合这一类，按惯例就该挂一个**默认 0** 的 CVar 来短路。

---

## 八、Plan 中两个待修 Bug（与 PSO 短路联动）

### 8.1 Bug A：`CreateMobileAfterTranslucencyPassProcessor` 构造调用错位（Plan L328）

Plan 当前写法：

```cpp
return new FMobileBasePassMeshProcessor(
    EMeshPass::MobileAfterTranslucencyPass,
    Scene, InViewIfDynamicMeshCommand,
    PassDrawRenderState, InDrawListContext,
    Flags,
    true);   // ← 这个 true 实际落到了第 7 个形参 InTranslucencyPassType 上
```

构造函数签名（Plan L224-233）有 8 个形参，第 7、8 个分别是：

```cpp
ETranslucencyPass::Type InTranslucencyPassType = ETranslucencyPass::TPT_MAX,  // 第 7
bool bAfterTranslucencyBasePass = false;                                      // 第 8
```

只传 7 个参数时：

- `true` 被隐式转换为 `ETranslucencyPass::Type(1)` ≈ `TPT_TranslucencyStandard`。
- `bAfterTranslucencyBasePass` 使用默认值 `false`。

后果（严重，远超 PSO 范畴）：

- `bTranslucentBasePass = (TranslucencyPassType != TPT_MAX) = true`，整个 Processor 被当作"半透明 BasePass"处理，会走 `MobileBasePass::SetTranslucentRenderState`、`SubpassIndex` 取错、`bPassUsesDeferredShading` 推算错。
- `bAfterTranslucencyBasePass` 永远为 false → `AddMeshBatch` 里你写的 Pass 分流条件失效；任何依赖它的代码都会被旁路。

**修复**：显式补出第 7 个参数。

```cpp
return new FMobileBasePassMeshProcessor(
    EMeshPass::MobileAfterTranslucencyPass,
    Scene, InViewIfDynamicMeshCommand,
    PassDrawRenderState, InDrawListContext,
    Flags,
    ETranslucencyPass::TPT_MAX,   // ★ 必须显式给
    true);                         // ★ bAfterTranslucencyBasePass = true
```

### 8.2 Bug B：用户给出的 PSO 短路条件逻辑反了

用户拟写：

```cpp
if (MeshPassType == EMeshPass::MobileAfterTranslucencyPass && !bAfterTranslucencyBasePass)
{
    return;
}
```

期望意图：当 Processor 是 AfterTranslucency 时短路 PSO Precache。
真实语义：当 PassType 是 AfterTranslucency **且** `bAfterTranslucencyBasePass == false` 才 return。

按设计（修完 8.1 之后），AfterTranslucency Processor 的 `bAfterTranslucencyBasePass` 应为 `true`，因此 `!bAfterTranslucencyBasePass == false`，整个条件为 false，**`return` 不会触发，PSO 仍然翻倍**。

> 用户当前之所以"看起来好像能跑"，恰好是因为 Bug A 让 `bAfterTranslucencyBasePass` 永远为 `false`，`!bAfterTranslucencyBasePass` 恒为 `true`，整段条件退化为 `if (MeshPassType == MobileAfterTranslucencyPass) return;`。**靠一个 bug 抵消另一个 bug**，一旦修了 Bug A，PSO 短路立即失效。

### 8.3 推荐写法（同步采纳 Plan §572-580 自评意见）

`bAfterTranslucencyBasePass` 与 `MeshPassType == MobileAfterTranslucencyPass` 是 1:1 等价的（构造时绑定、运行时不可分叉），保留这个成员变量徒增状态不一致风险。Plan 自评建议直接用 `MeshPassType` 判断，本文档采纳。

**最终建议代码（替换用户那段）：**

```cpp
void FMobileBasePassMeshProcessor::CollectPSOInitializers(...)
{
    // RenderAfterTranslucency: PSO Precache 短路
    // 该 Pass 由 PrimitiveSceneProxy::bRenderAfterTranslucency 标记触发，
    // 而材质本身无任何对应特征，全量预缓存会让所有 opaque 材质 PSO ~1.5x~1.9x。
    // 标记物体通常很少，运行时现编代价远小于全量预缓存收益。
    if (MeshPassType == EMeshPass::MobileAfterTranslucencyPass)
    {
        return;
    }

    static IConsoleVariable* PSOPrecacheTranslucencyAllPass =
        IConsoleManager::Get().FindConsoleVariable(TEXT("r.PSOPrecache.TranslucencyAllPass"));
    if (MeshPassType == EMeshPass::TranslucencyAll && PSOPrecacheTranslucencyAllPass->GetInt() == 0)
    {
        return;
    }
    // ... 原有逻辑
}
```

如希望保留运行时可控开关（与 `r.PSOPrecache.TranslucencyAllPass` 风格一致）：

```cpp
static TAutoConsoleVariable<int32> CVarPSOPrecacheMobileAfterTranslucencyPass(
    TEXT("r.PSOPrecache.MobileAfterTranslucencyPass"),
    0,
    TEXT(" 0: No PSOs are compiled for MobileAfterTranslucencyPass (default).\n")
    TEXT(" 1: PSOs are compiled for all opaque primitives that may render after translucency.\n"),
    ECVF_ReadOnly);

// CollectPSOInitializers 入口
if (MeshPassType == EMeshPass::MobileAfterTranslucencyPass &&
    CVarPSOPrecacheMobileAfterTranslucencyPass.GetValueOnAnyThread() == 0)
{
    return;
}
```

### 8.4 行动顺序

1. **先修 Bug A**（Plan L328 构造参数错位），否则运行时绘制路径整体错位，且 PSO 短路条件依赖 `bAfterTranslucencyBasePass` 永远不对。
2. **再写 PSO 短路**，用 `MeshPassType == EMeshPass::MobileAfterTranslucencyPass` 判断，不要依赖 `bAfterTranslucencyBasePass`。
3. （可选，最佳实践）按 Plan §572-580 自评建议删除 `bAfterTranslucencyBasePass` 成员，AddMeshBatch / CollectPSOInitializers 等处统一用 `GetMeshPassType()`。

---

## 九、用户当前写法的常见误解澄清（PSO 不存在"按 Pass 分流"）

### 9.1 用户拟写的两条 if

```cpp
if (MeshPassType == EMeshPass::MobileAfterTranslucencyPass && !bAfterTranslucencyBasePass)
{
    return;
}
if (MeshPassType != EMeshPass::MobileAfterTranslucencyPass && bAfterTranslucencyBasePass)
{
    return;
}
```

意图（用户原话）："如果是 MobileBasePass 则进 MobileBase 的 PSO 缓存，如果是 MobileAfterTranslucencyPass 则进 MobileAfterTranslucencyPass 的 PSO 缓存。"

### 9.2 为何这两条 if **都不会触发 return**（自洽断言陷阱）

`CreateMobileBasePassProcessor` 与 `CreateMobileAfterTranslucencyPassProcessor` 在构造时已经把两者 1:1 绑定：

| Collector 实例 | `MeshPassType` | `bAfterTranslucencyBasePass` |
|---|---|---|
| BasePass Collector | `EMeshPass::BasePass` | `false` |
| AfterTranslucency Collector | `EMeshPass::MobileAfterTranslucencyPass` | `true` |

代入用户两条条件：

| Collector | 条件 1 `... && !bAfterTrans...` | 条件 2 `... != ... && bAfterTrans...` |
|---|---|---|
| BasePass | `false && (true)` = **false** | `true && false` = **false** |
| AfterTranslucency | `true && false` = **false** | `false && true` = **false** |

**两条 return 在正常路径下都永远不会触发**，等价于没改动。它们只是"状态一致性断言"——只有当 `MeshPassType` 与 `bAfterTranslucencyBasePass` 出现错位时（如 §8.1 描述的 Bug A）才会进 return；而错位本身是构造 bug，应该修构造，不应该靠 return 兜底。

### 9.3 PSO Precache 不存在"路由分发"——根本机制澄清

`Engine/Private/Materials/MaterialShader.cpp:2746`：

```cpp
for (int32 Index = 0; Index < FPSOCollectorCreateManager::GetPSOCollectorCount(ShadingPath); ++Index)
{
    IPSOCollector* PSOCollector = CreateFunction(FeatureLevel);
    PSOCollector->CollectPSOInitializers(SceneTexturesConfig, Material, VertexFactoryData, PreCacheParams, PSOInitializers);
    delete PSOCollector;
}
```

- **对每个材质**，依次跑**所有**已注册 Collector；
- 所有 Collector 的产物丢进**同一个** `PSOInitializers` 数组；
- 用 `FGraphicsMinimalPipelineStateInitializer` 哈希在更上层做去重。

因此：

- 没有"把某个材质路由到对应 Pass Collector"的机制。
- BasePass Collector 与 AfterTranslucency Collector 都会对**每个 opaque 主 Pass 材质**完整跑一遍 → §三所述 PSO 翻倍。
- "MobileBasePass 进 MobileBase 缓存、AfterTranslucency 进 AfterTranslucency 缓存"——这种隔离**根本不存在**。

### 9.4 用户写法仍然 PSO 翻倍的实际证据链

- BasePass Collector：用户条件 1 取 false（因 `MeshPassType != AfterTranslucency`）；条件 2 取 false（因 `bAfterTranslucencyBasePass == false`）→ **完整展开 BasePass 的全套 PSO**。
- AfterTranslucency Collector：条件 1 取 false（因 `bAfterTranslucencyBasePass == true`）；条件 2 取 false（同因）→ **完整展开同一套 PSO 的 AfterTranslucency 版本**。

由于两者 `PassDrawRenderState` 不同（DepthState `<true>` vs `<false>`），上层 PSO 哈希不会去重 → **每个 opaque 材质仍然 ~1.5x~1.9x**。改了等于没改。

### 9.5 正确做法（最小补丁，三处）

> 推荐配合 §8.3 的建议：**删除 `bAfterTranslucencyBasePass` 成员**，统一靠 `MeshPassType` 判断，消除"状态二选一"的可能。

**(a) `CreateMobileAfterTranslucencyPassProcessor`（替换 Plan L319-329）：**

```cpp
FMeshPassProcessor* CreateMobileAfterTranslucencyPassProcessor(
    ERHIFeatureLevel::Type FeatureLevel,
    const FScene* Scene,
    const FSceneView* InViewIfDynamicMeshCommand,
    FMeshPassDrawListContext* InDrawListContext)
{
    FMeshPassProcessorRenderState PassDrawRenderState;
    PassDrawRenderState.SetBlendState(TStaticBlendStateWriteMask<CW_RGBA>::GetRHI());
    PassDrawRenderState.SetDepthStencilAccess(FExclusiveDepthStencil::DepthRead_StencilRead);
    PassDrawRenderState.SetDepthStencilState(TStaticDepthStencilState<false, CF_DepthNearOrEqual>::GetRHI());

    const FMobileBasePassMeshProcessor::EFlags Flags = FMobileBasePassMeshProcessor::EFlags::CanUseDepthStencil;

    return new FMobileBasePassMeshProcessor(
        EMeshPass::MobileAfterTranslucencyPass,
        Scene, InViewIfDynamicMeshCommand,
        PassDrawRenderState, InDrawListContext,
        Flags);
    // 不再传 InTranslucencyPassType / bAfterTranslucencyBasePass，对应成员可一并删除
}
```

**(b) `AddMeshBatch`（替换 Plan L271-282）：**

```cpp
const bool bIsAfterTranslucencyPass = (MeshPassType == EMeshPass::MobileAfterTranslucencyPass);
const bool bShouldRenderAfterTranslucency =
    PrimitiveSceneProxy && PrimitiveSceneProxy->ShouldRenderAfterTranslucency();
if (bIsAfterTranslucencyPass != bShouldRenderAfterTranslucency)
{
    return;
}
```

**(c) `CollectPSOInitializers` 入口（替换用户拟写的两条 if）：**

```cpp
void FMobileBasePassMeshProcessor::CollectPSOInitializers(
    const FSceneTexturesConfig& SceneTexturesConfig,
    const FMaterial& Material,
    const FPSOPrecacheVertexFactoryData& VertexFactoryData,
    const FPSOPrecacheParams& PreCacheParams,
    TArray<FPSOPrecacheData>& PSOInitializers)
{
    // RenderAfterTranslucency: 不参与 PSO 预缓存
    // 该 Pass 由 PrimitiveSceneProxy::bRenderAfterTranslucency 标记触发，
    // 材质本身没有等价的特征位，PSO Precache 链拿不到 SceneProxy；
    // 若展开会让每个 opaque 主 Pass 材质额外产出一份 PSO（~1.5x~1.9x）。
    if (MeshPassType == EMeshPass::MobileAfterTranslucencyPass)
    {
        return;
    }

    static IConsoleVariable* PSOPrecacheTranslucencyAllPass =
        IConsoleManager::Get().FindConsoleVariable(TEXT("r.PSOPrecache.TranslucencyAllPass"));
    if (MeshPassType == EMeshPass::TranslucencyAll && PSOPrecacheTranslucencyAllPass->GetInt() == 0)
    {
        return;
    }
    // ... 原有逻辑
}
```

### 9.6 三种可选策略小结

| 方案 | 实施要点 | 代价 | 推荐场景 |
|---|---|---|---|
| **A. 完全跳过 PSO 预缓存（推荐）** | `CollectPSOInitializers` 入口对 `MobileAfterTranslucencyPass` 直接 return | 标记物体首次绘制时现编一份 PSO（数量极少，可忽略） | 默认选项 |
| **B. 保留预缓存但让 PSO 与 BasePass 去重** | AfterTranslucency 的 `PassDrawRenderState` 与 BasePass 完全一致（含 DepthState `<true>`）| 失去"不写深度"的设计意图，需要重审深度写入策略 | 必须 PSO 预缓存且接受写深度 |
| **C. 不挂 PSO Collector** | 改写宏注册，仅用 `FRegisterPassProcessorCreateFunction`，不调用 `FRegisterPSOCollectorCreateFunction` | 首绘制 hitch，但标记物体很少，影响极小 | 想彻底解耦 PSO 路径 |

---

## 十、证据清单（可直接跳转）

- `Source/Runtime/Renderer/Public/MeshPassProcessor.h:2266` — 宏定义同时挂 Collector 与 Processor
- `Source/Runtime/Engine/Public/PSOPrecacheMaterial.h:52-122` — `FPSOCollectorCreateManager` / `FRegisterPSOCollectorCreateFunction`
- `Source/Runtime/Engine/Private/Materials/MaterialShader.cpp:2746` — 遍历所有 Collector 做 PSO 预缓存
- `Source/Runtime/Renderer/Private/MobileBasePass.cpp:1056-1149` — `CollectPSOInitializers` 主体（仅对 TranslucencyAll 短路）
- `Source/Runtime/Renderer/Private/MobileBasePass.cpp:1006-1054` — `CollectPSOInitializersForLMPolicy`，`SubpassIndex/Hint` 计算
- `Source/Runtime/Renderer/Private/MobileBasePass.cpp:448-580` — `GetUniformLightMapPolicyTypeForPSOCollection`（CSM/Lightmap 排列展开）
- `Source/Runtime/Renderer/Private/MobileBasePass.cpp:1151-1162` — 原 BasePass Processor 创建函数（PSO state 对比基线）
