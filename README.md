# Luxaxis

Luxaxis 提供两个相互独立的插件：

- **Noctalia 插件**：在 Noctalia bar 中显示 Hyprland workspace，支持鼠标左键切换、滚轮切换和 workspace 样式定制。
- **Hyprland 插件**：按 workspace 切换壁纸，并提供遮罩、Spotlight 和可选的切换 transition。

两者可以单独使用，也可以同时使用。Noctalia 插件不会管理壁纸，Hyprland 插件也不会修改 Hyprland 的 workspace 配置。

本文是使用和部署说明。命令默认从本仓库根目录执行。

## 注意事项

- Hyprland 插件目前面向 CachyOS 的 Hyprland `0.56.2` 构建。Hyprland 更新后需要重新构建插件。
- Hyprland 插件只处理正整数的普通 workspace。特殊、命名、负数和零 workspace 不会改变壁纸状态。
- Hyprland 插件读取配置，但不会写入或修改 Hyprland 的 workspace 配置。
- 使用 Hyprland 插件管理壁纸时，应关闭 Noctalia wallpaper 以及其他会在相同输出绘制壁纸的程序。Noctalia bar、桌面小组件等普通 shell surface 不需要关闭。
- 壁纸必须是本地 PNG、JPEG 或 WebP 文件。网络 URL、动画图片和视频不受支持。

## Noctalia 插件

### 依赖

- Noctalia v5，且支持 `plugin_api = 32`。
- Hyprland 和 `hyprctl`。
- `socat`，用于接收 Hyprland workspace 事件并即时刷新状态。

### 安装

将 Noctalia 插件文件复制到 Noctalia 的用户插件目录。下面的命令不会复制 Hyprland C++ 插件：

```bash
NOCTALIA_PLUGIN_DIR="${XDG_DATA_HOME:-$HOME/.local/share}/noctalia/plugins/luxaxis"
mkdir -p "$NOCTALIA_PLUGIN_DIR"
cp -a plugin.toml panel.luau service.luau widget.luau lib translations "$NOCTALIA_PLUGIN_DIR/"
```

确认 Noctalia 能发现插件并启用它：

```bash
noctalia msg plugins list
noctalia msg plugins enable yyc94/luxaxis
```

如果 Noctalia 已经在运行，重新加载配置或重启 Noctalia，使插件清单生效。

### 添加到 bar

可以在 Noctalia 的 Add widget 界面添加 **Luxaxis** workspace widget，也可以在 bar 配置中手动添加：

```toml
type = "yyc94/luxaxis:workspaces"
```

widget 的行为如下：

- 鼠标左键点击 workspace 切换到该 workspace。
- 在 widget 上滚动鼠标滚轮切换前一个或后一个 workspace。
- 键盘快捷键仍由 Hyprland 处理，不由插件接管。
- 每个输出独立显示自己的 active workspace。

### Noctalia 设置

在 Luxaxis 的 **Workspace styles** 面板中可以编辑基础样式、workspace 状态样式和单个 workspace 样式。可调整的内容包括标签来源、图标、文字、颜色、边框、圆角、间距和透明度。

插件设置中还提供两个选项：

| 设置 | 默认值 | 作用 |
| --- | --- | --- |
| Hide empty workspaces | `false` | 隐藏没有窗口且当前未激活的 workspace |
| Invert scroll direction | `false` | 反转滚轮切换方向 |

### 移除

先禁用插件：

```bash
noctalia msg plugins disable yyc94/luxaxis
```

然后删除 `${XDG_DATA_HOME:-$HOME/.local/share}/noctalia/plugins/luxaxis` 目录即可。

## Hyprland 插件

### 依赖

- Hyprland `0.56.2`，以及与当前运行版本匹配的 Hyprland 开发文件和 `pkg-config` metadata。
- CMake `3.25` 或更新版本。
- 支持 C++23 的编译器。
- `pkg-config`、线程库和图像解码依赖。CMake 会在系统没有 `tomlplusplus` 时尝试下载对应版本。

### 构建和安装

在仓库根目录执行：

