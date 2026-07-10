# 复用 FDepthPassMeshProcessor 渲染 MobileAfterTranslucencyDepthPass 方案分析

> 目标：移动端 Android Forward 路径，仅对标记物体（`bRenderAfterTranslucency`）生效。
> 在 BasePass 之后写入标记物体深度（不写颜色），透明物体之后再绘制标记物体颜色（读深度遮挡透明物体）。

---

## 一、结论

**可行，且比复用 `FMobileBasePassMeshProcessor` 写深度更高效，推荐采用。**

- **深度 Pass（MobileAfterTranslucencyDepthPass）**：改用 `FDepthPassMeshProcessor`。
  它使用 `TDepthOnlyVS` + `FDepthOnlyPS`（甚至 position-only 流），shader 极轻量；
  而原方案里用 `FMobileBasePassMeshProcessor` + `CW_NONE`，虽然不写颜色，但**仍然会跑完整的
  BasePass 像素着色器**（采样贴图、算光照），只是把颜色写入屏蔽掉，纯属浪费，移动端开销明显。
- **颜色 Pass（MobileAfterTranslucencyPass）**：**必须**继续用 `FMobileBasePassMeshProcessor`，
  因为它需要完整光照着色。这一半保持原方案不变。

即：原方案中第 4、7 节里关于 `CreateMobileAfterTranslucencyDepthPassProcessor` 用
`FMobileBasePassMeshProcessor` 的部分应删除，改为下面的 `FDepthPassMeshProcessor` 版本。

---

## 二、如何在 AddMeshBatch 中做区分

`FDepthPassMeshProcessor::AddMeshBatch`（`DepthRendering.cpp:1021`）**没有**按 `MeshPassType`
分流的逻辑，需要自己加。核心区分依据仍然是
`PrimitiveSceneProxy->ShouldRenderAfterTranslucency()`：

- 当 `MeshPassType == EMeshPass::MobileAfterTranslucencyDepthPass`：**只接受**标记物体；
- 当 `MeshPassType == EMeshPass::DepthPass`（普通早 Z / 深度预通道）：**必须排除**标记物体，
  否则标记物体会在 BasePass 之前就写入深度，破坏「BasePass 之后才写深度」的需求
  （在开启 Full Depth Prepass 时尤其重要）。

修改 `FDepthPassMeshProcessor::AddMeshBatch` 开头：

```cpp
void FDepthPassMeshProcessor::AddMeshBatch(const FMeshBatch& RESTRICT MeshBatch, uint64 BatchElementMask, const FPrimitiveSceneProxy* RESTRICT PrimitiveSceneProxy, int32 StaticMeshId)
{
    bool bDraw = MeshBatch.bUseForDepthPass;

    //RenderAfterTranslucency Added —— 按 Pass 类型与标记分流
    const bool bAfterTransDepthPass = (MeshPassType == EMeshPass::MobileAfterTranslucencyDepthPass);
    const bool bShouldRenderAfterTranslucency = PrimitiveSceneProxy && PrimitiveSceneProxy->ShouldRenderAfterTranslucency();
    if (bAfterTransDepthPass)
    {
        if (!bShouldRenderAfterTranslucency) return;
    }
    else // 普通 DepthPass / SecondStageDepthPass 等
    {
        if (bShouldRenderAfterTranslucency) return;
    }
    // ... 后续原有逻辑不变
```

> 注意：此函数被 Deferred 的 DepthPass / SecondStagePass 共用。上面 `else` 分支会让标记物体
> 也从所有非 AfterTranslucency 的深度通道中排除。若担心影响 PC/Deferred，可加
> `FeatureLevel == ERHIFeatureLevel::ES3_1` 或 `MeshPassType == EMeshPass::DepthPass` 的限定。

---

## 三、Processor 的创建与注册（替换原方案的深度 Processor）

在 `DepthRendering.cpp` 中新增创建函数（仿照 `CreateDepthPassProcessor`，`DepthRendering.cpp:1230`）：

