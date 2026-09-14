# models/ — 板上本地模型库

把 `.cvimodel` 丢进这个目录（部署后是 `$AKA_HOME/models/`），检测接口就能**按名字**用它：

```sh
# ① 直接 scp（不用重启 capp，接口是每次现查目录）
scp tennis.cvimodel root@<板子IP>:/root/AKA-00/models/

# ② 或者用接口上传（curl --data-binary，二分制安全）
curl -s --data-binary @tennis.cvimodel \
     'http://<板子IP>/api/models/install?name=tennis&names=tennis&conf=0.3'

# 看有哪些模型
curl -s http://<板子IP>/api/models

# 用库里的模型检测一帧
curl -s 'http://<板子IP>/api/detect?model=tennis'
```

## 模型自带元信息（可选）

给模型配一个同名 `.json`，检测时会**优先于** `config.toml` 的 `[detect]` 全局配置
（请求里的 `?conf=` 仍然最高）：

```json
{
  "names": "tennis",
  "conf": 0.25,
  "iou": 0.45,
  "has_objectness": false,
  "note": "网球 v3，2026-01 训练"
}
```

| 键 | 说明 |
|---|---|
| `names` | 类别名（逗号分隔）。接口里每个框带 `label`，调用方不用猜 `cls=0` 是什么 |
| `conf` / `iou` | 这个模型推荐的阈值（覆盖 `[detect]` 的全局默认） |
| `has_objectness` | 输出是 YOLOv5 风格（4+1+nc）时 `true`；YOLOv8 导出保持 `false` |

也可以用接口写：`POST /api/models/meta?name=tennis`，body 就是上面的 JSON。
删除：`POST /api/models/remove?name=tennis`（同时删掉 `.json`）。

## 打包怎么用它

`models/` 是**模型的唯一来源**：模型就放这里（入库、跟着 git 走），`make package`
把整个目录照搬进部署目录，跟 `demo/` 一样处理 —— 不做软链、不在别处再放一份。

```
models/tennis.cvimodel     ← 3.5MB
models/block.cvimodel      ← 3.6MB
demo/tennis/{init.sh, tennis}     ← demo 不带模型，init.sh 直接用 $APP_DIR/models/tennis.cvimodel
demo/block/{init.sh, tennis}      ← 同上，block.cvimodel
```

加新模型就两步：把 `.cvimodel` 拷进 `models/`、改 `config.toml` 的 `[detect] model`
（或者直接 `?model=<名字>` 指定）。打包时会校验 `model=` 指向的文件在不在，缺了就提示。

`config.toml` 的 `[detect] model` 默认就是库名 `tennis`（= `models/tennis.cvimodel`）；
打包最后会校验这个文件在部署目录里存不存在，缺了就打印提示，免得部署完才发现检测接口报错。
demo 的 `init.sh` 也优先用这个目录：自带 `./yolo_model.cvimodel`（前端上传/下载模型会放这里）
优先，否则回落到 `$APP_DIR/models/<demo 目录名>.cvimodel`。

## 其它说明

- 模型库是**板上本地**的：换模型、加模型都不需要联网，也不依赖 `demo_server`。
- **裸名字只在模型库里查，没有隐式回退**：`?model=tennis` = `models/tennis.cvimodel`；
  库里没有就报错（错误里会带库目录和库里现有的模型名）。
- 老路径仍然能用，但要写显式路径：`?model=demo/tennis/yolo_model.cvimodel`
  （这些老模型不在库里，`GET /api/models` 的 `legacy` 字段会列出来，供你决定要不要搬进库）。
- 名字规则：字母数字 `_` `-` `.`，不能带 `/` 或 `..`（防目录穿越）。
- 目录里的大文件不入 git（`.gitignore` 已忽略 `models/*.cvimodel`），
  这个 README 会跟着打包脚本一起进部署目录。
