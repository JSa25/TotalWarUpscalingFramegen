#!/usr/bin/env python3
"""Capture a human-guided Attila gameplay session without controlling the desktop."""
import argparse
import datetime as dt
import hashlib
import json
from pathlib import Path
import select
import shutil
import sys
import time


def stamp():
    return dt.datetime.now(dt.timezone.utc).isoformat()


def prompt(question, timeout):
    print(question, flush=True)
    if not select.select([sys.stdin], [], [], timeout)[0]:
        raise TimeoutError
    answer = sys.stdin.readline()
    if not answer:
        raise EOFError
    return answer.strip()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', required=True, type=Path, help='new per-run directory')
    parser.add_argument('--evidence', action='append', default=[], type=Path,
                        help='file to copy before and after the run; repeat as needed')
    parser.add_argument('--timeout', type=int, default=600, help='seconds allowed per state (default: 600)')
    args = parser.parse_args()
    if args.timeout <= 0:
        parser.error('--timeout must be positive')
    try:
        args.output.mkdir(parents=True, exist_ok=False)
    except OSError as exc:
        parser.error(f'cannot create new output directory: {exc}')
    record = {'started_utc': stamp(), 'events': [], 'evidence': [], 'outcome': 'incomplete'}
    def save():
        (args.output / 'session.json').write_text(json.dumps(record, indent=2) + '\n')
    def event(state, response):
        record['events'].append({'time_utc': stamp(), 'state': state, 'response': response})
        save()
    def capture(stage):
        dest = args.output / stage
        dest.mkdir()
        for i, path in enumerate(args.evidence):
            entry = {'stage': stage, 'source': str(path)}
            try:
                target = dest / f'{i:02d}-{path.name}'
                shutil.copyfile(path, target)
                with open(target, 'rb') as hashed:
                    entry.update(copy=str(target.relative_to(args.output)),
                                 sha256=hashlib.file_digest(hashed, 'sha256').hexdigest())
            except OSError as exc:
                entry['error'] = str(exc)
            record['evidence'].append(entry)
        save()
    save()
    try:
        print('No game input is sent. Keep existing launch options, Proton configuration and mods unchanged.')
        print('Use this only with access to the active desktop. Enter a response for every prompt.')
        for name, question in [
            ('configuration', 'Record Proton version, launch options, mods, settings and binary versions/hashes:'),
            ('scenario', 'Record campaign, faction, difficulty, battle and planned gameplay duration:'),
            ('diagnostic', 'Record display backend/timing changes, or enter baseline:')]:
            answer = prompt(question, args.timeout)
            if not answer:
                raise ValueError('empty observation')
            event(name, answer)
        capture('before')
        for state, instruction in [
            ('menu', 'Launch normally and reach the main menu.'),
            ('campaign', 'Load the recorded campaign and reach campaign gameplay.'),
            ('battle', 'Launch the recorded battle and reach battle gameplay.'),
            ('observation', 'Play for the recorded duration and observe the result.')]:
            answer = prompt(f'{instruction}\nState {state}: enter ok, unexpected, or crash:', args.timeout).lower()
            event(state, answer)
            if answer == 'crash':
                record['outcome'] = 'reported_game_crash'
                break
            if answer != 'ok':
                record['outcome'] = 'automation_or_operator_incomplete'
                break
        else:
            record['outcome'] = 'completed_observation'
    except TimeoutError:
        event('stopped', 'timeout')
        record['outcome'] = 'automation_or_operator_incomplete'
    except (EOFError, KeyboardInterrupt, ValueError) as exc:
        event('stopped', type(exc).__name__)
        record['outcome'] = 'automation_or_operator_incomplete'
    finally:
        capture('after')
        record['finished_utc'] = stamp()
        save()
    print(f"Outcome: {record['outcome']} (observations are user-reported); evidence: {args.output}")
    return 0 if record['outcome'] == 'completed_observation' else 1


if __name__ == '__main__':
    sys.exit(main())
