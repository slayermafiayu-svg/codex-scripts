"""Runs the official HY-Motion pipeline code through our backend with a tiny,
RANDOMLY INITIALISED network on the CPU. It checks the plumbing only (config
and checkpoint loading, the safety checks, guard/progress hooks, batch
handling, output format, retargeting of the real decoder's output); the
motion itself is meaningless and is never shown anywhere.

Needs torch and a HY-Motion checkout with its Git LFS files:
    AIMOTION_TEST_HYMOTION_REPO=/path/to/HY-Motion-1.0 pytest tests/test_hymotion_plumbing.py
"""

import os
from pathlib import Path

import numpy as np
import pytest

torch = pytest.importorskip("torch")
REPO = os.environ.get("AIMOTION_TEST_HYMOTION_REPO")
pytestmark = pytest.mark.skipif(not REPO, reason="AIMOTION_TEST_HYMOTION_REPO ayarlı değil")

from aimotion import paths  # noqa: E402
from aimotion.backends.hymotion import HYMotionBackend  # noqa: E402
from aimotion.guard import PlaybackGuard, VegasState  # noqa: E402
from aimotion.profiles import PROFILES  # noqa: E402
from aimotion.retarget import RetargetOptions, retarget  # noqa: E402

from fixtures import REST, rig_from_pose  # noqa: E402

TINY_CONFIG = """
train_pipeline: hymotion/pipeline/motion_diffusion.MotionFlowMatching
train_pipeline_args:
  text_encoder_module: hymotion/network/text_encoders/text_encoder.HYTextModel
  text_encoder_cfg: {llm_type: qwen3, max_length_llm: 64}
  noise_scheduler_cfg: {method: euler}
  infer_noise_scheduler_cfg: {validation_steps: 4}
  test_cfg: {text_guidance_scale: 5.0, mean_std_dir: STATS}
  train_frames: 360
  output_mesh_fps: 30
network_module: hymotion/network/hymotion_mmdit.HunyuanMotionMMDiT
network_module_args: {input_dim: 201, feat_dim: 64, ctxt_input_dim: 4096, vtxt_input_dim: 768, num_layers: 3,
                      num_heads: 4, mlp_ratio: 2.0, dropout: 0.0, mask_mode: narrowband}
"""


@pytest.fixture
def setup(tmp_path, monkeypatch):
    lay = paths.resolve_layout(tmp_path / "AIMotion", check_drive=False)
    paths.apply_environment(lay)
    lay.hymotion_repo.parent.mkdir(parents=True, exist_ok=True)
    os.symlink(REPO, lay.hymotion_repo)
    ckpt = lay.hymotion_ckpts / "HY-Motion-1.0"
    ckpt.mkdir(parents=True)
    (ckpt / "config.yml").write_text(TINY_CONFIG.replace("STATS", "stats"))
    for d in (lay.qwen3_8b, lay.clip_l):
        d.mkdir(parents=True)
        (d / "config.json").write_text("{}")
    monkeypatch.chdir(tmp_path)
    calls = []
    guard = PlaybackGuard(lay.vegas_state_file, reader=lambda f, s: calls.append(1) or VegasState("idle"))
    reports = []
    backend = HYMotionBackend(lay, PROFILES["24gb"], guard, lambda s, f: reports.append((s, f)))
    backend._import()
    from hymotion.utils.loaders import load_object
    import yaml

    cfg = yaml.safe_load(TINY_CONFIG.replace("STATS", "stats"))
    random_net = load_object(cfg["train_pipeline"], cfg["train_pipeline_args"], network_module=cfg["network_module"],
                             network_module_args=cfg["network_module_args"])
    torch.save({"model_state_dict": random_net.state_dict()}, ckpt / "latest.ckpt")
    return lay, backend, calls, reports, ckpt


def test_dit_path_with_random_weights(setup):
    lay, backend, guard_calls, reports, _ = setup
    assert backend.missing() == []
    info = {}
    pipe = backend.load_dit("cpu", "fp32", info)
    assert info["dit_dtype"] == "float32" and info["dit_params"] > 0
    state = backend._instrument(pipe, steps=4, fp16=False)
    n = 2
    hsd = {"text_vec_raw": torch.randn(1, 1, 768).repeat(n, 1, 1),
           "text_ctxt_raw": torch.randn(1, 64, 4096).repeat(n, 1, 1),
           "text_ctxt_raw_length": torch.tensor([12]).repeat(n)}
    with torch.no_grad():
        out = pipe.generate("plumbing", [1, 2], 2.0, cfg_scale=5.0, hidden_state_dict=hsd, length=60)
    k3d = out["keypoints3d"].numpy()
    assert k3d.shape == (2, 60, 52, 3) and np.isfinite(k3d).all()
    assert state["calls"] == 4 and len(guard_calls) == 4  # guard checked before every DiT step
    assert reports[-1] == ("Hareket üretiliyor", 1.0)
    # the decoder's real output layout goes through our retargeting
    res = retarget(k3d[0, :, :22], rig_from_pose(REST, 0.0), RetargetOptions(view_yaw_deg=0.0))
    assert res.num_frames == 60 and all(np.isfinite(v).all() for v in res.tracks.values())


def test_incomplete_checkpoint_is_refused(setup):
    lay, backend, _, _, ckpt = setup
    sd = torch.load(ckpt / "latest.ckpt", weights_only=False)["model_state_dict"]
    sd = {k: v for k, v in sd.items() if not k.startswith("motion_transformer.final")}
    torch.save({"model_state_dict": sd}, ckpt / "latest.ckpt")
    with pytest.raises(RuntimeError, match="DiT parametresini"):
        backend.load_dit("cpu", "fp32", {})


