# 引擎改造方案分析报告

> **目标项目**: UE5.4 Android VR Forward 移动端
> **改造目标**: 新增 `EMeshPass::MobileAfterTranslucencyPass`,让标记的不透明物体在透明物体之后渲染,从而"压住"透明物体,并复用移动端 BasePass 的深度测试逻辑
> **分析范围**: 方案正确性、行号准确性、潜在问题、缺失修改
> **分析日期**: 2026-06-23

---

## 1. 方案整体评价

方案对渲染管线主干的修改是合理的,思路与官方 Mobile BasePass 的扩展点一致:通过新增一个 `EMeshPass` 枚举并复用 `FMobileBasePassMeshProcessor`,让移动端前向渲染在不透明与透明之后多渲染一次。但方案存在 **多处关键修改遗漏** 与 **若干运行时正确性风险**,按当前描述实现会**直接编译失败或在运行时导致绘制重复 / 校验失败 / Vulkan-Metal 验证层报错**。

下面按文件逐项核对,并附每处的修复建议。

---

## 2. 逐项核对结果

### 2.1 `Engine/Source/Runtime/Renderer/Public/MeshPassProcessor.h`

| 改动 | 方案描述 | 源码实际 | 是否正确 |
|------|----------|----------|----------|
| 新增 `EMeshPass::MobileAfterTranslucencyPass` | 放在 `WaterInfoTexturePass` 之后、`#if WITH_EDITOR` 之前 | 当前 `WaterInfoTexturePass` 是第 32 项(从 0 计数),之后是 `#if WITH_EDITOR` 的 4 个 + `Num` | 位置合理,新位置使非 Editor 编译下 `Num` 由 32 变为 33,符合 `static_assert(EMeshPass::Num == 33, ...)` |
| 新增 `GetMeshPassName` case | 加 `MobileAfterTranslucencyPass` case | case 列表中存在 | 正确 |
| 修改底部 `static_assert` | `Num == 33 + 4`(Editor)/ `Num == 33`(非 Editor) | 当前断言实际是 `Num == 32 + 4` / `Num == 32`(**方案中描述原值 33 是错的**,源码是 32) | 新断言 `33 / 33+4` 正确,但叙述中原值 33 应改为 32 |
| `NumBits = 6`(最大 64 项) | — | 33/37 远小于 64 | 不会溢出 |
| `static_assert(EMeshPass::Num <= (1 << EMeshPass::NumBits)` | 仍生效 | 33 <= 64 通过 | 正确 |
| GUID `{674D7D62-CFD8-4971-9A8D-CD91E5612CD8}` | 保留 | 注释提示 change when changing the expression | 规范上建议同步更换,但非强制 |

**结论**: 这一步总体正确。只需把叙述中原值 33 修正为 32。

---

### 2.2 `Engine/Source/Runtime/Engine/Classes/Components/PrimitiveComponent.h`

| 改动 | 方案描述 | 源码实际 | 是否正确 |
|------|----------|----------|----------|
| `:407` 新增 `bRenderAfterTranslucency : 1` 字段 | 紧跟 `bRenderInMainPass` | `bRenderInMainPass` 实际在 `:407` 附近 | 行号基本准确 |
| `meta = (DisplayName = "Render Opaque After Translucency (Mobile)")` | 标注仅移动端 | — | 建议保留 |
| `:1917` 新增 `SetRenderAfterTranslucency` | 与 `SetRenderInMainPass` 风格一致 | 源码中 `SetRenderInMainPass` 在 `:1921` 附近 | 风格与行号基本一致 |
| `BlueprintCallable` | 暴露给蓝图 | — | 正确 |

**结论**: 这一步正确。

---

### 2.3 `Engine/Source/Runtime/Engine/Private/Components/PrimitiveComponent.cpp`

| 改动 | 方案描述 | 源码实际 | 是否正确 |
|------|----------|----------|----------|
| `:4457` 实现 `SetRenderAfterTranslucency` | 与 `SetRenderInMainPass` 对称 | `SetRenderInMainPass` 在 `:4458` 附近 | 正确 |
| `:333` 构造函数中加 `bRenderAfterTranslucency = false` | 紧跟 `bRenderInMainPass = true` | `bRenderInMainPass = true` 在 `:333`,下一行是 `bRenderInDepthPass = true` | 位置正确 |

**结论**: 这一步正确。

---

### 2.4 `Engine/Source/Runtime/Engine/Public/PrimitiveSceneProxy.h`

| 改动 | 方案描述 | 源码实际 | 是否正确 |
|------|----------|----------|----------|
| `:1200` 新增 `uint8 bRenderAfterTranslucency : 1;` | 紧跟 `bRenderInMainPass` | `bRenderInMainPass : 1;` 在 `:1198`,下方是 `bForceHidden : 1;` | 位置正确 |
| `:700` 新增 `ShouldRenderAfterTranslucency()` 内联函数 | — | 源代码中 `ShouldRenderInMainPass` 在 `:706` 附近 | 正确 |

**结论**: 这一步正确。

---

### 2.5 `Engine/Source/Runtime/Engine/Private/PrimitiveSceneProxy.cpp`

