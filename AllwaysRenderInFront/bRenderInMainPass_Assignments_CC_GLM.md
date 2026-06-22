# `bRenderInMainPass` 赋值点全集

> 目标字段：`FPrimitiveViewRelevance::bRenderInMainPass`
> 声明位置：`Engine/Source/Runtime/Engine/Public/PrimitiveViewRelevance.h:54`
> 含义注释：`/** The primitive should render to the base pass / normal depth / velocity rendering. */`
> 搜索范围：`Engine/Source/Runtime` + `Engine/Source/Runtime/Experimental` + `Engine/Plugins`
> 引擎版本：UE 5.4（MR01_DaNaoTianGong_Main）

---

## 0. 字段语义与"同名字段"说明

`bRenderInMainPass` 这一名字在引擎中存在 **三处不同结构体的同名字段**，它们共同构成一条数据流：

| 层级 | 类型 | 字段位置 | 含义 |
|------|------|----------|------|
| 游戏线程数据源 | `UPrimitiveComponent::bRenderInMainPass`（以及 `UVolumetricCloudComponent` / `USkyAtmosphereComponent` / `UExponentialHeightFogComponent` 等少数非 Primitive 组件） | `PrimitiveComponent.h` 等 | 用户可配置的"是否渲染到主 pass" |
| 渲染线程描述 | `FPrimitiveSceneProxy::bRenderInMainPass` | `PrimitiveSceneProxy.h` | 由 Component 的同名字段镜像而来，proxy 内部状态 |
| 渲染线程相关性 | **`FPrimitiveViewRelevance::bRenderInMainPass`** ← 本文聚焦 | `PrimitiveViewRelevance.h:54` | 每帧每视图重新计算，决定该 mesh 本帧是否进入 BasePass |
| 注册描述 | `FPrimitiveSceneProxyDesc::bRenderInMainPass` | `PrimitiveSceneProxyDesc.h` | 构造 proxy 时的快照（PSO 预缓存路径） |
| PSO 预缓存参数 | `FPSOPrecacheParams::bRenderInMainPass` | `PSOPrecache.h` | 仅用于 PSO 收集，不参与运行时可见性 |

下文将所有赋值点按"目标对象"分组列出。`Result.bRenderInMainPass = ...` 这一形式（占绝大多数）即针对本字段 `FPrimitiveViewRelevance::bRenderInMainPass`。

---

## 1. `FPrimitiveViewRelevance::bRenderInMainPass` 的赋值点

### 1.1 构造函数默认值

| 文件:行 | 赋值表达式 | 备注 |
|---------|-----------|------|
| `Engine/Source/Runtime/Engine/Public/PrimitiveViewRelevance.h:103` | `bRenderInMainPass = true;` | **默认构造函数**。先 memset 0 全字段，再对少数"例外位"置 true；注释明确写道：`// without it BSP doesn't render`。这是所有 `Result` 对象的隐式起点 |

### 1.2 Engine 内置 SceneProxy 的 `GetViewRelevance(...)` 实现

绝大多数 SceneProxy 都通过 `Result.bRenderInMainPass = ShouldRenderInMainPass();` 把 proxy 内部状态导出到 relevance。`FPrimitiveSceneProxy::ShouldRenderInMainPass()` 直接返回 proxy 上的 `bRenderInMainPass`（`PrimitiveSceneProxy.h:700`）。

