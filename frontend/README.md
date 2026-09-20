# 小车前端

## 1. 打包命令
可以前端静态打包，方便后端直接调用
```shell
npm run build            # 带屏版（设置页有"屏幕显示"开关）
npm run build:noscreen   # 不带屏版（没有屏的机器：那块界面整个不存在）
```
两个版本的差别由 `WITH_SCREEN` 编译期开关决定，与后端的 `AKA_WITH_SCREEN` 一一对应；
用 `if (__WITH_SCREEN__)` 包住的代码会被打包器直接删掉（声明见 `src/vite-env.d.ts`）。

**两种构建都产出到仓库根 `static/`**（同一目录，互相覆盖），所以顺序是：
先 build 对版本 → 再 `make -C cpp ota`（带屏）或 `ota-noscreen`（不带屏）。
拿错版本时打包脚本会报警。

## 2.调试命令
直接前端用dev方式进行调试
```shell
npm run dev
```

## 3. 界面介绍
/ 原始界面
/sim 小车模拟器界面