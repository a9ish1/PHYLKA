# Contributing to PHYLKA

Thanks for your interest. PHYLKA is experimental software, so careful review
matters more than speed.

## Getting started

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
./build/phylka selftest        # must print ALL OK
```

Please also build once with `-DPLK_PORTABLE_SHA=ON`; that is the code path
Linux uses.

## Ground rules

- Open an issue before large changes, especially anything that touches
  consensus (block/transaction format, validation, difficulty, finality). A
  consensus change alters the chain and needs discussion and a clear activation
  plan.
- Keep changes focused: one topic per pull request.
- C++17, no new third-party dependencies without discussion. Follow the style of
  the surrounding code (see `.editorconfig`); the code builds warning-free with
  `-Wall -Wextra`.
- Add or extend a check in `phylka selftest` for new primitives and
  consensus-relevant behavior, preferably against published test vectors.
- Platform-specific code belongs behind `src/platform.h`/`platform.cpp`, not
  scattered `#ifdef`s.
- Never commit wallets, seed phrases, keys, node data or absolute paths from
  your machine. Do not run experiments on the main network; use `--dev`.
- Do not use the main network for mining tests or "just trying".

## Pull requests

1. Fork and create a topic branch.
2. Make sure `phylka selftest` passes on your platform.
3. Describe what changed and why, and how you tested it.
4. CI (macOS and Linux) must be green. The Windows job is experimental.

## Security issues

Do not open public issues for vulnerabilities. Follow [SECURITY.md](SECURITY.md).

## License

By contributing you agree that your contribution is licensed under the MIT
license of this project (see [LICENSE](LICENSE)).