```bash
cmake -S hyprland -B /tmp/luxaxis-build \
  -DLUXAXIS_BUILD_PLUGIN=ON \
  -DLUXAXIS_BUILD_TESTS=OFF \
  -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/luxaxis-build --parallel
```

构建完成后，将插件放到一个固定路径：

```bash
install -Dm755 /tmp/luxaxis-build/luxaxis.so \
  "$HOME/.local/lib/hyprland/luxaxis.so"
```

如果 CMake 报告找不到 `hyprland=0.56.2`，说明当前系统没有提供匹配运行版本的开发文件；不要使用其他 Hyprland 版本的头文件或库混合构建。

### 创建配置

插件读取以下文件：

```text
${XDG_CONFIG_HOME}/hypr/luxaxis.toml
```

如果没有设置 `XDG_CONFIG_HOME`，则使用 `~/.config/hypr/luxaxis.toml`。先创建目录：

```bash
mkdir -p "${XDG_CONFIG_HOME:-$HOME/.config}/hypr"
```

将下面的内容保存为 `luxaxis.toml`，并把示例中的图片路径替换成实际存在的文件：

```toml
version = 1
default_profile = "default"
active_output = "cursor"
excluded_outputs = []
texture_cache_mib = 256
fallback_color = "#000000"

[transition]
type = "fade"
duration_ms = 220
easing = "ease-out"
origin = "cursor"

[profiles.default]
wallpaper = "~/Pictures/wallpapers/default.webp"
fit = "cover"
position = [0.5, 0.5]

[profiles.default.spotlight]
type = "none"

[profiles.focus]
wallpaper = "~/Pictures/wallpapers/focus.png"
fit = "cover"
position = [0.5, 0.5]

[profiles.focus.spotlight]
type = "circle"
mask_color = "#000000"
mask_opacity = 0.55
radius = "18%"
softness = "4%"

[profiles.beam]
wallpaper = "~/Pictures/wallpapers/beam.jpg"
fit = "cover"

[profiles.beam.spotlight]
type = "fan"
mask_color = "#102b12"
mask_opacity = 0.68
anchor = [0.5, 0.08]
radius = "18%"
aspect_ratio = 0.55
softness = "4%"
beam_start_reveal = 0.35

[workspaces]
"1" = "default"
"2" = "focus"
"3" = "beam"
```

配置是严格校验的。未知字段、错误的路径、缺少的 profile、重复 workspace ID 或无效数值会使整份新配置被拒绝，插件会继续使用上一份有效配置。

### 启用插件

当前会话中可以先手动加载，确认没有错误：

```bash
hyprctl plugin load "$HOME/.local/lib/hyprland/luxaxis.so"
```

要在 Hyprland 启动时自动加载，在 `hyprland.conf` 中加入一行绝对路径：

```ini
plugin = /home/your-user/.local/lib/hyprland/luxaxis.so
```

将路径中的 `your-user` 替换成实际用户名，然后重启 Hyprland。插件的二进制接口必须与正在运行的 Hyprland 完全匹配；如果 Hyprland 更新过，请重新构建并替换 `.so`。

### 配置说明

#### 根配置

| 字段 | 默认值 | 说明 |
| --- | --- | --- |
| `version` | 必填，`1` | 配置版本 |
| `default_profile` | 必填 | 未映射 workspace 使用的 profile |
| `active_output` | `"cursor"` | Spotlight 显示在鼠标所在输出，或使用 `"focused"` 显示在 Hyprland 聚焦输出 |
| `excluded_outputs` | `[]` | 不由 Luxaxis 绘制壁纸的输出名称列表 |
| `texture_cache_mib` | `256` | 壁纸纹理缓存大小，范围 `1..65536` MiB |
| `fallback_color` | `#000000` | 壁纸加载失败时使用的纯色 |
| `transition` | 无 | 全局 workspace 切换 transition；profile 可以单独覆盖 |

`position = [x, y]` 是图片裁剪的焦点，两个坐标均为 `0..1`。`fit` 支持：

