# Contest submission notes

Author：FDE WorkMate

- Base: `zihaomu/seekdb` branch `amd-oceanbase-official-contest-base-v5`, commit
  `634888d9b037153fa143d2c913ba4b25503e1d62` (official AMD contest baseline V5).
- This commit only adds this note. Build steps, SQL behaviour and the GPU CAGRA
  build/query path are identical to V5; it is used to confirm that the
  evaluation pipeline builds and runs this repository.
- Build: follow [README.md](README.md) (`tools/hipvs_bridge/build.sh`, then
  `./build.sh release -DOB_BUILD_CUVS=ON -DCUVS_BRIDGE_LIB=...`).