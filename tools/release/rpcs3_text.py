"""Every player-facing string of the RPCS3 import (first-run wizard and portable text UI), in one place so the text can be
reviewed as a whole. rpcs3_import.py and firstrun.py only format these; they write no prose of their own.
`python3 rpcs3_text.py` prints all strings (the review copy). Plain words, short sentences."""

FOUND = "Found your RPCS3 folder: {root}"
FOUND_DISC = "  Game disc folder: {path}"
FOUND_UPDATE = "  Update {ver}: {path}"
NO_DISC = "  RPCS3 has no Killzone 3 disc game listed. You can type the folder yourself."
NO_UPDATE = "  RPCS3 has no update for Killzone 3. You can type the folder yourself."
WRONG_UPDATE = "  RPCS3 has update {have}, but the port needs {want}. Update the game in RPCS3 first."
USE_RPCS3 = "Use these folders as the starting answers?"

ENCRYPTED = ("The update's EBOOT.BIN is still encrypted, and there is no EBOOT.elf next to it.\n"
             "The port cannot decrypt it. It needs a decrypted EBOOT.elf, made from your own copy, in the same folder.")
STEPS = ("  The file: {path}\n"
         "  Put EBOOT.elf next to it, then come back here and check again.")
STILL_ENCRYPTED = "There is still no EBOOT.elf in: {folder}"
CHECK_AGAIN = "Press Enter to check again, c to change the folders, q to quit: "

SAVES_FOUND = "RPCS3 has {n} Killzone 3 save folder(s):"
SAVES_ITEM = "  {name} (user {user})"
SAVES_ASK = ("Copy these saves into the port? They are copied as they are, and whether the port can read them "
             "depends on the game. Your RPCS3 folder is only read, never changed. The port keeps its own copy.")
SAVES_NONE = "No Killzone 3 saves found in RPCS3. Nothing to copy."
SAVES_EXISTS = "The port already has a save named {name}. Replace it with the RPCS3 copy?"
SAVES_COPIED = "  Copied {name} ({n} files)."
SAVES_SKIPPED = "  Kept the port's {name}."
SAVES_FAILED = "  Could not copy {name}: {why}"
SAVES_DONE = "Saves copied: {copied}. Kept: {kept}."



def all_strings():
    return {k: v for k, v in sorted(globals().items()) if k.isupper() and isinstance(v, str)}


if __name__ == "__main__":
    for k, v in all_strings().items():
        print(f"{k} =\n{v}\n")
