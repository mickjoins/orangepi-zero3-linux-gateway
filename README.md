# Orange Pi Zero 3 Linux 网关项目

一个可直接在 **Orange Pi Zero 3（Allwinner H618, Cortex-A53 aarch64）** 上运行的嵌入式 Linux C 项目。
它把一台小开发板变成一个“边缘计算网关”：采集系统信息与温湿度传感器数据、读取串口外设数据、控制 LED、
检测按键，并通过内置轻量 HTTP 服务器提供网页仪表盘与 JSON API。

---

## 1. 功能清单

| 模块 | 功能 | 说明 |
|---|---|---|
| 系统监控 | CPU 占用率、负载、内存、SoC 温度、CPU 频率、主机名、uptime | 全部读取 Linux `sysfs` / `procfs`，无需额外库 |
| 环境传感器 | AHT20/AHT21 温湿度采集 | 使用 Linux `i2c-dev` 接口 + ioctl 实现，无外部库依赖 |
| I2C 工具 | I2C 总线扫描 | `--scan-i2c` 命令可列出 0x03~0x77 上的设备 |
| GPIO | LED 输出（可配置 active-low） | 优先使用 `/dev/gpiochipN` GPIO character device（v1，自动尝试 v2），失败时回退 `/sys/class/gpio` |
| GPIO | 按键输入，按下翻转 LED | 独立监控线程，20 Hz 轮询并消抖；sysfs 回退时需外部或设备树上拉 |
| 串口 | 读取外部设备按行上报的数据 | termios 配置，保留跨读取片段直到换行；模拟模式下每 5 秒生成一条 JSON |
| Web 仪表盘 | 单页仪表盘，自动 2 秒刷新 | 内嵌 HTML/CSS/JS，无需外部网页文件 |
| REST API | `/api/status` `/api/sensor` `/api/serial` `/api/gpio` | JSON 格式，POST 控制 LED；远程访问需令牌 |
| 守护进程 | `--daemon` 后台运行、写入 syslog/日志文件 | 支持 systemd 托管 |
| 仿真模式 | `--simulate` 在 x86 Linux 主机上仿真全部功能 | 便于开发、演示与 CI 验证 |
| 交叉编译 | `make CROSS_COMPILE=aarch64-linux-gnu-` | 已在本仓库用 aarch64-linux-gnu-gcc 验证 |

---

## 2. 目录结构

```
orangepi-zero3-linux-gateway/
├── Makefile
├── README.md
├── config/
│   └── opiz3-gateway.conf          # 运行时配置文件
├── deploy/
│   ├── opiz3-gateway.service       # systemd 服务
│   └── install.sh                  # 目标板一键安装
├── docs/
│   ├── 功能文档.md                  # 详细功能说明、设计、API
│   └── 硬件连接.md                  # Orange Pi Zero 3 接线指导
└── src/
    ├── main.c                      # 入口、CLI、线程编排
    ├── config.c / config.h         # key=value 配置解析
    ├── log.c / log.h               # stderr/syslog/file 日志
    ├── sysinfo.c / sysinfo.h       # 系统信息采集
    ├── shared.c / shared.h         # 全局共享状态（互斥保护）
    ├── gpio_dev.c / gpio_dev.h     # GPIO 输出/输入（chardev + sysfs 回退）
    ├── i2c_sensor.c / i2c_sensor.h # I2C 扫描 + AHT20 读取
    ├── uart_reader.c / uart_reader.h # 串口按行读取
    ├── http_server.c / http_server.h # 轻量 HTTP 服务器/API
    └── version.h
```

---

## 3. 编译

### 3.1 在 x86 Ubuntu 主机上编译并仿真

```bash
cd orangepi-zero3-linux-gateway
make clean
make
make run-sim          # 前台仿真运行，端口 8080
make test             # 主机端串口组帧测试
make test-http        # 本机 HTTP 接口与退出集成测试
```

浏览器打开 `http://127.0.0.1:8080` 可查看仪表盘；或者：

```bash
curl http://127.0.0.1:8080/api/status
curl -X POST -H 'X-Requested-With: XMLHttpRequest' 'http://127.0.0.1:8080/api/gpio?state=toggle'
```

### 3.2 交叉编译（得到 aarch64 可执行文件）

```bash
make clean
make CROSS_COMPILE=aarch64-linux-gnu-
file build/opiz3-gateway
# 期望：ELF 64-bit LSB pie executable, ARM aarch64, ...
```

### 3.3 部署到 Orange Pi Zero 3

**方式 A：手动**

```bash
# 在主机上编译后
scp build/opiz3-gateway orangepi@<IP>:/tmp/
scp config/opiz3-gateway.conf orangepi@<IP>:/tmp/
ssh orangepi@<IP> 'sudo cp /tmp/opiz3-gateway /usr/bin && sudo mkdir -p /etc/orangepi && sudo cp /tmp/opiz3-gateway.conf /etc/orangepi/'
```

**方式 B：在板子上部署（或通过 NFS/拷贝整个项目到板子）**

```bash
make                     # 在板子上原生编译
sudo sh deploy/install.sh
```