| 改动 | 方案描述 | 源码实际 | 是否正确 |
|------|----------|----------|----------|
| `:277` 通过 `InComponent` 初始化 `bRenderAfterTranslucency` | `bRenderAfterTranslucency = InComponent->bRenderAfterTranslucency;` | 源码 `:277` 是 `bRenderInMainPass = InComponent->bRenderInMainPass;` | 位置正确 |
| `:428` 通过 `InProxyDesc` 初始化 `bRenderAfterTranslucency` | `bRenderAfterTranslucency(InProxyDesc.bRenderAfterTranslucency)` | 源码 `:428` 是 `, bRenderInMainPass(InProxyDesc.bRenderInMainPass)` | 位置正确 |
| **`FPrimitiveSceneProxyDesc::InitializeFrom` 中同步 `bRenderAfterTranslucency`** | **方案未列出** | 源码 `:265` 起的 `InitializeFrom` 必须把组件字段拷贝到 `FPrimitiveSceneProxyDesc` | **严重遗漏,见 §3.1** |

**结论**: 这一步有**严重遗漏**:`InitializeFrom` 中必须新增 `bRenderAfterTranslucency = InComponent->bRenderAfterTranslucency;`,否则 `FPrimitiveSceneProxyDesc::bRenderAfterTranslucency` 永远是默认的 `false`,整条链下游全部失效。

---

### 2.6 `Engine/Source/Runtime/Engine/Public/PrimitiveSceneProxyDesc.h`

| 改动 | 方案描述 | 源码实际 | 是否正确 |
|------|----------|----------|----------|
| `:93` 新增 `uint32 bRenderAfterTranslucency : 1;` | 紧跟 `bRenderInMainPass` | `uint32 bRenderInMainPass : 1;` 在 `:87` | 行号有 ~6 行偏差,但语义与位置正确 |
| `:25` 构造函数加 `bRenderAfterTranslucency = false;` | 默认 false | 源码中 `bRenderInMainPass = true;` 在 `:25` 附近 | 正确 |

**结论**: 行号略有偏差(实际更靠前),但语义与位置正确,可按字段定义位置就近调整。

---

### 2.7 `Engine/Source/Runtime/Renderer/Private/MobileBasePassRendering.h`

| 改动 | 方案描述 | 源码实际 | 是否正确 |
|------|----------|----------|----------|
| `:533` 新增 `const bool bAfterTranslucencyBasePass;` | 紧跟 `bPassUsesDeferredShading` | `bPassUsesDeferredShading` 在类的私有字段区(约 `:533`),后面是右花括号 | 正确 |
| `:480` 构造函数新增参数 `bool bAfterTranslucencyBasePass = false` | 紧跟 `ETranslucencyPass::Type InTranslucencyPassType = ETranslucencyPass::TPT_MAX` | 源码中构造函数声明在 `:480-487` | 正确 |
| **`CreateMobileAfterTranslucencyPassProcessor` 函数声明** | **方案仅在 `.cpp` 中给出定义** | 头文件中**没有任何**新增声明 | **遗漏,见 §3.2** |
| **`void RenderMobileAfterTranslucencyPass(...)` 成员函数声明** | 方案放在 `MobileBasePassRendering.cpp` 中定义 | `RenderMobileBasePass` 声明位于 `SceneRendering.h` 的 `FMobileSceneRenderer` 中(`:2695`) | **遗漏,见 §3.3** |

**结论**: 这一步缺失两个关键声明。

---

### 2.8 `Engine/Source/Runtime/Renderer/Private/MobileBasePass.cpp`

| 改动 | 方案描述 | 源码实际 | 是否正确 |
|------|----------|----------|----------|
| `:810` 构造函数实现新增 `IsAfterTranslucencyBasePass` 参数 | 与头文件对应 | 构造函数实现位于 `:802-816` 附近 | 行号偏小 ~8 行,需就近插入 |
| `:867` `AddMeshBatch` 入口过滤 | 仿照 `ShouldRenderInMainPass` 早退 | `AddMeshBatch` 在 `:868` 附近 | 风格与位置正确 |
| `:1151` 附近新增 `CreateMobileAfterTranslucencyPassProcessor` | 紧跟 `CreateMobileBasePassProcessor` | `CreateMobileBasePassProcessor` 在 `:1147-1162` | 行号基本准确 |
| `:1223` 附近 `REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR` | — | 现有注册宏在 `:1220-1226` 附近 | 正确 |

**关于 `CreateMobileAfterTranslucencyPassProcessor` 的 `EMeshPass::MobileAfterTranslucencyPass`**:方案中传入 `true` 作为最后一个参数,但 `FMobileBasePassMeshProcessor` 构造函数第 7 个参数是 `ETranslucencyPass::Type InTranslucencyPassType`,第 8 个是 `bool bAfterTranslucencyBasePass`。在 `CreateMobileAfterTranslucencyPassProcessor` 末尾应写 `, true)` 才会落到 `bAfterTranslucencyBasePass`,**方案写法正确**(`return new FMobileBasePassMeshProcessor(EMeshPass::MobileAfterTranslucencyPass, ..., Flags, true);`),但 `ETranslucencyPass::TPT_MAX` 默认参数被省略了,编译器会使用默认 `TPT_MAX`,语义上等于"不透明 basepass",**正确**。

