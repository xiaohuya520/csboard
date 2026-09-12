# csboard —— CS2 赛事看板固件

FoloToy AI Passport（ESP32-C3 / 8MB Flash / 240×320 ST7789P3）上的 CS2 赛事看板。
开机直接进看板，显示**实时比分 / 历史战绩 / 赛事预告**，支持三键操作与手机配网。

## 为什么重写

旧工程（`niko` 仓库 firmware 分支）有两个反复出现的问题：

1. **网络拉不到赛事**：数据源写死一条 jsDelivr 链接；上电 RTC 是 1970 年，
   mbedTLS 校验证书有效期必然失败（报 `ESP_ERR_HTTP_CONNECT`，看着像"网络不通"）；
   DHCP 下发的 DNS 不通就没有退路。
2. **双击返回会弹回原页**：返回后靠"冷却时间"硬扛驱动多报的 CLICK，长按时长/补报
   延迟不可控，冷却是创可贴。

本工程从架构上换掉这两处：

| 问题 | 旧做法 | 现在 |
| --- | --- | --- |
| 拉不到数据 | 单链 + 对 DNS/证书/连接不做区分 | **多镜像回退**（6 条线路）+ **HTTPS 前置校时** + **DNS 静态兜底** + **故障分类**（DNS/连接/证书/HTTP/空数据/解析）+ 指数退避 |
| 完全出不了网 | 无解 | **手机配网门户**：手机连板子热点，用**手机的流量**抓取 JSON 后投递给板子，彻底绕开板子的网络限制；也可手动粘贴 JSON |
| 双击弹回 | 600ms 冷却 | 按键**只改状态**、渲染由状态驱动；单击动作是**幂等**的（只触发刷新），双击/长按才换页 —— 多报一次 CLICK 也不再可能改变页面 |
| 中文乱码 | 队标/字体外置到 `csres` 分区，靠脚本搬运，正则曾静默丢字节 | 队标与字体**编译期编入 app**，由 C 编译器保证字节精确，无运行时搬运 |

## 按键

| 位置 | 上/下 | 确定单击 | 确定双击 | 确定长按 |
| --- | --- | --- | --- | --- |
| 看板（实时/战绩/预告） | 换一场比赛 | 立即刷新数据 | 换页（实时→战绩→预告） | 进入网络页 |
| 网络页 | 选择菜单项 | 执行 | 返回看板 | 返回看板 |
| 选网页 | 选择网络 | 连接（需密码则进密码页） | 返回 | 返回 |
| 密码页 | 切换字符 | 输入 / 退格 / 完成 | 退格 | 直接连接 |

> 复杂密码（含大写）建议用**手机门户**输入 —— 三键字符环只覆盖小写+数字+符号。

## 网络页的四种更新途径（按推荐顺序）

1. **自动**：连上 Wi-Fi 后每 5 分钟拉一次（失败则 30s 起指数退避，最多 5 分钟）。
2. **手机门户（推荐给"就是连不上"的网络）**：网络页选「打开配网门户」→ 手机连热点
   `CSBoard-XXXX`（密码 `12345678`）→ 浏览器打开 `http://192.168.4.1` → 点「抓取并写入」。
   这一步是**手机去下载**，板子只负责收，不依赖板子能否出网。
3. **手动投递**：同一个网页可整段粘贴 `cs_matches.json` 内容写入 Flash。
4. **换数据源**：网页里填任意 `https://.../xxx.json`，板子会优先用它。

## 结构

```
main/
  main.c           启动:板级初始化 -> 载缓存 -> 按键任务 -> 看板 -> 联网
  cs_app.c         界面与按键状态机(状态驱动渲染)
  cs_net.c         Wi-Fi + 多镜像抓取 + 诊断 + 退避 + 校时
  cs_data.c        数据模型 / JSON 解析 / 分区缓存(离线优先)
  cs_portal.c      softAP + 内嵌设置网页(手机代理抓取)
  cs_sys.c         NVS / netif / 事件循环
  cs_logos.c       队标查表(105 支)
  cs_logo_data.c   队标像素(生成物,勿手改)
  cs_font_cn16.c   中文 16px 字体(lv_font_conv 生成,勿手改)
components/bsp/    板级驱动(显示 / 按键 / I2C)
tools/
  update_matches.py   Sofascore -> cs_matches.json
  gen_logo_data.py    prebuilt/csres.bin -> main/cs_logo_data.c
  verify_firmware.py  合并镜像与受保护分区校验
  validate.sh         构建 + 合并 + 校验
```

## 出包

打标签即可（CI 编译并挂 Release）：

```bash
git tag cs-firmware-v1.0 && git push origin cs-firmware-v1.0
```

产物 `csboard-full.bin` 是**合并镜像**，烧录偏移 `0x0`。

## 本地构建

```bash
. $IDF_PATH/export.sh        # ESP-IDF 5.5.3
./tools/validate.sh --all
```

数据可单独更新：

```bash
python3 tools/update_matches.py --out cs_matches.json
```

## 分区与约束

```
nvs     0x9000  24KB
factory 0x10000 3MB     app 上限;verify_firmware.py 硬校验
cardid  0x356000 16KB   受保护,勿动
csdata  0x35A000 64KB   赛事 JSON 缓存
```

`cardid` 与 `factory` 的偏移/大小是产品约束，`tools/verify_firmware.py` 会强制校验。
