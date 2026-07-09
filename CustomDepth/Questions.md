	FRDGTextureDesc CustomDepthDesc(bRequireMultiView ?
		FRDGTextureDesc::Create2DArray(CustomDepthExtent, PF_DepthStencil, FClearValueBinding::DepthFar, CreateFlags, 2) :
		FRDGTextureDesc::Create2D(CustomDepthExtent, PF_DepthStencil, FClearValueBinding::DepthFar, CreateFlags));