安装脚本会保留已有配置。如果旧配置监听局域网地址但没有访问令牌，升级时会自动生成令牌并写入权限为 `0600` 的 `/etc/orangepi/opiz3-gateway.conf`；请在该文件中查看令牌，再用于网页或 API 请求。

### 3.4 运行

```bash
opiz3-gateway --help                 # 查看所有命令
opiz3-gateway --status               # 打印系统+传感器状态
opiz3-gateway --led on               # 点亮 LED
opiz3-gateway --scan-i2c             # 扫描 I2C 总线
opiz3-gateway --simulate             # 仿真模式
opiz3-gateway --daemon               # 后台运行（默认还是读 /etc? 不，读内建默认配置）
opiz3-gateway -c /etc/orangepi/opiz3-gateway.conf --daemon
```

安装 systemd 服务后：

```bash
sudo systemctl status opiz3-gateway
sudo systemctl restart opiz3-gateway
journalctl -u opiz3-gateway -f
```

> 注意：`--daemon` 本身不会自动读取 `/etc/orangepi/opiz3-gateway.conf`，需要显式 `--config`。
> systemd 服务文件中已经传入了 `--config`。

---

## 4. 配置说明

配置文件采用 `key = value` 格式，`#` 为注释，`[...]` 段落行可写可不写，解析器自动忽略。
完整默认值见 [config/opiz3-gateway.conf](config/opiz3-gateway.conf)。

| 键 | 默认值 | 说明 |
|---|---|---|
| `http_port` | `8080` | HTTP 监听端口 |
| `http_bind_ip` | `127.0.0.1` | HTTP 监听地址；本机使用时无需令牌 |
| `http_access_token` | 空 | 远程绑定地址时必填；API 请求使用 `Authorization: Bearer <token>`，配置文件权限应为 0600 |
| `gpio_chip` | `0` | GPIO 字符设备编号，`/dev/gpiochip0` |
| `led_line` | `69` | LED 引脚。Allwinner 全局编号常为 bank*32+偏移，如 PC5=69 |
| `button_line` | `70` | 按键引脚。设为 `-1` 禁用 |
| `led_active_low` | `1` | LED 是否低电平有效；与下文 3.3V → LED → GPIO 接法一致 |
| `button_active_low` | `1` | 按键是否低电平有效（按下时接地） |
| `i2c_bus` | `/dev/i2c-1` | I2C 控制器设备节点 |
| `i2c_sensor_type` | `aht20` | 传感器类型，当前支持 `aht20` |
| `uart_dev` | `/dev/ttyS1` | 串口设备节点 |
| `uart_baud` | `115200` | 串口波特率 |
| `collect_interval_s` | `2` | 系统/传感器采集周期（秒） |
| `simulate` | `0` | 是否启用仿真模式 |
| `log_file` | 空 | 日志文件；留空时前台用 stderr，后台用 syslog。示例配置文件中为 `/var/log/opiz3-gateway.log` |

---

## 5. HTTP API

开发时可浏览器直接打开 `/` 查看仪表盘。

默认只监听 `127.0.0.1`。GPIO 写请求还需 `X-Requested-With: XMLHttpRequest` 请求头，以阻止其他网页直接发起跨站写入。需要从局域网访问时，在配置中设置开发板的局域网 IP（或 `0.0.0.0`）和随机生成的 `http_access_token`，然后重启服务。网页会提示输入令牌，并在当前浏览器标签页中保存；命令行请求可加入 `Authorization: Bearer <token>` 请求头。令牌会通过普通 HTTP 传输，跨不可信网络时请使用 HTTPS 反向代理。

| 方法 | 路径 | 功能 | 成功响应示例 |
|---|---|---|---|
| GET | `/` | Web 仪表盘 | HTML |
| GET | `/api/status` | 系统、传感器、LED、串口等完整状态 | 见下 |
| GET | `/api/sensor` | 最近一次温湿度 | `{"valid":1,"temperature_c":28.50,"humidity_pct":63.20}` |
| GET | `/api/serial` | 最近一帧串口数据 | `{"line":"{...}"}` |
| GET | `/api/gpio` | LED/按键状态 | `{"led":0,"button_current_pressed":0,"button_pressed_count":3}` |
| POST | `/api/gpio?state=on` | 打开 LED | 同上 |
| POST | `/api/gpio?state=off` | 关闭 LED | 同上 |
| POST | `/api/gpio?state=toggle` | 翻转 LED | 同上 |

`/api/status` 示例：

```json
{
  "app": "opiz3-gateway",
  "version": "1.0.0",
  "hostname": "orangepizero3",
  "uptime_s": 3600,
  "cpu_count": 4,
  "cpu_usage_pct": 12.34,
  "loadavg": [0.10, 0.15, 0.12],
  "mem_total_kb": 1000000,
  "mem_avail_kb": 800000,
  "mem_usage_pct": 20.00,
  "soc_temp_c": 48.2,
  "cpu_freq_khz": 1512000,
  "sensor": {"valid":1,"temperature_c":28.50,"humidity_pct":63.20},
  "led": 0,
  "button_pressed_count": 3,
  "serial_line": "{\"device\":\"demo\"}"
}
```