**关于 `bAfterTranslucencyBasePass` 与 `bTranslucentBasePass` 的关系**:现有 `bTranslucentBasePass = InTranslucencyPassType != ETranslucencyPass::TPT_MAX`,新参数 `bAfterTranslucencyBasePass` 与之正交。`ShouldDraw` 中 `bTranslucentBasePass` 决定是否走 translucent 判定路径,因此**透传 pass(TranslucencyStandard / TranslucencyAfterDOF)不会受新参数影响**,不会被错误地接收 after-translucency 的不透明物体。这一点方案隐含正确。

**关于 `CreateMobileBasePassCSMProcessor`**:方案没有显式修改这里,但因为构造函数参数 `bAfterTranslucencyBasePass` 默认 `false`,`AddMeshBatch` 内的 else 分支会过滤掉 `bRenderAfterTranslucency = true` 的物体,**行为正确**,但前提是 §3.4 的 `SceneVisibility` 改动修复了,否则数据流不一致。

**结论**: 这一步基本正确;但行号有 ~8 行偏移,需就近查找 `FMobileBasePassMeshProcessor::FMobileBasePassMeshProcessor(` 后再修改。

---

### 2.9 `Engine/Source/Runtime/Renderer/Private/MobileBasePassRendering.cpp`

| 改动 | 方案描述 | 源码实际 | 是否正确 |
|------|----------|----------|----------|
| `:492` 附近新增 `RenderMobileAfterTranslucencyPass` 函数体 | 紧跟 `RenderMobileBasePass` 之后 | `RenderMobileBasePass` 在 `:480-490`,`RenderMobileEditorPrimitives` 在 `:492` 之后 | 位置基本正确 |
| 设置 `View.ParallelMeshDrawCommandPasses[EMeshPass::MobileAfterTranslucencyPass].DispatchDraw(...)` | 仿照 `RenderMobileBasePass` 末尾 | 源码中 `RenderMobileBasePass` 没有 `DispatchDraw` 调用,调用点在 `RenderForwardSinglePass/MultiPass` 中 | 方案中此函数也只调用 `DispatchDraw`,但**调用方还未声明**(见 §3.3) |

**关于深度/模板状态**:方案中 `CreateMobileAfterTranslucencyPassProcessor` 设置 `DefaultBasePassDepthStencilAccess`(= `DepthWrite_StencilWrite`)。在 `RenderForwardSinglePass` 中,`NextSubpass` 之后深度被设为 `DepthRead_StencilRead`;在 `RenderForwardMultiPass` 的第二个 `AddPass` 中也是 `DepthRead_StencilRead`。**直接调用此函数会导致 Vulkan / Metal 验证层报错**(subpass 内不允许从只读切换到可写)。详见 §3.5。

**结论**: 函数位置正确,但深度/模板访问权限与子通道不兼容,需要进一步处理。

---

### 2.10 `Engine/Source/Runtime/RenderCore/Private/RenderCore.cpp`

| 改动 | 方案描述 | 源码实际 | 是否正确 |
|------|----------|----------|----------|
| `:65` 附近 `DEFINE_STAT(STAT_AfterTranslucencyDrawTime);` | 紧跟 `STAT_BasePassDrawTime` | 源码中 `DEFINE_STAT(STAT_BasePassDrawTime);` 正好在 `:65` | 正确 |

**结论**: 这一步正确。

---

### 2.11 `Engine/Source/Runtime/Renderer/Private/SceneRendering.h`

| 改动 | 方案描述 | 源码实际 | 是否正确 |
|------|----------|----------|----------|
| `:2695` 附近**新增 `void RenderMobileAfterTranslucencyPass(...)` 声明** | 方案未列出 | 现有 `RenderMobileBasePass` 声明在 `:2695` | **遗漏,见 §3.3** |
| `:2796` 附近新增 `AfterTranslucencyInstanceCullingDrawParams` | 紧跟 `TranslucencyInstanceCullingDrawParams` | 源码中 `TranslucencyInstanceCullingDrawParams` 在 `:2796` | 位置正确 |

**结论**: 缺一个成员函数声明。

---

### 2.12 `Engine/Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp`

| 改动 | 方案描述 | 源码实际 | 是否正确 |
|------|----------|----------|----------|
| `:1433` 附近 `BuildInstanceCullingDrawParams` 中加一行 | 仿照其他 meshpass | 源码中 `BuildInstanceCullingDrawParams` 在 `:1433` 附近 | 位置正确 |
| `:1624` `RenderForwardSinglePass` 中调用 `RenderMobileAfterTranslucencyPass` | 紧跟 `RenderTranslucency` | 源码 `RenderForwardSinglePass` 中 `RenderTranslucency` 在 `:1620` 附近,后面还有 `bDoOcclusionQueries` 与 `PreTonemapMSAA` | 位置正确 |
| `:1736` `RenderForwardMultiPass` 中调用 `RenderMobileAfterTranslucencyPass` | 紧跟 `RenderTranslucency` | 源码 `RenderForwardMultiPass` 中 `RenderTranslucency` 在 `:1732` 附近,后面是 occlusion 与 `PreTonemapMSAA` | 位置正确 |

