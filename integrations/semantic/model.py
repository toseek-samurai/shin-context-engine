"""The one already measured local E5 runtime, with no provisioning path."""

import importlib.metadata
import os
from threading import Lock

from .artifact import decode, read, require, sha

MODEL = {
    "name": "intfloat/multilingual-e5-small", "revision": "614241f622f53c4eeff9890bdc4f31cfecc418b3",
    "weights_sha256": "ca456c06b3a9505ddfd9131408916dd79290368331e7d76bb621f1cba6bc8665",
    "manifest_sha256": "31065275f7f7fdfbb630f1f876d514ea352942c21177465a97d664ad65cca8e4",
    "dimensions": 384, "max_tokens": 512, "query_prefix": "query: ", "passage_prefix": "passage: ",
    "pooling": "mean-normalized", "threads": 4,
    "runtime": {"fastembed": "0.8.1", "onnxruntime": "1.30.0", "tokenizers": "0.23.2", "numpy": "2.4.6"},
}
FILES = {"model.onnx", "tokenizer.json", "tokenizer_config.json", "config.json", "special_tokens_map.json"}
_registration_lock = Lock()
_registered_embedding = None


def _embedding_type():
    global _registered_embedding
    # FastEmbed's custom registry is process-wide, while each Session/Producer
    # owns its own model instance. Do not adopt an unrelated preexisting entry.
    with _registration_lock:
        if _registered_embedding is None:
            from fastembed import TextEmbedding
            from fastembed.common.model_description import ModelSource, PoolingType
            try:
                TextEmbedding.add_custom_model(MODEL["name"], pooling=PoolingType.MEAN, normalization=True,
                    sources=ModelSource(hf=MODEL["name"]), dim=MODEL["dimensions"], model_file="model.onnx", license="MIT")
            except ValueError:
                require(False, "model-registration-conflict")
            _registered_embedding = TextEmbedding
        return _registered_embedding


def available():
    try:
        for name in MODEL["runtime"]:
            importlib.metadata.version(name)
    except importlib.metadata.PackageNotFoundError:
        return False
    return True


def load(directory, descriptor):
    require(descriptor == MODEL, "unsupported-semantic-model")
    require({name: importlib.metadata.version(name) for name in MODEL["runtime"]} == MODEL["runtime"], "model-runtime-mismatch")
    data = read(directory / "manifest.json", 65536)
    require(sha(data) == MODEL["manifest_sha256"], "model-manifest-changed")
    manifest = decode(data)
    require(manifest["model"] == MODEL["name"] and manifest["revision"] == MODEL["revision"] and set(manifest["files"]) == FILES)
    require(sum(item["bytes"] for item in manifest["files"].values()) <= 600 * 1024 * 1024, "model-byte-budget")
    for name, item in manifest["files"].items():
        content = read(directory / name, item["bytes"])
        require(len(content) == item["bytes"] and sha(content) == item["sha256"], "model-file-changed")
    require(manifest["files"]["model.onnx"]["sha256"] == MODEL["weights_sha256"])
    os.environ["HF_HUB_OFFLINE"] = "1"
    os.environ["HF_HUB_DISABLE_TELEMETRY"] = "1"
    os.environ["TOKENIZERS_PARALLELISM"] = "false"
    return _embedding_type()(model_name=MODEL["name"], specific_model_path=str(directory), cache_dir=str(directory),
        local_files_only=True, threads=MODEL["threads"], providers=["CPUExecutionProvider"], cuda=False)