| 文件:行 | 赋值表达式 | 备注 |
|---------|-----------|------|
| `Engine/Source/Runtime/Engine/Private/StaticMeshRender.cpp:2062` | `Result.bRenderInMainPass = ShouldRenderInMainPass();` | `FStaticMeshSceneProxy::GetViewRelevance` |
| `Engine/Source/Runtime/Engine/Private/SkeletalMesh.cpp:7115` | `Result.bRenderInMainPass = ShouldRenderInMainPass();` | 骨骼网格 proxy |
| `Engine/Source/Runtime/Engine/Private/Particles/ParticleSystemRender.cpp:6856` | `Result.bRenderInMainPass = ShouldRenderInMainPass();` | 粒子系统 proxy |
| `Engine/Source/Runtime/Engine/Private/Components/TextRenderComponent.cpp:857` | `Result.bRenderInMainPass = ShouldRenderInMainPass();` | 文本组件 proxy |
| `Engine/Source/Runtime/Engine/Private/Components/HeterogeneousVolumeComponent.cpp:171` | `Result.bRenderInMainPass = ShouldRenderInMainPass();` | 异构体积 proxy |
| `Engine/Source/Runtime/Engine/Private/Rendering/NaniteResources.cpp:1031` | `Result.bRenderInMainPass = true;` | **强制 true**，注释：`// Should always be covered by constructor of Nanite scene proxy.` |
| `Engine/Source/Runtime/Landscape/Private/LandscapeRender.cpp:1987` | `Result.bRenderInMainPass = ShouldRenderInMainPass();` | `FLandscapeComponentSceneProxy::GetViewRelevance` 主分支 |
| `Engine/Source/Runtime/Landscape/Private/LandscapeRender.cpp:2140` | `Result.bRenderInMainPass = false;` | Landscape 的特殊调试/可视化路径：在非编辑器或不满足条件时**强制 false** |
| `Engine/Source/Runtime/GeometryFramework/Private/Components/DynamicMeshSceneProxy.h:634` | `Result.bRenderInMainPass = ShouldRenderInMainPass();` | 动态网格 proxy |
| `Engine/Source/Runtime/UMG/Private/Components/WidgetComponent.cpp:559` | `Result.bRenderInMainPass = ShouldRenderInMainPass();` | UMG Widget Component |
| `Engine/Source/Runtime/MRMesh/Private/MRMeshComponent.cpp:492` | `Result.bRenderInMainPass = (bUseWireframe \|\| MaterialToUse != UMaterial::GetDefaultMaterial(MD_Surface)) && ShouldRenderInMainPass();` | **条件性赋值**：仅在使用线框或有效材质时进入主 pass |
| `Engine/Source/Runtime/Experimental/GeometryCollectionEngine/Private/GeometryCollection/GeometryCollectionSceneProxy.cpp:936` | `Result.bRenderInMainPass = ShouldRenderInMainPass();` | Chaos GeometryCollection 普通 proxy |
| `Engine/Source/Runtime/Experimental/GeometryCollectionEngine/Private/GeometryCollection/GeometryCollectionSceneProxy.cpp:1171` | `Result.bRenderInMainPass = true;` | **强制 true**，与 Nanite 路径同样注释：构造函数已覆盖 |
| `Engine/Source/Runtime/Renderer/Private/SparseVolumeTexture/SparseVolumeTextureViewerSceneProxy.cpp:77` | `Result.bRenderInMainPass = ShouldRenderInMainPass();` | 稀疏体积纹理查看器 |

### 1.3 Plugins 路径下的 SceneProxy `GetViewRelevance` 实现

