# Model formats, quantization, architectures and conversion for Onyx Engine (as of Oct 2026)

Method note: the primary evidence comes from a sparse clone of upstream llama.cpp `master` (commit `9a3b91c6801e68f412fcf1b6f8e153aa562b8536`, committed 2026-10-21), which covers `conversion/`, `gguf-py/`, `docs/`, `common/arg.cpp` and `include/llama.h`. Onyx's own code was read directly: `README.md`, `docs/architecture.md`, `scripts/convert-safetensors.py`, `src/args.cpp`, `src/server.cpp` and `packaging/vendor/llama.cpp.conf`, whose pin is commit `9723942ad`, build b10711. Web searches added secondary context. Links to the upstream source point to `github.com/ggml-org/llama.cpp` paths on master.

## Q0. What Onyx's converter and loader actually do today (verified in the repo)

### Takeaway
Onyx's converter is much narrower than the README suggests. Besides being limited to Llama only, it refuses any `rope_scaling`, which excludes Llama 3.1, 3.2 and 3.3. It also refuses any pre-tokenizer other than plain `ByteLevel`, which excludes the real Llama 3 tokenizer. It writes no chat template and cannot quantize, so its only output is f16 or f32. In practice it converts almost no real-world model in use today.

### Cited Findings
- The architecture is accepted only when it is `LlamaForCausalLM` or `model_type` is `llama`. Supported safetensors dtypes are F32, F16, BF16 and integer types. FP8, MXFP4, NVFP4 and other packed or quantized checkpoints are refused. — [scripts/convert-safetensors.py](/home/user/Onyx-Engine/scripts/convert-safetensors.py), lines 39-40 and 90-137
- `--outtype` choices are only `f16` and `f32`, and there is no quantization at conversion time. `src/convert.cpp` always invokes the converter with `--outtype f16`, and the cache name hard-codes `-converter-v2-f16.gguf`. — [convert-safetensors.py](/home/user/Onyx-Engine/scripts/convert-safetensors.py) line 525; [docs/architecture/](../../docs/architecture/README.md) §"Safetensors resolution and conversion"
- `if config.get("rope_scaling"): raise ConvertError(...)` makes every Llama 3.1+ checkpoint fail, because they all ship `rope_scaling` with `rope_type: "llama3"`. The "Llama 3.1/3.2/3.3 config content" part of this point is my domain knowledge and is not re-verified in this session. — [convert-safetensors.py](/home/user/Onyx-Engine/scripts/convert-safetensors.py) line 422
- The pre-tokenizer must be `None` or exactly `{"type":"ByteLevel","add_prefix_space":false}`, and the converter then always writes `tokenizer.ggml.pre = "default"`. Llama 3's `tokenizer.json` uses a `Sequence[Split(regex), ByteLevel]` pre-tokenizer (upstream registers it as `llama-bpe`), so it is refused. — [convert-safetensors.py](/home/user/Onyx-Engine/scripts/convert-safetensors.py) lines 400-402 and 476; upstream [convert_hf_to_gguf_update.py](https://github.com/ggml-org/llama.cpp/blob/master/convert_hf_to_gguf_update.py) line 88 (`llama-bpe` → Meta-Llama-3-8B)
- No `tokenizer.chat_template` key is written; grepping for `chat_template` finds nothing. A converted model therefore falls back to llama.cpp's built-in template detection or chatml. — repo grep of [convert-safetensors.py](/home/user/Onyx-Engine/scripts/convert-safetensors.py)
- All tensors are memory-mapped through `np.memmap`, which is good, but they are collected into one dict before writing. The comments say the GGUF writer and metadata keys were cross-checked against gguf-py at b10711. — convert-safetensors.py lines 108-183 and 18-21
- The vendored llama.cpp tree already keeps `gguf-py` (`VENDOR_SPARSE` includes `gguf-py` and `VENDOR_REQUIRED="CMakeLists.txt gguf-py/gguf"`), but it prunes `conversion` and `models`. — [packaging/vendor/llama.cpp.conf](/home/user/Onyx-Engine/packaging/vendor/llama.cpp.conf)
- On the server side, `/v1/embeddings` returns 400 for a `dimensions` parameter, there is no `/v1/rerank` endpoint, and the CLI has no `--cache-type-k/-v`, `-hf`, `--override-tensor` or `--n-cpu-moe` flag. The flags it does have include `-c -b -ub -ngl -t -tb -np -fa --mlock --no-mmap --embedding --pooling --cache-reuse --context-shift --keep --reasoning-budget`. — [src/server.cpp](/home/user/Onyx-Engine/src/server.cpp) lines 1191-1195; [src/args.cpp](/home/user/Onyx-Engine/src/args.cpp) lines 55-100

### Inferences
- The converter's real-world coverage is roughly limited to Llama-2-era BPE models without rope scaling (TinyLlama-like) and the repo's test fixture (`scripts/make-tiny-hf-model.py`). Qwen, Gemma, Mistral, Llama 3.x, gpt-oss and the rest all fail.
- Writing `pre="default"` is safe only because the plain-ByteLevel gate is so strict. Relaxing the gate without adding pre-tokenizer identification would cause silent mis-tokenization.

