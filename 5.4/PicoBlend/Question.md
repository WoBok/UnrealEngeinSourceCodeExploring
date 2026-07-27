我现在需要知道移动端前向渲染下的透明物体是如何进行混合到渲染到屏幕最终RT中的，这个RT的Alpha默认值是多少？这个混合公式是什么？这个RT中的Alpha是如何变化的？
举例说一个半透明物体的Alpha值是0.3，那么这个值是如何影响最终RT的Alpha的？
https://developer-cn.picoxr.com/document/unreal/seethrough/这个是Pico官方的文档，其中提到Eye Buffer，这个Eye Buffer是不是就是这个最终RT，文档中提到
会根据Alpha值做Eye Buffer与VST的混合，这里的Alpha应该就是指的Eye Buffer中的Alpha吧？也即最终RT的Alpha？
我现在需要弄明白的是我透明物体的Alpha如何影响了最终RT（Eye Buffer）的Alpha，进而影响了Eye Buffer与VST的混合？我这句话中的理解是否正确，如果正确给出答案，
如果理解有误则纠正，
你必须要列出的一些问题：
- 透明物体的Alpha如何与最终RT的Alpha混合，公式是什么？
- 最终RT的默认Alpha值是多少？
- r.Mobile.PICO.BlendModeSetting三种模式的混合方式是如何进行的？
- 透明物体的Alpha最终如何影响了Eye Buffer与VST的混合？
- 材质中的Write Alpha Only是如何进行的？它会直接覆盖RT中的Alpha吗？

Engine\PICO-Unreal-Integration-SDK路径下是Pico的SDK，你可以进行查看并进行验证

将结果保存到Engine\Docs中，md格式


1. r.Mobile.PICO.BlendModeSetting 三种模式混合公式，及使用方式，说明混合原理，Alpha如何影响混合，贴出相关代码，要给出文件路径和行号
2. Opaque / Masked，Translucent，Additive的Alpha混合公式，给出相关代码，要给出文件路径和行号
3. SceneColor RT到EyeBuffer再到Pico合成BlendMode的完整链路，其中表现出Opaque、Mask、Translucent、Additive的绘制，整理成Mermaid，要给出文件路径和行号佐证，其中要包含SceneColor RT的Alpha默认值是多少
4. 说明特殊情况，Translucent的Alpha相当于参与了两次混合时的相乘，变暗了，如何避免（我的方式，把我的Alpha在材质中进行Sqrt，这样最终输出就能保持一致了），还有Additive的特殊情况，不写入RT Alpha，材质中预乘等，导致VST中看不到，说明材质的Write Alpha Only是什么，以及是如何写入
5. 在VST下正常使用透明物体的两种方法：1. ClipMode模式下对需要在VST中显示的透明物体的Alpha在在材质中做Sqrt 2.切换成AdditiveMode模式
RT的
根据Engine\Docs中文档，整理上方问题，只关注以上问题即可，对结果的正确性要进行验证
要求：
整理的文档要干净整洁，针对以上问题只保留必要重要信息，但不能遗漏重要信息，要简洁明了易于阅读，有好的排版，逻辑清晰，方便查阅，是一种通用文档，避免使用特例及注意措辞

附：
https://developer-cn.picoxr.com/document/unreal/seethrough/这个是Pico官方的文档
Engine\PICO-Unreal-Integration-SDK路径下是Pico的SDK

将结果保存到Engine\Docs中，md格式

只对文档进行重新排版，保留必要信息