| 文件:行 | 赋值表达式 | 备注 |
|---------|-----------|------|
| `Engine/Plugins/2D/Paper2D/Source/Paper2D/Private/PaperRenderSceneProxy.cpp:512` | `Result.bRenderInMainPass = ShouldRenderInMainPass();` | Paper2D |
| `Engine/Plugins/Experimental/Water/Source/Runtime/Private/WaterMeshSceneProxy.cpp:1106` | `Result.bRenderInMainPass = ShouldRenderInMainPass();` | Water Plugin |
| `Engine/Plugins/Experimental/Dataflow/Source/DataflowEnginePlugin/Private/Dataflow/DataflowEngineSceneProxy.cpp:468` | `Result.bRenderInMainPass = ShouldRenderInMainPass();` | Chaos Dataflow |
| `Engine/Plugins/Experimental/ImagePlate/Source/ImagePlate/Private/ImagePlateComponent.cpp:224` | `Result.bRenderInMainPass = ShouldRenderInMainPass();` | Image Plate |
| `Engine/Plugins/Experimental/VirtualHeightfieldMesh/Source/VirtualHeightfieldMesh/Private/VirtualHeightfieldMeshSceneProxy.cpp:536` | `Result.bRenderInMainPass = ShouldRenderInMainPass();` | 虚拟高度场网格 |
| `Engine/Plugins/Importers/USDImporter/Source/USDClasses/Private/USDDrawModeComponent.cpp:619` | `Result.bRenderInMainPass = ShouldRenderInMainPass();` | USD 导入器绘制模式 |
| `Engine/Plugins/Enterprise/LidarPointCloud/Source/LidarPointCloudRuntime/Private/Rendering/LidarPointCloudRendering.cpp:301` | `Result.bRenderInMainPass = ShouldRenderInMainPass();` | LiDAR 点云 |
| `Engine/Plugins/FX/Niagara/Source/Niagara/Private/NiagaraComponent.cpp:286` | `Relevance.bRenderInMainPass = ShouldRenderInMainPass();` | **Niagara 粒子组件**（注意变量名 `Relevance`） |
| `Engine/Plugins/Runtime/CableComponent/Source/CableComponent/Private/CableComponent.cpp:416` | `Result.bRenderInMainPass = ShouldRenderInMainPass();` | Cable Component |
| `Engine/Plugins/Runtime/CustomMeshComponent/Source/CustomMeshComponent/Private/CustomMeshComponent.cpp:159` | `Result.bRenderInMainPass = ShouldRenderInMainPass();` | Custom Mesh Component |
| `Engine/Plugins/Runtime/HairStrands/Source/HairStrandsCore/Private/GroomComponent.cpp:1091` | `Result.bRenderInMainPass = bUseCardsOrMesh && ShouldRenderInMainPass();` | **条件性赋值**：仅 cards/mesh 表示形式才进入主 pass，strands 形式不进入。这也是 `SceneVisibility.cpp:2356` 注释"Hair strands are not rendered into the base pass"的来源 |
| `Engine/Plugins/Runtime/ProceduralMeshComponent/Source/ProceduralMeshComponent/Private/ProceduralMeshComponent.cpp:423` | `Result.bRenderInMainPass = ShouldRenderInMainPass();` | Procedural Mesh Component |
| `Engine/Plugins/Runtime/MeshModelingToolset/Source/ModelingComponents/Private/Drawing/TriangleSetComponent.cpp:171` | `Result.bRenderInMainPass = ShouldRenderInMainPass();` | 建模工具集 — 三角面集 |
| `Engine/Plugins/Runtime/MeshModelingToolset/Source/ModelingComponents/Private/Drawing/PointSetComponent.cpp:201` | `Result.bRenderInMainPass = ShouldRenderInMainPass();` | 建模工具集 — 点集 |
| `Engine/Plugins/Runtime/MeshModelingToolset/Source/ModelingComponents/Private/Drawing/MeshWireframeComponent.cpp:313` | `Result.bRenderInMainPass = ShouldRenderInMainPass();` | 建模工具集 — 线框 |
| `Engine/Plugins/Runtime/MeshModelingToolset/Source/ModelingComponents/Private/Drawing/LineSetComponent.cpp:187` | `Result.bRenderInMainPass = ShouldRenderInMainPass();` | 建模工具集 — 线集 |
| `Engine/Plugins/Runtime/MeshModelingToolset/Source/ModelingComponents/Private/Components/OctreeDynamicMeshSceneProxy.h:235` | `Result.bRenderInMainPass = ShouldRenderInMainPass();` | Octree 动态网格 |
| `Engine/Plugins/Editor/UVEditor/Source/UVEditorTools/Private/Drawing/BasicTriangleSetComponent.cpp:169` | `Result.bRenderInMainPass = ShouldRenderInMainPass();` | UV 编辑器 — 三角面集 |
| `Engine/Plugins/Editor/UVEditor/Source/UVEditorTools/Private/Drawing/BasicPointSetComponent.cpp:172` | `Result.bRenderInMainPass = ShouldRenderInMainPass();` | UV 编辑器 — 点集 |
| `Engine/Plugins/Editor/UVEditor/Source/UVEditorTools/Private/Drawing/BasicLineSetComponent.cpp:180` | `Result.bRenderInMainPass = ShouldRenderInMainPass();` | UV 编辑器 — 线集 |

