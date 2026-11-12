#!/usr/bin/env python3
"""Install the optional, pinned Intel XeSS runtime after explicit consent."""
import hashlib
import os
import re
import shutil
import subprocess
import tempfile
import urllib.request
import zipfile


SDK_VERSION = "3.0.2"
SR_API_VERSION = "2.0.2"
ARCHIVE_URL = (
    "https://github.com/intel/xess/releases/download/"
    "v3.0.2/XeSS_SDK_3.0.2.zip"
)
ARCHIVE_SIZE = 76839749
ARCHIVE_SHA256 = (
    "88b8a373f30e33f3558a77a93e634f11"
    "b8132fc3047ea1a8edeead32b8471990"
)
SIGNER_SUBJECT = "CN=Intel Corporation"
SIGNER_THUMBPRINT = "2B81BFC21940CD420FC263C975FB41A79B643DF4"
VC_REDIST_URL = "https://aka.ms/vs/17/release/vc_redist.x64.exe"
MIN_VC_RUNTIME = (14, 40)

FILES = {
    "libxess.dll": {
        "member": "bin/libxess.dll",
        "size": 77795704,
        "sha256": (
            "251659dd84a3e84de67c886a4186e01f"
            "3eca49b00641906fe38bb6b807e5d5b7"
        ),
    },
    "Intel-XeSS-LICENSE.txt": {
        "member": "LICENSE.txt",
        "size": 4194,
        "sha256": (
            "784be42c39a4a4d03cf85d88e15e59d"
            "75828fca773ce45b8dbfffbd7eaf208df"
        ),
    },
    "Intel-XeSS-THIRD-PARTY-NOTICES.txt": {
        "member": "third-party-programs.txt",
        "size": 35788,
        "sha256": (
            "e8d8899dbfc4de0e8ca25560be9358e"
            "03c1405d7c956d20a1838c3c709b9b51c"
        ),
    },
}


def sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as source:
        for chunk in iter(lambda: source.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def pinned_problem(path, pin, label):
    if not os.path.isfile(path) or os.path.islink(path):
        return f"{label} is missing"
    size = os.path.getsize(path)
    if size != pin["size"]:
        return (
            f"{label} is {size:,} bytes; SDK {SDK_VERSION} requires "
            f"{pin['size']:,} bytes"
        )
    digest = sha256(path)
    if digest != pin["sha256"]:
        return (
            f"{label} has SHA-256 {digest}; SDK {SDK_VERSION} requires "
            f"{pin['sha256']}"
        )
    return ""


def pe_x64_problem(path):
    try:
        with open(path, "rb") as source:
            head = source.read(4096)
    except OSError as error:
        return f"could not read the PE header ({error})"
    if len(head) < 64 or head[:2] != b"MZ":
        return "is not a PE file"
    pe_offset = int.from_bytes(head[60:64], "little")
    if pe_offset + 24 > len(head) or head[pe_offset:pe_offset + 4] != b"PE\0\0":
        return "has no valid PE signature"
    machine = int.from_bytes(head[pe_offset + 4:pe_offset + 6], "little")
    characteristics = int.from_bytes(
        head[pe_offset + 22:pe_offset + 24], "little"
    )
    if machine != 0x8664:
        return f"targets PE machine 0x{machine:04X}, not AMD64"
    if not characteristics & 0x2000:
        return "is a PE image but not a DLL"
    return ""


def _powershell():
    system_root = os.environ.get("SystemRoot", r"C:\Windows")
    return os.path.join(
        system_root,
        "System32",
        "WindowsPowerShell",
        "v1.0",
        "powershell.exe",
    )


def verify_authenticode(path):
    """Return "" only for the pinned, valid Intel signing certificate."""
    if os.name != "nt":
        return "Authenticode validation is available only on Windows"
    powershell = _powershell()
    if not os.path.isfile(powershell):
        return f"Windows PowerShell was not found at {powershell}"
    script = (
        "& { param([string]$Path) "
        "$module=Join-Path $env:SystemRoot "
        "'System32\\WindowsPowerShell\\v1.0\\Modules\\"
        "Microsoft.PowerShell.Security\\Microsoft.PowerShell.Security.psd1';"
        "Import-Module $module -ErrorAction Stop;"
        "$s=Get-AuthenticodeSignature -LiteralPath $Path;"
        "[Console]::Out.WriteLine([string]$s.Status);"
        "[Console]::Out.WriteLine([string]$s.SignerCertificate.Subject);"
        "[Console]::Out.WriteLine([string]$s.SignerCertificate.Thumbprint)"
        " }"
    )
    try:
        completed = subprocess.run(
            [
                powershell,
                "-NoProfile",
                "-NonInteractive",
                "-Command",
                script,
                path,
            ],
            check=False,
            capture_output=True,
            text=True,
            timeout=30,
        )
    except (OSError, subprocess.SubprocessError) as error:
        return f"could not run Authenticode validation ({error})"
    if completed.returncode:
        detail = (completed.stderr or completed.stdout).strip()
        return f"Authenticode validation failed ({detail or completed.returncode})"
    lines = completed.stdout.splitlines()
    if len(lines) != 3:
        return "Authenticode validation returned unreadable output"
    result = {
        "Status": lines[0],
        "Subject": lines[1],
        "Thumbprint": lines[2],
    }
    return authenticode_problem(result)


def authenticode_problem(result):
    status = result.get("Status", "")
    subject = result.get("Subject", "")
    thumbprint = result.get("Thumbprint", "").replace(" ", "").upper()
    if status != "Valid":
        return f"Authenticode status is {status or 'missing'}, not Valid"
    if SIGNER_SUBJECT not in subject:
        return f"Authenticode signer is {subject or 'missing'}, not Intel Corporation"
    if thumbprint != SIGNER_THUMBPRINT:
        return (
            f"signing certificate thumbprint is {thumbprint or 'missing'}, "
            f"not {SIGNER_THUMBPRINT}"
        )
    return ""


def _version_tuple(value):
    parts = [int(part) for part in re.findall(r"\d+", value or "")]
    return tuple(parts[:4])


def vc_runtime_problem():
    """Return "" when the installed x64 VC runtime is at least 14.40."""
    if os.name != "nt":
        return "the Microsoft Visual C++ x64 runtime can be checked only on Windows"
    try:
        import winreg

        access = winreg.KEY_READ | getattr(winreg, "KEY_WOW64_64KEY", 0)
        with winreg.OpenKey(
            winreg.HKEY_LOCAL_MACHINE,
            r"SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64",
            0,
            access,
        ) as key:
            installed = int(winreg.QueryValueEx(key, "Installed")[0])
            version = str(winreg.QueryValueEx(key, "Version")[0])
        if installed and _version_tuple(version) >= MIN_VC_RUNTIME:
            return ""
        found = version or "unknown"
    except (OSError, TypeError, ValueError):
        found = "not installed"
    return (
        "Microsoft Visual C++ Redistributable x64 14.40 or later is required "
        f"(found: {found}). Install it from {VC_REDIST_URL}"
    )


def validate_runtime(folder, signature_check=verify_authenticode):
    for name, pin in FILES.items():
        problem = pinned_problem(os.path.join(folder, name), pin, name)
        if problem:
            return problem
    dll = os.path.join(folder, "libxess.dll")
    problem = pe_x64_problem(dll)
    if problem:
        return f"libxess.dll {problem}"
    problem = signature_check(dll)
    return f"libxess.dll {problem}" if problem else ""


def download_archive(url, destination, timeout=60):
    request = urllib.request.Request(
        url,
        headers={"User-Agent": "Project-Helghast-XeSS-Installer/1"},
    )
    total = 0
    with urllib.request.urlopen(request, timeout=timeout) as response:
        with open(destination, "wb") as target:
            while True:
                chunk = response.read(1 << 20)
                if not chunk:
                    break
                total += len(chunk)
                if total > ARCHIVE_SIZE:
                    raise ValueError(
                        f"download exceeded the pinned {ARCHIVE_SIZE:,} bytes"
                    )
                target.write(chunk)
    if total != ARCHIVE_SIZE:
        raise ValueError(
            f"downloaded {total:,} bytes; expected {ARCHIVE_SIZE:,}"
        )


def _extract_member(archive, member, destination, pin):
    try:
        info = archive.getinfo(member)
    except KeyError:
        raise ValueError(f"archive is missing {member}") from None
    if info.is_dir() or info.flag_bits & 1:
        raise ValueError(f"archive member {member} is not a plain unencrypted file")
    if info.file_size != pin["size"]:
        raise ValueError(
            f"archive member {member} is {info.file_size:,} bytes; "
            f"expected {pin['size']:,}"
        )
    digest = hashlib.sha256()
    total = 0
    with archive.open(info) as source, open(destination, "wb") as target:
        while True:
            chunk = source.read(1 << 20)
            if not chunk:
                break
            total += len(chunk)
            if total > pin["size"]:
                raise ValueError(f"archive member {member} exceeded its pinned size")
            digest.update(chunk)
            target.write(chunk)
    if total != pin["size"] or digest.hexdigest() != pin["sha256"]:
        raise ValueError(f"archive member {member} does not match its pinned SHA-256")


def stage_archive(archive_path, stage, signature_check=verify_authenticode):
    if os.path.getsize(archive_path) != ARCHIVE_SIZE:
        raise ValueError(
            f"archive is {os.path.getsize(archive_path):,} bytes; "
            f"expected {ARCHIVE_SIZE:,}"
        )
    digest = sha256(archive_path)
    if digest != ARCHIVE_SHA256:
        raise ValueError(
            f"archive SHA-256 is {digest}; expected {ARCHIVE_SHA256}"
        )
    try:
        with zipfile.ZipFile(archive_path) as archive:
            for name, pin in FILES.items():
                _extract_member(
                    archive,
                    pin["member"],
                    os.path.join(stage, name),
                    pin,
                )
    except zipfile.BadZipFile as error:
        raise ValueError(f"download is not a valid ZIP ({error})") from error
    problem = validate_runtime(stage, signature_check)
    if problem:
        raise ValueError(problem)


def install_staged(stage, destination):
    """Replace notices first and the validated DLL last, rolling back on error."""
    os.makedirs(destination, exist_ok=True)
    backup = tempfile.mkdtemp(prefix=".xess-backup-", dir=destination)
    replaced = []
    try:
        for name in FILES:
            old = os.path.join(destination, name)
            if os.path.lexists(old):
                os.replace(old, os.path.join(backup, name))
        order = [name for name in FILES if name != "libxess.dll"]
        order.append("libxess.dll")
        for name in order:
            os.replace(os.path.join(stage, name), os.path.join(destination, name))
            replaced.append(name)
    except OSError:
        for name in reversed(replaced):
            path = os.path.join(destination, name)
            if os.path.lexists(path):
                os.remove(path)
        for name in FILES:
            old = os.path.join(backup, name)
            if os.path.lexists(old):
                os.replace(old, os.path.join(destination, name))
        raise
    finally:
        shutil.rmtree(backup, ignore_errors=True)


def _ini_values(path):
    values = {}
    section = ""
    try:
        with open(path, encoding="utf-8-sig") as source:
            lines = source
            for line in lines:
                text = line.strip()
                if not text or text[0] in ";#":
                    continue
                if text.startswith("[") and "]" in text:
                    section = text[1:text.index("]")].strip().lower()
                    continue
                if section == "display" and "=" in text:
                    key, value = (part.strip() for part in text.split("=", 1))
                    values.setdefault(key.lower(), value.lower())
    except (OSError, UnicodeError):
        pass
    return values


def selected_fallback(display_ini):
    values = _ini_values(display_ini)
    if values.get("upscaler") != "xess":
        return None
    fallback = values.get("upscaler_previous", "none")
    return fallback if fallback in ("none", "fsr3") else "none"


def prepare_selected(
    display_ini,
    destination,
    ui,
    ini_set,
    download=download_archive,
    signature_check=verify_authenticode,
    vc_check=vc_runtime_problem,
):
    """Ensure selected XeSS is usable. Return (state, detail, fallback)."""
    fallback = selected_fallback(display_ini)
    if fallback is None:
        return "not-selected", "", ""

    problem = validate_runtime(destination, signature_check)
    vc_problem = vc_check()
    if not problem and not vc_problem:
        return "ready", f"Intel XeSS SDK {SDK_VERSION} runtime verified", fallback
    if vc_problem:
        ini_set(display_ini, [("display", "upscaler", fallback)])
        return "failed", vc_problem, fallback

    consent = (
        f"XeSS needs Intel's optional XeSS SDK {SDK_VERSION} runtime.\n\n"
        f"Download {ARCHIVE_SIZE / (1024 * 1024):.1f} MiB from Intel's "
        f"official GitHub release?\n\n{ARCHIVE_URL}\n\n"
        "The launcher will verify the archive SHA-256, every installed file, "
        "the 64-bit DLL type, and Intel's Authenticode certificate before "
        "placing the DLL, licence, and notices beside the game. Nothing is "
        "downloaded unless you choose Yes."
    )
    if not ui.ask(consent):
        ini_set(display_ini, [("display", "upscaler", fallback)])
        return "declined", "download was declined", fallback

    os.makedirs(destination, exist_ok=True)
    stage = tempfile.mkdtemp(prefix=".xess-stage-", dir=destination)
    archive_path = os.path.join(stage, "XeSS_SDK_3.0.2.zip")
    try:
        download(ARCHIVE_URL, archive_path, 60)
        stage_archive(archive_path, stage, signature_check)
        os.remove(archive_path)
        install_staged(stage, destination)
    except (OSError, RuntimeError, ValueError, zipfile.BadZipFile) as error:
        ini_set(display_ini, [("display", "upscaler", fallback)])
        return "failed", str(error), fallback
    finally:
        shutil.rmtree(stage, ignore_errors=True)
    return "installed", f"Intel XeSS SDK {SDK_VERSION} runtime installed", fallback