def test_missing_statistics_are_refused(setup):
    lay, backend, _, _, ckpt = setup
    (ckpt / "config.yml").write_text(TINY_CONFIG.replace("STATS", "no_such_stats_dir"))
    with pytest.raises(RuntimeError, match="istatistik"):
        backend.load_dit("cpu", "fp32", {})


def _tiny_text_encoders(lay):
    """Random tiny Qwen3 + CLIP text models with toy tokenizers, saved where the layout expects the real ones."""
    import shutil

    from tokenizers import Tokenizer, models, pre_tokenizers
    from transformers import (CLIPTextConfig, CLIPTextModel, CLIPTokenizerFast, PreTrainedTokenizerFast, Qwen3Config,
                              Qwen3ForCausalLM)

    words = ["<pad>", "<unk>", "<|im_start|>", "<|im_end|>", "<BOC>", "<|startoftext|>", "<|endoftext|>",
             "system", "user", "a", "person", "walks", "forward", "summarize", "human", "motion"]
    vocab = {w: i for i, w in enumerate(words)}
    tok = Tokenizer(models.WordLevel(vocab, unk_token="<unk>"))
    tok.pre_tokenizer = pre_tokenizers.Whitespace()
    for d in (lay.qwen3_8b, lay.clip_l):
        shutil.rmtree(d, ignore_errors=True)
    qt = PreTrainedTokenizerFast(tokenizer_object=tok, unk_token="<unk>", pad_token="<pad>")
    qt.chat_template = ("{% for m in messages %}<|im_start|> {{ m['role'] }} {{ m['content'] }} <|im_end|> "
                        "{% endfor %}")
    qt.save_pretrained(lay.qwen3_8b)
    Qwen3ForCausalLM(Qwen3Config(vocab_size=len(words), hidden_size=32, intermediate_size=64, num_hidden_layers=2,
                                 num_attention_heads=4, num_key_value_heads=2, head_dim=8)).save_pretrained(lay.qwen3_8b)
    ct = PreTrainedTokenizerFast(tokenizer_object=tok, unk_token="<unk>", pad_token="<pad>",
                                 bos_token="<|startoftext|>", eos_token="<|endoftext|>")
    ct.save_pretrained(lay.clip_l)
    CLIPTextModel(CLIPTextConfig(vocab_size=len(words), hidden_size=16, intermediate_size=32, num_hidden_layers=1,
                                 num_attention_heads=2, max_position_embeddings=77)).save_pretrained(lay.clip_l)


def test_text_encoder_path_placement_and_cache(setup, monkeypatch):
    lay, backend, guard_calls, reports, _ = setup
    pytest.importorskip("tokenizers")
    _tiny_text_encoders(lay)
    # HY-Motion's HYTextModel loads CLIP with CLIPTokenizer (slow, needs vocab/merges); use the fast one here.
    from hymotion.network.text_encoders import text_encoder as te
    from transformers import PreTrainedTokenizerFast

    monkeypatch.setitem(te.SENTENCE_EMB_LAYOUT["clipl"], "tokenizer_class", PreTrainedTokenizerFast)
    from aimotion.backends import hymotion as hm

    monkeypatch.setattr(hm, "_available_ram_gib", lambda: 64.0)
    timings, peaks, info = {}, {}, {}
    gpu = {"available": False}
    out = backend.encode_prompt("a person walks forward", gpu, timings, peaks, info)
    assert info == {"text_placement": "cpu", "text_cache": "miss"}
    assert out["text_vec_raw"].shape == (1, 1, 16) and out["text_ctxt_raw"].shape == (1, 64, 32)
    assert out["text_ctxt_raw"].dtype == torch.float32 and int(out["text_ctxt_raw_length"][0]) > 0
    # layouts restored for the next run, tokenizer override aside
    assert te.LLM_ENCODER_LAYOUT["qwen3"]["text_encoder_class"].__name__ == "AutoModelForCausalLM"
    # second call: from the D: cache, no model load, no guard wait
    n_guard = len(guard_calls)
    info2 = {}
    out2 = backend.encode_prompt("a person walks forward", gpu, {}, {}, info2)
    assert info2 == {"text_cache": "hit"} and len(guard_calls) == n_guard
    assert torch.equal(out2["text_ctxt_raw"], out["text_ctxt_raw"])
    assert len(list(lay.text_embeddings.glob("*.pt"))) == 1


def test_split_placement_kwargs(setup, monkeypatch):
    lay, backend, *_ = setup
    from aimotion.backends import hymotion as hm

    monkeypatch.setattr(hm, "_available_ram_gib", lambda: 30.0)
    backend.profile = PROFILES["8gb"]
    placement, kw, clip_dev = backend._text_placement({"available": True, "bf16": True})
    assert placement == "split" and kw["device_map"] == "auto"
    assert kw["max_memory"] == {0: "5.0GiB", "cpu": "24GiB"} and kw["offload_folder"] == str(lay.offload)
    # Turing (no bf16): CPU; little RAM: CPU + D: disk offload
    assert backend._text_placement({"available": True, "bf16": False})[0] == "cpu"
    monkeypatch.setattr(hm, "_available_ram_gib", lambda: 12.0)
    assert backend._text_placement({"available": True, "bf16": False})[0] == "cpu+disk"