### 1.4 渲染线程内部聚合/合并

| 文件:行 | 赋值表达式 | 备注 |
|---------|-----------|------|
| `Engine/Source/Runtime/Renderer/Private/PrimitiveSceneInfo.cpp:727` | `CombinedPrimitiveRelevance.bRenderInMainPass = true;` | 在 Nanite drawlist 应用阶段（`NaniteDrawListApply` scope）构造合成 relevance，整体强制 true |

---

## 2. `FPrimitiveSceneProxy::bRenderInMainPass` 的赋值点（数据源 → 渲染线程）

虽然不属于本字段直接赋值，但它是 §1 中 `ShouldRenderInMainPass()` 的最终数据源，列在此供参照。

| 文件:行 | 赋值表达式 | 备注 |
|---------|-----------|------|
| `Engine/Source/Runtime/Engine/Private/PrimitiveSceneProxy.cpp:277` | `bRenderInMainPass = InComponent->bRenderInMainPass;` | `FPrimitiveSceneProxy` 构造时从 `UPrimitiveComponent` 镜像 |

---

## 3. `UPrimitiveComponent::bRenderInMainPass` 及其它 Component 同名字段的赋值点（游戏线程）

| 文件:行 | 赋值表达式 | 所属类 / 上下文 |
|---------|-----------|----------------|
| `Engine/Source/Runtime/Engine/Private/Components/PrimitiveComponent.cpp:333` | `bRenderInMainPass = true;` | `UPrimitiveComponent::UPrimitiveComponent` 构造函数默认值 |
| `Engine/Source/Runtime/Engine/Private/Components/PrimitiveComponent.cpp:4461` | `bRenderInMainPass = bValue;` | `UPrimitiveComponent::SetRenderInMainPass(bool bValue)`；写入后调用 `MarkRenderStateDirty()` 触发 proxy 重建 |
| `Engine/Source/Runtime/Engine/Private/Components/VolumetricCloudComponent.cpp:184` | `bRenderInMainPass = bValue;` | `UVolumetricCloudComponent::SetRenderInMainPass` |
| `Engine/Source/Runtime/Engine/Private/Components/SkyAtmosphereComponent.cpp:83` | `bRenderInMainPass = true;` | `USkyAtmosphereComponent` 构造默认值 |
| `Engine/Source/Runtime/Engine/Private/Components/SkyAtmosphereComponent.cpp:406` | `bRenderInMainPass = bValue;` | `USkyAtmosphereComponent::SetRenderInMainPass` |
| `Engine/Source/Runtime/Engine/Private/Components/HeightFogComponent.cpp:47` | `bRenderInMainPass = true;` | `UExponentialHeightFogComponent` 构造默认值 |
| `Engine/Source/Runtime/Engine/Private/Components/HeightFogComponent.cpp:368` | `bRenderInMainPass = bValue;` | `UExponentialHeightFogComponent::SetRenderInMainPass` |
| `Engine/Plugins/Experimental/Avalanche/Source/Avalanche/Public/Framework/AvaGizmoComponent.h:226` | `bool bRenderInMainPass = true;` | `UAvaGizmoComponent` 类成员默认值（独立 bool，非 Primitive 继承） |
| `Engine/Plugins/Experimental/Avalanche/Source/Avalanche/Private/Framework/AvaGizmoComponent.cpp:421` | `bRenderInMainPass = bInValue;` | `UAvaGizmoComponent::SetRenderInMainPass` |
| `Engine/Plugins/Experimental/Avalanche/Source/Avalanche/Private/Framework/AvaGizmoComponent.cpp:423` | `[bRenderInMainPass = bRenderInMainPass](...)` | Lambda 捕获，把值下发给被 gizmo 修饰的 PrimitiveComponent |