### Gaps
- I did not run the converter against real HF checkpoints. The Llama 3.x rejection is inferred from the code plus known config contents.

## Q1. Popular open-weight families for local use in 2026, their HF architecture names, and which are MoE, hybrid or SWA

### Takeaway
Local use in 2026 centres on Qwen3.5 (a hybrid of Gated DeltaNet and attention, in dense and MoE variants), Gemma 4 (SWA, with a 26B-A4B MoE and audio on the E-models), gpt-oss (MoE with MXFP4), Mistral and Devstral, GLM, DeepSeek V3.2/V4, Kimi, Llama 4 and the small families (Phi-4, SmolLM3, LFM2, Granite 4). Most new flagship models are MoE, hybrid recurrent/attention, or SWA. Plain-Llama dense models are now a minority.

### Cited Findings
- Upstream llama.cpp's README quick start now uses `llama cli -hf ggml-org/Qwen3.5-0.8B-GGUF` and `llama serve -hf ...`, which suggests Qwen3.5 is the default showcase model and that the CLI was renamed to `llama cli` / `llama serve`. — [llama.cpp README](https://github.com/ggml-org/llama.cpp/blob/master/README.md)
- The architecture classes registered in upstream `conversion/*.py` (`@ModelBase.register(...)`) on 2026-10-21 include the following. This is a primary source. — [llama.cpp conversion/](https://github.com/ggml-org/llama.cpp/tree/master/conversion)
  - **Qwen:** `Qwen3ForCausalLM`, `Qwen3MoeForCausalLM`, `Qwen3NextForCausalLM`, `Qwen3_5ForConditionalGeneration`/`Qwen3_5ForCausalLM`/`Qwen3_5TextModel`, `Qwen3_5MoeForConditionalGeneration`/`Qwen3_5MoeForCausalLM`, `Qwen3VLForConditionalGeneration`, `Qwen3VLMoeForConditionalGeneration`, `Qwen3OmniMoeForConditionalGeneration`, `Qwen3ASRForConditionalGeneration`, `Qwen3TTSForConditionalGeneration`, `Qwen2_5_VLForConditionalGeneration`, `Qwen2_5OmniModel`, and the newer `Qwen4ExpForCausalLM`/`Qwen4ExpForConditionalGeneration`.
  - **Gemma:** `Gemma3ForCausalLM`/`Gemma3ForConditionalGeneration`, `Gemma3TextModel` (EmbeddingGemma), `Gemma3nForConditionalGeneration`, `Gemma4ForConditionalGeneration`/`Gemma4ForCausalLM`, `Gemma4UnifiedForConditionalGeneration`, `Gemma4AssistantForCausalLM` (a draft model), and `EmbeddingGemma2Model`.
  - **Meta and Mistral:** `Llama4ForConditionalGeneration`, `Mistral3Model`, `VoxtralForConditionalGeneration` and `VoxtralRealtimeForConditionalGeneration`.
  - **OpenAI, DeepSeek, GLM:** `GptOssForCausalLM`; `DeepseekV32ForCausalLM`, `DeepseekV4ForCausalLM` and DeepSeek-OCR/OCR2; `Glm4MoeForCausalLM` (GLM-4.5/4.6), `Glm4MoeLiteForCausalLM`, `GlmMoeDsaForCausalLM`, `Glm5vForConditionalGeneration` and `Glm5NextForConditionalGeneration`.
  - **Kimi, MiniMax, Phi:** `KimiK25ForConditionalGeneration`, `KimiK3ForConditionalGeneration` and `KimiLinearForCausalLM`; `MiniMaxM2ForCausalLM` and `MiniMaxM3SparseForCausalLM`; `Phi3ForCausalLM`, `Phi4ForCausalLMV` and `PhiMoEForCausalLM`.
  - **Small and hybrid families:** `SmolLM3ForCausalLM` and `SmolVLMForConditionalGeneration`; `GraniteMoeHybridForCausalLM`/`BambaForCausalLM` (Granite 4, a hybrid), `GraniteSWAForCausalLM` and `GraniteSpeechForConditionalGeneration`; `NemotronHForCausalLM`; `FalconH1ForCausalLM`; `Lfm2ForCausalLM`, `Lfm2MoeForCausalLM`, `Lfm2VlForConditionalGeneration` and `Lfm2AudioForConditionalGeneration`; `JambaForCausalLM`; `Mamba2ForCausalLM`; `Rwkv7ForCausalLM`; `Ernie4_5_MoeForCausalLM`; `SeedOssForCausalLM`; `Olmo3ForCausalLM`; `ApertusForCausalLM`; `Exaone4ForCausalLM`; and `MiMoV2FlashForCausalLM`.
  - **Encoders for embedding and reranking:** `BertModel`, `XLMRobertaModel` (bge-m3), `NomicBertModel`, `ModernBertModel`, `JinaEmbeddingsV5Model`/`EuroBertModel` and `NeoBERT`.
  - Altogether there are about 120 converter modules.
- Qwen3.5 uses a 3:1 hybrid of Gated DeltaNet linear-attention layers and gated full-attention layers. The MoE variants pair this with sparse experts; the flagship is 397B total and 17B active with 262K context. GGUF cards warn that a very recent llama.cpp is needed for the new ops (GATED_DELTA_NET, SSM_CONV, TRI_SOLVE). — [morphllm Qwen 3.5 guide](https://www.morphllm.com/qwen-3-5) (secondary); [AaryanK/Qwen3.5-0.8B-GGUF](https://huggingface.co/AaryanK/Qwen3.5-0.8B-GGUF); [WebGPU op commit](https://app.semanticdiff.com/gh/ngxson/llama.cpp/commit/c1258830b28dddc65ac3aa3452214553fdece30c)
- Gemma 4 comes in E2B, E4B, 26B-A4B (MoE: 25.2B total, 3.8B active, 128 experts with 8 active plus 1 shared, 30 layers, 1024-token sliding window, a vision encoder of about 550M) and 31B. Vision covers the whole family and audio covers the small E-models. — [google/gemma-4-E2B model card](https://huggingface.co/google/gemma-4-E2B); [aman.ai Gemma 4 primer](https://aman.ai/primers/ai/gemma-4/) (secondary; sources disagree on 512 vs 1024 SWA window per size, and on whether a 12B exists)
- Recommendations from 2026 roundups name Qwen3, Gemma 4, gpt-oss-20b/120b, Qwen3-Coder-Next, Devstral Small 2, Mistral Small 3.x, Phi-4, Llama 3.3 and Llama 4 Scout, GLM and MiniMax, with DeepSeek V4 Flash described as too large for most local setups. — [HF blog: best open models to run locally 2026](https://huggingface.co/blog/daya-shankar/open-source-llm-models-to-run-locally); [starmorph local inference guide](https://blog.starmorph.com/blog/local-llm-inference-tools-guide); [vdf.ai coding roundup](https://vdf.ai/blog/best-local-llm-for-coding/) (low-authority aggregators; rankings inconsistent)

### Inferences
- MoE models (Qwen3.5-MoE, Gemma 4 26B-A4B, gpt-oss, GLM-4.5/4.6, DeepSeek, Kimi, Llama 4, LFM2-MoE) dominate the popular list. For local serving the key lever is expert offload to CPU, through llama.cpp's `--override-tensor`/`-ot` and `--n-cpu-moe`, and Onyx exposes neither. This is the single most valuable loader flag Onyx could add.
- Hybrid and recurrent models (Qwen3.5, Qwen3-Next, Granite 4, Nemotron-H, Falcon-H1, LFM2, Jamba, Kimi-Linear) have recurrent state that cannot be partially truncated. Onyx's `--context-shift` and `--cache-reuse` may need the same force-disable treatment they already get under mtmd, and the batching loop should be tested against `llama_memory_seq_rm` failures on these models.
- SWA models (Gemma 3/4, gpt-oss, Granite-SWA, EmbeddingGemma) interact with prefix caching. Upstream has a `--swa-full` option, and Onyx should consider exposing it.

### Gaps
- No authoritative download or usage statistics were found, such as HF download counts by family or Ollama pull counts. The popularity ranking rests on aggregator articles.
- Details of Qwen4Exp, Kimi K3, DeepSeek V4, GLM-5 and MuseSpark were not researched beyond their presence in the converter.

## Q2. Custom converter vs. upstream `convert_hf_to_gguf.py` vs. pre-quantized GGUFs; torch-free options; HF Hub GGUF features

### Takeaway
Keeping a hand-rolled converter in sync with about 120 upstream architectures, more than 100 pre-tokenizer hashes and new ops is not realistic. The best default is to pull pre-quantized GGUFs (ggml-org, unsloth, bartowski) through an `-hf repo:quant`-style resolver. The vendored `gguf-py` (numpy only, no torch) can replace the hand-written GGUF writer and add Q8_0 and other quantize-on-convert types. For anything outside Llama, Onyx should shell out to upstream `convert_hf_to_gguf.py`, which still imports torch.

### Cited Findings
- Upstream's converter has been split into a `conversion/` package (about 120 modules), with `convert_hf_to_gguf.py` reduced to about 312 lines of driver code. It still does `import torch` at the top and in `conversion/base.py`. — [convert_hf_to_gguf.py](https://github.com/ggml-org/llama.cpp/blob/master/convert_hf_to_gguf.py); [conversion/base.py](https://github.com/ggml-org/llama.cpp/blob/master/conversion/base.py) line 22
- The upstream converter's `--outtype` accepts `f32, f16, bf16, q8_0, tq1_0, tq2_0, auto` (auto picks the highest-fidelity 16-bit type), so quantize-on-convert is limited to Q8_0 and the ternary types. — convert_hf_to_gguf.py lines 60-61 and 216-218
- `gguf-py` (package `gguf` 0.19.0) depends only on `numpy>=2.2.6`, `tqdm`, `pyyaml` and `requests`, with no torch. It provides `gguf_writer.py`, `gguf_reader.py`, `tensor_mapping.py`, `vocab.py`, `lazy.py`, `metadata.py`, `quants.py` and CLI scripts (`gguf-dump`, `gguf-set-metadata`, `gguf-new-metadata`). — [gguf-py/pyproject.toml](https://github.com/ggml-org/llama.cpp/blob/master/gguf-py/pyproject.toml)
- `gguf-py/gguf/quants.py` implements numpy `quantize_blocks` for BF16, Q4_0, Q4_1, Q5_0, Q5_1, Q8_0, TQ1_0, TQ2_0, Q1_0, Q2_0 and MXFP4. K-quants, i-quants and NVFP4 are dequantize-only in Python. — [gguf-py/gguf/quants.py](https://github.com/ggml-org/llama.cpp/blob/master/gguf-py/gguf/quants.py)
- `gguf-py/gguf/vocab.py` (`SpecialVocab`) already loads `chat_template.jinja`, falling back to `chat_template.json` and then `tokenizer_config.json`. — gguf-py/gguf/vocab.py lines 330-341
- The HF Hub has native GGUF support: a `library=gguf` filter, an in-page metadata and tensor viewer (`?show_tensors=`), the `@huggingface/gguf` JS parser that reads remote headers through range requests, a quant-type reference table that includes TQ1_0, TQ2_0 and MXFP4, and the `ggml-org/gguf-my-repo` Space for conversion and quantization. — [HF Hub GGUF docs](https://huggingface.co/docs/hub/gguf)
- Upstream llama.cpp supports `-hf <user>/<model>[:quant]` (with `--hf-file` overrides), and also `-hfd` for a draft model. With `-hf`, the matching `mmproj` is auto-downloaded and can be disabled with `--no-mmproj`. — [common/arg.cpp](https://github.com/ggml-org/llama.cpp/blob/master/common/arg.cpp) lines 3074-3085 and 4016; [docs/multimodal.md](https://github.com/ggml-org/llama.cpp/blob/master/docs/multimodal.md)
- ggml-org publishes official pre-quantized GGUFs, mostly Q4_K_M, with matching mmproj files, for example `ggml-org/gemma-4-26B-A4B-it-GGUF`, `ggml-org/Qwen3-Omni-30B-A3B-Instruct-GGUF` and `ggml-org/embeddinggemma-300M-GGUF`. — [docs/multimodal.md](https://github.com/ggml-org/llama.cpp/blob/master/docs/multimodal.md); [ggml-org EmbeddingGemma collection](https://huggingface.co/collections/ggml-org/embeddinggemma-300m)

### Inferences
- Recommended layered strategy:
  1. **Prefer GGUF.** Add an `-hf repo[:quant]` resolver, or let JX Runtime handle downloads, with optional mmproj auto-pairing. Onyx's "no I/O on request behalf" principle applies to requests, not to startup, so this remains consistent with it.
  2. **Rebase the custom converter onto vendored `gguf-py`.** It is already in the tree, numpy-only and version-matched to the pinned llama.cpp. Use its `GGUFWriter`, `TensorNameMap` (tensor_mapping.py) and `SpecialVocab` (chat template, special tokens) instead of hand-rolled code. This removes about 200 lines of format code and fixes the missing chat template for free. Stop pruning `conversion/` if the torch-free path should reuse its tensor-name logic.
  3. **Add `--outtype q8_0` (and bf16).** gguf-py's numpy Q8_0 quantizer makes this a small change and halves cache size, from f16 to about 8.5 bits per weight.
  4. **Optionally run `llama-quantize` after conversion**, linking the vendored `tools/quantize` or calling `llama_model_quantize()` from `llama.h` in-process to produce Q4_K_M. That C API is available without Python.
  5. **For unsupported architectures**, fall back to upstream `convert_hf_to_gguf.py` when a Python environment with torch is present, keeping the error-on-unsupported principle for the zero-dependency path.
- The highest-ROI fixes for the existing Llama path are: (a) accept `rope_scaling.rope_type == "llama3"` and write `llama.rope.scaling.*` or the `rope_freqs` tensor the way upstream does, (b) accept the Llama 3 `Split` regex pre-tokenizer and write `pre="llama-bpe"`, and (c) write `tokenizer.chat_template`.

### Gaps
- I did not verify whether upstream has a torch-free mode such as a `--no-lazy` or safetensors-only path. Imports suggest it does not.

## Q3. Quantization advances worth exposing

### Takeaway
Mainline GGUF in 2026 has k-quants, i-quants with imatrix, ternary TQ1_0/TQ2_0, MXFP4 (gpt-oss, MoE experts), NVFP4 (type 40, added spring 2026), and new Q1_0/Q2_0 types. "Dynamic" per-layer mixes such as Unsloth Dynamic 2.0 are the de facto popular downloads. Trellis quants (IQ*_KT) exist only in the ik_llama.cpp fork. Onyx's main gaps are on the runtime side: KV cache quantization flags and MoE expert offload. A K-quant producer for its converter is lower priority.

### Cited Findings
- Upstream `GGMLQuantizationType` now includes F32, F16, Q4_0, Q4_1, Q5_0, Q5_1, Q8_0, Q8_1, Q2_K–Q8_K, IQ2_XXS, IQ2_XS, IQ3_XXS, IQ1_S, IQ4_NL, IQ3_S, IQ2_S, IQ4_XS, IQ1_M, BF16, TQ1_0=34, TQ2_0=35, MXFP4=39, NVFP4=40, Q1_0=41 and Q2_0=42. The file types include `MOSTLY_MXFP4_MOE=38`, `MOSTLY_NVFP4=39`, `MOSTLY_Q1_0=40` and `MOSTLY_Q2_0=41`. — [gguf-py/gguf/constants.py](https://github.com/ggml-org/llama.cpp/blob/master/gguf-py/gguf/constants.py)
- NVFP4 support merged in stages from late March to April 2026 (CUDA, SYCL and Vulkan backends). It gives native acceleration on Blackwell (RTX 5090, RTX PRO 6000), while older GPUs get only the memory savings. Metal reportedly lacks NVFP4 matmul. `llama-quantize` cannot produce NVFP4; instead, NVFP4 GGUFs come from converting checkpoints already quantized to NVFP4 (a compressed-tensors converter PR was still open). Activation `input_scale` is reportedly unused. — [insiderllm FP4 guide](https://insiderllm.com/guides/fp4-inference-llamacpp-nvfp4-mxfp4/) (secondary); [llama.cpp issue #15415](https://github.com/ggml-org/llama.cpp/issues/15415); example [CISCai/gemma-4-31B-it-NVFP4-turbo-GGUF](https://huggingface.co/CISCai/gemma-4-31B-it-NVFP4-turbo-GGUF)
- MXFP4 is listed on the HF Hub GGUF quant table as "4-bit Microscaling Block Floating Point" (llama.cpp PR #15091, gpt-oss). — [HF Hub GGUF docs](https://huggingface.co/docs/hub/gguf)
- For Unsloth Dynamic 2.0, Unsloth says it "dynamically adjust[s] the quantization type of every possible layer", with per-model layer mixes. It argues KL divergence, not perplexity, is the right metric because perplexity errors can cancel out. It claims that Wikipedia-based imatrix calibration overfits wiki-style evals and that text-only calibration is ineffective for instruct models, so it uses a curated chat calibration set. A Feb 27, 2026 update benchmarked every Qwen3.5 GGUF on perplexity and KLD and fixed tool-calling chat-template issues. All of these are self-reported claims. — [Unsloth Dynamic 2.0 docs](https://docs.unsloth.ai/basics/unsloth-dynamic-2.0-ggufs); [Unsloth blog](https://unsloth.ai/blog/dynamic-v2)
- ik_llama.cpp has trellis quants IQ1_KT, IQ2_KT, IQ3_KT and IQ4_KT, built on a "novel, integer-base trellis" for reasonable CPU speed, with CUDA, Metal, NEON and CPU implementations (PR 113; IQ1_KT in PR 616). QTIP, the trellis-coded quantization paper, is the conceptual ancestor, but no source confirmed the exact lineage. These types are not in mainline `GGMLQuantizationType`, so mainline-based Onyx cannot load them. — [ik_llama.cpp (mirror)](https://github.com/RodriMora/ik_llama.cpp); [QTIP paper](https://arxiv.org/html/2406.11235v4); mainline constants.py (absence)
- Upstream's KV cache types (`-ctk/--cache-type-k`, `-ctv/--cache-type-v`) are F32, F16, BF16, Q8_0, Q4_0, Q4_1, IQ4_NL, Q5_0 and Q5_1. A quantized V cache requires flash attention. — [common/arg.cpp](https://github.com/ggml-org/llama.cpp/blob/master/common/arg.cpp) lines 304-314 and 2441-2459 (the flash-attention requirement for V is my domain knowledge, not re-verified)
- Onyx exposes `-fa` but no `--cache-type-k/v`. — [src/args.cpp](/home/user/Onyx-Engine/src/args.cpp)

### Inferences
- The cheapest wins are adding `--cache-type-k/--cache-type-v`, which map directly to `llama_context_params.type_k/type_v` and halve KV memory with Q8_0, and adding `--override-tensor` and `--n-cpu-moe` for MoE.
- For the converter, `--outtype q8_0` through gguf-py is easy. A K-quant or i-quant producer should call the vendored `llama_model_quantize()` C API rather than reimplement anything, with an optional user-provided imatrix file.
- NVFP4 and MXFP4 *source* checkpoints (gpt-oss ships MXFP4 experts on HF; NVIDIA publishes NVFP4 checkpoints) are a real input class. The current converter refuses them because of their U8 packed tensors and scales. Passing them through needs per-architecture handling, which is another argument for deferring to upstream conversion.
- BitNet and ternary (`BitnetForCausalLM`, TQ1_0/TQ2_0) are niche. They are worth loading but not worth converter work.

### Gaps
- No independent KLD comparison of unsloth vs. bartowski vs. ggml-org quants was found; only self-reported Unsloth numbers exist, and those tables were truncated.
- The status of NVFP4 PR #22196 (the native Blackwell path) and of the compressed-tensors NVFP4 converter as of October 2026 is unconfirmed.

## Q4. Tokenizers: SentencePiece/Unigram, pre-tokenizer hashing, chat template sourcing

### Takeaway
Upstream identifies BPE pre-tokenizers by tokenizing a fixed probe string with the real HF tokenizer and SHA-256 hashing the token IDs ("chkhsh"), which requires `transformers`. There are about 100 known hashes. A torch-free converter has to match on `tokenizer.json`'s `pre_tokenizer` and `normalizer` structure (regexes) instead, and must refuse unknown ones. SentencePiece needs the `sentencepiece` package upstream. Chat templates now usually live in `chat_template.jinja`.

### Cited Findings
- `get_vocab_base_pre()` encodes a fixed multilingual, emoji and whitespace probe string (`chktxt`) and takes `sha256(str(tokenizer.encode(chktxt)))`. It maps the result to a `tokenizer.ggml.pre` name such as `chatglm-bpe`, with the comment "don't edit the hashes manually!". There are 102 `chkhsh ==` branches in `conversion/base.py`. — [conversion/base.py](https://github.com/ggml-org/llama.cpp/blob/master/conversion/base.py) lines 1803-1828
- Hashes are generated by `convert_hf_to_gguf_update.py` from a list of about 105 reference repos, each tagged SPM or BPE, for example `llama-spm` (Llama-2), `llama-bpe` (Llama-3) and `phi-3` (SPM). — [convert_hf_to_gguf_update.py](https://github.com/ggml-org/llama.cpp/blob/master/convert_hf_to_gguf_update.py) lines 87-91
- The SentencePiece path (`_create_vocab_sentencepiece`) does `from sentencepiece import SentencePieceProcessor`, so it needs the sentencepiece Python package. — conversion/base.py lines 2293-2306
- `chat_template.jinja` is read first, then `chat_template.json`, then `tokenizer_config.json`'s `chat_template`, both in gguf-py `SpecialVocab` and in `conversion/base.py`. — [gguf-py/gguf/vocab.py](https://github.com/ggml-org/llama.cpp/blob/master/gguf-py/gguf/vocab.py) lines 330-341; conversion/base.py line 2691
- Unsloth reports fixing tool-calling chat-template bugs in Qwen3.5 GGUFs (Feb 2026), which shows templates embedded in GGUFs can be wrong and need overriding. — [Unsloth Dynamic 2.0 docs](https://docs.unsloth.ai/basics/unsloth-dynamic-2.0-ggufs)

### Inferences
- A torch-free approach to pre-tokenizer identification would vendor or replicate a table that maps `tokenizer.json` `pre_tokenizer` JSON (the Split regex plus ByteLevel flags) to llama.cpp `pre` names for the most common models (llama-bpe, qwen2, gpt-4o/o200k, deepseek-v3, gemma, tekken). Exact-match the JSON and refuse anything else. A stronger alternative is to compute chkhsh without transformers by running the probe through llama.cpp's own tokenizer on a trial GGUF for each candidate `pre`, then comparing against the HF `tokenizer.json` tokenizer reimplemented in numpy. That is complex; the table is pragmatic.
- SentencePiece `tokenizer.model` is a protobuf. A minimal pure-Python protobuf reader for `pieces` (piece, score, type) is feasible without the `sentencepiece` package. Many SPM models (Gemma, Mistral-v1, Phi-3) also ship a `tokenizer.json` with a Unigram or BPE-with-byte-fallback model, which could be parsed instead.
- Onyx should add a `--chat-template-file` runtime override, mirroring upstream `--chat-template-file` (domain knowledge, not re-verified in arg.cpp this session). This lets users fix broken embedded templates without reconverting.

### Gaps
- I did not enumerate the full `pre` name list or measure how many popular models share the top few pre-tokenizers.

## Q5. Multimodal trends and mtmd support

### Takeaway
mtmd now covers image, audio and video input. Supported models include Gemma 3, Gemma 4 (vision on all sizes, audio on E2B and E4B), Qwen2.5-VL, Qwen3-VL and Qwen3.5 (conditional generation), Qwen2.5-Omni and Qwen3-Omni (audio plus vision), Voxtral, Qwen3-ASR, Ultravox, InternVL 2.5/3, SmolVLM/SmolVLM2, Pixtral, Mistral Small 3.1, Llama 4 Scout, Moondream2, LFM2-VL and a wave of OCR models. Onyx's mtmd integration is aligned with upstream, but it lacks video input and `-hf` mmproj auto-pairing.

### Cited Findings
- "Currently, we support **image**, **audio** and **video** input." Models are loaded with `-m model.gguf --mmproj mmproj.gguf` or `-hf` (auto mmproj, `--no-mmproj`, `--no-mmproj-offload`). — [docs/multimodal.md](https://github.com/ggml-org/llama.cpp/blob/master/docs/multimodal.md)
- Official pre-quantized multimodal GGUFs:
  - **Vision:** Gemma 3 (4B/12B/27B), SmolVLM and SmolVLM2 (256M–2.2B, including video variants), Pixtral 12B, Qwen2-VL, Qwen2.5-VL (3B–72B), Mistral Small 3.1 24B, InternVL 2.5/3, Llama 4 Scout, Moondream2 and Gemma 4 (E2B/E4B/26B-A4B/31B).
  - **Audio:** Ultravox 0.5, Voxtral-Mini-3B-2507, Qwen3-ASR 0.6B/1.7B, and Qwen2-Audio (convertible but not published because of poor results).
  - **Mixed:** Qwen2.5-Omni 3B/7B, Qwen3-Omni-30B-A3B (Instruct and Thinking) and Gemma 4 E2B/E4B (audio plus vision).
  - **OCR models need specific prompts:** PaddleOCR-VL, GLM-OCR, DeepSeek-OCR, Dots.OCR and HunyuanOCR.
  - Source: [docs/multimodal.md](https://github.com/ggml-org/llama.cpp/blob/master/docs/multimodal.md)
- Additional multimodal converter classes include `Qwen3VLMoeForConditionalGeneration`, `Lfm2VlForConditionalGeneration`, `Lfm2AudioForConditionalGeneration`, `VoxtralRealtimeForConditionalGeneration`, `GraniteSpeechForConditionalGeneration`, `Granite4VisionForConditionalGeneration`, `KimiVLForConditionalGeneration`, `MiniCPMV4_6/4_7`, `AudioFlamingo3ForConditionalGeneration` and `Glm4vForConditionalGeneration`. — [conversion/](https://github.com/ggml-org/llama.cpp/tree/master/conversion)

### Inferences
- Onyx accepts only `image_url` and `input_audio` content parts. Upstream now handles video, which is worth tracking: either accept `video_url` data URIs or frame lists, or explicitly reject them.
- Because mmproj files are separate GGUFs that need the right pairing, an `-hf` resolver, or JX Runtime metadata that pairs `mmproj-*.gguf` from the same repo, removes a common user error.
- The safetensors converter never produces an mmproj, so multimodal safetensors models effectively require upstream conversion (`--mmproj` flag in convert_hf_to_gguf.py, domain knowledge) or pre-built GGUFs.

### Gaps
- I did not check whether the pinned b10711 vendored mtmd already includes video support. The check would be to compare `tools/mtmd` at the pin with master.

## Q6. Embedding and reranker models and pooling needs

### Takeaway
Popular local embedders are Qwen3-Embedding (0.6B/4B/8B, last-token pooling plus a query instruction prefix), EmbeddingGemma-300M (SWA, with QAT q4_0/q8_0 GGUFs), bge-m3 (XLM-R, CLS), nomic-embed (NomicBert, mean) and Jina v5. Qwen3-Reranker uses rank pooling. Onyx supports `--pooling none|mean|cls|last|rank`, but it rejects the `dimensions` parameter, which blocks Matryoshka truncation, and it has no `/v1/rerank` endpoint.

### Cited Findings
- llama.cpp pooling types are `NONE`, `MEAN`, `CLS`, `LAST`, and `RANK` ("used by reranking models to attach the classification head to the graph"; the rank output is `float[n_cls_out]`). — [include/llama.h](https://github.com/ggml-org/llama.cpp/blob/master/include/llama.h) lines 178-183 and 1166-1167
- Qwen3-Embedding appends `[EOS]` and uses the last-layer hidden state of that token, so it needs last-token pooling. Community GGUF guidance is `llama-server --embedding --pooling last` plus an instruction prefix on queries only, and "retrieval quality collapses" without both (a community claim). Official GGUFs are published, for example `Qwen/Qwen3-Embedding-8B-GGUF`. — [Qwen3 Embedding paper](https://arxiv.org/html/2506.05176v1); [smarttasks/Qwen3-Embedding-0.6B-GGUF](https://huggingface.co/smarttasks/Qwen3-Embedding-0.6B-GGUF); [Qwen/Qwen3-Embedding-8B-GGUF](https://huggingface.co/Qwen/Qwen3-Embedding-8B-GGUF)
- For Qwen3-Reranker GGUFs, at least one community card claims an unmerged fix branch was needed and that other HF reranker GGUFs "typically fail in mainline". This could not be confirmed as still true. — [JonathanMiddleton/Qwen3-Reranker-0.6B](https://huggingface.co/JonathanMiddleton/Qwen3-Reranker-0.6B/blob/main/README.md)
- EmbeddingGemma-300M support landed in llama.cpp in September 2025 and required a new SWA masking type. ggml-org publishes `embeddinggemma-300M-GGUF` plus QAT q4_0/q8_0 variants. — [llama.cpp commit (mirror)](https://app.semanticdiff.com/gh/ngxson/llama.cpp/commit/fb15d649ed14ab447eeab911e0c9d21e35fb243e); [ggml-org collection](https://huggingface.co/collections/ggml-org/embeddinggemma-300m)
- Encoder architectures in the upstream converter include `BertModel`, `XLMRobertaModel`/`XLMRobertaForSequenceClassification` (bge-m3, bge-reranker), `NomicBertModel`, `ModernBertModel`, `JinaEmbeddingsV5Model`, `NeoBERT` and `EmbeddingGemma2Model`. — [conversion/](https://github.com/ggml-org/llama.cpp/tree/master/conversion)
- Onyx's `/v1/embeddings` returns `400 "dimensions is not supported"`. — [src/server.cpp](/home/user/Onyx-Engine/src/server.cpp) line 1193

### Inferences
- Implementing OpenAI's `dimensions` parameter for Matryoshka-trained models is a cheap win: truncate the pooled vector to the first N components, then L2-renormalize. Qwen3-Embedding and EmbeddingGemma are Matryoshka-trained (my domain knowledge: EmbeddingGemma supports 768/512/256/128 and Qwen3-Embedding supports user-defined dims, per the paper). It could be gated to N ≤ n_embd.
- A `/v1/rerank` (Jina/Cohere-style) endpoint over `--pooling rank` would match llama-server, and JX Runtime may need it. The plumbing for `--pooling rank` raw scores already exists.
- Onyx could auto-select `last` pooling when GGUF metadata says so; llama.cpp reads `<arch>.pooling_type` from GGUF. Its "models without pooling metadata get mean" fallback (engine.cpp:54) is wrong for Qwen3-Embedding GGUFs that lack metadata.

### Gaps
- No source in this session confirmed Matryoshka dims for EmbeddingGemma or Qwen3-Embedding, so that is domain knowledge only. Whether mainline now runs Qwen3-Reranker GGUFs correctly is also unconfirmed.

## Q7. Model security: GGUF parsing CVEs, safetensors safety, provenance and signing

### Takeaway
GGUF parsing in ggml has had repeated integer-overflow CVEs in 2025-2026, all fixed well before Onyx's b10711 pin. Because fixes have been incomplete before, Onyx should track upstream promptly and treat GGUFs as untrusted input. Safetensors is safe by design, unlike pickle, but Onyx's own converter parses headers itself and must bounds-check them. OpenSSF model-signing v1.0 (sigstore) is the emerging provenance standard.

### Cited Findings
- **CVE-2026-33298:** an integer overflow in `ggml_nbytes` triggered by a crafted GGUF can cause a heap overflow and possible RCE (CVSS 7.8). It is fixed in b7824. Debian listed its llama.cpp package as still vulnerable. — [NVD](https://nvd.nist.gov/vuln/detail/CVE-2026-33298); [OSV](https://osv.dev/vulnerability/CVE-2026-33298); [Debian tracker](https://security-tracker.debian.org/tracker/CVE-2026-33298)
- **CVE-2026-27940:** an integer overflow in `gguf_init_from_file_impl()` leads to an undersized heap allocation. It is a bypass of the incomplete fix for CVE-2025-53630 and is fixed in b8146. — [CVE-2026-27940](https://ospire.mind.be/buildroot/master/vulnerability/CVE-2026-27940)
- **CVE-2026-7482 "Bleeding Llama" (May 2026, Cyera):** an out-of-bounds heap read in *Ollama's own* GGUF handling, where a crafted GGUF plus unauthenticated API calls leaks memory. It is not a llama.cpp bug, but it shows that a server accepting user-supplied models is an attack surface. — [pasqualepillitteri.it writeup](https://pasqualepillitteri.it/en/news/2343/bleeding-llama-malicious-gguf-local-llm-memory-leak) (secondary)
- OpenSSF model-signing v1.0 (April 2025) is a library and CLI (`pip install model-signing`) that signs and verifies model directories of any format and size. It is built on sigstore-python and also supports self-signed certificates and key pairs with the same hashing and signature format. The team planned hub upload-flow integration, but no confirmed Hugging Face integration was found. — [Sigstore blog](https://blog.sigstore.dev/model-transparency-v1.0); [Google blog](https://blog.google/security/taming-wild-west-of-ml-practical-mode/)
- Onyx's converter already guards shard paths against traversal (absolute paths, `..`, escaping the model directory). — [convert-safetensors.py](/home/user/Onyx-Engine/scripts/convert-safetensors.py) lines 157-162

### Inferences
- Onyx's pin (b10711) is after both fixes, b7824 and b8146. Recommendations:
  - Add a CI or "security bump" policy that follows upstream GGUF and mtmd fixes, since the mmproj loader (clip.cpp) is a second parser.
  - Run onyx-engine as a non-root user, and document that it must not load untrusted GGUFs if it ever accepts model paths from the network (it currently does not).
  - Fuzz the hand-written safetensors header parser: JSON header length, offsets within the file, and shape × dtype size matching `data_offsets`.
- Optionally verify OpenSSF model-signing signatures, or at minimum record the SHA-256 of the GGUF and source safetensors in `/props` and `/v1/models` for provenance. The HF Hub exposes per-file SHA-256 (LFS/Xet), so a `-hf` downloader can verify downloads.

### Gaps
- I could not produce a full list of 2025-2026 llama.cpp and ggml CVEs, including any in mtmd/clip mmproj parsing. GitHub security advisories for ggml-org/llama.cpp should be checked directly.
- Whether HF has shipped native sigstore model-signing verification by October 2026 is unconfirmed.
