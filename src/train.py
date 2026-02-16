import os
import json
import time
from datetime import datetime
import torch
from torch.utils.data import DataLoader, Dataset

from src.policies.act.configuration_act import ACTConfig
from src.policies.act.modeling_act import ACT
from src.utils.constants import OBS_STATE, OBS_ENV_STATE, ACTION
from src.configs.types import PolicyFeature, FeatureType

import torch
import torch.nn.functional as F
from torch import nn
from tqdm import tqdm


class ACTDataset(Dataset):
    def __init__(
        self,
        states: torch.Tensor,
        env_states: torch.Tensor,
        actions: torch.Tensor,
        action_is_pad: torch.Tensor,
    ):
        self.states = states
        self.env_states = env_states
        self.actions = actions
        self.action_is_pad = action_is_pad

    def __len__(self):
        return self.states.shape[0]

    def __getitem__(self, idx):
        return {
            OBS_STATE: self.states[idx],
            OBS_ENV_STATE: self.env_states[idx],
            ACTION: self.actions[idx],
            "action_is_pad": self.action_is_pad[idx],
        }

def train_act(
    model: ACT,
    dataloader: torch.utils.data.DataLoader,
    num_epochs: int,
    lr: float = 3e-4,
    device: str = "cuda",
    use_vae: bool = False,
    kl_weight: float = 1.0,
    grad_clip_norm: float | None = 1.0,
    save_dir: str | None = None,
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

    global_step = 0
    samples_seen = 0
    epoch_losses: list[float] = []
    step_metrics: list[dict] = []
    for epoch in range(num_epochs):
        epoch_loss = 0.0

        pbar = tqdm(dataloader, desc=f"Epoch {epoch+1}/{num_epochs}")

        for batch in pbar:
            data_start = time.perf_counter()
            # ===== 把 batch 放到 device =====
            for k, v in batch.items():
                if torch.is_tensor(v):
                    batch[k] = v.to(device)
            data_s = time.perf_counter() - data_start

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
                grad_norm = nn.utils.clip_grad_norm_(model.parameters(), grad_clip_norm)
            else:
                grad_norm = torch.sqrt(
                    sum(torch.sum(p.grad.detach() ** 2) for p in model.parameters() if p.grad is not None)
                )

            update_start = time.perf_counter()
            optimizer.step()
            updt_s = time.perf_counter() - update_start

            # ===== logging =====
            epoch_loss += loss.item()
            global_step += 1
            batch_size = actions_gt.shape[0]
            samples_seen += batch_size
            lr_value = optimizer.param_groups[0]["lr"]
            step_metrics.append(
                {
                    "step": global_step,
                    "loss": float(loss.item()),
                    "lr": float(lr_value),
                    "epoch": epoch + 1,
                    "samples_seen": int(samples_seen),
                }
            )
            print(
                f"step={global_step} loss={loss.item():.6f} lr={lr_value:.6f} "
                f"ep={epoch+1} grdn={float(grad_norm):.6f} smpl={samples_seen} "
                f"updt_s={updt_s:.4f} data_s={data_s:.4f}"
            )
            pbar.set_postfix(
                loss=f"{loss.item():.4f}",
                kl=f"{kl_loss.item():.4f}",
            )

        avg_loss = epoch_loss / len(dataloader)
        epoch_losses.append(avg_loss)
        print(f"[Epoch {epoch+1}] avg loss = {avg_loss:.6f}")

    if len(epoch_losses) >= 2:
        trend = "down" if epoch_losses[-1] < epoch_losses[0] else "up"
        print(
            f"loss_trend={trend} first={epoch_losses[0]:.6f} last={epoch_losses[-1]:.6f}"
        )

    if save_dir is not None:
        os.makedirs(save_dir, exist_ok=True)
        save_path = os.path.join(save_dir, "act_checkpoint.pt")
        torch.save(
            {
                "model_state_dict": model.state_dict(),
            },
            save_path,
        )
        metrics_path = os.path.join(save_dir, "training_metrics.json")
        with open(metrics_path, "w", encoding="utf-8") as f:
            json.dump(step_metrics, f, ensure_ascii=False, indent=2)
        print(f"✅ ACT training finished, saved to {save_path}")
    else:
        print("✅ ACT training finished")


def run_inference(model: ACT, batch: dict, device: str):
    model.eval()
    with torch.no_grad():
        actions_pred, _ = model(batch)
    print(f"inference output shape: {tuple(actions_pred.shape)}")
    print(f"inference sample: {actions_pred[0, 0, :5].tolist()}")


def load_dataset_from_local(data_path: str):
    if data_path and os.path.exists(data_path):
        if os.path.isdir(data_path):
            try:
                from datasets import load_from_disk
            except ImportError as exc:
                raise RuntimeError("datasets is not installed") from exc
            dataset = load_from_disk(data_path)
            if isinstance(dataset, dict):
                dataset = dataset.get("train") or next(iter(dataset.values()))

            def get_column(name: str):
                if name in dataset.column_names:
                    return dataset[name]
                if "." in name:
                    root, *rest = name.split(".")
                    if root in dataset.column_names:
                        column = dataset[root]
                        out = []
                        for item in column:
                            cur = item
                            for key in rest:
                                cur = cur[key]
                            out.append(cur)
                        return out
                raise KeyError(f"missing column: {name}")

            states = torch.as_tensor(get_column(OBS_STATE), dtype=torch.float32)
            env_states = torch.as_tensor(get_column(OBS_ENV_STATE), dtype=torch.float32)
            actions = torch.as_tensor(get_column(ACTION), dtype=torch.float32)
            try:
                action_is_pad = torch.as_tensor(get_column("action_is_pad"), dtype=torch.bool)
            except KeyError:
                action_is_pad = torch.zeros(actions.shape[0], actions.shape[1], dtype=torch.bool)
            return ACTDataset(
                states=states,
                env_states=env_states,
                actions=actions,
                action_is_pad=action_is_pad,
            )
        payload = torch.load(data_path, map_location="cpu")
        return ACTDataset(
            states=payload[OBS_STATE],
            env_states=payload[OBS_ENV_STATE],
            actions=payload[ACTION],
            action_is_pad=payload["action_is_pad"],
        )
    return None


def load_model_from_checkpoint(config: ACTConfig, checkpoint_path: str, device: str):
    model = ACT(config).to(device)
    state = torch.load(checkpoint_path, map_location=device)
    model.load_state_dict(state["model_state_dict"])
    return model


def train():
    state_dim = 14
    env_state_dim = 6
    action_dim = 7
    chunk_size = 16
    device = "cuda" if torch.cuda.is_available() else "cpu"
    run_name = os.getenv("ACT_RUN_NAME") or f"act_{datetime.now().strftime('%Y%m%d_%H%M%S')}"
    output_dir = os.path.join("output", "train", run_name)
    data_path = os.getenv("ACT_DATA_PATH") or os.path.join("output", "train", "dataset.pt")

    config = ACTConfig(
        chunk_size=16,
        use_vae=False,
        input_features={
            OBS_STATE: PolicyFeature(type=FeatureType.STATE, shape=(state_dim,)),
            OBS_ENV_STATE: PolicyFeature(type=FeatureType.ENV, shape=(env_state_dim,)),
        },
        output_features={
            ACTION: PolicyFeature(type=FeatureType.ACTION, shape=(action_dim,)),
        },
    )
    model = ACT(config)

    train_dataset = load_dataset_from_local(data_path)
    if train_dataset is None:
        N = 512
        states = torch.randn(N, state_dim)
        env_states = torch.randn(N, env_state_dim)
        actions = torch.randn(N, chunk_size, action_dim)
        action_is_pad = torch.zeros(N, chunk_size, dtype=torch.bool)
        train_dataset = ACTDataset(
            states=states,
            env_states=env_states,
            actions=actions,
            action_is_pad=action_is_pad,
        )

    train_loader = DataLoader(
        dataset=train_dataset,
        batch_size=64,
        shuffle=True,
        num_workers=0,
        pin_memory=torch.cuda.is_available(),
    )

    train_act(
        model=model,
        dataloader=train_loader,
        num_epochs=2,
        lr=3e-4,
        device=device,
        use_vae=config.use_vae,
        kl_weight=1.0,
        save_dir=output_dir,
    )

    sample = next(iter(train_loader))
    for k, v in sample.items():
        if torch.is_tensor(v):
            sample[k] = v.to(device)
    run_inference(model, sample, device)
    checkpoint_path = os.path.join(output_dir, "act_checkpoint.pt")
    loaded_model = load_model_from_checkpoint(config, checkpoint_path, device)
    run_inference(loaded_model, sample, device)


def main():
    train()


if __name__ == "__main__":
    main()
