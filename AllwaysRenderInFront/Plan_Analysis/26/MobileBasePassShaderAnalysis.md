# 移动端 Forward 渲染路径：Shader 获取与材质编译分析

> **引用问题：** `Engine/Source/Runtime/Renderer/Private/MobileBasePass.cpp:207:GetShader` 中获取的具体是什么 Shader？都是 `MobileBasePassPixelShader.usf` 中的或者 `MobileBasePassVertexShader.usf` 的吗？我自定义创建的材质是怎么被获取的呢？怎么被编译的呢？

---

## 一、直接结论

**是的**，`MobileBasePass::GetShaders`（`MobileBasePass.cpp:207`）获取的就是 `MobileBasePassVertexShader.usf` 的 VS 和 `MobileBasePassPixelShader.usf` 的 PS，但这只是"Shader 框架代码"。你自定义材质的节点图会被翻译成 HLSL 代码，以虚拟文件 `/Engine/Generated/Material.ush` 的形式被这两个 usf `#include` 进去，一起编译成最终的 Shader。

---

## 二、GetShaders 获取的具体 Shader

### 2.1 函数签名与位置

```
Engine/Source/Runtime/Renderer/Private/MobileBasePass.cpp:207
bool MobileBasePass::GetShaders(
    ELightMapPolicyType LightMapPolicyType,
    EMobileLocalLightSetting LocalLightSetting,
    const FMaterial& MaterialResource,
    const FVertexFactoryType* VertexFactoryType,
    bool bEnableSkyLight,
    TShaderRef<TMobileBasePassVSPolicyParamType<FUniformLightMapPolicy>>& VertexShader,
    TShaderRef<TMobileBasePassPSPolicyParamType<FUniformLightMapPolicy>>& PixelShader)
```

### 2.2 获取的 Shader 类型

获取的是 `TMobileBasePassVS` 和 `TMobileBasePassPS` 两个模板类实例化的 Shader，它们继承自 `FMeshMaterialShader`（不是 GlobalShader），定义在：

```
Engine/Source/Runtime/Renderer/Private/MobileBasePassRendering.h
  L212: class TMobileBasePassVS    : public TMobileBasePassVSBaseType<LightMapPolicyType>
  L360: class TMobileBasePassPS    : public TMobileBasePassPSBaseType<LightMapPolicyType>
```

模板参数（排列维度）：
| 维度 | 可选值 |
|------|--------|
| `LightMapPolicyType` | `TUniformLightMapPolicy<LMP_XXX>`，如 `LMP_NO_LIGHTMAP`、`LMP_MOBILE_DIRECTIONAL_LIGHT_CSM` 等 10 种 |
| `OutputFormat` | `LDR_GAMMA_32` / `HDR_LINEAR_64` |
| `bEnableSkyLight` | true / false |
| `LocalLightSetting` | `LOCAL_LIGHTS_DISABLED` / `LOCAL_LIGHTS_ENABLED` / `LOCAL_LIGHTS_BUFFER` |
| `ThinTranslucencyFallback` | `DEFAULT` / `SINGLE_SRC_BLENDING` 等 |

### 2.3 Shader 与 .usf 文件的绑定关系（关键）

绑定通过 `IMPLEMENT_MATERIAL_SHADER_TYPE` 宏完成，位于：

```
Engine/Source/Runtime/Renderer/Private/MobileBasePassRendering.cpp:134-148
```

宏展开示例：
```cpp
#define IMPLEMENT_MOBILE_SHADING_BASEPASS_LIGHTMAPPED_VERTEX_SHADER_TYPE(LightMapPolicyType, LightMapPolicyName) \
    typedef TMobileBasePassVS<LightMapPolicyType, LDR_GAMMA_32> TMobileBasePassVS##LightMapPolicyName##LDRGamma32; \
    typedef TMobileBasePassVS<LightMapPolicyType, HDR_LINEAR_64> TMobileBasePassVS##LightMapPolicyName##HDRLinear64; \
    IMPLEMENT_MATERIAL_SHADER_TYPE(template<>, TMobileBasePassVS##LightMapPolicyName##LDRGamma32, \
        TEXT("/Engine/Private/MobileBasePassVertexShader.usf"), TEXT("Main"), SF_Vertex); \
    IMPLEMENT_MATERIAL_SHADER_TYPE(template<>, TMobileBasePassVS##LightMapPolicyName##HDRLinear64, \
        TEXT("/Engine/Private/MobileBasePassVertexShader.usf"), TEXT("Main"), SF_Vertex);
```