---

## 4. 其它"持有 `bRenderInMainPass` 的结构体"的赋值点

这些字段名相同但属于不同结构体，属于"快照/传参"用途。

| 文件:行 | 赋值表达式 | 所属结构 / 用途 |
|---------|-----------|----------------|
| `Engine/Source/Runtime/Engine/Public/PrimitiveSceneProxyDesc.h:25` | `bRenderInMainPass = true;` | `FPrimitiveSceneProxyDesc` 默认构造 |
| `Engine/Source/Runtime/Engine/Public/PSOPrecache.h:34` | `bRenderInMainPass = true;` | `FPSOPrecacheParams` 默认构造 |
| `Engine/Source/Runtime/Engine/Private/Components/PrimitiveComponent.cpp:4622` | `Params.bRenderInMainPass = bRenderInMainPass;` | `UPrimitiveComponent::SetupPrecachePSOParams`，从 Component 拷贝到 PSO 预缓存参数 |
| `Engine/Source/Runtime/Engine/Private/SceneManagement.cpp:340` | `bRenderInMainPass = PrimitiveSceneProxy->ShouldRenderInMainPass();` | `FStaticMeshBatchRelevance` 构造（每个 static mesh batch 的相关性） |
| `Engine/Source/Runtime/Renderer/Private/SceneCore.cpp:422` | `bRenderInMainPass = InComponent->bRenderInMainPass;` | `FExponentialHeightFogSceneInfo` 等渲染场景结构镜像 Component 字段 |

---

## 5. 赋值模式分类

把所有 `FPrimitiveViewRelevance::bRenderInMainPass`（§1）的赋值点按形式归类：

### 5.1 默认 true（构造函数初始化）

只有一个：
- `PrimitiveViewRelevance.h:103` — `bRenderInMainPass = true;`

这是所有 `Result` 对象的默认起点。注释提示：BSP 依赖这一默认值。

### 5.2 镜像 proxy 状态（`= ShouldRenderInMainPass()`）

最常见模式，约 25 处。代表所有标准 SceneProxy：StaticMesh、SkeletalMesh、Landscape、Niagara、Cable、Procedural、Modeling、Paper2D、Water、Niagara 等。
本质上是把"游戏线程 Component → 渲染线程 Proxy"的状态，进一步透传给"每帧每视图的可见性结果"。

### 5.3 条件性赋值

| 文件 | 条件 | 含义 |
|------|------|------|
| `MRMeshComponent.cpp:492` | `(bUseWireframe \|\| 非默认材质) && ShouldRenderInMainPass()` | 只有真正可视的 MR mesh 才进主 pass |
| `GroomComponent.cpp:1091` | `bUseCardsOrMesh && ShouldRenderInMainPass()` | Hair strands 不进 BasePass，仅 cards/mesh 进 |

### 5.4 强制 true

| 文件 | 上下文 |
|------|--------|
| `NaniteResources.cpp:1031` | Nanite proxy 强制保证 true |
| `GeometryCollectionSceneProxy.cpp:1171` | Nanite GC proxy 强制保证 true |
| `PrimitiveSceneInfo.cpp:727` | `NaniteDrawListApply` 阶段合成 relevance 强制 true |

### 5.5 强制 false

仅一处：
- `LandscapeRender.cpp:2140` — Landscape 的非编辑器/特殊路径分支，明确禁用主 pass

---

## 6. 调用链总结

