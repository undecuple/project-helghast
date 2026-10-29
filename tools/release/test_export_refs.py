#!/usr/bin/env python3
"""Host test: every tools/... path that an exported script or Python file runs or reads exists in the export tree.

Found 2026-10-28: the exported tools/proton-test.sh called tools/runenv_check.py and tools/release/padrun.sh called
tools/spu/run.sh, neither of them exported. Checked: non-comment lines of *.sh, and string literals (not docstrings)
of *.py, under TREE/tools, for tokens like tools/name.ext (ext py sh tsv json c). Tokens with a placeholder
(<, *, $, {, %) are skipped. Comments and docstrings are prose and may name private tools.

Run: python3 tools/release/test_export_refs.py [TREE]    TREE defaults to work/public-export (after the export script has run);
without a tree the test says SKIP and exits 0. Control: a script that names a missing tool must fail.
"""
import ast
import os
import re
import sys
import tempfile

# A reference counts when it starts a word or follows $ROOT/ (the checkout); "$PT/tools/..." and the like name another tree.
REF = re.compile(r"(?:(?<![\w./-])|(?<=\$ROOT/)|(?<=\$\{ROOT\}/))tools/[A-Za-z0-9_./-]+?\.(?:py|sh|tsv|json|c)\b(?![\w<*${%])")
# Tests that run inside the patched ps3recomp tree (cd $P / $T), where tools/ is that tree's own.
OTHER_TREE = {"tools/test_font_hle_nids.py", "tools/test_gcm_ea2io_range.py", "tools/test_savedata_user_autosave.py",
              "tools/test_ppu_guest_call_stack.py",
              "tools/ppu_lifter.py"}   # ps3recomp's own lifter, read by smoke/ppu_store_note_extract.py from the patched tree
# Named but not required: run-host-tests.sh skips a test when the developer-only marker file is absent (existence guard); profile_edit.py
# prints level-run.sh in a usage hint.
_T = "tools/"  # split so the export scrub does not read these names as needed files
NOT_REQUIRED = {_T + "agents/merge-lane.sh", _T + "level-run.sh"}


def refs_sh(text):
    for n, line in enumerate(text.splitlines(), 1):
        if line.lstrip().startswith("#"):
            continue
        code = re.sub(r"\s#\s.*$", "", line)          # trailing comment
        for m in REF.finditer(code):
            yield n, m.group(0)


def refs_py(text):
    tree = ast.parse(text)
    doc = set()
    for node in ast.walk(tree):
        if isinstance(node, (ast.Module, ast.FunctionDef, ast.ClassDef, ast.AsyncFunctionDef)) and node.body:
            b = node.body[0]
            if isinstance(b, ast.Expr) and isinstance(getattr(b, "value", None), ast.Constant) and isinstance(b.value.value, str):
                doc.add(id(b.value))
    for node in ast.walk(tree):
        if isinstance(node, ast.Constant) and isinstance(node.value, str) and id(node) not in doc:
            for m in REF.finditer(node.value):
                yield node.lineno, m.group(0)


def scan(tree):
    """List of 'file:line: tools/x' for missing references."""
    missing = []
    for dp, dn, fn in os.walk(os.path.join(tree, "tools")):
        dn[:] = [d for d in dn if d != "__pycache__"]
        for f in fn:
            p = os.path.join(dp, f)
            if not f.endswith((".sh", ".py")) or f == os.path.basename(__file__):  # this file holds fake names for its control
                continue
            try:
                text = open(p, encoding="utf-8").read()
                it = refs_sh(text) if f.endswith(".sh") else refs_py(text)
                for n, r in it:
                    if r not in OTHER_TREE and r not in NOT_REQUIRED and not os.path.exists(os.path.join(tree, r)):
                        missing.append(f"{os.path.relpath(p, tree)}:{n}: {r}")
            except (UnicodeDecodeError, SyntaxError):
                continue
    return missing


def main():
    root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
    # control: a missing reference must be found, a present one and prose must not
    with tempfile.TemporaryDirectory() as t:
        os.makedirs(os.path.join(t, "tools"))
        open(os.path.join(t, "tools", "have.py"), "w").write("")
        open(os.path.join(t, "tools", "a.sh"), "w").write("# tools/prose.py\npython3 tools/have.py\npython3 tools/gone.py\n")
        open(os.path.join(t, "tools", "b.py"), "w").write('"""tools/doc.py"""\nX = "tools/gone2.sh"\nY = "tools/have.py"\n')
        got = scan(t)
        if len(got) != 2 or "gone.py" not in got[0] + got[1] or "gone2.sh" not in got[0] + got[1]:
            print("FAIL control: expected exactly the two missing references, got", got)
            return 1
        print("ok   control: missing references found, present ones and prose ignored")
    tree = sys.argv[1] if len(sys.argv) > 1 else os.path.join(root, "work", "public-export")
    if not os.path.isdir(os.path.join(tree, "tools")):
        print(f"SKIP no export tree at {tree} (run the export script first)")
        return 0
    missing = scan(tree)
    for m in missing:
        print("FAIL missing in export:", m)
    if not missing:
        print("ok   every tools/ path referenced by an exported script exists in the export tree")
    return 1 if missing else 0


if __name__ == "__main__":
    sys.exit(main())