**单 Pass 路径问题(Vulkan / Metal 验证)**: `RenderForwardSinglePass` 整段都在一个 `AddPass` 内,默认开启 `ESubpassHint::DepthReadSubpass`,意味着子通道边界由 `RHICmdList.NextSubpass()` 决定。当前主流程是 BasePass 子通道 → `NextSubpass` → translucency / decals / fog 子通道(深度 `DepthRead_StencilRead`)。在该子通道内再写一个 `DispatchDraw` 走 basepass shader 是允许的(深度测试开关由 shader 与 RHI state 决定),但**深度访问权限被 `SecondPassParameters` / 主 `PassParameters` 的子通道锁定为 read-only**,所以子通道内只能 `DepthRead`,**不能** `DepthWrite`。方案里把 `DefaultBasePassDepthStencilAccess` (= `DepthWrite_StencilWrite`) 传给新的 processor 会在 Vulkan / Metal 验证层报错。详见 §3.5。

**多 Pass 路径问题**: `RenderForwardMultiPass` 第二个 `AddPass` 显式构造了 `SecondPassParameters`,其 `DepthStencil.SetDepthStencilAccess(ExclusiveDepthStencil)` 默认为 `DepthRead_StencilRead`。如果想让 after-translucency 写深度,需要为它单独再开一个 `AddPass` 并配置为 `DepthWrite_StencilWrite`;或者就保持只读(只做深度测试,符合用户核心需求)。详见 §3.5。

**关于 VR 多视图**: 方案未显式处理多视图。在 Forward 移动 VR 下,`RenderForwardSinglePass` / `RenderForwardMultiPass` 内部已经按 `ViewContext.bIsFirstView / bIsLastView` 处理多视图与 occlusion queries。新加的 `DispatchDraw` 直接调用 `View.ParallelMeshDrawCommandPasses[...].DispatchDraw`,内部会按 `View` 数组遍历,**对 VR 立体渲染应自动生效**,但需要在设备上目视验证(单眼测试看不出)。

**结论**: 调用点位置正确,但子通道深度访问与多 Pass 路径的渲染通道安排有兼容问题。

---

### 2.13 `Engine/Source/Runtime/Engine/Public/PrimitiveViewRelevance.h`

| 改动 | 方案描述 | 源码实际 | 是否正确 |
|------|----------|----------|----------|
| `:54` 新增 `uint32 bRenderAfterTranslucency : 1;` | 紧跟 `bRenderInMainPass` | 源码中 `bRenderInMainPass` 在 `:54` 附近,下一行是 `bEditorPrimitiveRelevance` | 位置正确 |
| `:103` 构造函数新增 `bRenderAfterTranslucency = false;` | 紧跟 `bRenderInMainPass = true;` | 构造函数中 `bRenderInMainPass = true;` 在 `:103` 附近 | 位置正确 |

**结构体大小变化**:`FPrimitiveViewRelevance` 多 1 bit,理论上会让整个 struct 增加 4 字节对齐。但该结构体没有固定大小数组或序列化依赖,只是被 `memzero` 后|= 合并,**对运行时安全**。在 Shipping 包不会引发问题。

**结论**: 这一步正确。

---

### 2.14 `Engine/Source/Runtime/Engine/Private/StaticMeshRender.cpp` 与 `SkeletalMesh.cpp`

| 改动 | 方案描述 | 源码实际 | 是否正确 |
|------|----------|----------|----------|
| `StaticMeshRender.cpp:2055` `Result.bRenderAfterTranslucency = ShouldRenderAfterTranslucency();` | 紧跟 `bRenderInMainPass` 赋值 | 源码 `:2062` 是 `Result.bRenderInMainPass = ShouldRenderInMainPass();` | 位置正确 |
| `SkeletalMesh.cpp:7107` `Result.bRenderAfterTranslucency = ShouldRenderAfterTranslucency();` | 紧跟 `bRenderInMainPass` 赋值 | 源码 `:7115` 是 `Result.bRenderInMainPass = ShouldRenderInMainPass();` | 位置正确 |

**关于其他 Proxy**:`HeterogeneousVolumeComponent`、`TextRenderComponent`、`ParticleSystemRender` 这三处也有 `Result.bRenderInMainPass = ShouldRenderInMainPass();`,但都**没有 `bRenderAfterTranslucency` 字段**。由于 `FPrimitiveViewRelevance` 构造函数把 `bRenderAfterTranslucency` 默认为 `false`,这些组件的 after-translucency 永远为 `false`,**与用户需求一致**(用户只关心 Mesh / SkeletalMesh),但**应注意**:这些组件的物体如果被一个具有 `bRenderAfterTranslucency = true` 的 SkeletalMesh 引用,行为不会受影响(因为是不同的 Proxy)。

