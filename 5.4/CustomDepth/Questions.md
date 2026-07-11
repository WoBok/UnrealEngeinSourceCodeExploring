	FRDGTextureDesc CustomDepthDesc(bRequireMultiView ?
		FRDGTextureDesc::Create2DArray(CustomDepthExtent, PF_DepthStencil, FClearValueBinding::DepthFar, CreateFlags, 2) :
		FRDGTextureDesc::Create2D(CustomDepthExtent, PF_DepthStencil, FClearValueBinding::DepthFar, CreateFlags));

给我一份完整的修改方案，不要有任何遗漏，Shader是否需要修改也要考虑到，方案的格式大致为
文件及行号
相关代码（包含一定的上下文）
修改为：
修改后的代码
存到Docs文件夹中

我需要验证一个问题，bIsFullDepthPrepassEnabled代表了EarlyZ是否开启，默认移动渲染Forward路径不开启EarlyZ，方案中通过bIsFullDepthPrepassEnabled验证是否需要写入我的深度，我需要知道会有其他因素影响到这个RenderMobileAfterTranslucencyDepthPass的写入吗？
因为我看源码时发现移动端延迟渲染关系的EarlyZ深度写入判断逻辑好像挺复杂，将验证结果放到新的md当中