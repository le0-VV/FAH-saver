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


def close(a, b):
    return math.isclose(a, b, rel_tol=1e-6, abs_tol=1e-6)


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
    check(all(name in report for name in ('lifecycle_interval', 'active_interval', 'shutdown_interval', 'submission_call_cadence')),
          'Missing interval scopes; older lifecycle-only reports need their bundled checker')
    life, active, shutdown = (report[name] for name in ('lifecycle_interval', 'active_interval', 'shutdown_interval'))
    for interval in (life, active, shutdown):
        for field in ('elapsed_seconds', 'cpu_process_seconds', 'main_thread_cpu_seconds'):
            check(nonnegative(interval[field]), f'Invalid scoped timing: {field}')
        check(interval['main_thread_cpu_seconds'] <= interval['cpu_process_seconds'] + .05,
              'Scoped thread CPU exceeds process CPU')
    check(close(report['elapsed_seconds'], life['elapsed_seconds']) and
          close(report['cpu_process_seconds'], life['cpu_process_seconds']) and
          close(main_cpu, life['main_thread_cpu_seconds']) and
          diagnostics['main_thread_cpu_scope'] == 'lifecycle_interval', 'Legacy totals have incompatible interval scope')
    for field in ('elapsed_seconds', 'cpu_process_seconds', 'main_thread_cpu_seconds'):
        check(active[field] + shutdown[field] <= life[field] + .05, f'Active/shutdown exceed lifecycle: {field}')
    cadence = report['submission_call_cadence']
    first, last = cadence['first_seconds_after_active_start'], cadence['last_seconds_after_active_start']
    check(nonnegative(first) and nonnegative(last) and first <= last <= active['elapsed_seconds'] + 1e-6,
          'Submission timestamps exceed the active interval')
    check(cadence['interval_count'] == frames - 1, 'Wrong submission interval count')
    if frames > 1:
        check(last > first and close(cadence['interval_seconds'], last - first) and
              close(cadence['calls_per_second'], (frames - 1) / (last - first)), 'Invalid submit-call cadence')
    else:
        check(first == last and cadence['interval_seconds'] is None and cadence['calls_per_second'] is None,
              'A single submission cannot establish cadence')
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
    check(sum(p['wall_ms'] for p in phases.values()) <= active['elapsed_seconds'] * 1000 + 50,
          'Measured phase walls exceed active interval')
    check(sum(p['main_thread_cpu_ms'] for p in phases.values()) <= active['main_thread_cpu_seconds'] * 1000 + 50,
          'Measured phase CPU exceeds active main-thread CPU interval')


def self_test():
    report = {'backend': 'vulkan', 'elapsed_seconds': 1, 'submitted_frames': 60, 'cpu_process_seconds': .2,
              'lifecycle_interval': {'elapsed_seconds': 1, 'cpu_process_seconds': .2, 'main_thread_cpu_seconds': .1},
              'active_interval': {'elapsed_seconds': .9, 'cpu_process_seconds': .18, 'main_thread_cpu_seconds': .09},
              'shutdown_interval': {'elapsed_seconds': .05, 'cpu_process_seconds': .01, 'main_thread_cpu_seconds': .005},
              'submission_call_cadence': {'first_seconds_after_active_start': .01, 'last_seconds_after_active_start': .89,
                                          'interval_count': 59, 'interval_seconds': .88, 'calls_per_second': 59/.88},
              'diagnostics': {'main_thread_cpu_seconds': .1, 'main_thread_cpu_scope': 'lifecycle_interval',
                              'pump_iterations': 61, 'messages_dispatched': 2,
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
    for interval, field, value in (('active_interval', 'elapsed_seconds', 1.1),
                                   ('shutdown_interval', 'cpu_process_seconds', .2),
                                   ('lifecycle_interval', 'elapsed_seconds', .9),
                                   ('submission_call_cadence', 'calls_per_second', 60),
                                   ('submission_call_cadence', 'last_seconds_after_active_start', .95)):
        invalid = copy.deepcopy(report)
        invalid[interval][field] = value
        try:
            validate(invalid)
        except ValueError:
            pass
        else:
            raise AssertionError(f'Inconsistent scope accepted: {interval}.{field}')
    single = copy.deepcopy(report)
    single['submitted_frames'] = 1
    for phase in single['diagnostics']['phase_totals'].values():
        phase['calls'] = 1
    single['submission_call_cadence'] = {'first_seconds_after_active_start': .1, 'last_seconds_after_active_start': .1,
                                         'interval_count': 0, 'interval_seconds': None, 'calls_per_second': None}
    validate(single)
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
