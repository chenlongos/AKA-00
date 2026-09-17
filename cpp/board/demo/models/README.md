# demo/models/ — 板上本地模型库

模型放这里（部署后是 `$AKA_HOME/demo/models/`），检测接口就能**按名字**用它。
**demo 名 = 模型名**：这里有几个 `.cvimodel`，Demo 页就有几张卡片。

```sh
# ① 平台推模型走这条（推荐，raw body，二进制安全）
curl -s --data-binary @tennis.cvimodel \
     'http://<板子IP>/api/models/upload?name=tennis'

# ② 直接 scp 也行（不用重启 capp，接口每次现查目录）
scp tennis.cvimodel root@<板子IP>:/root/AKA-00/demo/models/

# 用库里的模型检测一帧
curl -s 'http://<板子IP>/api/detect?model=tennis'
```

## 一个 demo = 三样同名东西

| 东西 | 位置 | 谁提供 |
|---|---|---|
| 模型 | `demo/models/<名字>.cvimodel` | 平台推 / scp |
| 流程脚本 | `demo/<名字>.lua` | 手写；`tennis.lua`、`block.lua` 是现成例子 |
| 运行参数（可选） | `demo/configs/<名字>.json` | 界面 Demo 页保存，或抄回仓库（**仓库是唯一真源**） |

三样都按同一个名字对齐。只有模型、没有同名脚本时，卡片仍然会列出来，
但点"开始"会明确报错（`这个 demo 还没有流程脚本：demo/<名字>.lua`）。

## 其它说明

- 名字规则：字母数字与 `_` `-` `.`，不能带 `/` 或 `..`（防目录穿越）。
- **裸名字只在库里查，没有隐式回退**：`?model=tennis` = `demo/models/tennis.cvimodel`；
  库里没有就报错，错误里带绝对路径。
- 只认 `.cvimodel` 后缀 —— 同目录的 README、临时文件都不会被当成 demo。
- 模型**跟着 git 走**（仓库里的这两颗是提交过的），`make package` 整目录照搬进部署目录。
  板上运行时推上来的模型不在仓库里，OTA 升级时按文件名取并集保留（见 `cpp/scripts/build-ota.sh`）。