```cpp
//RenderAfterTranslucency Added
FMeshPassProcessor* CreateMobileAfterTranslucencyDepthPassProcessor(ERHIFeatureLevel::Type FeatureLevel, const FScene* Scene, const FSceneView* InViewIfDynamicMeshCommand, FMeshPassDrawListContext* InDrawListContext)
{
    FMeshPassProcessorRenderState DepthPassState;
    SetupDepthPassState(DepthPassState); // CW_NONE + 深度写 + CF_DepthNearOrEqual

    // 关键：强制 DDM_AllOpaque，保证标记的「非 Masked 不透明」物体也会写深度；
    // bRespectUseAsOccluderFlag=false 关闭遮挡体/屏幕尺寸过滤；bEarlyZPassMovable=true 允许可移动物体。
    return new FDepthPassMeshProcessor(
        EMeshPass::MobileAfterTranslucencyDepthPass, Scene, FeatureLevel, InViewIfDynamicMeshCommand,
        DepthPassState, /*bRespectUseAsOccluderFlag*/ false, DDM_AllOpaque, /*bEarlyZPassMovable*/ true,
        /*bDitheredLODFadingOutMaskPass*/ false, InDrawListContext);
}

REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR(MobileAfterTranslucencyDepthPass, CreateMobileAfterTranslucencyDepthPassProcessor, EShadingPath::Mobile, EMeshPass::MobileAfterTranslucencyDepthPass, EMeshPassFlags::CachedMeshCommands | EMeshPassFlags::MainView);
```

> **重点（易踩坑）**：不要直接用 `FScene::GetEarlyZPassMode()` 取场景的 EarlyZPassMode。
> 移动端常见为 `DDM_None` 或 `DDM_MaskedOnly`，此时
> `FDepthPassMeshProcessor::ShouldRender`（`DepthRendering.cpp:939`）会把**非 Masked 的不透明物体跳过**，
> 导致标记物体深度根本不写。**必须硬编码传 `DDM_AllOpaque`**。

颜色 Pass `CreateMobileAfterTranslucencyPassProcessor` 仍放在 `MobileBasePass.cpp`，保持原方案不变。

---

## 四、相比原方案需要调整 / 注意的点

### 1. 删除 MobileBasePass.cpp 中的深度 Processor
原方案第 4 节的 `CreateMobileAfterTranslucencyDepthPassProcessor`（用 `FMobileBasePassMeshProcessor`，
`CW_NONE`）以及第 7 节 `CollectPSOInitializers` 里对 `MobileAfterTranslucencyDepthPass` 的处理
（`MobileBasePass.cpp:1056` 附近）应**移除**，深度 Pass 交给 `FDepthPassMeshProcessor`。
`MobileBasePass.cpp` 的 `AddMeshBatch` 分流只需保留对 **颜色 Pass（MobileAfterTranslucencyPass）** 的判断。

### 2. `MobileBasePass.cpp::AddMeshBatch` 的分流要改写
原方案让 BasePass 的 `bShouldRenderAfterTranslucency` 物体直接 `return`，这点保留。
但深度 Pass 已不走该 Processor，所以分流条件简化为：

```cpp
const bool bAfterTransColorPass = (MeshPassType == EMeshPass::MobileAfterTranslucencyPass);
const bool bShouldRenderAfterTranslucency = PrimitiveSceneProxy && PrimitiveSceneProxy->ShouldRenderAfterTranslucency();
if (bAfterTransColorPass)
{
    if (!bShouldRenderAfterTranslucency) return;
}
else // BasePass / MobileBasePassCSM 等
{
    if (bShouldRenderAfterTranslucency) return;
}
```

### 3. `bUseForDepthPass` 必须为 true
`FDepthPassMeshProcessor::AddMeshBatch` 第一行 `bool bDraw = MeshBatch.bUseForDepthPass;`。
StaticMesh 的该标志由 `FStaticMeshBatch`（`StaticMeshBatch.cpp:41`）从 `StaticMesh.bUseForDepthPass`
拷贝，且在 `StaticMeshRender.cpp:1316/1367` 处可能因 LOD/统一网格逻辑被改写。
**验证点**：标记物体进入 `MobileAfterTranslucencyDepthPass` 时 `bUseForDepthPass` 必须为 1，
否则深度不写。Skeletal/动态网格默认 `bUseForDepthPass(true)`（`MeshBatch.h:485`），一般没问题。

### 4. `ShouldRenderInDepthPass()` 不会误伤
`TryAddMeshBatch`（`DepthRendering.cpp:977`）会判断
`ShouldRenderInDepthPass() = bRenderInMainPass || bRenderInDepthPass`（`PrimitiveSceneProxy.h:701`）。
原方案标记物体仍保留 `bRenderInMainPass = true`，所以此条满足，无需额外改动。
（若后续把标记物体的 `bRenderInMainPass` 改 false，这里会拦截，需一并放开。）

