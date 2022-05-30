# test case management tool - manual execution from testopia test cases
#
# Copyright (c) 2018, Intel Corporation.
#
# This program is free software; you can redistribute it and/or modify it
# under the terms and conditions of the GNU General Public License,
# version 2, as published by the Free Software Foundation.
#
# This program is distributed in the hope it will be useful, but WITHOUT
# ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
# FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
# more details.
#
import argparse
import json
import os
import sys
import datetime
import re
from oeqa.core.runner import OETestResultJSONHelper


def load_json_file(file):
    with open(file, "r") as f:
        return json.load(f)


class ManualTestRunner(object):
    def __init__(self):
        self.jdata = ''
        self.test_module = ''
        self.test_cases_id = ''
        self.configuration = ''
        self.starttime = ''
        self.result_id = ''
        self.write_dir = ''

    def _get_testcases(self, file):
        self.jdata = load_json_file(file)
        self.test_cases_id = []
        self.test_module = self.jdata[0]['test']['@alias'].split('.', 2)[0]
        for i in self.jdata:
            self.test_cases_id.append(i['test']['@alias'])
    
    def _get_input(self, config):
        while True:
            output = input('{} = '.format(config))
            if re.match('^[a-zA-Z0-9_-]+$', output):
                break
            print('Only alphanumeric and underscore/hyphen are allowed. Please try again')
        return output

    def _create_config(self):
        from oeqa.utils.metadata import get_layers
        from oeqa.utils.commands import get_bb_var
        from resulttool.resultutils import store_map

        layers = get_layers(get_bb_var('BBLAYERS'))
        self.configuration = {}
        self.configuration['LAYERS'] = layers
        current_datetime = datetime.datetime.now()
        self.starttime = current_datetime.strftime('%Y%m%d%H%M%S')
        self.configuration['STARTTIME'] = self.starttime
        self.configuration['TEST_TYPE'] = 'manual'
        self.configuration['TEST_MODULE'] = self.test_module

        extra_config = set(store_map['manual']) - set(self.configuration)
        for config in sorted(extra_config):
            print('---------------------------------------------')
            print('This is configuration #%s. Please provide configuration value(use "None" if not applicable).'
                  % config)
            print('---------------------------------------------')
            value_conf = self._get_input('Configuration Value')
            print('---------------------------------------------\n')
            self.configuration[config] = value_conf

    def _create_result_id(self):
        self.result_id = 'manual_' + self.test_module + '_' + self.starttime

    def _execute_test_steps(self, test_id):
        test_result = {}
        total_steps = len(self.jdata[test_id]['test']['execution'].keys())
        print('------------------------------------------------------------------------')
        print('Executing test case:' + '' '' + self.test_cases_id[test_id])
        print('------------------------------------------------------------------------')
        print('You have total ' + str(total_steps) + ' test steps to be executed.')
        print('------------------------------------------------------------------------\n')
        for step in sorted((self.jdata[test_id]['test']['execution']).keys()):
            print('Step %s: ' % step + self.jdata[test_id]['test']['execution']['%s' % step]['action'])
            print('Expected output: ' + self.jdata[test_id]['test']['execution']['%s' % step]['expected_results'])
            done = input('\nPlease press ENTER when you are done to proceed to next step.\n')
        while True:
            done = input('\nPlease provide test results: (P)assed/(F)ailed/(B)locked/(S)kipped? \n')
            done = done.lower()
            result_types = {'p':'PASSED',
                                'f':'FAILED',
                                'b':'BLOCKED',
                                's':'SKIPPED'}
            if done in result_types:
                for r in result_types:
                    if done == r:
                        res = result_types[r]
                        if res == 'FAILED':
                            log_input = input('\nPlease enter the error and the description of the log: (Ex:log:211 Error Bitbake)\n')
                            test_result.update({self.test_cases_id[test_id]: {'status': '%s' % res, 'log': '%s' % log_input}})
                        else:
                            test_result.update({self.test_cases_id[test_id]: {'status': '%s' % res}})
                break
            print('Invalid input!')
        return test_result

    def _create_write_dir(self):
        basepath = os.environ['BUILDDIR']
        self.write_dir = basepath + '/tmp/log/manual/'

    def run_test(self, file):
        self._get_testcases(file)
        self._create_config()
        self._create_result_id()
        self._create_write_dir()
        test_results = {}
        print('\nTotal number of test cases in this test suite: ' + '%s\n' % len(self.jdata))
        for i in range(0, len(self.jdata)):
            test_result = self._execute_test_steps(i)
            test_results.update(test_result)
        return self.configuration, self.result_id, self.write_dir, test_results

def manualexecution(args, logger):
    testrunner = ManualTestRunner()
    get_configuration, get_result_id, get_write_dir, get_test_results = testrunner.run_test(args.file)
    resultjsonhelper = OETestResultJSONHelper()
    resultjsonhelper.dump_testresult_file(get_write_dir, get_configuration, get_result_id,
                                          get_test_results)
    return 0

def register_commands(subparsers):
    """Register subcommands from this plugin"""
    parser_build = subparsers.add_parser('manualexecution', help='helper script for results populating during manual test execution.',
                                         description='helper script for results populating during manual test execution. You can find manual test case JSON file in meta/lib/oeqa/manual/',
                                         group='manualexecution')
    parser_build.set_defaults(func=manualexecution)
    parser_build.add_argument('file', help='specify path to manual test case JSON file.Note: Please use \"\" to encapsulate the file path.')
