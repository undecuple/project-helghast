#!/usr/bin/env python3
"""Host tests for the consented, pinned XeSS runtime installer."""
import hashlib
import os
import shutil
import tempfile
import zipfile

import xess_runtime as xr


FAILS = []


def check(condition, message, detail=""):
    print(("ok   " if condition else "FAIL ") + message)
    if not condition:
        FAILS.append(message + (f": {detail}" if detail else ""))


class FakeUI:
    def __init__(self, answers=()):
        self.answers = list(answers)
        self.asked = []

    def ask(self, text):
        self.asked.append(text)
        return self.answers.pop(0) if self.answers else False


def put(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as target:
        target.write(data)


def fake_pe():
    data = bytearray(256)
    data[:2] = b"MZ"
    data[60:64] = (64).to_bytes(4, "little")
    data[64:68] = b"PE\0\0"
    data[68:70] = (0x8664).to_bytes(2, "little")
    data[86:88] = (0x2000).to_bytes(2, "little")
    return bytes(data)


def set_display(path, upscaler, previous="none"):
    with open(path, "w", encoding="utf-8") as target:
        target.write(
            "[display]\n"
            f"upscaler = {upscaler}\n"
            f"upscaler_previous = {previous}\n"
            "filter = nearest\n"
        )


def ini_set(path, items):
    text = open(path, encoding="utf-8").read()
    for _, key, value in items:
        lines = text.splitlines()
        for index, line in enumerate(lines):
            if line.split("=", 1)[0].strip().lower() == key:
                lines[index] = f"{key} = {value}"
        text = "\n".join(lines) + "\n"
    with open(path, "w", encoding="utf-8") as target:
        target.write(text)


def configure_fixture(archive_path):
    members = {
        "libxess.dll": ("bin/libxess.dll", fake_pe()),
        "Intel-XeSS-LICENSE.txt": ("LICENSE.txt", b"Intel licence\n"),
        "Intel-XeSS-THIRD-PARTY-NOTICES.txt": (
            "third-party-programs.txt",
            b"Intel notices\n",
        ),
    }
    xr.FILES = {
        name: {
            "member": member,
            "size": len(data),
            "sha256": hashlib.sha256(data).hexdigest(),
        }
        for name, (member, data) in members.items()
    }
    with zipfile.ZipFile(archive_path, "w", zipfile.ZIP_DEFLATED) as archive:
        for _, (member, data) in members.items():
            archive.writestr(member, data)
    xr.ARCHIVE_SIZE = os.path.getsize(archive_path)
    xr.ARCHIVE_SHA256 = xr.sha256(archive_path)


def main():
    temp = tempfile.mkdtemp(prefix="kz3-xess-runtime-")
    original = xr.ARCHIVE_SIZE, xr.ARCHIVE_SHA256, xr.FILES
    try:
        source_archive = os.path.join(temp, "official.zip")
        configure_fixture(source_archive)
        display = os.path.join(temp, "display.ini")
        destination = os.path.join(temp, "build")
        calls = []

        def download(url, output, timeout):
            calls.append((url, timeout))
            shutil.copyfile(source_archive, output)

        valid_signature = lambda path: ""  # noqa: E731
        valid_vc = lambda: ""  # noqa: E731

        set_display(display, "none")
        ui = FakeUI()
        state, _, _ = xr.prepare_selected(
            display,
            destination,
            ui,
            ini_set,
            download,
            valid_signature,
            valid_vc,
        )
        check(
            state == "not-selected" and not ui.asked and not calls,
            "nothing is downloaded or prompted when XeSS is not selected",
        )

        set_display(display, "xess", "fsr3")
        ui = FakeUI([False])
        state, detail, fallback = xr.prepare_selected(
            display,
            destination,
            ui,
            ini_set,
            download,
            valid_signature,
            valid_vc,
        )
        check(
            state == "declined"
            and fallback == "fsr3"
            and "declined" in detail
            and xr.selected_fallback(display) is None
            and not calls,
            "declining consent performs no network access and restores FSR 3.1",
            detail,
        )
        check(
            xr.ARCHIVE_URL in ui.asked[0]
            and "Nothing is downloaded unless you choose Yes" in ui.asked[0]
            and "SHA-256" in ui.asked[0],
            "consent names the source, size, installed files, and validation",
        )

        set_display(display, "xess", "none")
        ui = FakeUI([True])

        def network_failure(*_):
            calls.append("network")
            raise OSError("offline")

        state, detail, fallback = xr.prepare_selected(
            display,
            destination,
            ui,
            ini_set,
            network_failure,
            valid_signature,
            valid_vc,
        )
        check(
            state == "failed"
            and "offline" in detail
            and fallback == "none"
            and xr.selected_fallback(display) is None,
            "network failure restores the prior upscaler",
            detail,
        )

        set_display(display, "xess", "fsr3")
        ui = FakeUI([True])

        def corrupt_download(url, output, timeout):
            put(output, b"not the pinned archive")

        state, detail, _ = xr.prepare_selected(
            display,
            destination,
            ui,
            ini_set,
            corrupt_download,
            valid_signature,
            valid_vc,
        )
        check(
            state == "failed"
            and ("bytes" in detail or "SHA-256" in detail)
            and xr.selected_fallback(display) is None,
            "archive size or hash failure installs nothing and restores fallback",
            detail,
        )

        set_display(display, "xess", "none")
        ui = FakeUI([True])
        state, detail, _ = xr.prepare_selected(
            display,
            destination,
            ui,
            ini_set,
            download,
            lambda path: "signer is not Intel Corporation",
            valid_vc,
        )
        check(
            state == "failed"
            and "not Intel" in detail
            and not os.path.exists(os.path.join(destination, "libxess.dll")),
            "signature failure installs no DLL",
            detail,
        )

        calls.clear()
        set_display(display, "xess", "fsr3")
        ui = FakeUI([True])
        vc_problem = (
            "Microsoft Visual C++ Redistributable x64 14.40 or later "
            "is required"
        )
        state, detail, fallback = xr.prepare_selected(
            display,
            destination,
            ui,
            ini_set,
            download,
            valid_signature,
            lambda: vc_problem,
        )
        check(
            state == "failed"
            and "14.40" in detail
            and fallback == "fsr3"
            and not ui.asked
            and not calls,
            "missing VC++ 14.40 is diagnosed before consent or network access",
            detail,
        )

        set_display(display, "xess", "fsr3")
        ui = FakeUI([True])
        state, detail, fallback = xr.prepare_selected(
            display,
            destination,
            ui,
            ini_set,
            download,
            valid_signature,
            valid_vc,
        )
        check(
            state == "installed"
            and fallback == "fsr3"
            and all(os.path.isfile(os.path.join(destination, name)) for name in xr.FILES)
            and xr.selected_fallback(display) == "fsr3",
            "consented valid archive installs the DLL, licence, and notices",
            detail,
        )

        calls.clear()
        ui = FakeUI()
        state, detail, _ = xr.prepare_selected(
            display,
            destination,
            ui,
            ini_set,
            download,
            valid_signature,
            valid_vc,
        )
        check(
            state == "ready" and not ui.asked and not calls,
            "an installed pinned runtime is revalidated without network access",
            detail,
        )

        check(
            not xr.authenticode_problem(
                {
                    "Status": "Valid",
                    "Subject": xr.SIGNER_SUBJECT
                    + ", O=Intel Corporation, S=California, C=US",
                    "Thumbprint": xr.SIGNER_THUMBPRINT.lower(),
                }
            )
            and "status" in xr.authenticode_problem(
                {
                    "Status": "HashMismatch",
                    "Subject": xr.SIGNER_SUBJECT,
                    "Thumbprint": xr.SIGNER_THUMBPRINT,
                }
            )
            and "thumbprint" in xr.authenticode_problem(
                {
                    "Status": "Valid",
                    "Subject": xr.SIGNER_SUBJECT,
                    "Thumbprint": "00",
                }
            ),
            "Authenticode status, Intel signer, and certificate thumbprint are pinned",
        )

        bad_pe = os.path.join(temp, "bad.dll")
        put(bad_pe, b"MZ")
        check(
            "PE" in xr.pe_x64_problem(bad_pe),
            "truncated or wrong-architecture DLLs are refused",
        )
    finally:
        xr.ARCHIVE_SIZE, xr.ARCHIVE_SHA256, xr.FILES = original
        shutil.rmtree(temp, ignore_errors=True)
    print(
        "test_xess_runtime: "
        + ("PASS" if not FAILS else "FAIL " + "; ".join(FAILS))
    )
    return 1 if FAILS else 0


if __name__ == "__main__":
    raise SystemExit(main())
