# Luxaxis 与 Noctalia v5 Wallpaper 性能比较

研究日期：2026-09-28。本文是源码研究与有限诊断记录，不是 GPU 性能验收。

本文的 Luxaxis CPU 路径描述是迁移前的记录。随后实现的 GPU 路径及其离屏
shader 验证范围见 [验证记录](./VALIDATION.md#gpu-renderer)。
本次不修改产品实现。

## 基线与证据边界

- Noctalia 固定为官方已发布的 `v5.1.0`，提交
  [`c7b9197af77ff22bfb9a83c52a95643a1d90ca86`](https://github.com/noctalia-dev/noctalia/tree/c7b9197af77ff22bfb9a83c52a95643a1d90ca86)。
  以下 Noctalia 链接均指向该提交，不将 `main` 的代码算作该发行版能力。
- Luxaxis 为本工作区当前 Hyprland 插件实现；宿主基线仍为 CachyOS/Arch
  `hyprland 0.56.2-3`，与 [已接受规格](./HYPRLAND_PLUGIN.md) 相同。
- **源码事实**指调用路径、数据结构、循环与资源生命周期；**性能推断**指由这些
  机制推导的潜在开销；**实测**只指明确写出环境和测量范围的数值。
- 没有实际 DRM/GPU 输出，因此不提供两者 GPU 毫秒、FPS、功耗或总体速度排名。
  先前加载/卸载回归和 headless UI 测试不等同于 wallpaper 性能测试，见
  [验证记录](./VALIDATION.md)。

Noctalia v5 确实是原生 C++/OpenGL ES，而不是 v4 的 Quickshell/QML。
来源：[官方 v5 FAQ，42 行][n-faq]。

## 结论

最值得借鉴的是 Noctalia 的**双源纹理加参数的 GPU shader**：转场几何和颜色
混合在一次 wallpaper draw 中执行。Luxaxis 的集成位置可以保留，而把目前的
CPU Spotlight 栅格和复杂 clip-region 转场改为 compositor 内的专用 shader。
这能去除 CPU 栅格重建、动态 mask 上传和大量细碎 clip 处理，同时提高 4K 边缘
精度。它是有源码依据的优化方向，不是已经测得的 GPU 加速倍数。

不能据此认定 Noctalia 所有方面更快。`v5.1.0` 的普通 wallpaper 冷加载使用
同步全尺寸解码和上传；Luxaxis 已将文件读取/解码放在 worker，且有按字节计费
的暖缓存。Noctalia 的第三张壁纸会等待当前动画结束，不能直接移植为 Luxaxis
要求的“立即打断并从当前合成结果转场”。

## Noctalia 已核实路径

### 普通壁纸不是额外离屏 blur 管线

实际调用链为：

```text
Wallpaper::createInstance
  -> 每个 output 的 background LayerSurface + WallpaperNode
  -> Surface::render
  -> RenderContext::renderScene / NodeType::Wallpaper
  -> GlesRenderBackend::drawWallpaper / WallpaperProgram::draw
  -> EGL window surface / eglSwapBuffers
  -> compositor 导入并合成该 surface buffer
```

`createInstance` 创建四边锚定、click-through 的 background layer-shell surface，
再向 scene 加入 fill Box 和 WallpaperNode；configure callback 设置逻辑尺寸。
来源：[wallpaper.cpp，1221 行][n-instance]。

普通 wallpaper 由 `Surface::render` 调用 `RenderContext::renderScene`，后者在
`NodeType::Wallpaper` 中传入双源纹理、progress、fill mode、transform 和 span。
来源：[surface.cpp，1229 行][n-surface-render]；
[render_context.cpp，195 行][n-scene]、[523 行][n-node-draw]。

这一普通路径有 clear、fill Box、wallpaper shader 绘制，随后 swap；不能把“一次
wallpaper shader draw”写成“整个客户端只有一次 GPU 操作”。
`WallpaperRenderer::renderBackdropContent` 的 FBO、blur、tint、presentTexture
和 blit 属于 backdrop 等额外需求，不是普通 wallpaper 的必经路径。
来源：[backdrop_surface.cpp，50 行][n-backdrop]；
[wallpaper_renderer.cpp，191 行][n-backdrop-passes]。

### GPU shader 与采样

`WallpaperProgram` 为 fade、wipe、disc、stripes、zoom、honeycomb 编译独立 fragment
program。每帧 CPU 只绑定纹理和写 uniforms，最终是
`glDrawArrays(GL_TRIANGLES, 0, 6)`。fade 用 `mix(color1, color2, progress)`；wipe/disc
在 GLSL 中用 `smoothstep` 计算边界，不生成 CPU 网格。
来源：[wallpaper_program.cpp，72 行][n-fragments]、
[288 行][n-program-init]、[365 行][n-program-draw]、[450 行][n-one-draw]。

公共 sampling GLSL 在 shader 中处理 fill mode 和跨 output span；同一个 program
支持图片或纯色源。静态时没有第二张纹理会绑定第一张作为第二源、将 progress
设为 0，但片段源码仍存在两次 sample 调用，不能仅凭这一分支断言驱动消除了
第二次采样。来源：[render_context.cpp，523 行][n-node-draw]；
[wallpaper_program.cpp，49 行][n-sampling]。

可借鉴的是实现方式，不是新增 Noctalia 的 stripes/zoom/honeycomb 产品承诺。
Luxaxis 的 grow/outer/clock、live cursor、mask 语义仍按自身规格。

### 解码、上传与缓存

`application_ui.cpp` 给 Wallpaper 注入的是 `SharedTextureCache`，不是
`AsyncTextureCache`。冷加载路径是：

```text
Wallpaper::loadWallpaper -> acquireTexture(path)
  -> SharedTextureCache::acquire(path)
  -> TextureManager::loadFromFile(path, targetSize=0, mipmap=true)
  -> readBinaryFile -> decodeRasterImage -> glTexImage2D
```

这条路径没有切到 worker；普通状态变更和初次 configure 回调内同步完成。
来源：[application_ui.cpp，134 行][n-inject]；
[wallpaper.cpp，1287 行][n-acquire]；[1322 行][n-load]；
[shared_texture_cache.cpp，44 行][n-cache-acquire]；
[gles_texture_manager.cpp，88 行][n-load-file]；
[image_file_loader.cpp，498 行][n-file-io]。

`SharedTextureCache` 按路径字符串共享 TextureId、引用计数；多 output 或同一
share-group 的 lock/backdrop 复用同一路径时不必再次解码/上传。引用计数归零立即
卸载，没有以 MiB 为单位的 LRU 暖缓存。若该路径仍被别的 subsystem 持有，归零
条件未满足，纹理仍可命中。来源：[shared_texture_cache.h，12 行][n-cache-header]；
[shared_texture_cache.cpp，119 行][n-cache-release]。

`AsyncTextureCache` 确实有 2..4 workers、eventfd、ready subscription、128 个零引用
条目的保留策略，但不是本版本 Wallpaper 的调用路径，不能据此宣称 wallpaper
异步加载或有 128 张壁纸暖缓存。来源：[async_texture_cache.cpp，19 行][n-async]，
结合上述注入和 acquire 路径。

性能推断：冷加载大图可能卡住 Noctalia 自己的主循环；这是客户端卡顿风险，
不等于解码在 Hyprland render thread 上执行。Luxaxis 将冷解码放到 worker 的
做法应保留，但两者最终的 GL 上传仍可能受驱动同步和显存分配影响。

### 尺寸、mipmap 与内存边界

Noctalia wallpaper 调用固定 `targetSize=0`，不按 output 分辨率预缩小。
`loadImageBytes` 只有 `targetSize > 0 && maxDim > targetSize` 才降采样，且发生在
完整 raster decode 之后，不能降低首次完整解码的峰值内存。
来源：[shared_texture_cache.cpp，68 行][n-cache-acquire]；
[image_file_loader.cpp，340 行][n-image-resize]。

`GlesRenderBackend::maxTextureSize` 虽然查询 `GL_MAX_TEXTURE_SIZE`，但本版本调用者
是 text renderer，不是 wallpaper uploader。wallpaper 使用 `glTexImage2D` 并检查
GL 错误，没有按 GPU 上限自动缩图的分支。来源：
[gles_render_backend.cpp，530 行][n-max-texture]；
[cairo_text_renderer.cpp，737 行][n-text-limit]；
[gles_texture_manager.cpp，53 行][n-tex-error]、[334 行][n-upload]。

默认 shared GL 与 mipmap 开启；wallpaper 上传后生成 mipmap，使用 trilinear
minification。它能提高大图缩小时的采样质量，但增加上传/构建工作及源纹理显存。
`disable_mipmaps` 可关闭。来源：[config_types.h，1116 行][n-defaults]；
[gles_texture_manager.cpp，334 行][n-upload]。

按 RGBA8 理想存储量估算，不含 driver 对齐、压缩、buffer 池、CPU 解码临时对象：

| 单张源图 | level 0 | 完整 mipmap 链近似 |
| --- | ---: | ---: |
| 3840x2160 | 31.64 MiB | 42.19 MiB |
| 7680x4320 | 126.56 MiB | 168.75 MiB |

计算为 `width * height * 4`，mipmap 链约再增加 1/3。这是容量估算，不是进程 RSS
或实测 VRAM。转场通常同时持有 current/next，随后 release 旧源；每个 output
还需自己的 EGL surface buffer(s)，其数量由 EGL/driver/compositor 决定。
多 output 共享源纹理不等于共享呈现 buffer，也不等于所有内存只保留一份。
来源：[wallpaper_instance.h，36 行][n-instance-state]；
[wallpaper.cpp，1484 行][n-finish]；
[gles_render_backend.cpp，197 行][n-window-buffer]。

### 缩略图不是桌面壁纸纹理

WallpaperTile 的预览使用 ThumbnailService，按 tile 的物理显示尺寸申请。
服务有 2..4 workers，缩放后把 WebP 写入磁盘缓存；key 包括源路径、文件大小、
mtime、targetPx 与版本，缓存目录为 `$XDG_CACHE_HOME/noctalia/thumbnails`。
引用归零释放预览 GPU 纹理，磁盘缓存可供下次访问。
来源：[wallpaper_tile.cpp，416 行][n-thumbnail-tile]；
[thumbnail_service.cpp，61 行][n-thumbnail-key]、[181 行][n-thumbnail-workers]、
[300 行][n-thumbnail-release]、[509 行][n-thumbnail-decode]。

这是未来若有壁纸选择面板时值得参考的异步缩略图管线，不能用来推断普通桌面
wallpaper 已降采样、已持久缓存或已绕过全尺寸解码。

### 调度、damage 与遮挡

Surface 合并 dirty/layout/update 状态，用 `wl_surface.frame` 驱动动画；无 dirty
且无 active animation 时停止帧循环。动画仍 active 但像素未变时，只提交 frame
callback 并保留当前 buffer，不必重画。EGL swap interval 设为 0，节奏由 compositor
callback 控制。来源：[surface.cpp，371 行][n-frame-done]、
[1327 行][n-frame-loop]；[gles_render_backend.cpp，352 行][n-frame-backend]。

AnimationManager 按 `steady_clock` 的真实起止时间计算进度，适应稀疏 frame
callbacks，下一次收到 callback 时追上墙钟时间。wallpaper 专用 `animateTimer`
不受全局 animation speed/enabled 开关影响。
来源：[animation_manager.cpp，118 行][n-animation-clock]；
[wallpaper.cpp，1426 行][n-animation-start]。

普通 wallpaper 主路径未实现 cursor Spotlight，也没有 wallpaper 自己的
fullscreen occlusion 判定；它是否在隐藏时停止动画更新，取决于 compositor 是否
停止发送 surface frame callbacks。因此不能把它写成“客户端主动保证完全遮挡时
零动画工作”。dirty 节点标记不是输出上的空间 damage；每次客户端 render 的
`beginFrame` 禁用 scissor 并清空整个 target，普通路径未调用 damage-aware swap。
来源：[surface.cpp，371 行][n-frame-done]、[1428 行][n-frame-loop]；
[gles_render_backend.cpp，382 行][n-begin-end]。

### “共享纹理”和“零拷贝”的准确含义

Noctalia 的 root/shared EGL context 是**自身进程内**的 GL share group，能共享
源 TextureId；不是与 Hyprland 共享同一个 GL context 或直接交出壁纸 source
TextureId。它先绘制到自身 `wl_egl_window`/EGL window surface，再提交给 compositor。
来源：[gl_shared_context.cpp，102 行][n-share-root]；
[gles_render_backend.cpp，197 行][n-window-buffer]、[394 行][n-begin-end]。

Wayland EGL buffer 通常可以由 compositor 导入 DMA-BUF，避免整帧 CPU 读回/复制，
但这不能表述成“没有 GPU 工作”或“没有中间 render target”：客户端仍写入一张
输出 buffer，compositor 仍可能读取它进行合成；特定硬件 plane/direct scanout
是否消除这一步，需要实际呈现路径证据。Hyprland 的 buffer-to-texture 接口先尝试
DMA-BUF，失败才走 SHM 路径，来源：
[Hyprland v0.56.2 Renderer.cpp，905 行](https://github.com/hyprwm/Hyprland/blob/v0.56.2/src/render/Renderer.cpp#L905)。
本次未测量 DMA-BUF import、cross-GPU copy 或硬件 plane，不能断言始终零拷贝。

## 与当前 Luxaxis 的比较

| 场景/机制 | Noctalia v5.1.0 | 当前 Luxaxis | 判断边界 |
| --- | --- | --- | --- |
| 不变的静态壁纸 | 客户端停止重画，保留已合成 buffer | 不持续发 damage；compositor 需要绘制时使用源纹理 | 二者均可 idle，无总体速度排名 |
| 冷图加载 | 自己的主循环同步 IO/decode/upload | worker IO/decode，render stage 每次最多上传 1 张 | Luxaxis 隔离解码较好，上传仍需测量 |
| 返回最近 workspace | 无引用后删纹理；若仍有人引用则共享命中 | 256 MiB 字节 LRU，active/in-transition pin | Luxaxis 更适合频繁返回，但可能使用更多常驻内存 |
| 普通 fade | GPU shader 双源、一次 wallpaper draw，再提交 buffer | 多个 compositor texture pass | Noctalia shader 值得借鉴；客户端合成 pass 也有成本 |
| grow/clock 边界 | disc 等几何在每个 fragment 中连续计算 | 48x48 CPU 采样构造 clip region | Luxaxis 现路径有块状精度与 region 处理代价 |
| 光标 Spotlight | 普通 wallpaper 无对应 live cursor 效果 | 192x192 CPU mask 重建/上传，再叠加 pass | 功能不对等，不能拿纯 wallpaper CPU 对 Spotlight 全负载排名 |
| source 共享 | 路径字符串共享，进程内 share group | cache 全局路径共享，compositor context | 两者都已避免同源多输出重复 source 上传 |
| fractional scale | surface 以物理 buffer 尺寸渲染 | 使用 compositor output/transform 路径 | 物理像素工作量都随 scale 平方变化；需实测 transform 正确性 |
| 完全遮挡 | 依赖 compositor callback 停止，无客户端主动 occlusion | 抑制 fullscreen damage，但以 hasFullscreen 粗判定 | 二者不应被写成已通过完全遮挡验收 |

Luxaxis 依据：
[image_cache.cpp，25 行](../hyprland/src/core/image_cache.cpp#L25)、
[202 行](../hyprland/src/core/image_cache.cpp#L202)；
[hyprland_adapter.cpp，590 行](../hyprland/src/hyprland_adapter.cpp#L590)、
[638 行](../hyprland/src/hyprland_adapter.cpp#L638)、
[674 行](../hyprland/src/hyprland_adapter.cpp#L674)、
[702 行](../hyprland/src/hyprland_adapter.cpp#L702)、
[798 行](../hyprland/src/hyprland_adapter.cpp#L798)。

192x192 RGBA mask 是 36,864 个 CPU geometry samples 与 144 KiB 新上传；不是每帧
CPU 生成整张 4K 图。若每帧变化，理论提交字节量约为 8.44 MiB/s @60Hz 或
20.25 MiB/s @144Hz，多个 evaluated masks 相应增加。实际 CPU 时间、driver
allocation/stall 和 GPU fragment 工作不能从字节量直接算出。
48x48 采样也**不等于 2,304 draw calls**：region 会合并相邻单元，draw 数应以
合并后的矩形以及 Hyprland 实际 draw 路径核实。

Luxaxis cache budget 只计源纹理 level-0 bytes，不是整个插件 VRAM/RSS 的硬上限。
mask、replacementFade 持有的旧纹理、解码队列临时像素、render pass 以及未来
interruption snapshot 均需独立核算；active pin 可超预算是规格允许的行为。
来源：[image_cache.cpp，144 行](../hyprland/src/core/image_cache.cpp#L144)、
[218 行](../hyprland/src/core/image_cache.cpp#L218)；
[hyprland_adapter.cpp，391 行](../hyprland/src/hyprland_adapter.cpp#L391)、
[590 行](../hyprland/src/hyprland_adapter.cpp#L590)。

## 中断语义不能照搬

Noctalia 从 A 到 B 动画中请求 A，会从当前时间反向运行；请求 B 继续运行；请求
无关 C 只替换 `queuedPath`，等 A/B 动画结束后才加载 C，不捕获当前混合 framebuffer。
这用两张源图保持简单和有限资源，但会增加 C 的生效延迟。
来源：[wallpaper.cpp，1322 行][n-load]、[1383 行][n-redirect]、[1461 行][n-finish]。

Luxaxis 规格要求 A->B->C 时立即转向 C，以当前合成结果为起点；cursor 在转场中
仍保持 live。当前代码用 `interruptedSourceProfile + previousProfile + progress`
重放一层，不是 FBO snapshot；继续 A->B->C->D 会丢失更早的 A 合成分量。
此处既不能宣称已经等价于 snapshot，也不能以 Noctalia queue 取代已接受语义。
依据：[engine.cpp，311 行](../hyprland/src/core/engine.cpp#L311)、
[hyprland_adapter.cpp，773 行](../hyprland/src/hyprland_adapter.cpp#L773)，
以及 [Transition Model](./HYPRLAND_PLUGIN.md#transition-model-phase-2b)。

GPU snapshot 是可评估方向，但额外的 output-size texture 必须计入内存；snapshot
内容应明确为 wallpaper composition，不是连窗口/bar 一起抓取，也不能冻结要求
继续 live 的 Spotlight cursor。Noctalia 的两个 source model 可参考，其 queue/
reverse 状态机不能直接代替这项设计。

## 建议优先级

1. 将几何转场与 Spotlight 求值搬入 Hyprland adapter 专用 GLSL，普通帧直接输出
   到 compositor 当前 framebuffer；只在真正中断时使用受控的 offscreen snapshot。
   保留 Engine/缓存边界与产品合同，先验证输出 transform、色彩和完整中断序列。
2. 保留异步解码和字节 LRU，补充 decoded staging、旧源、mask/snapshot 的峰值核算；
   根据实际 output/fit 与 GPU limit 在 worker 降采样，不能把缩略图画质当桌面画质。
   Noctalia mipmap 是质量/VRAM tradeoff，可研究，不能不计预算直接开启。
3. 帧调度继续由 compositor cadence/真实时间驱动，idle 不发 damage，动画只运行至
   完成；修正全屏遮挡判定后，再用 opaque fullscreen 与透明/非覆盖 fullscreen
   两类场景验证。Noctalia callback-based 停顿不构成我们可直接使用的 occlusion API。
4. 若以后新增壁纸选择面板，参考 ThumbnailService 的尺寸感知、异步缩略图和
   mtime/version disk key；当前两个产品边界不因此合并。

若实际复制/改编 Noctalia shader 的实质内容，应保留
[MIT 版权与许可证](https://github.com/noctalia-dev/noctalia/blob/c7b9197af77ff22bfb9a83c52a95643a1d90ca86/LICENSE#L1)，
而不是引入整个 Noctalia Wayland/EGL/scene graph 运行时。

## 测量记录

本节保留给此次 CPU microbenchmark 和合并 clip region 诊断。它们可以定位 Luxaxis
当前 CPU 热点，但不包括 GPU draw、上传阻塞、客户端额外合成或 DRM present，
也不是 Noctalia 与 Luxaxis 的端到端 A/B 测试。

正式性能比较仍需同机同图同输出的至少四组：静态 none、纯转场、cursor Spotlight、
opaque fullscreen。分辨率至少 4K，固定 scale、refresh、blur 与 driver，记录 CPU
主线程/worker、GPU timer、frame p95/p99、RSS/VRAM、idle wakeups 和缓存冷/暖状态；
不能用 Noctalia 整个 shell 的 RSS/CPU 与 Luxaxis wallpaper 增量成本混为一谈。

## 固定官方源码索引

[n-faq]: https://github.com/noctalia-dev/noctalia/blob/c7b9197af77ff22bfb9a83c52a95643a1d90ca86/docs/user/getting-started/faq.mdx#L42
[n-inject]: https://github.com/noctalia-dev/noctalia/blob/c7b9197af77ff22bfb9a83c52a95643a1d90ca86/src/app/application_ui.cpp#L134
[n-instance]: https://github.com/noctalia-dev/noctalia/blob/c7b9197af77ff22bfb9a83c52a95643a1d90ca86/src/shell/wallpaper/wallpaper.cpp#L1221
[n-instance-state]: https://github.com/noctalia-dev/noctalia/blob/c7b9197af77ff22bfb9a83c52a95643a1d90ca86/src/shell/wallpaper/wallpaper_instance.h#L36
[n-acquire]: https://github.com/noctalia-dev/noctalia/blob/c7b9197af77ff22bfb9a83c52a95643a1d90ca86/src/shell/wallpaper/wallpaper.cpp#L1287
[n-load]: https://github.com/noctalia-dev/noctalia/blob/c7b9197af77ff22bfb9a83c52a95643a1d90ca86/src/shell/wallpaper/wallpaper.cpp#L1322
[n-redirect]: https://github.com/noctalia-dev/noctalia/blob/c7b9197af77ff22bfb9a83c52a95643a1d90ca86/src/shell/wallpaper/wallpaper.cpp#L1383
[n-animation-start]: https://github.com/noctalia-dev/noctalia/blob/c7b9197af77ff22bfb9a83c52a95643a1d90ca86/src/shell/wallpaper/wallpaper.cpp#L1426
[n-finish]: https://github.com/noctalia-dev/noctalia/blob/c7b9197af77ff22bfb9a83c52a95643a1d90ca86/src/shell/wallpaper/wallpaper.cpp#L1461
[n-scene]: https://github.com/noctalia-dev/noctalia/blob/c7b9197af77ff22bfb9a83c52a95643a1d90ca86/src/render/render_context.cpp#L195
[n-node-draw]: https://github.com/noctalia-dev/noctalia/blob/c7b9197af77ff22bfb9a83c52a95643a1d90ca86/src/render/render_context.cpp#L523
[n-fragments]: https://github.com/noctalia-dev/noctalia/blob/c7b9197af77ff22bfb9a83c52a95643a1d90ca86/src/render/programs/wallpaper_program.cpp#L72
[n-sampling]: https://github.com/noctalia-dev/noctalia/blob/c7b9197af77ff22bfb9a83c52a95643a1d90ca86/src/render/programs/wallpaper_program.cpp#L49
[n-program-init]: https://github.com/noctalia-dev/noctalia/blob/c7b9197af77ff22bfb9a83c52a95643a1d90ca86/src/render/programs/wallpaper_program.cpp#L288
[n-program-draw]: https://github.com/noctalia-dev/noctalia/blob/c7b9197af77ff22bfb9a83c52a95643a1d90ca86/src/render/programs/wallpaper_program.cpp#L365
[n-one-draw]: https://github.com/noctalia-dev/noctalia/blob/c7b9197af77ff22bfb9a83c52a95643a1d90ca86/src/render/programs/wallpaper_program.cpp#L450
[n-cache-header]: https://github.com/noctalia-dev/noctalia/blob/c7b9197af77ff22bfb9a83c52a95643a1d90ca86/src/render/core/shared_texture_cache.h#L12
[n-cache-acquire]: https://github.com/noctalia-dev/noctalia/blob/c7b9197af77ff22bfb9a83c52a95643a1d90ca86/src/render/core/shared_texture_cache.cpp#L44
[n-cache-release]: https://github.com/noctalia-dev/noctalia/blob/c7b9197af77ff22bfb9a83c52a95643a1d90ca86/src/render/core/shared_texture_cache.cpp#L119
[n-async]: https://github.com/noctalia-dev/noctalia/blob/c7b9197af77ff22bfb9a83c52a95643a1d90ca86/src/render/core/async_texture_cache.cpp#L19
[n-load-file]: https://github.com/noctalia-dev/noctalia/blob/c7b9197af77ff22bfb9a83c52a95643a1d90ca86/src/render/backend/gles_texture_manager.cpp#L88
[n-tex-error]: https://github.com/noctalia-dev/noctalia/blob/c7b9197af77ff22bfb9a83c52a95643a1d90ca86/src/render/backend/gles_texture_manager.cpp#L53
[n-upload]: https://github.com/noctalia-dev/noctalia/blob/c7b9197af77ff22bfb9a83c52a95643a1d90ca86/src/render/backend/gles_texture_manager.cpp#L334
[n-defaults]: https://github.com/noctalia-dev/noctalia/blob/c7b9197af77ff22bfb9a83c52a95643a1d90ca86/src/config/config_types.h#L1116
[n-file-io]: https://github.com/noctalia-dev/noctalia/blob/c7b9197af77ff22bfb9a83c52a95643a1d90ca86/src/render/core/image_file_loader.cpp#L498
[n-image-resize]: https://github.com/noctalia-dev/noctalia/blob/c7b9197af77ff22bfb9a83c52a95643a1d90ca86/src/render/core/image_file_loader.cpp#L340
[n-max-texture]: https://github.com/noctalia-dev/noctalia/blob/c7b9197af77ff22bfb9a83c52a95643a1d90ca86/src/render/backend/gles_render_backend.cpp#L530
[n-text-limit]: https://github.com/noctalia-dev/noctalia/blob/c7b9197af77ff22bfb9a83c52a95643a1d90ca86/src/render/text/cairo_text_renderer.cpp#L737
[n-backdrop]: https://github.com/noctalia-dev/noctalia/blob/c7b9197af77ff22bfb9a83c52a95643a1d90ca86/src/shell/backdrop/backdrop_surface.cpp#L50
[n-backdrop-passes]: https://github.com/noctalia-dev/noctalia/blob/c7b9197af77ff22bfb9a83c52a95643a1d90ca86/src/render/wallpaper_renderer.cpp#L191
[n-thumbnail-tile]: https://github.com/noctalia-dev/noctalia/blob/c7b9197af77ff22bfb9a83c52a95643a1d90ca86/src/shell/wallpaper/panel/wallpaper_tile.cpp#L416
[n-thumbnail-key]: https://github.com/noctalia-dev/noctalia/blob/c7b9197af77ff22bfb9a83c52a95643a1d90ca86/src/render/core/thumbnail_service.cpp#L61
[n-thumbnail-workers]: https://github.com/noctalia-dev/noctalia/blob/c7b9197af77ff22bfb9a83c52a95643a1d90ca86/src/render/core/thumbnail_service.cpp#L181
[n-thumbnail-release]: https://github.com/noctalia-dev/noctalia/blob/c7b9197af77ff22bfb9a83c52a95643a1d90ca86/src/render/core/thumbnail_service.cpp#L300
[n-thumbnail-decode]: https://github.com/noctalia-dev/noctalia/blob/c7b9197af77ff22bfb9a83c52a95643a1d90ca86/src/render/core/thumbnail_service.cpp#L509
[n-surface-render]: https://github.com/noctalia-dev/noctalia/blob/c7b9197af77ff22bfb9a83c52a95643a1d90ca86/src/wayland/surface.cpp#L1229
[n-frame-done]: https://github.com/noctalia-dev/noctalia/blob/c7b9197af77ff22bfb9a83c52a95643a1d90ca86/src/wayland/surface.cpp#L371
[n-frame-loop]: https://github.com/noctalia-dev/noctalia/blob/c7b9197af77ff22bfb9a83c52a95643a1d90ca86/src/wayland/surface.cpp#L1327
[n-frame-backend]: https://github.com/noctalia-dev/noctalia/blob/c7b9197af77ff22bfb9a83c52a95643a1d90ca86/src/render/backend/gles_render_backend.cpp#L352
[n-begin-end]: https://github.com/noctalia-dev/noctalia/blob/c7b9197af77ff22bfb9a83c52a95643a1d90ca86/src/render/backend/gles_render_backend.cpp#L382
[n-animation-clock]: https://github.com/noctalia-dev/noctalia/blob/c7b9197af77ff22bfb9a83c52a95643a1d90ca86/src/render/animation/animation_manager.cpp#L118
[n-share-root]: https://github.com/noctalia-dev/noctalia/blob/c7b9197af77ff22bfb9a83c52a95643a1d90ca86/src/render/gl_shared_context.cpp#L102
[n-window-buffer]: https://github.com/noctalia-dev/noctalia/blob/c7b9197af77ff22bfb9a83c52a95643a1d90ca86/src/render/backend/gles_render_backend.cpp#L197
