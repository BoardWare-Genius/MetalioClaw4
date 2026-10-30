# Metalio 蓝屏与重启排查

2026-10-30 的 P4 串口日志 `build/log/idf_py_stdout_output_2879489`
捕获到开机约 13.9 秒发生的崩溃：

```text
Timeout waiting for Resp for [0x116](Req_WifiInit)
ESP_ERROR_CHECK failed: esp_err_t 0xffffffff (ESP_FAIL)
file: "./managed_components/78__esp-wifi-connect/wifi_station.cc" line 108
func: void WifiStation::Start()
expression: esp_wifi_init(&cfg)
abort() was called
```

P4 已经建立 SDIO 传输并识别 C5，但 Wi-Fi 初始化 RPC 没有及时返回。
旧版调用 `ESP_ERROR_CHECK`，将网络初始化失败升级为整机崩溃。
随后自动重启并联网成功。该日志只捕获一次异常，不能证明其他蓝屏均为此原因；
C5 没有回复的底层原因仍需后续串口记录确认。

## 修改

- 将 `78/esp-wifi-connect` 2.6.2 复制到项目组件并通过 `override_path` 使用，
  不直接修改会被重新下载覆盖的 `managed_components`。
- `Start()` 返回启动结果，最多尝试三次，间隔一秒。每次失败先取消事件处理、
  删除定时器、停止并反初始化 Wi-Fi、释放默认网络接口；清理失败则停止重试。
  RPC 自身的超时时间仍由 ESP-Hosted 控制。
- 启动失败时显示现有“WiFi 初始化失败”提示，直接返回上层，不等待额外的
  60 秒连接超时。原有 OTA/激活检查流程未变，断网时在线功能仍不可用。
- `Stop()` 可重复调用；断连时读取 RSSI、频道、设置省电模式不再触发 abort。
- 扫描定时器在驱动启动前创建。无网络和扫描失败时按十秒间隔重试，
  修正原实现实际仅等待十毫秒的问题。
- 网络设置页和 Wi-Fi 测试页在初始化失败时清理部分资源；切模式、启动失败
  返回错误而不 abort；使用默认 Wi-Fi 接口配套销毁函数释放其事件处理器。

## 验证

```bash
python3 scripts/test_wifi_station.py
source /home/verachen/esp/v5.5.4/export.sh
idf.py build
```

第一轮以上故障注入测试和 ESP-IDF 5.5.4 编译均通过，生成
`build/xiaozhi.bin`（9,047,904 字节）。依赖版本保持原锁定版本，
仅将 Wi-Fi 组件切换为项目内覆盖。

故障注入直接编译修改后的 `wifi_station.cc` 和两个 Wi-Fi 页面的实际生命周期函数，覆盖瞬时/持续初始化失败、
清理失败、部分初始化失败、重复启动和停止、再次启动、断连查询与扫描/连接失败。
使用 ASan/UBSan 检查宿主测试中的内存错误。

修改不代表已经解决 C5 超时本身，也不代表全部蓝屏已排除。
第一轮未自动烧录。后续验证记录见下文。

## 第二轮：仍存在底层强制重启

用户反馈仍间歇蓝屏。2026-10-30 14:02 从设备读取应用描述符，确认设备 ELF
SHA256 为 `4157174396d74884ce2f8f414650b20655b540f4dd5c8bd3408137c7c1c533dd`，
与当时本机构建一致，包含第一轮修改。读取了 Flash 转储作为备份，但转储对应
较早的 `b6657ea47` 构建，与当前 ELF 不匹配，未用它推断当前调用栈。

保存串口日志后，现场复现到：

```text
14:02:25 WifiStation: WiFi startup attempt 1/3 failed: ESP_FAIL
14:02:30 rpc_core: Timeout waiting for Resp ... (Req_WifiScanStop)
14:02:31 H_SDIO_DRV: sdio_write_task: 0: Failed to send data: 263 29 29
14:02:32 H_SDIO_DRV: sdio_write_task: 1: Failed to send data: 263 29 29
14:02:32 H_SDIO_DRV: Unrecoverable host sdio state
14:02:32 os_wrapper_esp: Restarting host
14:02:32 rst:0xc (SW_CPU_RESET)
```

第一轮确实阻止了上层 `ESP_ERROR_CHECK` 的 abort，但
`CONFIG_ESP_HOSTED_TRANSPORT_RESTART_ON_FAILURE=y` 仍让 ESP-Hosted
在 SDIO 连续失败时直接重启主控。这条路径不会生成新的 panic 转储。

第二轮修改：

- 关闭该配置，改由 `ESP_HOSTED_EVENT_TRANSPORT_FAILURE` 使连接状态失效、
  停止扫描/连接回调，保留本地应用。不会在事件回调里销毁仍被其他线程使用的
  RPC/SDIO 对象，也不尝试未经验证的强制热重置。
- 在首次连接前注册 C5 事件监听；等待 `ESP_HOSTED_EVENT_CP_INIT` 后再初始化
  Wi-Fi。`esp_hosted_connect_to_slave()` 返回仅表示传输链路就绪，不能替代
  C5 的 RPC 服务启动完成事件。等待有十秒上限，失败返回错误。
- 设置页和测试页使用同一就绪检查。启动时 Wi-Fi 不可用则跳过在线 OTA 检查，
  避免指数退避长时间卡住启动流程。
- 添加开启 ESP-Hosted 的故障注入测试，检查初始化顺序、等待超时、通信失败后
  清除连接状态，以及收到新 C5 启动事件后允许再次连接。

