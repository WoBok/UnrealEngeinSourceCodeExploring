# UE5 延迟渲染与前向渲染 —— 深入解析

> 按「原理 → 对比 → 配合 → 实操」组织，尽量把每个结论背后的**为什么**都拆到底层。

---

## 目录

- [一、先建立共同起点：渲染器到底在解什么问题](#一先建立共同起点渲染器到底在解什么问题)
- [二、延迟渲染（Deferred Shading）](#二延迟渲染deferred-shading)
- [三、前向渲染（Forward / Forward+）](#三前向渲染forward--forward)
- [四、正面对比](#四正面对比)
- [五、二者的关系与配合 —— 这一节是理解的关键](#五二者的关系与配合--这一节是理解的关键)
- [六、动手：在 UE5 里把这些都看见](#六动手在-ue5-里把这些都看见)
- [七、三个实战场景](#七三个实战场景)
- [八、一页纸总结](#八一页纸总结)

---

# 一、先建立共同起点：渲染器到底在解什么问题

任何实时渲染器的核心循环，本质上是求解这个式子：

```
最终颜色(像素) = Σ_光源 BRDF(材质属性, 光源方向, 视线方向) × 光强 × 阴影 × NdotL
```

要算出它，你必须在**同一时刻**同时握有两组数据：

| 数据组 | 来源 | 特点 |
|---|---|---|
| **几何/材质属性**：法线、BaseColor、Roughness、Metallic… | 顶点 → 光栅化 → 材质图 | 由**物体**决定，随物体数量增长 |
| **光照信息**：光源位置/颜色/衰减、阴影贴图 | 场景光源列表 | 由**光源**决定，随光源数量增长 |

**前向渲染和延迟渲染的全部分歧，就在于「在哪里、以什么方式把这两组数据凑到一起」。**

- **前向**：在光栅化物体的当场，把光源信息拉进来，一次算完 → 两组数据在**着色器寄存器**里汇合。
- **延迟**：先把几何属性写进显存（G-Buffer），之后再以屏幕像素为单位把光源信息拉进来 → 两组数据在**显存**里汇合。

记住这一句，后面所有的优劣、限制、配合方式，都是这一个设计选择的必然推论。

---

# 二、延迟渲染（Deferred Shading）

## 2.1 核心思想：用 G-Buffer 把「几何」和「光照」切开

朴素前向的复杂度是 `O(物体数 × 光源数)`，而且被 overdraw 放大——一个后来被遮挡的像素，它跑过的所有光照计算全部作废。100 个物体 × 50 个光源 = 5000 次 draw 级的光照计算，其中大部分是浪费。

延迟渲染的做法是**插入一个中间层**：

```
【第一遍 BasePass】几何 → 光栅化 → 运行材质图 → 写出「这个像素表面长什么样」
                                                    ↓
                                              G-Buffer（显存）
                                                    ↓
【第二遍 Lighting】 遍历光源 → 读 G-Buffer → 算 BRDF → 累加进 SceneColor
```

复杂度从乘法变成了加法：

```
O(物体数 × 光源数)   →   O(物体数)  +  O(受影响像素 × 光源数)
```

关键在于第二项：它的上界是**屏幕分辨率**，与场景几何复杂度**完全解耦**。你场景里有 100 万个三角形还是 1 个平面，光照 Pass 的成本一模一样。这就是延迟渲染能扛住上百个动态光源的根本原因。

同时，因为 BasePass 有 Depth Test，**只有最终可见的那个表面**会留在 G-Buffer 里，光照只对可见表面计算一次——overdraw 对光照的浪费被彻底消除。

## 2.2 UE5 的 G-Buffer 到底存了什么

这是延迟渲染最具体的部分。UE5 的定义在 `Engine/Shaders/Private/DeferredShadingCommon.ush` 的 `FGBufferData` 结构和 `EncodeGBuffer/DecodeGBuffer` 函数里。BasePass 用 MRT（多渲染目标）一次输出这几张图：

| RT | 典型格式 | 内容 |
|---|---|---|
| **SceneDepth** | D32/D24S8 | 深度（用于重建世界坐标，**不单独存 Position**） |
| **GBufferA** | RGBA8 / RGB10A2 | `WorldNormal.xyz` + `PerObjectGBufferData` |
| **GBufferB** | RGBA8 | `Metallic`, `Specular`, `Roughness`, **`ShadingModelID` + `SelectiveOutputMask`**（打包在 A 通道） |
| **GBufferC** | RGBA8 | `BaseColor.rgb` + `GBufferAO` / `IndirectIrradiance` |
| **GBufferD** | RGBA8 | **`CustomData`**——含义随 ShadingModel 变化 |
| **GBufferE** | RGBA8 | `PrecomputedShadowFactors`（静态光的预计算阴影，4 个通道对应 4 盏静态光） |
| **GBufferF** | RGBA8 | `WorldTangent` + `Anisotropy`（UE5 新增，各向异性材质用） |
| **Velocity** | RG16 | 屏幕空间运动矢量，供 TSR/TAA、运动模糊使用 |

有四个细节值得单独拎出来，它们解释了延迟渲染的很多「怪癖」：

**① 不存 Position，从 Depth 反推。**
世界坐标 = 由屏幕 UV + 深度 + 逆投影矩阵重建。存一个 float 比存三个 float 省 2/3 带宽，这是延迟渲染最重要的一个优化。代价是精度依赖深度缓冲的分布。

**② `ShadingModelID` 是延迟渲染能支持多种材质的唯一机制。**
它只有几个 bit，枚举了 `DefaultLit`、`Subsurface`、`PreintegratedSkin`、`ClearCoat`、`SubsurfaceProfile`、`TwoSidedFoliage`、`Hair`、`Cloth`、`Eye`、`SingleLayerWater`、`ThinTranslucent` 等。光照 Pass 里靠 `switch(ShadingModelID)` 分支到不同的 BRDF。

**③ `CustomData`（GBufferD）是被复用的 4 个通道。**
ClearCoat 用它存清漆层强度和粗糙度；Subsurface 用它存次表面颜色；Hair 用它存散射和切线偏移。**同一个像素只能是一种 ShadingModel**——这是固定布局带来的直接约束。

**④ BasePass 里几乎不做光照。**
唯一的例外是自发光（Emissive）和静态光照（Lightmap / Volumetric Lightmap 的间接光，被压进 `IndirectIrradiance`）。所有动态光都留到下一遍。

## 2.3 光照 Pass：把光源画成「几何体」

延迟光照 Pass 的经典技巧是：**不要对全屏跑每一个光源**，而是画出光源的影响体积，让光栅化器帮你做剔除。

| 光源类型 | 绘制的代理几何 |
|---|---|
| Directional Light | 全屏四边形（影响所有像素） |
| Point Light | 一个球体（半径 = 衰减半径） |
| Spot Light | 一个圆锥 |
| Rect Light | 一个包围盒 |

配合 Stencil / Depth Bounds Test 剔除掉体积内但深度不在范围内的像素。每个通过的像素执行：

```hlsl
// 概念伪码，对应 Engine/Shaders/Private/DeferredLightPixelShaders.usf
FGBufferData GBuffer = GetGBufferData(ScreenUV);           // 解码 G-Buffer
float3 WorldPos      = ReconstructWorldPos(ScreenUV, Depth);
float  Shadow        = ComputeShadow(WorldPos);            // 采样 shadow map / VSM
float3 Radiance      = IntegrateBxDF(GBuffer, L, V, ...);  // 按 ShadingModelID 分支
SceneColor += Radiance * Shadow * LightColor * Attenuation; // 加法混合累积
```

注意最后是**加法混合（Additive Blending）**——光照天然满足叠加性，所以多个光源可以独立地一遍遍往 SceneColor 上累加。

## 2.4 UE5 的实际实现：两条光照路径并存

UE5 并不是傻乎乎地一盏灯一个 draw call。它把光源分成两类：

**① Clustered Deferred Shading（`r.UseClusteredDeferredShading`，默认开）**

对**没有阴影、没有 Light Function** 的局部光源，UE5 复用了前向渲染的 **Light Grid**：把视锥切成 3D 网格（XY 按 64×64 像素分块，Z 按指数分布切 32 片），每个 cell 记录「哪些光源影响我」。然后一个 Compute Shader 遍历屏幕，每个像素只读自己 cell 的光源列表，**一遍打完几十上百盏灯**。

> 这一点非常重要，在第五节会展开：**UE5 的延迟渲染器内部借用了前向渲染的核心技术。**

**② Standard Deferred Lighting**

对**有阴影贴图、有 IES 配置、有 Light Function** 的光源，仍然一盏一盏画代理体积，因为每盏灯要绑不同的 shadow map 资源。

## 2.5 为什么 UE5 的所有旗舰特性都长在延迟管线上

这不是巧合，而是必然。G-Buffer 是一张**全屏、逐像素、包含完整表面属性的场景描述**——一旦你有了它，一大类算法就变得可行：

| 特性 | 依赖 G-Buffer 的什么 |
|---|---|
| **SSAO** | Depth + Normal |
| **SSR / SSGI** | Depth + Normal + Roughness（做射线步进和 BRDF 重要性采样） |
| **Lumen（Screen Probe Gather）** | Depth + Normal 放置屏幕探针；Roughness/BaseColor 做最终合成 |
| **Virtual Shadow Maps** | Depth 决定需要哪些 shadow page（按需分配 16K×16K 虚拟页） |
| **Deferred Decals** | 直接改写已经写好的 G-Buffer（贴花「重绘」表面属性） |
| **TSR / TAA** | Velocity + Depth |
| **Nanite** | 见 5.5，它本身就是延迟思想的延伸 |

反过来说：**关掉延迟渲染，你就同时关掉了上面这一整列。** 这是 UE5 时代做技术选型时最重要的一句话。

## 2.6 延迟渲染的四个硬伤（都是原理级的，不是实现缺陷）

**① 带宽。**
1080p 下 G-Buffer 约 40+ bytes/pixel，一帧要写约 80MB、读回更多；4K 直接翻 4 倍。在移动端的 TBDR（Tile-Based Deferred Rendering）GPU 上，这意味着 tile memory 装不下、必须回写系统内存，功耗和带宽双爆炸。

**② MSAA 基本废掉。**
这一条经常被误解，讲清楚它的原理：

- 硬件 MSAA 的做法是「每像素多个覆盖采样点，共享一次着色，最后平均」。
- 延迟渲染要 MSAA，就得让 G-Buffer 变成 multisampled → 显存和带宽 ×4/×8。
- **更致命的是：你不能先 resolve G-Buffer 再光照。** 因为几何边缘处，前景金属（Metallic=1）和背景塑料（Metallic=0）平均出来是 Metallic=0.5——一个物理上根本不存在的材质。法线插值同理，两个方向相反的法线平均出来是零向量。
- 所以只能 **per-sample 跑光照**，成本 ×N。业界的折中是「边缘检测 + stencil 标记，只在边缘 per-sample」，但复杂度极高。

**UE 的延迟路径干脆不提供 MSAA，只有 TSR / TAA / FXAA。**

**③ 半透明无法进 G-Buffer。**
G-Buffer 每个像素只能存**一组**表面属性，而半透明的定义就是「一个像素上叠了多层表面」。这在原理上无解（除非上 OIT 或 Deep G-Buffer，成本不可接受）。**所以 UE 的半透明物体永远走前向路径**——这是第五节的核心配合点。

**④ 材质表达被固定布局锁死。**
想加一种新的着色模型？你得抢 `ShadingModelID` 的位、抢 `CustomData` 的通道。想让一个材质有「两层法线各自算光照」？G-Buffer 只有一份法线的位置。这就是 UE5 引入 **Substrate** 的动机（见 5.6）。

---

# 三、前向渲染（Forward / Forward+）

## 3.1 朴素前向为什么被淘汰

```
for each 物体:
    for each 影响它的光源:
        算 BRDF，累加
    输出最终颜色
```

三个问题：

- 复杂度 `O(物体 × 光源)`，光源一多就崩；
- Overdraw：被遮挡的像素白算了；
- 光源必须在 CPU 侧提前绑定到每个物体的 draw call 上，导致 shader 排列组合爆炸（UE 早期移动端就是「每物体最多 4 盏动态光」）。

**但注意：被淘汰的是「朴素前向」，不是「前向」本身。** 现代前向渲染是 **Forward+ / Clustered Forward**，UE5 的前向渲染器就是这一类。

## 3.2 UE5 的 Forward+ / Clustered Forward

它借用了延迟渲染最精华的一个思路——**先剔除，再着色**——但剔除的是**光源**而不是几何：

```
【Pass 1】Depth Prepass（强制全量）
              ↓  拿到完整深度缓冲
【Pass 2】Light Grid Injection（Compute Shader）
          把视锥切成 froxel 网格（XY 64px 一格，Z 指数分布 32 层）
          每盏光源投影到它覆盖的 cell，写进 CulledLightDataGrid
              ↓
【Pass 3】BasePass
          每个像素：
            1. 根据自己的 (x, y, depth) 算出所属 cell 索引
            2. 读这个 cell 的光源列表（通常只有几盏）
            3. for 这几盏灯：采样阴影 + 算 BRDF + 累加
            4. 直接输出最终颜色到 SceneColor  ← 一次性完成
```

复杂度回到了 `O(物体 + 像素 × 每格光源数)`，而「每格光源数」通常是个位数。**Forward+ 已经能处理成百上千的光源**，光源数量本身早已不是前向渲染的瓶颈了。

相关 cvar：`r.Forward.LightGridPixelSize`（默认 64）、`r.Forward.LightGridSizeZ`（默认 32）、`r.Forward.MaxCulledLightsPerCell`（默认 32）。

## 3.3 Depth Prepass 为什么在前向里是必须的

延迟渲染的 BasePass 只写 G-Buffer（很便宜），overdraw 浪费的是带宽；而前向的 BasePass **包含完整光照计算**（非常贵），一次 overdraw 浪费的是全部 BRDF 循环。

所以前向必须先跑一遍**只写深度、不跑像素着色**的 Prepass，把深度缓冲填满，然后 BasePass 用 `DepthTest = Equal` 保证**每个像素只被着色一次**。

代价是：**几何要提交两遍**，顶点着色、剔除、draw call 全部翻倍。这就是前向渲染的隐性成本——它把「像素侧的浪费」换成了「几何侧的重复」。

> UE 的延迟路径也常开 Depth Prepass（`r.EarlyZPass`），但那是可选优化；前向路径下它是**强制**的。

## 3.4 前向的优势本质：数据从未离开着色器

因为 BRDF 是在 BasePass 的像素着色器里、在寄存器上算完的，几个能力就白送了：

**① MSAA 天生可用。**
输出的就是最终颜色，硬件多重采样直接生效，没有「插值材质属性」的悖论。**这是 VR 选择前向渲染的第一理由**——VR 里 TAA 的时域重投影会在头部快速转动时产生明显的 ghosting 和抖动，而 MSAA 提供的是纯空间域、零时延的边缘抗锯齿。

**② 带宽极小。**
不写 G-Buffer，只有一个 SceneColor + Depth。在移动 TBDR GPU 上，整个 tile 的渲染可以完全在片上内存里完成，一次都不写回主存。

**③ 材质光照模型完全自由。**
不需要把属性塞进固定通道，你想在一个材质里算三层各向异性 + 自定义能量守恒项，随便。风格化 / NPR / 卡通渲染在前向路径下要自由得多。

**④ 精度更高。**
不需要压缩法线到 RGB8、不需要从深度反推位置，没有编解码损失。

## 3.5 前向在 UE5 里的实际限制清单（务必记住）

这是选型时最关键的表。开启 `r.ForwardShading=1` 后，你将失去：

| 失去的能力 | 原因 |
|---|---|
| **Lumen（GI + 反射）** | 依赖 G-Buffer 放置屏幕探针 |
| **Nanite** | Nanite 的材质 Pass 就是往 G-Buffer 写 |
| **Virtual Shadow Maps** | 与 Nanite / 延迟深度管线深度耦合 |
| **SSAO** | 需要 Normal Buffer |
| **SSR / SSGI** | 同上，SSR 只有受限支持 |
| **Deferred Decals** | 贴花的原理就是改写 G-Buffer；只剩 **DBuffer Decals**（在 BasePass 之前写一个小 buffer，被 BasePass 读取）可用 |
| **Subsurface Profile** 等部分着色模型 | 依赖屏幕空间的分离式模糊 Pass |
| **大部分光追特性** | 需要 G-Buffer 作为 primary hit 的输入 |

以及光照上的硬约束：

- **动态阴影**：只有 **1 盏方向光**能有完整的级联阴影（CSM）。局部动态光的阴影支持有限且昂贵。
- **静态光阴影**：最多 **4 盏**重叠的静态光可以用 Distance Field Shadow 打包进 4 个通道。
- 每个像素受影响的光源数受 `MaxCulledLightsPerCell` 上限约束。

**换句话说，在 UE5 里开前向渲染 ≈ 回到一个 UE4.27 水准、但抗锯齿更好的渲染器。** 这个 trade-off 必须想清楚。

---

# 四、正面对比

## 4.1 复杂度公式

| | 几何处理 | 光照计算 | 带宽 |
|---|---|---|---|
| **朴素前向** | O(G) | O(G × L) ×（1+overdraw） | 低 |
| **Forward+** | **O(2G)**（prepass + basepass） | O(P × L_cell) | 低 |
| **延迟** | O(G)（或 2G 带 prepass） | O(P × L) | **高** |

> G = 几何量，L = 光源数，P = 屏幕像素数，L_cell = 每 cluster 平均光源数

注意 Forward+ 和 Clustered Deferred 的光照复杂度**几乎一样**。**光源数量早已不是两者的分水岭了**——真正的分水岭是带宽、MSAA、和「有没有 G-Buffer 可供后续 Pass 消费」。

## 4.2 全维度对比

| 维度 | 延迟渲染（Deferred） | 前向渲染（Forward+） |
|---|---|---|
| **光照与几何耦合** | 解耦（两个 Pass） | 耦合（同一个 Pass） |
| **中间产物** | G-Buffer（40+ B/px） | 无 |
| **大量动态光** | 极强 | 强（Forward+ 后已不弱） |
| **带宽消耗** | **高** | **低** |
| **MSAA** | ❌ 实际不可用 | ✅ **原生支持** |
| **抗锯齿方案** | TSR / TAA / FXAA | MSAA / TAA |
| **半透明** | ❌ 原理上不支持 | ✅ 唯一方案 |
| **材质自由度** | 受 G-Buffer 布局限制 | 完全自由 |
| **Overdraw 敏感度** | 低（BasePass 很轻） | 高（必须 Depth Prepass） |
| **几何提交次数** | 1 遍（可选 prepass） | **2 遍（强制）** |
| **屏幕空间效果** | ✅ 全支持 | ❌ 大部分不可用 |
| **UE5 旗舰特性** | ✅ Lumen/Nanite/VSM/Decal | ❌ 全部不可用 |
| **VR 适配** | 差 | **优** |
| **移动端适配** | 差（除非用 Subpass） | **优** |
| **Shader 复杂度** | BasePass 简单、Light Pass 复杂 | BasePass 巨大，寄存器压力高、occupancy 低 |

## 4.3 一个可以直接用的选择判据

先问三个问题，按顺序：

1. **要不要 Lumen / Nanite / VSM / 贴花 / SSR？** → 要 → **延迟，没得选。**
2. **是不是 VR（尤其 PCVR 高保真）？** → 是 → **前向**（为了 MSAA）。
3. **是不是移动端？** → 是 → 走 **Mobile Renderer**（这是第三条路，见 5.4），中低端用 Mobile Forward，高端可试 Mobile Deferred。

其余情况（PC/主机的常规 3A、开放世界、写实项目）：**默认延迟**。UE5 的整个技术栈就是围绕它建的。

---

# 五、二者的关系与配合 —— 这一节是理解的关键

很多教程讲到这里就结束了，把它讲成「二选一」。**这是最大的误解。** 真实情况是：

> **UE5 里根本不存在「纯粹的延迟渲染器」。你的每一帧都是延迟和前向的混合产物。**

一个直接的证据：UE4.22 之后，独立的 `FForwardShadingSceneRenderer` 类被删除了。UE5 的场景渲染器只有两个——`FDeferredShadingSceneRenderer` 和 `FMobileSceneRenderer`。**前向着色只是前者内部的一个分支**（由 `IsForwardShadingEnabled()` 判断）。它们共享 InitViews、共享阴影系统、共享 Light Grid、共享后处理链。

下面是五个具体的配合点。

## 5.1 配合点一：半透明永远走前向（最重要）

即使你的项目是标准的延迟渲染，任何 Blend Mode 为 `Translucent` / `Additive` / `Modulate` 的材质，走的都是**独立的 `TranslucentBasePass`**，用的是**前向光照**（读 Light Grid，在像素着色器里当场算完）。

原因回到 2.6 的第 ③ 点：G-Buffer 一个像素只能存一层表面，半透明在原理上进不去。

这个配合在材质编辑器里是**直接暴露给你的**——半透明材质的 `Lighting Mode` 选项：

| Lighting Mode | 做法 | 成本 |
|---|---|---|
| `Volumetric NonDirectional` | 从 Translucency Lighting Volume（一张 64³ 的 3D 辐照度纹理）取无方向性的光 | 最低 |
| `Volumetric Directional` | 同上，但保留主方向，有基础的明暗 | 低 |
| `Volumetric PerVertex *` | 逐顶点采样上述 volume | 更低 |
| `Surface TranslucencyVolume` | 按表面法线着色，但光照数据仍来自 volume | 中 |
| **`Surface ForwardShading`** | **完整的逐像素前向光照，支持镜面高光和反射** | **最高** |

**这张表就是「前向光照的精度阶梯」。** 想让玻璃、水面有正确的高光，就得选 `Surface ForwardShading`——你是在一个延迟项目里，显式地为某个材质开启了完整前向管线。

> 顺带解释一个常见困惑：为什么半透明的烟雾、粒子的光照看起来「糊」？因为它们默认用的是 Volumetric 模式，光照来自一张 64³ 的低频 3D 纹理，而不是逐像素计算。

## 5.2 配合点二：延迟管线借用了前向的 Light Grid

这是双向的。回到 2.4：UE5 的 `ClusteredDeferredShading` Pass **直接复用了前向渲染的 froxel Light Grid**。

所以在一个纯延迟项目里，`r.Forward.LightGridPixelSize` 这个「前向」cvar **依然生效**，Light Grid Injection Pass **依然每帧执行**。它同时服务于：

- 延迟的 Clustered Lighting（无阴影局部光）
- 半透明的前向光照
- 体积雾（Volumetric Fog）的光照注入

**这说明「clustered light culling」是一项与前向/延迟正交的独立技术**，两条管线都在用。把它归为「前向渲染的特征」是不准确的。

## 5.3 配合点三：材质级的前向开关

材质细节面板里有一个 **`Forward Shading`** 复选框（`bUseForwardShading`）。勾上它，这个不透明材质会在延迟项目里**额外走一遍前向光照**。

典型用途是 **Clear Coat（清漆）**：它需要底层和清漆层两套法线各自算光照，而 G-Buffer 只有一份法线槽位。开启前向后，这个材质能在 BasePass 里就把双层光照算准，代价是它变成一个昂贵的特例。

## 5.4 配合点四：移动端把延迟「塞进」Tile Memory

移动 GPU 是 TBDR 架构，片上有一块很快的 tile memory。UE5 的 **Mobile Deferred**（`r.Mobile.ShadingPath=1`）做了一件很聪明的事：

用 Vulkan 的 **Subpass** / Metal 的 **Memoryless RenderTarget**，让 G-Buffer **写入后直接在 tile memory 里被光照 Pass 读取，全程不回写系统内存**。

```
传统延迟：BasePass → [写显存 80MB] → LightPass → [读显存 80MB]
Mobile：  BasePass → [tile memory, 片上] → LightPass   ← 带宽成本 ≈ 0
```

**这本质上是「用前向渲染的带宽模型，执行延迟渲染的算法」**——延迟渲染最大的缺点（带宽）在这个架构下被消解了。这是理解两者关系的一个绝佳案例：它们的边界并不是固定的。

## 5.5 配合点五：Nanite 是「延迟」思想推到极致

Nanite 的管线是这样的：

```
【Pass 1】剔除 + 软件/硬件光栅化 → Visibility Buffer
          每像素只存 (InstanceID, TriangleID, Depth) —— 64 bit，不跑任何材质
              ↓
【Pass 2】Material Pass：按材质分类，用 Depth-Equal 测试筛出属于该材质的像素
          反解三角形 → 重建重心坐标 → 插值属性 → 运行材质图 → 写入 G-Buffer
              ↓
【Pass 3】常规的延迟光照
```

看出来了吗？**Nanite 是在延迟渲染的基础上，又延迟了一层**——先延迟光照（G-Buffer），再延迟材质（Visibility Buffer）。这被称为 **Deferred Materials**。

好处是几何光栅化和材质求值彻底解耦，微多边形（每个三角形 ≈ 1 像素）的场景不会因为 quad overdraw 而崩溃。

**代价就是它和 G-Buffer 是绑死的——所以 Nanite 无法在前向渲染下工作。**

## 5.6 未来：Substrate 想消灭这个二分

UE5.2 起引入的 **Substrate（原名 Strata）**，直接冲着 2.6 的第 ④ 点去：它用**可变长度的材质字节流（material byte stream）**替代固定布局的 G-Buffer 通道。

一个材质可以声明任意的分层结构（金属 + 清漆 + 灰尘 + 次表面），Substrate 把它序列化成变长数据，光照 Pass 反序列化后逐层求解。ShadingModelID 的枚举限制、CustomData 的通道争抢，在这个模型下都不再存在。

而且它对前向和延迟提供**统一的材质表达**——从架构上，Substrate 是在弥合两条管线的材质能力鸿沟。（截至目前它仍是实验性 / Beta 特性，生产项目需谨慎评估。）

## 5.7 一帧的完整全景

把上面所有配合点拼起来，一个标准 UE5 延迟项目的帧结构长这样（🟦 = 延迟，🟩 = 前向，⬜ = 共享）：

```
⬜ InitViews / GPU Scene 剔除
⬜ Depth Prepass
🟦 Nanite 剔除 + 光栅化 → Visibility Buffer
🟩 Light Grid Injection            ← 前向技术，两边共用
⬜ DBuffer Decals
🟦 BasePass  →  写 G-Buffer (A~F + Velocity)
🟦 Nanite Material Pass → 补写 G-Buffer
🟦 Deferred Decals → 改写 G-Buffer
⬜ Shadow Depths / Virtual Shadow Maps
🟦 Lumen Scene Update / Screen Probe Gather / Reflections
🟦 SSAO
🟦 Clustered Deferred Lighting (无阴影局部光, 走 Light Grid)
🟦 Standard Deferred Lighting (有阴影光源, 逐光源画代理体积)
🟦 Reflection Environment + SSR
🟩 Volumetric Fog                  ← 光照注入也读 Light Grid
🟩 Translucency Pass               ← 完全的前向光照
⬜ Post Processing (TSR → Bloom → Tonemap → ...)
```

而开启 `r.ForwardShading=1` 后，帧结构塌缩成：

```
⬜ InitViews
⬜ Depth Prepass（强制全量）
🟩 Light Grid Injection
⬜ Shadow Depths（CSM，仅 1 盏方向光）
⬜ DBuffer Decals
🟩 BasePass  →  读 Light Grid，算完所有光照，直接输出 SceneColor（MSAA 生效）
🟩 Fog
🟩 Translucency
⬜ MSAA Resolve
⬜ Post Processing
```

对比这两张图，你能一眼看出：**延迟渲染多出来的那一大段 🟦，正是 UE5 的全部核心竞争力所在。**

---

# 六、动手：在 UE5 里把这些都看见

## 6.1 切换前向渲染

`Project Settings → Rendering → Forward Renderer → Forward Shading` 勾上，**重启编辑器并等待全量 shader 重编译**（大项目可能一两个小时）。或在 `DefaultEngine.ini`：

```ini
[/Script/Engine.RendererSettings]
r.ForwardShading=1
r.MSAACount=4
r.DefaultFeature.AntiAliasing=2   ; 2 = MSAA
```

> `r.ForwardShading` 是 read-only cvar，运行时改无效，必须走配置 + 重启。

## 6.2 亲眼看 G-Buffer

编辑器视口左上角 → `View Mode → Buffer Visualization`，可以逐张查看：

- `Base Color` / `World Normal` / `Metallic` / `Roughness` / `Specular` → 对应 GBufferA/B/C
- `Scene Depth` → 深度
- `Ambient Occlusion` → GBufferC 的 A 通道
- `Subsurface Color` → GBufferD

**建议做的第一个实验**：开着 `World Normal` 视图，然后开启前向渲染重启，再看同一个视图——你会发现它变成一片纯色/黑色，因为**没有 G-Buffer 了**。这是对「延迟 vs 前向」最直观的一次认知冲击。

## 6.3 用 ProfileGPU 对照两条管线

在编辑器或运行时按 <kbd>`</kbd> 打开控制台，输入：

```text
ProfileGPU
```

会弹出 GPU Visualizer。在**延迟**项目里你会看到 `BasePass`、`ClusteredDeferredShading`、`StandardDeferredLighting`、`ReflectionEnvironment`、`Lumen*` 等 Pass；切到**前向**后，光照 Pass 全部消失，`BasePass` 的耗时显著上升——**成本从 Lighting 迁移到了 BasePass**。这个迁移过程本身就是两者关系最好的注解。

配合使用：

```text
stat GPU
stat SceneRendering
r.RHISetGPUCaptureOptions 1
```

## 6.4 关键 cvar 速查

```text
r.ForwardShading                       前向总开关（需重启 + 重编译 shader）
r.MSAACount                            MSAA 采样数（仅前向有效）
r.GBufferFormat                        G-Buffer 精度：0=8bit, 1=默认, 3=16bit float, 5=高精度法线
r.UseClusteredDeferredShading          延迟下的 clustered 光照（默认 1）
r.Forward.LightGridPixelSize           Light Grid 的 XY 尺寸（默认 64）
r.Forward.LightGridSizeZ               Z 方向切片数（默认 32）
r.Forward.MaxCulledLightsPerCell       每 cell 光源上限（默认 32）
r.EarlyZPass                           Depth Prepass 模式（前向下强制全量）
r.TranslucencyLightingVolumeDim        半透明光照体积分辨率（默认 64）
r.Mobile.ShadingPath                   移动端：0=Forward, 1=Deferred
r.Lumen.DiffuseIndirect.Allow          Lumen GI 开关
r.Nanite                               Nanite 开关
r.Shadow.Virtual.Enable                Virtual Shadow Maps 开关
```

## 6.5 想读源码的话，从这几个文件切入

```
Engine/Shaders/Private/DeferredShadingCommon.ush       ← FGBufferData / Encode / Decode（先读这个）
Engine/Shaders/Private/BasePassPixelShader.usf         ← 两条路径的分岔点（FORWARD_SHADING 宏）
Engine/Shaders/Private/DeferredLightPixelShaders.usf   ← 延迟光照
Engine/Shaders/Private/LightGridInjection.usf          ← froxel light culling
Engine/Shaders/Private/ForwardShadingCommon.ush        ← 前向光照循环

Engine/Source/Runtime/Renderer/Private/DeferredShadingRenderer.cpp  ← 整帧调度（两条路径都在这）
Engine/Source/Runtime/Renderer/Private/LightRendering.cpp           ← 光照 Pass 分派
Engine/Source/Runtime/Renderer/Private/TranslucentRendering.cpp     ← 半透明的前向路径
```

**读源码的建议顺序**：`DeferredShadingCommon.ush` 的 `EncodeGBuffer` → `BasePassPixelShader.usf` 里搜 `#if FORWARD_SHADING`（那一行就是两个世界的分界线）→ `DeferredShadingRenderer.cpp::Render()` 的整体流程。

---

# 七、三个实战场景

## 场景 A：写实开放世界（PC / 主机）

**选延迟。** 理由：Nanite 处理海量几何、Lumen 处理动态 GI、VSM 处理大范围高质量阴影、Deferred Decals 做地表混合与破损——这四项**全部依赖 G-Buffer**。抗锯齿用 TSR（UE5 的时域超分，在 4K 下可以从 1080p 内部分辨率上采样，质量远超 TAA）。

**关键配置**：`r.GBufferFormat=1`（默认）保证法线精度足够 Lumen 用；水面用 `SingleLayerWater` 着色模型（它是个特例：写 G-Buffer，但在单独 Pass 里处理水下折射和散射）。

## 场景 B：PCVR 高保真展示 / 建筑可视化

**选前向。** 理由：

- MSAA 4x 在 VR 里对边缘质量的贡献是决定性的，TAA 在头显快速转动时的 ghosting 会直接破坏沉浸感；
- VR 是双眼渲染 + 高分辨率（每眼 2K+），G-Buffer 的带宽成本翻倍，前向的低带宽优势被放大；
- 建筑可视化场景通常是**静态光照为主**，可以烘焙高质量 Lightmap，前向渲染在这一点上毫不吃亏。

**代价与对策**：

- 没有 Lumen → 用 Lightmass / GPU Lightmass 烘焙静态 GI；
- 没有 SSR → 密集布置 Reflection Capture + Planar Reflection（玻璃/地面用平面反射，成本可控）；
- 没有 SSAO → 烘焙 AO 到 Lightmap，或在材质里用 Distance Field AO 的替代方案；
- 只有 1 盏方向光有动态阴影 → 主光源用 Movable 方向光做太阳，其余全部 Static。

## 场景 C：延迟项目里做一个高质量玻璃幕墙

这是「配合」的教科书案例，一个项目里两条管线同时工作：

1. **建筑主体**（不透明）→ 走延迟，进 G-Buffer，享受 Lumen GI + VSM 阴影 + Nanite。
2. **玻璃**（`Blend Mode = Translucent`）→ 自动走前向 `TranslucentBasePass`。
   - `Lighting Mode` 设为 **`Surface ForwardShading`** → 玻璃能拿到逐像素的高光和反射；
   - `Refraction` 输入接 IOR，走屏幕空间折射（读取已经渲染好的不透明 SceneColor —— **这里前向的半透明又反过来消费了延迟管线的输出**）；
   - 反射来自 Reflection Capture / Sky Light / Lumen Reflection（Lumen 对半透明有受限支持）。
3. **性能注意**：`Surface ForwardShading` 的半透明是全场景最贵的像素之一，且**没有 Depth Prepass 保护**，overdraw 会直接叠加。多层玻璃重叠是最典型的性能陷阱——用 `Shader Complexity` 视图（`View Mode → Optimization Viewmodes → Shader Complexity`）检查，白色/粉色区域就是重灾区。

**这个场景完整体现了两者的关系**：不透明用延迟拿到 UE5 的全部高级特性，半透明用前向拿到延迟做不到的分层混合，而半透明的折射又反过来采样延迟渲染的结果。它们不是竞争者，是**分工者**。

---

# 八、一页纸总结

**如果只能记住五句话：**

1. **两者的唯一分歧是「几何属性和光照信息在哪里汇合」**——前向在寄存器里，延迟在显存（G-Buffer）里。所有其他差异都是这一个选择的推论。

2. **延迟渲染真正的价值不是「支持很多光源」**（Forward+ 也能做到），而是它产出了 **G-Buffer 这个全屏场景描述**，从而让 Lumen、Nanite、VSM、SSR、SSAO、Deferred Decals 这一整个技术家族成为可能。

3. **前向渲染真正的价值是 MSAA 和低带宽**——这两件事在原理上是延迟渲染做不到的，也正是 VR 和移动端的核心诉求。

4. **UE5 里不存在纯粹的延迟渲染**：不透明走延迟，半透明必然走前向；延迟的 Clustered Lighting 借用了前向的 Light Grid；移动端用 Subpass 把延迟塞进片上内存获得前向的带宽特性。**它们是同一个渲染器里协作的两套子系统。**

5. **UE5 的选型答案很干脆**：除非你在做 VR 或移动端，否则用延迟。因为 UE5 这一代引擎的全部技术投资，都压在 G-Buffer 之上。

---

## 可以继续深挖的方向

- **逐行拆解 `EncodeGBuffer` / `DecodeGBuffer` 的位打包细节**（法线八面体压缩、ShadingModelID 的 bit 布局）
- **Nanite Visibility Buffer 到 G-Buffer 的完整数据流**
- **Lumen 的 Screen Probe Gather 具体怎么消费 G-Buffer**
- **自定义一个新的 ShadingModel**（引擎级改造，需要动 G-Buffer 布局和 shader）
- **Substrate 的材质字节流格式与分层求解**