PS 的宏类似（`MobileBasePassRendering.cpp:140-148`），指向 `MobileBasePassPixelShader.usf`，入口函数均为 `Main`。

**结论确认**：所有 MobileBasePass 的 VS 都绑定到 `MobileBasePassVertexShader.usf`，所有 PS 都绑定到 `MobileBasePassPixelShader.usf`，入口都是 `Main`。在 `MobileBasePassRendering.cpp:161-170` 为 10 种 LightMapPolicy 各调用一次宏，生成数十个具体 Shader 类型。

### 2.4 获取流程（运行时查找）

```
MobileBasePass::GetShaders                         (MobileBasePass.cpp:207)
  └─ GetMobileBasePassShaders<LocalLightSetting>   (MobileBasePass.cpp:168)
       └─ GetUniformMobileBasePassShaders<Policy>  (MobileBasePass.cpp:125)
            ├─ ShaderTypes.AddShaderType<TMobileBasePassVS<...>>()    // L141/L145 填充期望的VS类型
            ├─ AddMobileBasePassPixelShaderTypes<...>(...)            // L98 填充期望的PS类型
            └─ Material.TryGetShaders(ShaderTypes, VFType, Shaders)  // L158 从材质ShaderMap查找
                 ├─ Shaders.TryGetVertexShader(VertexShader)
                 └─ Shaders.TryGetPixelShader(PixelShader)
```

`Material.TryGetShaders` 实现于 `Engine/Source/Runtime/Engine/Private/Materials/MaterialShared.cpp:3624`，它从材质的 `FMaterialShaderMap` 中按 ShaderType 查找已编译好的 `FShader` 实例。

---

## 三、自定义材质如何被"获取"

### 3.1 调用链（移动端 Forward 路径）

```
FMobileSceneRenderer::Render                              (MobileShadingRenderer.cpp:910)
  └─ RenderForwardSinglePass / RenderForwardMultiPass      (MobileShadingRenderer.cpp:1578)
       └─ RenderMobileBasePass(RHICmdList, View, ...)      (MobileBasePassRendering.cpp:470)
            └─ View.ParallelMeshDrawCommandPasses[EMeshPass::BasePass].DispatchDraw(...)
                 ↑ 命令在此前由 FMobileBasePassMeshProcessor 生成
       
FMobileBasePassMeshProcessor::AddMeshBatch                 (MobileBasePass.cpp:867)
  └─ TryAddMeshBatch                                       (MobileBasePass.cpp:851)
       └─ Process                                          (MobileBasePass.cpp:892)
            └─ MobileBasePass::GetShaders(...)             (MobileBasePass.cpp:930)  ← 获取VS/PS
            └─ BuildMeshDrawCommands(...)                  (MobileBasePass.cpp:990)  生成MeshDrawCommand
```

### 3.2 MeshProcessor 的创建

```
CreateMobileBasePassProcessor                             (MobileBasePass.cpp:1151)
  └─ new FMobileBasePassMeshProcessor(EMeshPass::BasePass, ...)
```

`FMobileBasePassMeshProcessor` 构造时固定 `FeatureLevel = ERHIFeatureLevel::ES3_1`（`MobileBasePass.cpp:818`），即移动端。

### 3.3 材质如何匹配到 Shader

`Process` 函数（`MobileBasePass.cpp:892`）依据：
- 材质的 `ShadingModels.IsLit()` → 决定 `LocalLightSetting`
- `Scene->SkyLight` → 决定 `bEnableSkyLight`
- `MobileBasePass::SelectMeshLightmapPolicy(...)` → 决定 `LightMapPolicyType`
- `MeshBatch.VertexFactory->GetType()` → 顶点工厂类型（如 `FLocalVertexFactory`）

把这些参数传给 `MobileBasePass::GetShaders`，从而在该材质的 ShaderMap 中定位到唯一一个编译好的 Shader 实例。

---

## 四、自定义材质如何被编译

### 4.1 编译总流程

