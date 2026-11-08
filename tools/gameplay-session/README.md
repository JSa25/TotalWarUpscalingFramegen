# Human-guided Attila gameplay sessions

This temporary, session-scoped runner records a repeatable campaign-to-battle observation. It does not launch the game, inject input, inspect the screen, or verify the player's answers. Desktop keyboard, pointer and captured-game input have not been verified for automation; no automated control should be assumed. A human must operate the game and have permission to access the desktop. The runner changes no game, desktop, launch, Proton or mod settings.

Start from a terminal on the machine used for gameplay, with a new output directory and the relevant existing logs or binaries supplied as evidence, for example:

```sh
python3 tools/gameplay-session/run.py --output /tmp/attila-run-01 \
  --evidence "$HOME/steam-325610.log" --timeout 900
```

Repeat `--evidence` for each log, settings file, or binary to preserve. Copies and SHA-256 hashes are captured before and after the run; missing or unreadable files are recorded as errors. Choose all relevant paths yourself, including the game and helper logs and binaries. The runner does not discover them. Copy any screenshots, crash dumps or other evidence into the run directory separately, without overwriting the captured files. Protect the directory as it can contain personal launch options and logs.

Record the actual Proton version, launch options, mods, settings, binary hashes, scenario and observation duration at the prompts. Use the same campaign, battle and duration for comparisons. Follow each instruction and enter `ok` only after reaching the named state. Enter `unexpected` for another screen or an input/control problem; enter `crash` only when a game crash is actually observed. The runner stops on any other response, end of input, interrupt or timeout. `session.json` preserves timestamps, responses, evidence and outcome; `completed_observation` means only that the operator confirmed each state, not that upscaling or frame generation worked. Inspect logs and independent visual evidence for those claims. `reported_game_crash` is a report, not independently verified; timeout, unexpected screen and control failure are classified as incomplete, not crashes.

Keep the original Proton version, launch options and unrelated mods for baseline runs. Record any alternate backend or timing stress as a separate diagnostic run. The runner cannot prevent changes made outside it and does not restore them. It does not automate gameplay or validate desktop input; further automation requires applicable consent and verified keyboard, pointer and captured-game input. A terminal prompt timeout is per state and includes time spent operating the game.