**结论**: 这一步正确,且与用户 Mesh / SkeletalMesh 需求匹配。

---

### 2.15 `Engine/Source/Runtime/Renderer/Private/SceneVisibility.cpp`

| 改动 | 方案描述 | 源码实际 | 是否正确 |
|------|----------|----------|----------|
| `:1564` 静态 mesh 路径:若 `bRenderAfterTranslucency` 则加入 `EMeshPass::MobileAfterTranslucencyPass`,否则加入 `EMeshPass::BasePass`,**保持 `MobileBasePassCSM` 调用不变** | — | 源码 `:1564` 附近是 `if (!StaticMeshRelevance.bUseSkyMaterial)` 分支 | 写法与位置正确,但**会破坏 `MobileBasePassCSM` 与 `BasePass` 的合并校验**,详见 §3.4 |
| `:2211` 动态 mesh 路径:同样的 if/else 分流 | — | 源码 `:2211` 附近是 `PassMask.Set(EMeshPass::BasePass)` | 同上,详见 §3.4 |

**严重问题**:见 §3.4 —— 在 `bMobileBasePassAlwaysUsesCSM == false` 时,`MeshDrawCommands.cpp` 中 `MergeMobileBasePassMeshDrawCommands` 内的 `checkf(MeshCommands.Num() == MeshCommandsCSM.Num(), ...)` 会在 BasePass 比 CSM 少一项时触发断言失败。

**结论**: 这一步在 `bRenderAfterTranslucency == true` 且 `bMobileBasePassAlwaysUsesCSM == false` 的组合下会触发断言失败,需要修复。

---

## 3. 关键遗漏与正确性修复

按风险高低排序。

### 3.1 【严重·必须修】 `FPrimitiveSceneProxyDesc::InitializeFrom` 缺失同步

**位置**:`Engine/Source/Runtime/Engine/Private/PrimitiveSceneProxy.cpp:265` 起的 `FPrimitiveSceneProxyDesc::InitializeFrom`。

**问题**:`PrimitiveComponent` 的 `bRenderAfterTranslucency` 字段从未被拷贝到 `FPrimitiveSceneProxyDesc`,所以 `InProxyDesc.bRenderAfterTranslucency` 始终是默认的 `false`,再传到 `FPrimitiveSceneProxy::bRenderAfterTranslucency` 始终是 `false`,再传到 `FPrimitiveViewRelevance::bRenderAfterTranslucency` 始终是 `false`。**整个 after-translucency pass 在运行时没有任何绘制**。

**修复**:

```cpp
void FPrimitiveSceneProxyDesc::InitializeFrom(const UPrimitiveComponent* InComponent)
{
    // ... 现有代码 ...
    bRenderInMainPass = InComponent->bRenderInMainPass;
    bRenderAfterTranslucency = InComponent->bRenderAfterTranslucency; // RenderAfterTranslucency Added
    // ... 现有代码 ...
}
```

行号参考:`bRenderInMainPass = InComponent->bRenderInMainPass;` 在 `PrimitiveSceneProxy.cpp:277` 之后紧跟一行。

---

### 3.2 【严重·编译失败】 `CreateMobileAfterTranslucencyPassProcessor` 函数声明缺失

**位置**:`Engine/Source/Runtime/Renderer/Private/MobileBasePass.cpp` 与 `MobileBasePassRendering.h`。

**问题**:方案只在 `.cpp` 中给出 `CreateMobileAfterTranslucencyPassProcessor` 的实现,但没有在 `MobileBasePassRendering.h` 中声明它。`REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR` 宏会展开成对 `CreateXXX` 函数的外部引用,**链接会失败**。

**修复**:在 `MobileBasePassRendering.h` 中,放在 `CreateMobileBasePassCSMProcessor` 声明之后(全文搜索该函数名确定位置),新增:

```cpp
FMeshPassProcessor* CreateMobileAfterTranslucencyPassProcessor(ERHIFeatureLevel::Type FeatureLevel, const FScene* Scene, const FSceneView* InViewIfDynamicMeshCommand, FMeshPassDrawListContext* InDrawListContext);
```

---

### 3.3 【严重·编译失败】 `RenderMobileAfterTranslucencyPass` 成员函数声明缺失

**位置**:`Engine/Source/Runtime/Renderer/Private/SceneRendering.h:2695` 附近,`FMobileSceneRenderer` 类内,与 `RenderMobileBasePass` 同级。

**问题**:方案在 `MobileBasePassRendering.cpp` 中定义 `void FMobileSceneRenderer::RenderMobileAfterTranslucencyPass(...)`,但没有在 `SceneRendering.h` 的 `FMobileSceneRenderer` 类内声明它。`MobileShadingRenderer.cpp` 中调用 `RenderMobileAfterTranslucencyPass(...)` 时**编译失败**(类外引用未声明成员函数)。

**修复**:在 `SceneRendering.h:2695` 之后紧跟:

```cpp
/** Renders the marked opaque primitives after translucency, for mobile. */
void RenderMobileAfterTranslucencyPass(FRHICommandList& RHICmdList, const FViewInfo& View, const FInstanceCullingDrawParams* InstanceCullingDrawParams);
```