就绪等待针对启动顺序这一可能原因；尚不能据此断定 SDIO 超时完全由时序引起。
硬件链路、电源和 C5 固件的其他问题仍需要持续日志判断。

### 第二轮实机结果（40 MHz）

第二轮编译及故障注入通过，主程序于 14:13 烧录到 `0x200000`，工具校验通过。
未重写 NVS、资源、分区表或 C5 固件。该版 ELF SHA256 为
`9a5978b0db42bf6d422d9903006990a8b93c6d9c25a2dcb52b04e090d95af795`。

串口记录：`build/log/metalio_wifi_validation_20261030.log`。
三次受控启动中，前两次成功获取 IP 并进入 idle；第三次收到 CP_INIT 后仍发生
`Req_WifiInit` 超时及 SDIO 写入失败，说明启动事件等待并没有解决底层超时。
第三次故障后没有 `Restarting host`、abort 或非预期复位，应用于 14:14:44
继续进入 idle；在线 OTA 检查被跳过，离线回退得到实机验证。

### SDIO 时钟对比

进一步将 `CONFIG_ESP_HOSTED_SDIO_CLOCK_FREQ_KHZ` 从 40000 降到 20000，
作为单独的链路稳定性对比。降频可能降低峰值网络吞吐率；即使短期启动成功，
也不能据此证明存在硬件信号完整性问题。官方建议先用较低 SDIO 时钟验证链路：
[ESP-Hosted MCU 接入指南](https://github.com/espressif/esp-hosted-mcu/blob/main/docs/getting-started-mcu.md#1-sdio)。

20 MHz 版 ELF 为 `7853774d77ab105c8381e36e91ce94ddd4e8ec2c7343626668bec25555f634c8`。
记录在 `build/log/metalio_wifi_20mhz_validation_20261030.log`。前两次自动联网成功；
之后用户在启动过程中进入 Wi-Fi 设置，两次手动连接成功后由设置页正常调用
`Application::Reboot()`。再后面的启动仍出现 SDIO 建链失败，不能宣称降频
消除了 C5 故障。

## 第三轮：显示欠载与断网回退中的断言

### 显示数据不足会直接使屏幕变蓝

14:18:44、14:19:30 的串口日志出现：

```text
E lcd.dsi: can't fetch data from external memory fast enough, underrun happens
```

ESP-IDF 5.5.4 的 `components/esp_lcd/dsi/esp_lcd_panel_dpi.c` 中断处理明确说明
欠载发生时屏幕可能已经变蓝，该处理器仅打印错误，不会自动恢复显示。
[官方 LCD FAQ](https://docs.espressif.com/projects/esp-faq/en/latest/software-framework/peripherals/lcd.html)
也将该日志与 PSRAM 带宽不足造成的蓝屏关联，并建议降低像素时钟。

本板实际识别为 NV3051F。板级代码给它设置 48 MHz，但同项目
`esp_lcd_nv3051f.h` 的标准 720×720 时序宏为 36 MHz。
总时序为 812×752，48 MHz 对应约 78.6 Hz，36 MHz 对应约 59 Hz。
将 NV3051F 恢复到 36 MHz，使持续读帧带宽需求降低约 25%；保持三帧缓冲和
RGB888，避免引入 LVGL 色深、摄像头转换等额外变化。两个面板的 DPI 配置
结构都改为零初始化，确保没有显式填写的 `disable_lp` 位不会取栈上随机值。

### 本轮断网回退漏掉 lwIP 初始化

14:21:51 起的重启捕获到匹配 ELF 的调用栈：

```text
assert failed: tcpip_send_msg_wait_sem ... (Invalid mbox)
tcpip_api_call -> netconn_gethostbyname_addrtype -> lwip_getaddrinfo
-> esp_tls_hostname_to_fd -> esp_mqtt_task
```

原因是新增 C5 就绪检查先于 `esp_netif_init()`；建链失败时返回，跳过了网络栈
初始化，但后面的 MQTT 仍会启动 DNS 请求。这是第二轮新增回退路径中的遗漏。
将 `esp_netif_init()` 移到 `app_main()`、应用和板级启动之前，使 TCP/IP 基础设施
与 Wi-Fi 驱动启动独立。现有重复调用由 ESP-IDF 自身的初始化检查处理。

### 第三轮构建与实机验证

ESP-IDF 5.5.4 编译通过；主程序 9,055,424 字节，烧录后校验通过。
ELF SHA256：`55ac0fb29b4fc3015dee82fb026e271e6ca0a1c38827f38412a9178a80ea185e`。
14:25:38 启动，日志确认 NV3051F 为 36 MHz，14:25:54 获取 IP，14:25:55
连接 MQTT 并进入 idle。串口记录：`build/log/metalio_display_validation_20261030.log`。

截至 14:28:06，三次受控启动（14:25:38、14:27:01、14:27:40）均确认运行该 ELF，
成功获取 IP、连接 MQTT、进入 idle，未发现 DSI underrun、断言、RPC 超时或
非预期复位。首次运行还于 14:26:42 正常切换到待机页面。

这三次均正常联网，未在第三轮实机上强制制造 C5 故障，因此不能将它们当作
断网回退全流程的故障注入验证；lwIP 初始化修正依据上一版本匹配调用栈和
ESP-IDF 初始化契约。此前 WiFi 生命周期及 Hosted 等待/故障状态的宿主故障
注入测试均通过 ASan/UBSan，完整固件编译通过。

当前短期验证不能证明全部间歇蓝屏都已消除，尤其是页面高负载操作和软件
重启后的 C5 建链仍需持续观察。保留最终固件的后台串口记录以便复现追踪。