```
FMaterial::BeginCompileShaderMap                         (MaterialShared.cpp:3398)
  ├─ Translate(ShaderMapId, ...)                         (MaterialShared.cpp:3373)
  │    ├─ Translate_Legacy → FHLSLMaterialTranslator::Translate + GetMaterialShaderCode
  │    │    （HLSLMaterialTranslator.cpp:2865，旧版生成器）
  │    └─ Translate_New → MaterialEmitHLSL
  │         （MaterialHLSLEmitter.cpp，新版生成器）
  ├─ SetupMaterialEnvironment(...)                       (MaterialShared.cpp:2551) 设置MATERIALBLENDING_*等宏
  └─ NewShaderMap->Compile(this, ...)                    (MaterialShared.cpp:3450)
       └─ FMaterialShaderMap::Compile                    (MaterialShader.cpp:2110)
            └─ SubmitCompileJobs                          (MaterialShader.cpp:1858)
                 └─ 对每个 (ShaderType, VertexFactoryType, PermutationId):
                    FMeshMaterialShaderType::BeginCompileShader   (MeshMaterialShader.cpp:81)
                      └─ PrepareMeshMaterialShaderCompileJob       (MeshMaterialShader.cpp:12)
                           └─ GlobalBeginCompileShader(...ShaderType->GetShaderFilename()...)  (L64)
```

### 4.2 材质 HLSL 代码的生成与注入（关键）

材质节点图翻译成 HLSL 字符串后，存入虚拟路径 `/Engine/Generated/Material.ush`：

**旧版生成器（Legacy）：**
```cpp
// MaterialShared.cpp:3355-3357
const FString MaterialShaderCode = MaterialTranslator.GetMaterialShaderCode();
OutMaterialEnvironment->IncludeVirtualPathToContentsMap.Add(
    TEXT("/Engine/Generated/Material.ush"), MaterialShaderCode);
```

**新版生成器（New HLSL Generator）：**
```cpp
// MaterialHLSLEmitter.cpp:1056
OutMaterialEnvironment->IncludeVirtualPathToContentsMap.Add(
    TEXT("/Engine/Generated/Material.ush"), MoveTemp(MaterialTemplateSource));
```

其中 `MaterialTemplateSource` 由 `GenerateMaterialTemplateHLSL(...)`（`MaterialHLSLEmitter.cpp:1031`）基于 `MaterialTemplate.ush` 模板填充参数生成。模板加载自 `/Engine/Private/MaterialTemplate.ush`（`MaterialSourceTemplate.cpp:59`）。

### 4.3 编译时的文件包含关系

`MobileBasePassPixelShader.usf:109` 与 `MobileBasePassVertexShader.usf:14` 均包含：
```hlsl
#include "/Engine/Generated/Material.ush"
```

编译时该虚拟路径被替换为材质生成的 HLSL 代码。错误回溯时（`ShaderCompiler.cpp:3327-3333`）会将 `/Engine/Generated/Material.ush` 重映射回 `/Engine/Private/MaterialTemplate.ush` 便于定位。

完整的 include 链（以 PS 为例）：
```
MobileBasePassPixelShader.usf
  ├─ Common.ush
  ├─ /Engine/Generated/Material.ush        ← 你的自定义材质HLSL代码注入点
  │    └─ (基于 MaterialTemplate.ush 填充)
  ├─ MobileBasePassCommon.ush
  ├─ /Engine/Generated/VertexFactory.ush   ← 顶点工厂代码注入点
  ├─ LightmapCommon.ush
  ├─ MobileLightingCommon.ush
  ├─ ShadingModelsMaterial.ush
  └─ ...
```

### 4.4 Shader 排列的筛选

并非所有排列都会编译，通过 `ShouldCompilePermutation` 过滤：
- `TMobileBasePassVS::ShouldCompilePermutation`（`MobileBasePassRendering.h:217`）：检查 `IsMobilePlatform` + LightMapPolicy
- `TMobileBasePassPS::ShouldCompilePermutation`（`MobileBasePassRendering.h:365`）：综合判断 skylight、local light、deferred shading、translucent 等
- `FMaterial::ShouldCache`（`MaterialShared.cpp:3511`）：材质层级的过滤

### 4.5 编译环境的设置

每个 Shader 类的 `ModifyCompilationEnvironment` 设置 #define：
- `TMobileBasePassPS::ModifyCompilationEnvironment`（`MobileBasePassRendering.h:402`）：设置 `ENABLE_SKY_LIGHT`、`ENABLE_CLUSTERED_LIGHTS`、`MERGED_LOCAL_LIGHTS_MOBILE`、`USE_SHADOWMASKTEXTURE` 等
- `FMaterial::SetupMaterialEnvironment`（`MaterialShared.cpp:2551`）：设置 `MATERIALBLENDING_SOLID/MASKED/TRANSLUCENT` 等
- `MobileBasePassModifyCompilationEnvironment`（`MobileBasePassRendering.cpp:193`）：设置 `IS_BASE_PASS`、`IS_MOBILE_BASE_PASS`、`OUTPUT_MOBILE_HDR` 等

### 4.6 编译结果存储