```
┌─────────────────────────────────────────────────────────────────┐
│  游戏线程                                                        │
│  UPrimitiveComponent::bRenderInMainPass                          │
│    ├─ ctor default = true       (PrimitiveComponent.cpp:333)     │
│    └─ SetRenderInMainPass(bool) (PrimitiveComponent.cpp:4461)    │
└───────────────────────┬─────────────────────────────────────────┘
                        │ Component → Proxy 镜像
                        ▼
┌─────────────────────────────────────────────────────────────────┐
│  渲染线程 proxy 状态                                              │
│  FPrimitiveSceneProxy::bRenderInMainPass                         │
│    └─ ctor = InComponent->bRenderInMainPass                      │
│                       (PrimitiveSceneProxy.cpp:277)              │
│  bool ShouldRenderInMainPass() const { return bRenderInMainPass; }│
│                       (PrimitiveSceneProxy.h:700)                │
└───────────────────────┬─────────────────────────────────────────┘
                        │ 每帧每视图调用 GetViewRelevance(View)
                        ▼
┌─────────────────────────────────────────────────────────────────┐
│  本帧本视图相关性                                                 │
│  FPrimitiveViewRelevance::bRenderInMainPass                      │
│    ├─ ctor default = true   (PrimitiveViewRelevance.h:103)       │
│    ├─ Result.bRenderInMainPass = ShouldRenderInMainPass()  (×25) │
│    ├─ Result.bRenderInMainPass = true                       (×3) │
│    ├─ Result.bRenderInMainPass = false                      (×1) │
│    └─ 条件表达式（HairStrands / MRMesh）                    (×2) │
└───────────────────────┬─────────────────────────────────────────┘
                        │ 用于 SceneVisibility.cpp 的 pass 分配
                        ▼
       BasePass / MobileBasePassCSM / DepthPass / Velocity / ...
       （即你之前看到的 SceneVisibility.cpp:1500 附近）
```

---

## 7. 统计与要点

| 项 | 数量 |
|----|------|
| `FPrimitiveViewRelevance::bRenderInMainPass` 直接赋值点 | **31 处** |
| 其中 Engine/Source/Runtime | 14 处 |
| 其中 Engine/Plugins | 17 处 |
| `= ShouldRenderInMainPass()` 模式 | 25 处 |
| 强制 true | 3 处（均与 Nanite 相关） |
| 强制 false | 1 处（Landscape 特殊分支） |
| 条件性赋值 | 2 处（HairStrands、MRMesh） |
| 默认构造 | 1 处（`true`） |

**关键观察：**

1. **唯一"默认 true"** 在 `PrimitiveViewRelevance.h:103` 的构造函数；其余 `Result` 路径都以此为起点再被显式覆盖。
2. **Nanite 在三处强制 true**，确保 Nanite 通路始终参与主 pass，不被 proxy 状态意外关闭。
3. **HairStrands 是唯一"按表示形式条件性禁用"** 的例子（strands 模式不进 BasePass），与 `SceneVisibility.cpp` 的注释相互印证。
4. **Landscape 的 false 分支** 是引擎里少见的"按编辑器路径强制关闭"案例，常被忽略——若调试 Landscape 缺失主 pass，应直接查 `LandscapeRender.cpp:2140`。
5. **不要混淆五个同名字段**：修改其中任何一处都需要确认数据流上下游是否需要同步。例如 `SetRenderInMainPass` 写 Component 字段后必须 `MarkRenderStateDirty()`，才能让 proxy/relevance 重新计算。

---

## 8. 参考文件索引

| 文件 | 角色 |
|------|------|
| `Engine/Source/Runtime/Engine/Public/PrimitiveViewRelevance.h` | 本字段声明（line 54）与默认值（line 103） |
| `Engine/Source/Runtime/Engine/Public/PrimitiveSceneProxy.h` | `ShouldRenderInMainPass()` 内联返回（line 700） |
| `Engine/Source/Runtime/Engine/Private/PrimitiveSceneProxy.cpp` | Proxy 字段从 Component 镜像（line 277） |
| `Engine/Source/Runtime/Engine/Private/Components/PrimitiveComponent.cpp` | Component 字段默认值与 setter（line 333、4461） |
| `Engine/Source/Runtime/Renderer/Private/SceneVisibility.cpp` | 本字段的下游消费方（pass 分配） |
| `Engine/Source/Runtime/Renderer/Private/PrimitiveSceneInfo.cpp` | Nanite drawlist 合并相关性（line 727） |
