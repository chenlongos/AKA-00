# 第二章 使用sg2002
SG2002 是一款面向 AIoT 领域的高性能、低功耗 SoC，内置多个处理器核心，集成 TPU、视频编解码器、丰富外设接口，适用于智能视觉、边缘计算等场景。

## 2.1 硬件架构

- 处理器

  主处理器： RISCV C906 @ 1.0Ghz 和 ARM Cortex-A53 @ 1.0Ghz
  协处理器： RISCV C906 @700Mhz

- TPU

  算力为 1TOPS（INT8），适用于AI推理计算

- 视频子系统

  视频输出：支持 2L MIPI DSI 输出（分辨率 2880×1620@30fps），兼容 LVDS、BT.601/656/1120 等传统接口。  
  视频输入：支持 ISP（图像信号处理器），最高 5MP@30fps；支持 4L 或 2L+2L MIPI CSI 接口，兼容 DVP、Sub-LVDS、HisPI 等。  
  视频编解码：解码：H.264，支持 5MP@30fps。  
  编码：H.264/H.265，支持 5MP@30fps。  

## 2.2 连接方式

- 串口连接

   ```
   sudo apt install minicom     # 安装minicom
   minicom -D /dev/ttyUSB0 -b 115200    # 连接串口，用户名：root,密码：root
   ```

- usb rndis 网口连接

  ```
  ip a show     # 查看网口信息.如果主机是10.245.118.100，则开发板是10.245.118.1。
  ssh root@10.245.118.1    # 连接开发板，密码：root
  ```

- wifi 连接

  ```
  # 假设分配的地址为192.168.1.2
  ssh root@192.168.1.2    # 连接开发板，密码：root
  ```

## 2.3 配置uart串口

- 验证方法

  ```
  # 开发板上执行（利用Python的pyserial库）
  python3 -m serial.tools.miniterm /dev/ttyS0 115200    # 一般 UARTx 对应 /dev/ttySx
  
  # 主机上执行（利用minicom）
  minicom -D /dev/ttyUSB0 -b 115200    # 连接串口
  ```

- uart0 默认开启，无需配置

- uart1 默认开启，无需配置。但如果要同时使用uart1和uart2, 则需要进行配置。

  ```
  devmem 0x03001070 32 0x2 # GPIOA 28 UART2 TX
  devmem 0x03001074 32 0x2 # GPIOA 29 UART2 RX
  devmem 0x03001068 32 0x6 # GPIOA 18 UART1 RX
  devmem 0x03001064 32 0x6 # GPIOA 19 UART1 TX
  ```

- uart3 引脚默认复用为SDIO。而SDIO被用于wifi连接。所以在有wifi连接的情况下，不能使用uart3。

  ```
  devmem 0x030010D0 32 0x5 # GPIOP 18 UART3 CTS
  devmem 0x030010D4 32 0x5 # GPIOP 19 UART3 TX
  devmem 0x030010D8 32 0x5 # GPIOP 20 UART3 RX
  devmem 0x030010DC 32 0x5 # GPIOP 21 UART3 RTS
  ```