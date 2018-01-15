# coding: utf-8

# Copyright (C) 1994-2017 Altair Engineering, Inc.
# For more information, contact Altair at www.altair.com.
#
# This file is part of the PBS Professional ("PBS Pro") software.
#
# Open Source License Information:
#
# PBS Pro is free software. You can redistribute it and/or modify it under the
# terms of the GNU Affero General Public License as published by the Free
# Software Foundation, either version 3 of the License, or (at your option) any
# later version.
#
# PBS Pro is distributed in the hope that it will be useful, but WITHOUT ANY
# WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
# A PARTICULAR PURPOSE. See the GNU Affero General Public License for more
# details.
#
# You should have received a copy of the GNU Affero General Public License
# along with this program. If not, see <http://www.gnu.org/licenses/>.
#
# Commercial License Information:
#
# The PBS Pro software is licensed under the terms of the GNU Affero General
# Public License agreement ("AGPL"), except where a separate commercial license
# agreement for PBS Pro version 14 or later has been executed in writing with
# Altair.
#
# Altair’s dual-license business model allows companies, individuals, and
# organizations to create proprietary derivative works of PBS Pro and
# distribute them - whether embedded or bundled with other software - under
# a commercial license agreement.
#
# Use of Altair’s trademarks, including but not limited to "PBS™",
# "PBS Professional®", and "PBS Pro™" and Altair’s logos is subject to Altair's
# trademark licensing policies.

from tests.functional import *


class TestPbsInitScript(TestFunctional):
    """
    Testing PBS Pro init script
    """
    def test_env_vars_precede_pbs_conf_file(self):
        """
        Test PBS_START environment variables overrides values in pbs.conf file
        """
        self.du.run_cmd(cmd=['/etc/init.d/pbs', 'stop'])

        conf = {'PBS_START_SERVER': '1', 'PBS_START_SCHED': '1',
                'PBS_START_COMM': '1', 'PBS_START_MOM': '0'}
        self.du.set_pbs_config(confs=conf)

        env = {'PBS_START_SERVER': '0', 'PBS_START_SCHED': '0',
               'PBS_START_COMM': '0', 'PBS_START_MOM': '1'}

        rc = self.du.run_cmd(cmd=['/etc/init.d/pbs', 'start'], env=env)
        output = rc['out']

        self.assertFalse("PBS server" in output)
        self.assertFalse("PBS sched" in output)
        self.assertFalse("PBS comm" in output)
        self.assertTrue("PBS mom" in output)