编译完成后通过 `FMeshMaterialShaderType::FinishCompileShader`（`MeshMaterialShader.cpp:157`）构造 `FShader` 实例，存入 `FMeshMaterialShaderMap`（按 VertexFactoryType 分组），最终挂在 `FMaterialShaderMap` 上。运行时 `TryGetShaders` 即从这里按 ShaderType 查找。

---

## 五、移动端 Forward 渲染路径完整调用链

```
FMobileSceneRenderer::Render                                     (MobileShadingRenderer.cpp:910)
  ├─ InitViews                                                   (MobileShadingRenderer.cpp:1033)
  │    └─ 生成 ParallelMeshDrawCommandPasses[EMeshPass::BasePass]
  │         └─ FMobileBasePassMeshProcessor::AddMeshBatch        (MobileBasePass.cpp:867)
  │              └─ TryAddMeshBatch → Process                    (MobileBasePass.cpp:851/892)
  │                   └─ MobileBasePass::GetShaders              (MobileBasePass.cpp:930 → 207)
  │                        └─ Material.TryGetShaders             (MaterialShared.cpp:3624)
  │                   └─ BuildMeshDrawCommands                   (MobileBasePass.cpp:990)
  ├─ RenderForwardSinglePass                                     (MobileShadingRenderer.cpp:1578)
  │    └─ RenderMobileBasePass                                   (MobileBasePassRendering.cpp:470)
  │         └─ ParallelMeshDrawCommandPasses[BasePass].DispatchDraw  (L478)
  └─ ... (translucency, fog, postprocess)
```

材质编译期（独立于渲染帧）：
```
UMaterial::RecompileShaders / 资源加载触发
  └─ FMaterial::BeginCompileShaderMap                           (MaterialShared.cpp:3398)
       └─ Translate → 生成 /Engine/Generated/Material.ush
       └─ FMaterialShaderMap::Compile                           (MaterialShader.cpp:2110)
            └─ SubmitCompileJobs                                (MaterialShader.cpp:1858)
                 └─ FMeshMaterialShaderType::BeginCompileShader  (MeshMaterialShader.cpp:81)
                      └─ GlobalBeginCompileShader
                           (SourceFilename = MobileBasePassPixelShader.usf / VertexShader.usf)
                           (MaterialEnvironment 包含 /Engine/Generated/Material.ush)
                 → GShaderCompilingManager 异步编译
                 → FinishCompileShader 存入 ShaderMap
```

---

## 六、关键文件索引

| 文件 | 作用 |
|------|------|
| `Engine/Source/Runtime/Renderer/Private/MobileBasePass.cpp` | MeshProcessor、GetShaders 运行时查找 |
| `Engine/Source/Runtime/Renderer/Private/MobileBasePassRendering.h` | TMobileBasePassVS/PS 类定义、ShouldCompilePermutation |
| `Engine/Source/Runtime/Renderer/Private/MobileBasePassRendering.cpp` | IMPLEMENT_MATERIAL_SHADER_TYPE 宏绑定 .usf、RenderMobileBasePass |
| `Engine/Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp` | FMobileSceneRenderer::Render 主入口 |
| `Engine/Shaders/Private/MobileBasePassVertexShader.usf` | VS 入口 Main，include Material.ush |
| `Engine/Shaders/Private/MobileBasePassPixelShader.usf` | PS 入口 Main，include Material.ush |
| `Engine/Source/Runtime/Engine/Private/Materials/MaterialShared.cpp` | FMaterial::TryGetShaders、BeginCompileShaderMap、Translate |
| `Engine/Source/Runtime/Engine/Private/Materials/MaterialShader.cpp` | FMaterialShaderMap::Compile、SubmitCompileJobs |
| `Engine/Source/Runtime/Engine/Private/Materials/MeshMaterialShader.cpp` | FMeshMaterialShaderType::BeginCompileShader |
| `Engine/Source/Runtime/Engine/Private/Materials/HLSLMaterialTranslator.cpp` | 旧版材质 HLSL 生成 |
| `Engine/Source/Runtime/Engine/Private/Materials/MaterialHLSLEmitter.cpp` | 新版材质 HLSL 生成 |
| `Engine/Source/Runtime/Engine/Private/Materials/MaterialSourceTemplate.cpp` | MaterialTemplate.ush 模板加载 |
| `Engine/Source/Runtime/Engine/Public/MaterialShaderType.h` | IMPLEMENT_MATERIAL_SHADER_TYPE 宏定义 |
| `Engine/Source/Runtime/RenderCore/Public/Shader.h:1620` | IMPLEMENT_SHADER_TYPE 宏定义 |
