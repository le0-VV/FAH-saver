"""Check the bounded, incomplete CPU series emitted by the WebView2 benchmark."""

import copy
import json
import math
import sys


def check(condition, message):
    if not condition:
        raise ValueError(message)


def counter(value):
    return isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(value) and value >= 0


def validate(report):
    check(report['host'] == 'WebView2' and report['complete_process_cost_measured'] is False,
          'CPU series must retain its incomplete-cost qualification')
    check(report['cpu_sample_series_limit'] == 8192, 'Unexpected CPU series limit')
    dropped = report['cpu_sample_series_dropped']
    check(type(dropped) is int and dropped >= 0, 'Invalid dropped-sample count')
    samples = report['cpu_sample_series']
    check(isinstance(samples, list) and 0 < len(samples) <= 8192, 'Invalid CPU series size')
    check(not dropped or len(samples) == 8192, 'Samples dropped before the series reached its limit')
    elapsed = host_cpu = browser_cpu = 0
    observed = 0
    for sample in samples:
        for key in ('inventory_available', 'inventory_live', 'process_metric_access_or_sampling_failures'):
            check(type(sample[key]) is bool, f'Invalid sample flag: {key}')
        check(counter(sample['elapsed_seconds']) and sample['elapsed_seconds'] >= elapsed, 'Sample time decreased')
        check(counter(sample['host_cpu_seconds_since_start']) and sample['host_cpu_seconds_since_start'] >= host_cpu,
              'Host CPU counter decreased')
        count = sample['observed_browser_process_count']
        check(type(count) is int and count >= observed, 'Retained process count decreased')
        value = sample['observed_browser_cpu_seconds_since_creation']
        if sample['inventory_available']:
            check(counter(value) and value >= browser_cpu, 'Observed browser CPU counter decreased')
            browser_cpu = value
        else:
            check(value is None and not sample['inventory_live'], 'Unavailable inventory reported browser CPU')
        elapsed, host_cpu, observed = sample['elapsed_seconds'], sample['host_cpu_seconds_since_start'], count


def self_test():
    report = {'host': 'WebView2', 'complete_process_cost_measured': False, 'cpu_sample_series_limit': 8192,
              'cpu_sample_series_dropped': 0, 'cpu_sample_series': [
                  {'elapsed_seconds': .5, 'host_cpu_seconds_since_start': .1,
                   'observed_browser_cpu_seconds_since_creation': .2, 'observed_browser_process_count': 3,
                   'inventory_available': True, 'inventory_live': True, 'process_metric_access_or_sampling_failures': False},
                  {'elapsed_seconds': 1, 'host_cpu_seconds_since_start': .15,
                   'observed_browser_cpu_seconds_since_creation': .3, 'observed_browser_process_count': 3,
                   'inventory_available': True, 'inventory_live': True, 'process_metric_access_or_sampling_failures': False}]}
    validate(report)
    unavailable = copy.deepcopy(report)
    unavailable['cpu_sample_series'][0].update(inventory_available=False, inventory_live=False,
                                              observed_browser_cpu_seconds_since_creation=None,
                                              observed_browser_process_count=0)
    validate(unavailable)
    capped = copy.deepcopy(report)
    capped['cpu_sample_series'] = [report['cpu_sample_series'][-1]] * 8192
    capped['cpu_sample_series_dropped'] = 2
    validate(capped)
    for field, value in (('elapsed_seconds', .1), ('host_cpu_seconds_since_start', .01),
                         ('observed_browser_cpu_seconds_since_creation', .1), ('observed_browser_process_count', 2)):
        invalid = copy.deepcopy(report)
        invalid['cpu_sample_series'][1][field] = value
        try:
            validate(invalid)
        except ValueError:
            pass
        else:
            raise AssertionError(f'Decreasing series accepted: {field}')
    invalid = copy.deepcopy(report)
    invalid['cpu_sample_series'] *= 4097
    try:
        validate(invalid)
    except ValueError:
        pass
    else:
        raise AssertionError('Oversized series accepted')
    print('WebView2 CPU series validator self-check passed')


if __name__ == '__main__':
    if len(sys.argv) == 2 and sys.argv[1] == '--self-test':
        self_test()
    elif len(sys.argv) == 2:
        with open(sys.argv[1], encoding='utf-8') as source:
            validate(json.load(source))
        print('WebView2 CPU series consistency checks passed; process cost remains incomplete')
    else:
        raise SystemExit('Usage: check_cpu_series.py FILE.json | --self-test')