---

### 3.4 【严重·运行时崩溃】 `SceneVisibility.cpp` 改动会破坏 MobileBasePassCSM 合并

**位置**:`Engine/Source/Runtime/Renderer/Private/SceneVisibility.cpp:1564`(静态路径)与 `:2211`(动态路径)。

**问题**:
1. 静态路径下,方案让 `bRenderAfterTranslucency == true` 的 mesh **不进入** `EMeshPass::BasePass` 桶,只进入 `EMeshPass::MobileAfterTranslucencyPass` + `EMeshPass::MobileBasePassCSM`。
2. 动态路径下,`ComputeDynamicMeshRelevance` 同样把 `bRenderAfterTranslucency == true` 的 mesh 设置到 `EMeshPass::MobileAfterTranslucencyPass`,**但同时**仍会 `PassMask.Set(EMeshPass::MobileBasePassCSM)`(因为这是无条件 mobile 分支)。
3. `MeshDrawCommands.cpp:235` 的 `MergeMobileBasePassMeshDrawCommands` 内部,在 `bAlwaysUseCSM == false && bMobileDynamicCSMInUse` 分支下有断言:
   ```cpp
   checkf(MeshCommands.Num() == MeshCommandsCSM.Num(), TEXT("VisibleMeshDrawCommands of BasePass and MobileBasePassCSM are expected to match."));
   ```
4. 因此**当 `bMobileBasePassAlwaysUsesCSM == false` 时,BasePass 桶比 MobileBasePassCSM 桶少一项,触发断言失败**。

**修复方案 A(推荐,改动最小)**:在 `SceneVisibility` 中**始终把 mesh 加入 BasePass**,然后让 `AddMeshBatch` 内的 `bAfterTranslucencyBasePass == false` 分支负责早退:

```cpp
// SceneVisibility.cpp 静态路径
if (!StaticMeshRelevance.bUseSkyMaterial)
{
    DrawCommandPacket.AddCommandsForMesh(... EMeshPass::BasePass);          // 永远加入
    DrawCommandPacket.AddCommandsForMesh(... EMeshPass::MobileAfterTranslucencyPass); // 永远加入
    if (!bMobileBasePassAlwaysUsesCSM)
    {
        DrawCommandPacket.AddCommandsForMesh(... EMeshPass::MobileBasePassCSM);
    }
}
```

`MobileBasePass.cpp::AddMeshBatch` 已经写好了 if/else 早退逻辑(方案中已包含),会分别在 BasePass 早退、MobileAfterTranslucencyPass 早退、MobileBasePassCSM 早退 —— `MobileBasePassCSM` 走默认 `bAfterTranslucencyBasePass == false` 分支,会早退 `bShouldRenderAfterTranslucency == true` 的物体。

**修复方案 B**:把方案中"if bRenderAfterTranslucency 才加 MobileAfterTranslucencyPass,否则加 BasePass"改为**两个都加**,加完后由 `AddMeshBatch` 过滤(同方案 A)。

注意:**动态路径 `ComputeDynamicMeshRelevance` 同样需要把 `EMeshPass::MobileAfterTranslucencyPass` 永远加进去**,而不是只在 `bRenderAfterTranslucency` 时加。

---

### 3.5 【严重·Vulkan/Metal 验证层错误】 子通道深度/模板访问冲突

**位置**:
- `Engine/Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp:1624`(`RenderForwardSinglePass`)
- `Engine/Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp:1736`(`RenderForwardMultiPass`)

**问题**:
1. **单 Pass 路径**:`RenderForwardSinglePass` 整段在一个 `AddPass` 内,使用 `ESubpassHint::DepthReadSubpass` 隐式子通道。`PostRenderBasePass` 之后调用 `RHICmdList.NextSubpass()` 进入第二个子通道,深度模板被设为 `DepthRead_StencilRead`(`PassParameters->RenderTargets.DepthStencil.SetDepthStencilAccess(FExclusiveDepthStencil::DepthRead_StencilRead)`)。在该子通道内:
   - 现有 translucent 物体走 `TranslucencyStandard` / `TranslucencyAfterDOF` 等 pass,深度只读,不写。
   - 方案中 `RenderMobileAfterTranslucencyPass` 调用 `ParallelMeshDrawCommandPasses[...].DispatchDraw`,其内部 mesh pass processor 用 `DefaultBasePassDepthStencilAccess`(= `DepthWrite_StencilWrite`)。**Vulkan / Metal 验证层会报"depth-stencil attachment is read-only in this subpass"**。
2. **多 Pass 路径**:`RenderForwardMultiPass` 的第二个 `AddPass`(`DecalsAndTranslucency`)显式构造 `SecondPassParameters`,其 `DepthStencil.SetDepthStencilAccess(ExclusiveDepthStencil)` 默认 `DepthRead_StencilRead`。在该 pass 内同样不允许写深度。

