"""Shared by the staged-PR tests (docs/upstream/submission/*.diff): the tree before the change (OLD_TREE, upstream
master a2c2010 or any tree the diff applies to) and a temporary copy of it with the diff applied.

NEW_TREE, if set, is used as the tree under test instead of the temporary copy. Exits with SKIP (status 0) when the
upstream tree is not available or the diff does not apply to it (the public CI only has the old submodule)."""
import atexit, os, shutil, subprocess, sys, tempfile

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
SUBMISSION = os.path.join(ROOT, "docs", "upstream", "submission")

def trees(diff_name):
    old = os.environ.get("OLD_TREE", os.path.join(ROOT, "work/upstream-snapshot/master"))
    if not os.path.isdir(os.path.join(old, "libs")):
        print(f"SKIP: no upstream tree at {old} (set OLD_TREE)"); sys.exit(0)
    if os.environ.get("NEW_TREE"):
        return old, os.environ["NEW_TREE"]
    tmp = tempfile.mkdtemp(prefix="staged-"); atexit.register(shutil.rmtree, tmp, True)
    new = os.path.join(tmp, "tree")
    shutil.copytree(old, new, symlinks=True, ignore=shutil.ignore_patterns(".git"))
    r = subprocess.run(["patch", "-p1", "-s", "--no-backup-if-mismatch", "-d", new, "-i",
                        os.path.abspath(os.path.join(SUBMISSION, diff_name))], capture_output=True, text=True)
    if r.returncode:
        print(f"SKIP: {diff_name} does not apply to {old}: {(r.stdout + r.stderr)[:200]}"); sys.exit(0)
    return old, new
