# ESP-Claw 单固件集成

本工程将 Espressif 官方 `esp-claw` 固定为 Git 子模块，并接入它的
`claw_core`、持久会话/长期记忆、Skill Manager、文件能力和 Lua 运行时。
MetalioClaw 继续管理屏幕、麦克风、相机、网络和后台录制服务，因此切换
Agent 不会重启设备，也不会停止持续录音/拍照。

## 获取与构建

首次克隆后初始化子模块：

```bash
git submodule update --init --recursive
. /home/verachen/esp/v5.5.4/export.sh
idf.py build
```

烧录和查看日志：

```bash
idf.py -p /dev/ttyACM0 flash monitor
```

ESP-Claw 固定在提交 `556e432d314c51de5285a4ae895ad3cdda5bade8`。

## 配置

运行前需要配置 OpenAI-compatible API 的 `base_url`、`api_key` 和 `model`。
推荐在 SD 卡创建 `/system/esp_claw.json`，避免把密钥提交到代码仓库：

```json
{
  "backend": "openai_compatible",
  "base_url": "https://api.openai.com/v1",
  "api_key": "YOUR_API_KEY",
  "model": "YOUR_MODEL"
}
```

插入 SD 卡后重新点击 `未配置` 按钮即可重新加载，不需要重启。请妥善保管
这张 SD 卡，因为文件内含明文密钥。

SD 卡可以一直插在设备中，切换 `数字人 / Claw` 模式不需要插拔。Agent 数据
统一保存在 `/sdcard/esp_claw/`：

- `sessions/`：稳定的 Metalio Claw 会话历史以及已激活 Skill 状态；
- `memory/`：长期记忆和可编辑的角色/用户资料；
- `skills/`：用户创建的 Skill；
- `scripts/`：Chat Coding 生成的 Lua 程序；
- `.builtin_skills/`：固件启动时刷新的内置 Skill，不应手工修改。

如果开机时没有挂载 SD 卡，基础对话和设备状态工具仍可启动，但持久记忆、
Skills、写程序和运行游戏能力不会启用。

也可以通过 `idf.py menuconfig` 设置：

```text
Xiaozhi Assistant
  -> Enable in-process ESP-Claw agent mode
```

配置优先级为 NVS、SD 卡文件、编译期配置。也保留了仅限用户触发的
`self.esp_claw.configure` MCP 工具用于受控部署；不要在不可信服务中通过
对话传递密钥，也不要把真实 API Key 提交到 `sdkconfig`。

## 使用方式与边界

- 顶部状态栏的 `数字人 / Claw` 按钮用于即时切换，不会重启。
- 未配置时按钮显示 `未配置`，点击不会离开普通数字人模式。
- ESP-Claw 模式复用现有 WebSocket 的麦克风和 ASR，将最终识别文本提交给
  `claw_core`；普通后端的 LLM/TTS 返回会被屏蔽，防止双重回答。
- 已接入三个设备工具：查询设备/电量/网络/录制状态、统计 SD 卡待上传的
  WAV/JPEG 文件，以及按用户明确要求启动或停止后台录制。另提供明确触发的
  `reset_conversation`，用于清空会话历史和已激活 Skill，但保留长期记忆。
- Claw 使用固定设备会话 `metalio-claw`，不会跟随语音 WebSocket 的临时
  session id 变化，因此断网重连后仍能继续上一轮对话。
- 已接入官方轻量记忆模式：每轮的用户消息、工具调用和最终回答持久化到 SD
  卡；用户明确说“记住”时可激活 `memory_ops` 写入长期记忆。为了控制 RAM 和
  模型调用次数，没有启用后台自动记忆提取。
- 已接入官方 Skill Manager。固件内置 `metalio_chat_coding`、`memory_ops`、
  `profile_memory_ops` 和 `skill_creator`，模型先按需加载 Skill，再得到对应工具
  和完整执行说明，普通聊天不会携带整份游戏开发提示词。
- Chat Coding 已接入官方 `cap_files` 和 `cap_lua`：Agent 可以在受控目录中
  创建、读取和修改 Lua 文件，并以异步任务运行、查询或停止它们。
- 自定义 `metalio_ui` Lua 模块复用设备现有的 720×720 LVGL 屏幕与触摸输入，
  支持矩形、圆、线条、文字和逐帧绘制。游戏退出后会恢复数字人界面。
- 游戏运行时，`BackgroundCaptureService` 继续录音和拍照；游戏只接管显示层，
  不会接管或关闭后台采集模块。
- ESP-Claw 最终回答会通过现有 WebSocket 发送
  `{"type":"tts_request","source":"esp_claw","text":"..."}`，复用数字人
  服务端已经配置的 TTS 音色和 Opus 流。固件只播放带 `source=esp_claw` 标记的
  Claw 音频，仍会丢弃普通数字人 LLM 同时产生的回答，防止双重播报。
  配套的 `voice-assistant-server` 必须包含 `tts_request` 协议扩展并在修改后重启。
