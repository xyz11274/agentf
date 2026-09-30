"""Learn the completed official train preflight once; never rerun its Agent."""
from __future__ import annotations

import os
import time
import argparse
from pathlib import Path

import appworld_experiment as controller
from appworld_learning import freeze_seed


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--report', type=Path)
    parser.add_argument('--label', choices=('seed-pilot', 'seed0'), default='seed-pilot')
    args = parser.parse_args()
    destination = controller.TRAINING / 'pilot-learning'
    destination.mkdir(parents=True, exist_ok=True)
    status_path = destination / 'status.json'

    def state(**fields):
        value = controller.read(status_path) if status_path.exists() else {}
        value.update(fields, updated_at=time.time(), pid=os.getpid())
        controller.save(status_path, value)

    # Keep the main capture controller's heartbeat separate from this writer.
    controller.state = state
    source = args.report or controller.TRAINING / 'preflight/train/report.json'
    state(phase='learning', source_report=str(source), host_version=controller.HOST_VERSION,
          error='', learning=None)
    try:
        result = controller.learn(controller.read(source), source, controller.environment())
        state(phase='learning_complete', learning=result)
        if not result.get('usable'):
            state(phase='learning_unusable')
            return
        artifact = freeze_seed(controller, label=args.label)
        state(phase='compiled', strategy=str(controller.PUBLISHED / args.label / 'strategy.json'),
              version=controller.version(artifact))
    except Exception as exc:
        state(phase='failed', error=str(exc))
        raise


if __name__ == '__main__':
    main()
