# Test the SBAPI for GetStatistics()

import json
import lldb
from lldbsuite.test.decorators import *
from lldbsuite.test.lldbtest import *
from lldbsuite.test import lldbutil


class TestStatsAPI(TestBase):
    mydir = TestBase.compute_mydir(__file__)

    def setUp(self):
        TestBase.setUp(self)

    def test_stats_api(self):
        self.build()
        exe = self.getBuildArtifact("a.out")
        target = self.dbg.CreateTarget(exe)
        stats = target.GetStatistics()
        stream = lldb.SBStream()
        res = stats.GetAsJSON(stream)
        stats_json = sorted(json.loads(stream.GetData()))
        self.assertEqual(len(stats_json), 4)
        self.assertTrue("Number of expr evaluation failures" in stats_json)
        self.assertTrue("Number of expr evaluation successes" in stats_json)
        self.assertTrue("Number of frame var failures" in stats_json)
        self.assertTrue("Number of frame var successes" in stats_json)
