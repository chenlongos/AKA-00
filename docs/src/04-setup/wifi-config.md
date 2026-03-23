# 第一次连接

## 如何连接机器人
### 第一步，连接机器人自身的热点，用于配置机器人
1. 确保机器人已连接到电源，等待机器人控制板灯亮
2. 等待20秒到30秒，此时控制板正在启动网络模块
3. 开发者打开电脑/手机，进入wifi连接，找到控制板的热点并连接，例如`chenlong-robot-01`
<img src="https://oss.opencamp.cn/file/2026034/2026034_105027.jpg" 
     alt="GitHub图标" 
     width="300" 
     style="display:block; margin:0 auto;">

---
### 第二步，让机器人连接到开发者的WiFi网络
1. 连接热点之后，打开浏览器，输入`192.168.4.1`（不同的机器人可能不同，已实际为准），即可进入机器人的WiFi配置页面
2. 在配置页面中，刷新网络，找到开发者需要的WiFi网络，点击连接，如有密码需要输入密码
   <img src="https://oss.opencamp.cn/file/2026034/2026034_760774.png" 
     alt="GitHub图标" 
     width="300" 
     style="display:block; margin:0 auto;">
3. 连接成功后，网页会显示当前连接的WiFi网络名称以及为机器人分配的IP
<img src="https://oss.opencamp.cn/file/2026034/2026034_717789.jpg" 
     alt="GitHub图标" 
     width="300" 
     style="display:block; margin:0 auto;">

---
### 第三步，ssh登录机器人的控制板
1. 第二步进行完之后，开发者需要记下为机器人分配的IP,并且让自己的电脑和机器人连接到同一个WiFi网络
    <img src="https://oss.opencamp.cn/file/2026034/2026034_134104.png" 
     alt="GitHub图标" 
     width="500" 
     style="display:block; margin:0 auto;">
2. 开发者打开终端，输入`ssh root@[机器人分配的IP]`，即可登录机器人的控制板，密码为`root`
3. 登录成功后，即可在终端中操作机器人的控制板，输入`ping www.baidu.com`或者`curl www.baidu.com`，用来检测控制板网络是否成功连接并且能够访问互联网
<img src="https://oss.opencamp.cn/file/2026034/2026034_865318.png" 
     alt="GitHub图标" 
     width="500" 
     style="display:block; margin:0 auto;">