### 5. SceneVisibility 路由
原方案第 6 节在 `SceneVisibility.cpp` 静态/动态网格里向
`MobileAfterTranslucencyDepthPass` 和 `MobileAfterTranslucencyPass` 各 `AddCommandsForMesh` 一次，
这部分**保留有效**。深度 Pass 用 `FDepthPassMeshProcessor` 不改变 visibility 端的添加方式，
因为最终都会回调对应 Processor 的 `AddMeshBatch`，由其内部按 `bUseForDepthPass` / 标记过滤。

### 6. PSO 预缓存
`FDepthPassMeshProcessor::CollectPSOInitializers`（`DepthRendering.cpp:1096`）会自动为新 Pass 类型
预缓存 depth-only PSO，无需在其中特判排除（与原方案需在 `MobileBasePass::CollectPSOInitializers`
里 `return` 不同，这里反而**应该让它正常预缓存**）。

### 7. 渲染调用位置（与原方案一致，确认无误）
- `RenderMobileAfterTranslucencyDepthPass` 放在 `RenderMobileBasePass` 之后、`NextSubpass` 之前
  （单 Pass Forward：`MobileShadingRenderer.cpp:1609` 附近）。此时深度仍可写。
- `RenderMobileAfterTranslucencyPass`（颜色）放在 `RenderTranslucency` 之后
  （`MobileShadingRenderer.cpp:1623` 附近）。此时已 `NextSubpass`，深度为只读，
  颜色 Processor 用「深度测试开、深度写关」（原方案 `CF_DepthNearOrEqual`, 深度 false）正确。

> **多 Pass 路径补充**：`RenderForwardMultiPass`（`MobileShadingRenderer.cpp:1662`）里 BasePass 与
> Translucency 分处两个 `AddPass`/RenderPass。深度 Pass 调用要放在**第一个 Pass（BasePass 那个 lambda）
> 内**、`PostRenderBasePass` 之前；颜色 Pass 放在第二个 `DecalsAndTranslucency` Pass 的
> `RenderTranslucency` 之后。原方案只在 `RenderForwardSinglePass` 加了调用，**多 Pass 路径会遗漏**，
> 若项目可能触发 `bRequiresMultiPass`（如开启某些后处理/自定义深度），需补上。

### 8. 关于 `CW_NONE` 是否影响后续 Translucency（原方案第 4 节疑问）
**不会**。每个 draw command 把自己的 BlendState 固化进各自 PSO，Processor 设置的 RenderState
只作用于它自己 `BuildMeshDrawCommands` 出来的命令。Translucency 各 Processor 自带 BlendState，
互不干扰。

---

## 五、改动量评估

| 模块 | 改动 | 量级 |
|------|------|------|
| `MeshPassProcessor.h` 枚举/名称/断言 | 同原方案 | 小 |
| `PrimitiveComponent` / `SceneProxy` / `ProxyDesc` / `ViewRelevance` 标记字段 | 同原方案 | 小 |
| `DepthRendering.cpp::AddMeshBatch` 加分流 | 新增 ~8 行 | 小 |
| `DepthRendering.cpp` 新增 Create + 注册 | 新增 ~15 行 | 小 |
| `MobileBasePass.cpp` 只保留颜色 Pass Processor + 分流 | 比原方案更少 | 小 |
| `MobileShadingRenderer.cpp` 调用 + InstanceCulling 参数 | 同原方案（注意多 Pass 路径） | 中 |
| `SceneVisibility.cpp` 路由 | 同原方案 | 小 |

**总体改动量与原方案相当甚至更小**（深度 Pass 不再需要在 MobileBasePass 里写一套 Processor 与
PSO 特判），且运行时性能更好（depth-only shader）。唯一新增风险点是 **`DDM_AllOpaque` 强制** 与
**多 Pass 路径调用遗漏**，按第三、四节处理即可。

---

## 六、待你验证的清单

1. 标记物体的 `FMeshBatch.bUseForDepthPass` 在 `MobileAfterTranslucencyDepthPass` 中为 1。
2. 工程实际的 `Scene->EarlyZPassMode`（确认普通 DepthPass 是否会带标记物体，验证第二节 `else` 排除生效）。
3. 是否会走 `RenderForwardMultiPass`（决定是否需要补多 Pass 调用）。
4. 颜色 Pass 的深度比较：BasePass 后深度已写入标记物体，颜色 Pass 用 `CF_DepthNearOrEqual` 且
   深度写关，确认能正确只在标记表面着色且遮挡透明物体。
