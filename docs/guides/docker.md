[Documentation](../README.md) › [Guides](README.md) › Running it in Docker

# Running it in Docker

The job: serve a model without installing a toolchain. Mount a directory of GGUF models and run:

```bash
docker run --rm -p 127.0.0.1:8080:8080 -v "$HOME/models:/models:ro" \
    ghcr.io/jx-holdings-llc/onyx-engine:latest \
    -m /models/your-model.gguf --alias my-model --allow-unauthenticated
```

Then talk to it as usual: `curl http://127.0.0.1:8080/health`, and the OpenAI base URL
`http://127.0.0.1:8080/v1`.

The image is published by the Release workflow from the release's own source commit, tagged with
the version (`:0.6.0`) and, on the stable channel, `:latest`. It is a CPU build for `linux/amd64`
that assumes AVX2, like the `linux-x64` archive. Releases before 0.6.0 have no image.

## Why `--allow-unauthenticated`, and when to use `--api-key` instead

The image's entrypoint is `onyx-engine --host 0.0.0.0 --port 8080`: inside a container the
engine has to listen on all interfaces for Docker's port publishing to reach it. Onyx refuses a
non-loopback `--host` without a key, so you choose one of two things explicitly:

- **`--allow-unauthenticated`**, together with publishing the port on loopback only
  (`-p 127.0.0.1:8080:8080`, as above). Only programs on your machine can reach it.
- **`--api-key KEY`**, when the port is published more widely (`-p 8080:8080`) or other
  containers call the engine. Clients then send `Authorization: Bearer KEY`.

Everything after the image name is passed to `onyx-engine`, so any flag from the
[command line reference](../reference/cli.md) works: `-c`, `-np`, `--max-queue`, `--metrics`,
`--embedding`, `--mmproj` and so on.

## Health checks and supervision

`GET /health` answers `503` while the model loads and `200` once it is ready, so it works as a
container health check or a readiness probe. In Compose:

```yaml
services:
  onyx:
    image: ghcr.io/jx-holdings-llc/onyx-engine:latest
    command: ["-m", "/models/your-model.gguf", "--alias", "my-model", "--api-key", "${ONYX_KEY}"]
    volumes: ["./models:/models:ro"]
    ports: ["127.0.0.1:8080:8080"]
```

The image has no `curl` for a built-in `HEALTHCHECK`; probe `/health` from your orchestrator (a
Kubernetes `httpGet` readiness probe on port 8080, for example). `docker stop` sends `SIGTERM`,
which ends the engine at once.

## Building the image yourself

From a clone:

```bash
docker build -t onyx-engine .                      # CPU, the default target
docker build -t onyx-engine:cuda --target cuda .   # NVIDIA CUDA 12.6
```

The build uses CMake alone: the first configure fetches the pinned llama.cpp commit with Git
inside the build stage. Build arguments:

| Argument | Default | Effect |
|---|---|---|
| `ONYX_X64_FLAGS` | AVX, AVX2, FMA, F16C | CPU features assumed on x86-64. Set it to `""` for an image that runs on any x86-64 CPU. Ignored on arm64. |
| `UBUNTU_VERSION` | `24.04` | Base image for both stages |
| `CUDA_VERSION` | `12.6.3` | The `nvidia/cuda` base for the CUDA target |
| `CUDA_ARCHITECTURES` | `75;80;86;89;90` | GPU generations compiled into the CUDA target (Turing to Hopper) |

On an arm64 machine (Apple Silicon, Graviton), `docker build` produces an arm64 image; no image
is published for arm64 yet.

The CUDA image runs with the NVIDIA Container Toolkit:

```bash
docker run --rm --gpus all -p 127.0.0.1:8080:8080 -v "$HOME/models:/models:ro" \
    onyx-engine:cuda -m /models/your-model.gguf --allow-unauthenticated
```

The CUDA target is not published and has not been built by this project's CI yet; treat it as a
starting point. The CPU target is built and checked (`--version`) on every release run.

## What is not in the image

- **Python, so no safetensors conversion.** Mount GGUF files, or convert elsewhere first (see
  [serving models](serving-models.md)).
- **Models.** Onyx never downloads anything; mount them.
- **A shell user with privileges.** The engine runs as an unprivileged `onyx` user (uid 10001);
  the models directory needs to be readable by it.