---

## 6. Orange Pi Zero 3 硬件连接

> 不同 Armbian/Ubuntu 镜像的设备树可能不同。请以实际丝印和 `gpioinfo`、`i2cdetect -l`、
> `ls /dev/ttyS*` 为准。详细接线与管脚排查方法见 [docs/硬件连接.md](docs/硬件连接.md)。

最小接线示例（默认配置）：

| 外设 | 板子信号 | 说明 |
|---|---|---|
| LED | GPIO PC5（全局编号 69，长脚串 330Ω 电阻） | 3.3V → 电阻 → LED → GPIO |
| 按键 | GPIO PC6（全局编号 70，一端接 GND） | 字符设备模式请求内部上拉；sysfs 模式需外部或设备树上拉 |
| AHT20 | 3.3V、GND、I2C1 SDA、I2C1 SCL | 默认 `/dev/i2c-1`，地址 0x38 |
| USB-TTL | UART TX/RX、GND | 默认 `/dev/ttyS1`，115200 8N1 |

> GPIO 后端优先顺序为：字符设备 v1 → 字符设备 v2 → sysfs。若 `/dev/gpiochip0` 请求失败，
> 程序会自动尝试下一级回退。active-low 极性由内核处理（chardev v1/v2）；sysfs 回退路径由程序手动取反。sysfs 无法请求内部上拉，按键需外部或设备树上拉。

---

## 7. 模拟模式与开发

模拟模式完全脱离硬件：
- GPIO 为内存假数据，LED/按键 API 正常返回；
- 传感器数据由正弦函数生成；
- 串口数据每 5 秒生成一条模拟 JSON；
- Web/API 功能与真实模式完全一致。

因此本仓库可以作为 **源码级 CI 验证** 的基线。主机编译零外部库依赖，只需 glibc。

---

## 8. 已知边界与说明

- GPIO 字符设备按 v1 → v2 顺序自动探测（`gpio_dev.c`），并保留 `/sys/class/gpio` 回退。
  极端精简内核如同时关闭 v1/v2 与 sysfs GPIO，则需要 libgpiod 或内核配置调整。
- 当前 HTTP 服务器为单线程 accept + 短连接，设计目标是轻量和零依赖，不是高并发
  静态文件服务器。生产环境若要处理高并发，建议用 lighttpd/nginx 反代，或扩展线程池。
- HTTP 服务按 `http_bind_ip` 绑定；非本机地址必须设置 `http_access_token`，API 才会启动。
- 当前只内置 AHT20 驱动，项目结构预留了扩展其他 I2C 传感器在 `i2c_sensor.c` 中的位置。

---

## 9. 代码风格与线程模型

- C11 + POSIX，模块化 `.c/.h`，仅链接 `pthread` 与 `m`。
- 所有共享数据集中在 `shared.c`，用 `pthread_mutex_t` 保护。
- 4 个线程：
  1. collector：周期性采集系统信息与传感器；
  2. serial：阻塞读取串口按行数据；
  3. button monitor：20 Hz 轮询 GPIO 按键边沿，短回调翻转 LED；
  4. http server：accept + 短连接处理。
- 退出模型：SIGINT/SIGTERM → `shared_set_running(0)` → 各线程退出 → 主线程 join。SIGHUP 被忽略。

---

## 10. 常见问题排查

| 现象 | 排查 |
|---|---|
| HTTP 端点 `led=-1` | GPIO 后端初始化失败。执行 `gpioinfo`/`ls /dev/gpiochip*`，确认 `led_line`/`gpio_chip` 正确 |
| 传感器 `valid=0` | `ls /dev/i2c-*`，确认 AHT20 地址 `i2cdetect -y 1` 是否出现 0x38；核对 `i2c_bus` 编号 |
| 串口没有数据 | `ls /dev/ttyS*`，用 `picocom -b 115200 <dev>` 验证；确认设备树 overlay 已打开 UART |
| 编译报 `i2c-dev.h` 缺失 | 安装内核头文件：`sudo apt install linux-libc-dev` |
| 找不到 `aarch64-linux-gnu-gcc` | `sudo apt install gcc-aarch64-linux-gnu` |
| 服务启动失败 | `journalctl -u opiz3-gateway -n 50` 查看；手动前台运行 `opiz3-gateway -c /etc/orangepi/opiz3-gateway.conf` |

---

## 附：快速命令

```bash
# 主机仿真
make && ./build/opiz3-gateway --simulate -p 8080

# 板子交叉编译后
scp build/opiz3-gateway config/opiz3-gateway.conf deploy/opiz3-gateway.service root@<IP>:/tmp/
ssh root@<IP> 'mv /tmp/opiz3-gateway /usr/bin/opiz3-gateway && mkdir -p /etc/orangepi && mv /tmp/opiz3-gateway.conf /etc/orangepi/ && mv /tmp/opiz3-gateway.service /etc/systemd/system/ && systemctl daemon-reload && systemctl enable --now opiz3-gateway'
```