**修复方案**:
- **如果业务上不需要写深度**(只读取 depth 做深度测试),把 `CreateMobileAfterTranslucencyPassProcessor` 改为:
  ```cpp
  PassDrawRenderState.SetDepthStencilAccess(FExclusiveDepthStencil::DepthRead_StencilRead);
  ```
  这样只做深度测试、不写深度,**完美契合用户需求**(用户只要求"压住透明物体",不需要写深度影响后续 pass)。
- 如果**确实需要写深度**,则需要把 `RenderMobileAfterTranslucencyPass` 放在**新的独立 `AddPass`** 中,该 pass 配置 `DepthWrite_StencilWrite`,在 `RenderForwardMultiPass` 中较为容易,在 `RenderForwardSinglePass` 中需要再调用一次 `RHICmdList.NextSubpass()` 或把整个子通道拆开(改动较大,需谨慎)。

**推荐方案**:采用 `DepthRead_StencilRead`,因为用户的核心需求是"让标记物体渲染在透明物体之上并被前方不透明物体正确遮挡",**不需要写深度**。

---

### 3.6 【次要】 `MobileBasePassCSM` 桶的 filter 一致性

**位置**:`MobileBasePass.cpp:1180` 附近的 `CreateMobileBasePassCSMProcessor`。

**问题**:方案没有显式修改 `CreateMobileBasePassCSMProcessor` 也没有传 `bAfterTranslucencyBasePass`,但因为新参数有默认值 `false`,`AddMeshBatch` 在 CSM pass 走 else 分支会过滤掉 `bShouldRenderAfterTranslucency == true` 的物体。**行为正确**,但阅读代码时容易忽略,建议在 `CreateMobileBasePassCSMProcessor` 中显式注释说明。

---

### 3.7 【次要·VR 性能】 Tile-Based GPU 上的 tile memory 反复加载

**位置**:`MobileShadingRenderer.cpp` 的两个调用点。

**问题**:Adreno / Mali / Apple GPU 都是 Tile-Based,理想情况下整帧 color + depth 都保存在 on-chip tile memory。`RenderTranslucency` 结束后通常会做一次 store(因为后续 post-process 需要解析 color 与 depth)。在 store 之后再插入一个读 depth / 写 color 的 pass,会**强制**把 depth 从 main memory 重新 load 到 tile,**破坏 tile 局部性**。在 Adreno 6xx / Mali-G78 等较新 GPU 上,驱动会插入额外的 load/store,带来性能损失。

**建议**:
- 在真机上用 Snapdragon Profiler / RenderDoc 抓帧,确认 VR 性能影响是否可接受。
- 如果性能不可接受,考虑使用 `r.Mobile.UseHWsRGBEncoding` / `r.Mobile.SupportGPUScene` 等相关 cvar,或与 Epic 团队的 `LateSubpass` / `OnChipDepthResolve` 选项对齐。
- 长远看,若 Epic 后续把 `EMobileSubpassHint` 扩展成更细粒度,可考虑把 after-translucency 纳入第三个 subpass。

---

### 3.8 【次要·PSO 预编译】 `REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR` 的 PSO 编译开销

**位置**:`MobileBasePass.cpp:1223`。

**问题**:`REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR(MobileAfterTranslucencyPass, CreateMobileAfterTranslucencyPassProcessor, EShadingPath::Mobile, EMeshPass::MobileAfterTranslucencyPass, EMeshPassFlags::CachedMeshCommands | EMeshPassFlags::MainView);` 会让 PSO 预编译器**为新 pass 额外缓存一份 PSO 状态**。对移动端 PSO 数量敏感的平台(主要是 Android Vulkan / Metal),这会增加 shader cache 大小和首次加载时间。

**建议**:
- 确认项目对 shader cache 大小的限制。
- 如果 PSO 数量爆炸,考虑用 `EMeshPassFlags::CachedMeshCommands` 之外的其他 flag 配合自定义剔除。

---

### 3.9 【次要·`MeshPassProcessor` 之外】 自定义 Primitive 类型可能漏改

**位置**:`Engine/Source/Runtime/Engine/Private/PrimitiveSceneProxy.cpp` 等。

**问题**:
- `HeterogeneousVolumeComponent`、`TextRenderComponent`、`ParticleSystemRender` 三处的 `GetViewRelevance` 没有同步 `bRenderAfterTranslucency` 字段。但因为 `FPrimitiveViewRelevance` 构造函数中默认 `false`,这些组件的 after-translucency 永远为 `false`,**与用户"只要 Mesh / SkeletalMesh"需求一致**,不是 bug。
- 但 `FVolumetricCloudSceneProxy` 等自定义 Proxy 也有自己的 `bRenderInMainPass` 字段(`SceneRendering.cpp:1539`),如果后续要扩展到这些类型,需要在 `PrimitiveSceneProxyDesc.h` / `PrimitiveSceneProxy.h` / `PrimitiveSceneProxy.cpp` 同步添加 `bRenderAfterTranslucency`,并在新加 pass 中调用 `ShouldRenderAfterTranslucency()` 过滤。

---

### 3.10 【次要·编辑器路径】 `RenderMobileEditorPrimitives` 行为

**位置**:`MobileBasePassRendering.cpp:492` 附近。