- `BackgroundCaptureService` 仍在启动阶段常驻，模式切换代码不会调用它的
  `Start()` 或 `Stop()`。

## 测试写游戏

烧录后先切换到 Claw 并提交一次请求；记忆和 Skill 是首次启动 Claw 时按需
初始化的，不会出现在最前面的系统开机日志中。随后确认设备日志出现：

```text
Agent memory, skills and Chat Coding ready at /sdcard/esp_claw
```

切换到 `Claw` 后可以直接说：

```text
帮我写一个可以触摸控制方向的贪吃蛇游戏，把 Lua 文件保存到 SD 卡并马上运行。
```

模型应该先调用 `activate_skill` 加载 `metalio_chat_coding`，再依次调用
`write_file` 和 `lua_run_script_async`。屏幕进入游戏后可触摸操作；说“退出
游戏”时，模型应调用 `lua_stop_async_job` 或
`lua_stop_all_async_jobs`，随后恢复数字人界面。若它只回复代码而不调用工具，
通常是所配置模型的 function/tool calling 能力不足，应换用明确支持工具调用、
且指令遵循较强的 OpenAI-compatible 模型。

## 测试会话、记忆与 Skill

建议按顺序说下面几句：

```text
我给这只设备取名叫小爪，请在这次对话里记住。
我刚才给设备取了什么名字？
请长期记住我喜欢简短回答。
我希望你以后保持沉稳的嵌入式开发教练风格。
你现在有哪些技能？
```

第二句用于验证跨轮会话历史；第三句应激活 `memory_ops` 并调用
`memory_store`；第四句应激活 `profile_memory_ops`，读取并编辑
`memory/soul.md` 或 `memory/user.md`。重启或断网重连后再次询问，可以验证 SD
卡持久化。说“清空当前对话，从头开始”时模型应调用
`reset_conversation`；也可发送精确命令 `/session reset`，直接开始新会话。
这两种操作都不会删除 `memory/` 下的长期记忆。

ESP-Claw 也可以管理 SD 卡上的用户媒体。说“列出我的照片和录音”时会调用
`list_media_files`；明确说“删除照片 XXX”或“删除所有手动录音”时会调用
`delete_media_files`。“删除我所有的图片和录音”对应 `personal_media`，不会
误删仍待上传的后台录制队列；“删除系统保护/后台录制目录里的图片和录音”对应
`background_media`，同时覆盖 `/sdcard/bgphoto` 和 `/sdcard/bgaudio`。
可管理的范围严格限制为相机相册中的 JPG、
`/sdcard/recordings` 中的手动 OPUS/WAV，以及 `/sdcard/bgphoto`、
`/sdcard/bgaudio` 中的后台录制队列，不能借此删除任意路径。删除手动 OPUS
时会同时清理对应的 `.dur` 时长文件。后台录制仍在运行或正在停止时，后台媒体
删除会被拒绝；应先明确要求停止后台录制，等待停止完成后再删除。

异步游戏返回 `RUNNING` 即代表启动成功，Agent 会立即结束本轮并显示“完成”，
不会等待常驻游戏自然退出，也不会无故反复轮询任务状态。失败时状态栏会明确
显示“执行失败”；切换 Agent 模式会将状态栏恢复为“待命”。

执行期间中间状态栏每 5 秒显示简短的阶段和累计耗时，例如“分析 · 10s”
“写代码”“检查1 · 25s”和“启动游戏”；其他过长状态会在按钮之间循环滚动。
Claw 正在分析或调用工具时，点击数字人不会并发开启新一轮聆听；游戏启动后
触摸仍由 `metalio_ui` 正常接收。分析或执行期间双击数字人会调用官方核心的
请求取消接口，状态先显示“正在取消”，HTTP/当前工具安全退出后回到“待命”；
普通单击仍会被忽略，以免误触造成并发请求。
由于当前官方 OpenAI-compatible 后端返回的
是整轮 JSON 而不是流式 token，模型分析阶段不会逐字显示。单次模型 HTTP 请求
超时为 60 秒；创建游戏通常包含约三次模型请求和两次工具调用，网络与模型正常
时一般需要几十秒，复杂游戏可能需要一到三分钟。

为适应设备较紧张的内部堆，注入每轮模型请求的最近会话历史限制在约 20 KB。
完整历史仍保存在 SD 卡；异步 Lua 任务查询返回精简状态，不在 Agent 请求期间
拼装大段运行日志。日志出现 `Trimmed session history context` 表示保护机制生效，
不是会话存储失败。

当前构建仍未启用官方示例中的 IM、MCP、网页搜索、HTTP 任意请求和定时事件
循环。这些能力需要额外连接配置、权限边界和运行内存；设备历史最低内部内存
曾只有约 49 KB，直接全开会明显增加卡顿、断网和 OOM 风险。它们应作为后续
按需模块接入，而不是默认常驻。
