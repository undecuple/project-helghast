# Network host test

After preparing `work/ps3recomp-patched` from the pinned submodule and the project's patch series, run:

```text
python3 tools/net/test_network.py
```

The test needs Python 3 and a host compiler with C and C++ frontends (`CC`, `cc`, `gcc` or `clang`). It compiles the
port's code and patched runtime network components, generates unsigned ticket and RT fixtures, and removes its
generated files from `work/`. Resolution, socket creation, sends and the psnr transport connection are test doubles:
no server, game assets, real tickets or captures are required or contacted. Failures are errors, not skips.
`tools/host-tests.txt` includes this test for CI.

The fixtures check offline/online gating, HLE failure propagation, NP manager status and Score/Matching psnr
connection gating, selection and failure paths, policy state, synthetic NP ticket shape/callbacks, RT parsing,
modeled replies and redaction. They do not test a real service, game session or the separate generic runtime
resolver-policy blocker; see [network evidence](../../docs/NETWORK-EVIDENCE.md).
