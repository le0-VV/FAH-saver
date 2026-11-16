"""Validate aggregate diagnostics from a completed bounded Vulkan capture trial."""

import copy
import json
import math
import sys


def check(condition, message):
    if not condition:
        raise ValueError(message)


def nonnegative(value):
    return isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(value) and value >= 0


def validate(report):
    check(report.get('backend') == 'vulkan', 'Expected a Vulkan report')
    check(nonnegative(report['elapsed_seconds']) and report['elapsed_seconds'] > 0, 'Invalid elapsed time')
    frames = report['submitted_frames']
    check(isinstance(frames, int) and frames > 0, 'Trial did not submit any frames')
    check(nonnegative(report['cpu_process_seconds']), 'Invalid process CPU time')
    diagnostics = report['diagnostics']
    main_cpu = diagnostics['main_thread_cpu_seconds']
    check(nonnegative(main_cpu), 'Invalid main-thread CPU time')
    check(main_cpu <= report['cpu_process_seconds'] + .05, 'Main-thread CPU exceeds process CPU interval')
    for field in ('pump_iterations', 'messages_dispatched', 'render_calls', 'timer_wakes', 'input_queue_wakes'):
        check(isinstance(diagnostics[field], int) and diagnostics[field] >= 0, f'Invalid counter: {field}')
    check(diagnostics['pump_iterations'] >= diagnostics['timer_wakes'] + diagnostics['input_queue_wakes'],
          'Wake counts exceed pump iterations')
    check(diagnostics['pump_iterations'] >= diagnostics['render_calls'] >= frames, 'Inconsistent render-call count')
    phases = diagnostics['phase_totals']
    for field in ('scene_build_upload', 'fence_query_wait', 'acquire', 'record_submit', 'present'):
        phase = phases[field]
        check(isinstance(phase['calls'], int) and phase['calls'] >= 0, f'Invalid phase count: {field}')
        check(nonnegative(phase['wall_ms']) and nonnegative(phase['main_thread_cpu_ms']), f'Invalid phase timing: {field}')
    check(phases['present']['calls'] == phases['record_submit']['calls'] == frames, 'Submission/present counts differ')
    check(phases['fence_query_wait']['calls'] >= phases['acquire']['calls'] >= frames, 'Inconsistent wait/acquire count')
    check(sum(p['wall_ms'] for p in phases.values()) <= report['elapsed_seconds'] * 1000 + 50,
          'Measured phase walls exceed trial interval')
    check(sum(p['main_thread_cpu_ms'] for p in phases.values()) <= main_cpu * 1000 + 50,
          'Measured phase CPU exceeds main-thread CPU interval')


def self_test():
    report = {'backend': 'vulkan', 'elapsed_seconds': 1, 'submitted_frames': 60, 'cpu_process_seconds': .2,
              'diagnostics': {'main_thread_cpu_seconds': .1, 'pump_iterations': 61, 'messages_dispatched': 2,
                              'render_calls': 60, 'timer_wakes': 59, 'input_queue_wakes': 1,
                              'phase_totals': {name: {'calls': 60, 'wall_ms': 1, 'main_thread_cpu_ms': 1}
                                               for name in ('scene_build_upload', 'fence_query_wait', 'acquire', 'record_submit', 'present')}}}
    validate(report)
    invalid = copy.deepcopy(report)
    invalid['diagnostics']['phase_totals']['acquire']['wall_ms'] = float('nan')
    try:
        validate(invalid)
    except ValueError:
        pass
    else:
        raise AssertionError('Non-finite diagnostics accepted')
    invalid = copy.deepcopy(report)
    invalid['diagnostics']['phase_totals']['present']['calls'] = 59
    try:
        validate(invalid)
    except ValueError:
        pass
    else:
        raise AssertionError('Missing presentation call accepted')
    print('Diagnostic report validator self-check passed')


if __name__ == '__main__':
    if len(sys.argv) == 2 and sys.argv[1] == '--self-test':
        self_test()
    elif len(sys.argv) == 2:
        with open(sys.argv[1], encoding='utf-8') as source:
            validate(json.load(source))
        print('Vulkan aggregate diagnostics passed consistency checks')
    else:
        raise SystemExit('Usage: check_diagnostics.py FILE.json | --self-test')