**问题**:`RenderMobileBasePass` 末尾会调用 `RenderMobileEditorPrimitives`,后者用 `FEditorPrimitivesBasePassMeshProcessor` 走自己的一套 mesh pass processor,**不走 `FMobileBasePassMeshProcessor`**,所以新加的 `bAfterTranslucencyBasePass` 过滤不会影响编辑器图元。**这是预期行为**(编辑器图元不算"业务物体"),但如果要让编辑器图元也参与 after-translucency 渲染,需要在 `RenderMobileEditorPrimitives` 之后再额外调用一次 `RenderMobileAfterTranslucencyPass`(并传入合适的 `InstanceCullingDrawParams`)。

---

## 4. 推荐修改顺序

按依赖关系与风险递减的顺序:

1. **(必须)** `PrimitiveSceneProxy.cpp::InitializeFrom` 补 `bRenderAfterTranslucency = InComponent->bRenderAfterTranslucency;` —— 否则所有下游失效。
2. **(必须)** `SceneRendering.h` 在 `FMobileSceneRenderer` 中声明 `RenderMobileAfterTranslucencyPass` —— 解决编译失败。
3. **(必须)** `MobileBasePassRendering.h` 声明 `CreateMobileAfterTranslucencyPassProcessor` —— 解决链接失败。
4. **(必须)** `SceneVisibility.cpp` 静态 + 动态路径**永远同时加入 `BasePass` 与 `MobileAfterTranslucencyPass`**,由 `AddMeshBatch` 过滤 —— 解决合并校验失败。
5. **(强烈建议)** `CreateMobileAfterTranslucencyPassProcessor` 中把 `DefaultBasePassDepthStencilAccess` 改为 `FExclusiveDepthStencil::DepthRead_StencilRead` —— 避免 Vulkan / Metal 验证层报错;与用户核心需求一致。
6. **(建议)** 文档化"只对 StaticMesh / SkeletalMesh 生效"的行为,避免后续误解。
7. **(建议)** 在真机上做性能 profile,确认 tile-based GPU 的额外 load/store 影响。

---

## 5. 完整性清单

把方案中所有改动按"必须/建议补"分类,得到下表。

| # | 文件 | 改动 | 状态 |
|---|------|------|------|
| 1 | `MeshPassProcessor.h` | 新增 enum、case、`static_assert` | 方案正确,叙述中"原值 33"需改为 32 |
| 2 | `PrimitiveComponent.h` | 字段 + Setter | 正确 |
| 3 | `PrimitiveComponent.cpp` | 字段初始化 + Setter 实现 | 正确 |
| 4 | `PrimitiveSceneProxy.h` | 字段 + inline getter | 正确 |
| 5 | `PrimitiveSceneProxy.cpp` | InComponent / InProxyDesc 初始化 | **必须补 InitializeFrom** |
| 6 | `PrimitiveSceneProxyDesc.h` | 字段 + 默认值 | 行号有 6 行偏差,语义正确 |
| 7 | `MobileBasePassRendering.h` | 字段 + 构造参数 | **必须补 `CreateMobileAfterTranslucencyPassProcessor` 声明** |
| 8 | `MobileBasePass.cpp` | 构造实现 + `AddMeshBatch` + 新 processor + 注册 | 正确(行号偏移 8 行) |
| 9 | `MobileBasePassRendering.cpp` | `RenderMobileAfterTranslucencyPass` 函数体 | **depth access 需调整(见 §3.5)**,函数位置正确 |
| 10 | `RenderCore.cpp` | `DEFINE_STAT` | 正确 |
| 11 | `SceneRendering.h` | 字段 + 成员函数声明 | **必须补 `RenderMobileAfterTranslucencyPass` 成员声明** |
| 12 | `MobileShadingRenderer.cpp` | `BuildInstanceCullingDrawParams` + 两个调用点 | 位置正确,子通道需协同 §3.5 |
| 13 | `PrimitiveViewRelevance.h` | 字段 + 默认值 | 正确 |
| 14 | `StaticMeshRender.cpp` / `SkeletalMesh.cpp` | `GetViewRelevance` 同步 | 正确 |
| 15 | `SceneVisibility.cpp` | 静态 + 动态路径分流 | **必须修,见 §3.4** |

---

## 6. 总结

方案在**主干思路上与官方 Mobile BasePass 扩展点吻合**,通过新增 `EMeshPass` + 复用 `FMobileBasePassMeshProcessor` 的方式实现"after-translucency opaque"是合理的。但方案在**数据流(`InitializeFrom`)、编译完整性(头文件声明)、运行时正确性(`MobileBasePassCSM` 合并、深度/模板子通道兼容)** 上有 4 处必须修复的遗漏,以及 5 处次要问题(VR 性能、PSO 缓存、其他 Proxy、编辑器图元、tile-based GPU 兼容性)。

按 §4 的顺序修复后,功能可以正确工作。建议在真机上对 depth 写行为、tile load/store、PSO 缓存做一次 profile 验证。

---

**分析人**:Codex (GPT-5)
**分析对象**:UE 5.4 引擎源码 + 用户方案
