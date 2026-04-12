# 贡献指南

感谢您对 AKA-00 项目的兴趣！我们欢迎任何形式的贡献。

## 🙌 我们需要什么

### 软件开发（高优先级）

- **视觉识别**：YOLOv8 网球检测、图像分割、模型优化
- **运动控制**：机械臂轨迹规划、夹爪精细控制、底盘运动算法
- **Web 接口**：REST API 扩展、Web UI 优化、手机/语音控制
- **仿真训练**：IsaacLab / MuJoCo 仿真环境、Sim2Real 迁移
- **具身智能**：VLA/VLM 模型集成、任务规划、多模态推理

### 硬件改进

- 机械结构优化（低成本方案）
- 传感器扩展（更多摄像头、触觉传感器等）
- PCB 设计改进

### 文档与教程

- 完善现有文档（见 [docs/src](./docs/src/)）
- 编写组装教程、使用案例
- 视频演示和博客文章

### 社区建设

- 分享你的使用经验和项目案例
- 帮助新手入门
- 组织和参与社区活动

## 🔧 如何贡献

### 1. 准备工作

- 查看 [现有 Issues](https://github.com/AKA-00/AKA-00/issues)，避免重复工作
- 在 Issue 下留言，说明你的思路
- 小的 PR 更容易 review 和合并

### 2. 提交代码

```bash
# Fork 本仓库，克隆到本地
git clone https://github.com/YOUR_USERNAME/AKA-00.git
cd AKA-00

# 创建分支
git checkout -b feature/your-feature-name
# 或修复 bug
git checkout -b fix/bug-fix-name

# 开发完成后，推送到你的 Fork
git push origin feature/your-feature-name
```

### 3. 提交 Pull Request

- 使用清晰的标题，描述做了什么
- 关联相关 Issue（如 `Fixes #123`）
- 新功能请附上使用示例
- 更新相关文档

## 📐 规范

### 代码风格

- 遵循现有代码的命名和格式风格
- Python 代码请符合 PEP 8
- 为复杂逻辑添加注释
- 确保改动不影响现有功能

### 硬件改动

- 不进行大幅度硬件重新设计，确有需要请先在 Issue 中讨论
- 成本增加需控制在合理范围内
- 组装难度不能明显提高

### Commit 信息

推荐格式：

```
<type>: <subject>

<body>
```

type 可选：`feat` / `fix` / `docs` / `refactor` / `test`

示例：

```
feat: 添加网球识别置信度阈值配置

- 新增 ARM_CONFIDENCE_THRESHOLD 环境变量
- 默认值 0.5
- 解决低光照环境下误检问题
```

## 🐛 报告问题

报告 Bug 或请求新功能时，请：

1. 确认问题是否已存在
2. 使用清晰明确的标题
3. 提供详细信息：
   - 环境（操作系统、Python 版本）
   - 复现步骤
   - 预期行为 vs 实际行为
   - 相关日志或截图

## 📂 代码结构参考

```
AKA-00/
├── run.py              # Web 服务器入口
├── tennis_hunter.py    # 机器人主程序
├── app/                # Flask Web 应用
├── src/                # 硬件控制模块
│   ├── arm/            # 机械臂控制
│   ├── motor/          # 电机控制
│   └── camera/         # 摄像头 / 视觉
├── frontend/           # React 前端
└── models/             # YOLOv8 模型
```

详见 [文档](./docs/src/06-development/structure.md)

## 💬 获取帮助

- **QQ 群**：901307286
- **GitHub Issues**：Bug 和功能请求
- **GitHub Discussions**：问题和头脑风暴

## 📄 许可证

参与本项目即表示你同意你的贡献将遵循相同的开源许可证。

---

感谢每一个为 AKA-00 贡献的人！
