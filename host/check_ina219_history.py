#!/usr/bin/env python3
"""Hardware check of INA219 averaging/history; restores interval on exit.

Temporarily changes the interval and waits for the 200-entry ring to wrap.
Completed history records are overwritten by this test.
"""
import argparse
import json
import time
from check_wifi import Console


def objects(text):
    return [json.loads(line) for line in text.splitlines() if line.startswith('{')]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('host')
    args = parser.parse_args()
    console = Console(args.host, 4440)
    original = objects(console.command('ina219 interval'))[0]['interval_ms']
    try:
        assert objects(console.command('ina219 interval 1000'))[0]['interval_ms'] == 1000
        time.sleep(2.3)
        rows = objects(console.command('ina219 history 2'))[1:]
        assert len(rows) == 2
        for row in rows:
            assert row['interval_ms'] == 1000 and row['samples'] > 0
            assert row['end_ms'] - row['start_ms'] == 1000
            assert row['timestamp_ms'] >= row['end_ms']
            assert row['errors'] == 0
        print('1-second averages:', rows)
        for value in ('0', '-1', '99', '3600001', '999999999999999999999999', '1x'):
            assert 'Usage:' in console.command('ina219 interval ' + value)
        for value in ('0', '201', '-1'):
            assert 'Usage:' in console.command('ina219 history ' + value)
        assert objects(console.command('ina219 interval'))[0]['interval_ms'] == 1000
        print('Invalid inputs rejected without changing interval: PASS')
        console.command('ina219 interval 100')
        print('Waiting 21 seconds to wrap the 200-record ring...', flush=True)
        time.sleep(21)
        snapshot = objects(console.command('ina219 history'))
        header, rows = snapshot[0], snapshot[1:]
        assert header['count'] == 200 and len(rows) == 200
        assert header['timebase'] == 'uptime_ms'
        for i, row in enumerate(rows):
            assert row['interval_ms'] == 100
            assert row['end_ms'] - row['start_ms'] == 100
            assert row['timestamp_ms'] >= row['end_ms']
            assert row['samples'] > 0 and row['errors'] == 0
            for field in ('bus_voltage_v', 'shunt_voltage_mv', 'current_ma', 'power_mw'):
                assert isinstance(row[field], (float, int))
            if i:
                assert row['sequence'] == rows[i-1]['sequence'] + 1
                assert row['start_ms'] == rows[i-1]['end_ms']
        # Reading does not consume records; a subsequent snapshot stays full.
        assert objects(console.command('ina219 history'))[0]['count'] == 200
        newest = objects(console.command('ina219 history 3'))
        assert newest[0]['count'] == 3 and len(newest) == 4
        assert newest[1]['sequence'] >= rows[-3]['sequence']
        print('Ring capacity, overwrite order, timestamps and non-destructive reads: PASS')
        print('Sequences:', rows[0]['sequence'], '..', rows[-1]['sequence'])
    finally:
        print('Restore interval:', console.command('ina219 interval ' + str(original)))
        console.close()


if __name__ == '__main__':
    main()
