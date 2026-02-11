import torch
from torch.utils.data import DataLoader

from src.policies.act.configuration_act import ACTConfig
from src.policies.act.modeling_act import ACT
from src.utils.constants import OBS_STATE, OBS_ENV_STATE, ACTION


import torch
import torch.nn.functional as F
from torch import nn
from tqdm import tqdm

def train_act(
    model: ACT,
    dataloader: torch.utils.data.DataLoader,
    num_epochs: int,
    lr: float = 3e-4,
    device: str = "cuda",
    use_vae: bool = False,
    kl_weight: float = 1.0,
    grad_clip_norm: float | None = 1.0,
):
    """
    训练 ACT 策略（最小可用版本）

    Args:
        model: 你写的 ACT(nn.Module)
        dataloader: 返回 batch dict 的 DataLoader
        num_epochs: 训练轮数
        lr: 学习率
        device: "cpu" or "cuda"
        use_vae: 是否启用 VAE KL loss
        kl_weight: KL loss 权重
        grad_clip_norm: 梯度裁剪（None 表示不用）
    """

    model = model.to(device)
    model.train()

    optimizer = torch.optim.AdamW(model.parameters(), lr=lr)

    for epoch in range(num_epochs):
        epoch_loss = 0.0

        pbar = tqdm(dataloader, desc=f"Epoch {epoch+1}/{num_epochs}")

        for batch in pbar:
            # ===== 把 batch 放到 device =====
            for k, v in batch.items():
                if torch.is_tensor(v):
                    batch[k] = v.to(device)

            # ===== Forward =====
            actions_pred, (mu, log_sigma_x2) = model(batch)

            # ===== 行为克隆 loss（核心）=====
            actions_gt = batch[ACTION]  # (B, S, A)
            loss = F.mse_loss(actions_pred, actions_gt)

            # ===== VAE KL loss（可选）=====
            if use_vae and mu is not None:
                kl_loss = -0.5 * torch.mean(
                    1 + log_sigma_x2 - mu.pow(2) - log_sigma_x2.exp()
                )
                loss = loss + kl_weight * kl_loss
            else:
                kl_loss = torch.tensor(0.0, device=device)

            # ===== Backward =====
            optimizer.zero_grad()
            loss.backward()

            if grad_clip_norm is not None:
                nn.utils.clip_grad_norm_(model.parameters(), grad_clip_norm)

            optimizer.step()

            # ===== logging =====
            epoch_loss += loss.item()
            pbar.set_postfix(
                loss=f"{loss.item():.4f}",
                kl=f"{kl_loss.item():.4f}",
            )

        avg_loss = epoch_loss / len(dataloader)
        print(f"[Epoch {epoch+1}] avg loss = {avg_loss:.6f}")

    print("✅ ACT training finished")


def train():
    config = ACTConfig(
        chunk_size=16,
        use_vae=False,
    )
    model = ACT(config)

    N = 10_000
    state_dim = 14
    action_dim = 7
    chunk_size = 16

    states = torch.randn(N, state_dim)
    actions = torch.randn(N, chunk_size, action_dim)

    action_is_pad = torch.zeros(N, chunk_size, dtype=torch.bool)

    train_dataset = ACTDataset(
        states=states,
        actions=actions,
        action_is_pad=action_is_pad,
    )

    train_loader = DataLoader(
        dataset=train_dataset,
        batch_size=64,
        shuffle=True,
        num_workers=4,
        pin_memory=True,
    )

    train_act(
        model=model,
        dataloader=train_loader,
        num_epochs=50,
        lr=3e-4,
        device="cuda",
        use_vae=config.use_vae,
        kl_weight=1.0,
    )


def main():
    train()


if __name__ == "__main__":
    main()