- `cover`：保持比例并覆盖整个输出，可能裁剪图片。
- `contain`：保持比例并完整显示图片，可能留下空白区域。
- `stretch`：拉伸图片填满输出。

#### workspace 映射

`[workspaces]` 的键必须是正整数 workspace ID，值是 profile 名称。例如：

```toml
[workspaces]
"1" = "default"
"2" = "focus"
```

没有映射的 workspace 使用 `default_profile`。workspace 映射属于 workspace 本身；workspace 移到另一个输出时，仍使用同一个 profile。每个输出会按照自己的分辨率、缩放和变换单独绘制。

#### Spotlight

`type = "none"` 表示不加遮罩，直接显示壁纸。其他类型都需要 `mask_color`、`mask_opacity` 和 `softness`：

- `circle`：以鼠标为中心的圆形亮区，需要 `radius`。
- `strip`：以鼠标为中心、贯穿输出的横条或竖条，需要 `orientation = "horizontal"` 或 `"vertical"`，以及 `thickness`。
- `fan`：以鼠标为椭圆中心、以 `anchor` 为锚点的扇形/束状亮区。`anchor` 是输出内的归一化坐标，`radius` 控制鼠标周围椭圆半径，`aspect_ratio` 控制椭圆纵横比，`beam_start_reveal` 控制从锚点开始的亮度揭示程度。

长度可以写成逻辑像素（例如 `"240px"`）或输出短边的百分比（例如 `"18%"`）。颜色格式为 `#RRGGBB`，透明度单独由 `mask_opacity` 控制。

Spotlight 只作用于壁纸，不会使窗口、bar、桌面小组件或鼠标指针变暗。非 active output 保留当前 profile 的遮罩，但不显示亮区。

#### Transition

transition 可以放在根配置中，也可以放在某个 profile 中。支持：

- `none`：立即切换。
- `fade`：交叉淡入淡出。
- `wipe`：直线擦除。
- `grow`：从 origin 向外扩大的圆形揭示。
- `outer`：从边缘向 origin 收拢。
- `clock`：围绕 origin 的时钟式扫过。
- `random`：从 `allowlist` 中随机选择，避免连续重复。

可选字段：

```toml
[profiles.focus.transition]
type = "wipe"
duration_ms = 300
easing = "ease-in-out"
origin = [0.5, 0.5]
```

`duration_ms` 范围为 `50..2000`。`easing` 支持 `linear`、`ease-in`、`ease-out` 和 `ease-in-out`。`origin` 可以是 `"cursor"`、`"center"` 或 `[x, y]`。

`random` 必须提供非空的 `allowlist`，例如：

```toml
[transition]
type = "random"
allowlist = ["fade", "wipe", "grow", "outer", "clock"]
```

### 热重载和临时控制

修改 TOML 后，插件会自动监视并重新加载配置。也可以手动请求重载：

```bash
hyprctl luxaxis:reload
```

Spotlight 可以临时关闭或恢复。这个状态只存在于当前 Hyprland 会话，不会写入配置文件：

```bash
hyprctl luxaxis:spotlight on
hyprctl luxaxis:spotlight off
hyprctl luxaxis:spotlight toggle
```

### 移除

从 `hyprland.conf` 删除 `plugin = .../luxaxis.so`，然后重启 Hyprland。之后可以删除 `~/.local/lib/hyprland/luxaxis.so` 和 `luxaxis.toml`；插件不会自动删除或修改这些文件。

## 同时使用两个插件

推荐顺序：

1. 先配置并启用 Hyprland 插件，确认壁纸和 Spotlight 正常显示。
2. 再安装并启用 Noctalia 插件，把 `yyc94/luxaxis:workspaces` 添加到 bar。
3. 如果 Noctalia 仍启用了自己的 wallpaper 功能，关闭它，或者把对应输出加入 Luxaxis 的 `excluded_outputs`，避免两个程序同时管理同一输出的壁纸。

两个插件的配置互不覆盖：Noctalia 的 workspace 样式保存在 Noctalia 的插件数据目录，Hyprland 的壁纸和效果配置保存在 `luxaxis.toml`。
