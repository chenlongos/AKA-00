# demo/models/ — 板上本地模型库

模型放这里（部署后是 `$AKA_HOME/demo/models/`），检测接口就能**按名字**用它：

```sh
# ① 平台推模型走这条（推荐，raw body，二进制安全）
curl -s --data-binary @tennis.cvimodel \
     'http://<板子IP>/api/models/upload?name=tennis'

# ② 训练平台/浏览器直传（multipart）也行
curl -F "file=@model.cvimodel" -F "name=orange" 'http://<板子IP>/api/model/upload'

# 用库里的模型检测一帧
curl -s 'http://<板子IP>/api/detect?model=tennis'
```

## 一张 demo 卡片 = 动作 × 模型

模型只是"找什么"；"怎么动"是**动作脚本**（`demo/grab.lua` 追到就夹、
`demo/approach.lua` 只接近不夹），两者都跟对方无关：

| | 在哪 | 谁提供 |
|---|---|---|
| 模型 | `demo/models/<名字>.cvimodel` | 平台推 / scp（本文件所在目录） |
| 动作 | `demo/<动作>.lua` | 仓库自带；再加一份就是加一个新动作 |
| 卡片（动作+模型+参数） | `demo/configs/<卡片名>.json` | **用户在 Demo 页新建**（动作 × 模型 + 参数） |

所以**传上来一个新模型不需要额外配脚本** —— 在 Demo 页新建一张卡（选动作、选这个模型），
或者直接 `POST /api/demo/init {"action":"grab","model":"<新模型>"}` 跑一下。

卡片是现场数据：OTA 升级时**同名保留板上的**（你在界面上调好的参数不会被覆盖）；
动作脚本则跟着包走 —— 想按模型调参就改卡片配置，别改动作脚本。

## 其它说明

- 名字规则：字母数字与 `_` `-` `.`，不能带 `/` 或 `..`（防目录穿越）。
- **裸名字只在库里查，没有隐式回退**：`?model=tennis` = `demo/models/tennis.cvimodel`；
  库里没有就报错，错误里带绝对路径。
- 只认 `.cvimodel` 后缀 —— 同目录的 README、临时文件都不会被当成模型。
- 模型**跟着 git 走**（仓库里的这两颗是提交过的），`make package` 整目录照搬进部署目录。
  板上运行时推上来的模型不在仓库里，OTA 升级时按文件名取并集保留（见 `cpp/scripts/build-ota.sh`）。
