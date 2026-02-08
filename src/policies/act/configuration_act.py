from dataclasses import dataclass, field

from src.configs.types import PolicyFeature, FeatureType
from src.utils.constants import OBS_STATE, ACTION


@dataclass
class ACTConfig:
    # Transformer layers.
    pre_norm: bool = False
    dim_model: int = 512 # 模型维度

    # VAE.变分自编码器
    use_vae: bool = True
    latent_dim: int = 32

    # Input / output structure.
    n_obs_steps: int = 1
    chunk_size: int = 100
    n_action_steps: int = 100

    input_features: dict[str, PolicyFeature] = field(default_factory=dict)
    output_features: dict[str, PolicyFeature] = field(default_factory=dict)

    @property
    def robot_state_feature(self) -> PolicyFeature | None:
        for ft_name, ft in self.input_features.items():
            if ft.type is FeatureType.STATE and ft_name == OBS_STATE:
                return ft
        return None

    @property
    def action_feature(self) -> PolicyFeature | None:
        for ft_name, ft in self.output_features.items():
            if ft.type is FeatureType.ACTION and ft_name == ACTION:
                return ft
        return None