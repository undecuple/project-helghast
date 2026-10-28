"""Every player-facing string of the support package, in one place so the text can be reviewed as a whole.
launch.py and support.py only format these. `python3 support_text.py` prints all strings (the review copy).
Plain words, short sentences."""

MENU_NAME = "Make a support package"            # the Start menu entry (installer/kz3.iss)
DONE = ("Your support package is ready:\n{path}\n\n"
        "It holds only logs and settings. Your user name is taken out of the folder paths in them. Attach it to a bug report: {issues}")
FAILED = "The support package could not be made: {why}"
NOTHING = "There were no logs to collect yet. Start the game once, then try again."
FACTS_TRIMMED = "Some logs were shortened (oldest lines first) to stay under {mb} MB: {names}."
FACTS_NOT_TRIMMED = "No log was shortened."

if __name__ == "__main__":
    for k, v in sorted(globals().items()):
        if k.isupper():
            print(f"{k}: {v}\n")
