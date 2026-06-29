	auto ShouldCacheShaderType = [&ShaderPlatform, this, &PermutationFlags](const FShaderType* ShaderType, const FVertexFactoryType* VertexFactoryType, const int32 PermutationId) -> bool {
		// Check to see if the FMaterial should cache these types.
		if (!ShouldCache(ShaderPlatform, ShaderType, VertexFactoryType))
		{
			return false;
		}

		// if we are just a MaterialShaderType (not associated with a mesh)
		if (const FMaterialShaderType* MaterialShader = ShaderType->GetMaterialShaderType())
		{
			return MaterialShader->ShouldCompilePermutation(ShaderPlatform, this, PermutationId, PermutationFlags);
		}

		// if we are a MeshMaterialShader
		if (const FMeshMaterialShaderType* MeshMaterialShader = ShaderType->GetMeshMaterialShaderType())
		{
			const bool bVFShouldCache = FMeshMaterialShaderType::ShouldCompileVertexFactoryPermutation(ShaderPlatform, this, VertexFactoryType, ShaderType, PermutationFlags);
			const bool bShaderShouldCache = MeshMaterialShader->ShouldCompilePermutation(ShaderPlatform, this, VertexFactoryType, PermutationId, PermutationFlags);
			return bVFShouldCache && bShaderShouldCache;
		}
		
		return false;
	};
Engine/Source/Runtime/Engine/Private/Materials/MaterialShared.cpp:3648中ShouldCacheShaderType的必要性？缓存的作用？如果不缓存会怎么样？



1. Engine/Source/Runtime/Renderer/Private/MobileBasePass.cpp:207:GetShader前面所做的都是在为MobileBasePassPixelShader.usf等文件做命令宏处理对吗？所做的分类等最终会体现在MobileBasePassPixelShader.usf中的命令宏里面对吗？           
2. Engine/Source/Runtime/Renderer/Private/MobileBasePass.cpp:1154 PassDrawRenderState.SetBlendState(TStaticBlendStateWriteMask<CW_RGBA>::GetRHI());这里我可以改成CW_NONE，这样就不会写入颜色了对吧？对应到MobileBasePassPixelShader.usf中的什么？中间都经过了哪些处理？ 


Engine/Source/Runtime/Renderer/Private/MobileBasePass.cpp:1187中，
	PassDrawRenderState.SetDepthStencilState(TStaticDepthStencilState<false, CF_DepthNearOrEqual>::GetRHI());
	PassDrawRenderState.SetDepthStencilAccess(FExclusiveDepthStencil::DepthRead_StencilRead);
对于透明物体来说，这里设置了不写入深度，或者说设置了写入深度，对MobileBasePassPixelShader.usf有影响吗？

如果我在材质中设置了Default Lit或者Unlit，MobileBasePassPixelShader.usf中发生了什么变化？相关的代码都有哪些？主要是移动端Forward路径，分析中要包含文件名和行号，分析完成后放到Engine\Docs中，md格式，我的这个问题以引用的方式放到回答开头

MobileBasePassVertexShader.usf
Engine/Source/Runtime/Renderer/Private/MobileBasePass.cpp:207:GetShader中获取的具体是什么Shader？都是MobileBasePassPixelShader.usf中的或者MobileBasePassVertexShader.usf的吗？
我自定义创建的材质是怎么被获取的呢？怎么被编译的呢？给我找到相关代码，还要找到调用链，只顺着移动端Forward渲染路径找，分析完成后存到Engine\Docs中，md格式，当前我问的这个问题以引用的方式放到开头


也就是说：MaterialShared.cpp:3398 BeginCompileShaderMap先执行，在MobileBasePass.cpp:930:     if (!MobileBasePass::GetShaders(时这些Shader就已经编译好了，那么预先编译好的Shader在GetShaders时是怎么与当前Mesh进行对应的，
找到相关代码，找到调用链，只寻找Mobil的Forward路径的调用链，需要找到代码证明，贴出关键代码和行号，最后还要整理Mermaid，完成后附到这篇文档后方，记得把这个引用问题也一并附上 