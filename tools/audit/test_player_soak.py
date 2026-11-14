"""Protect match acceptance when event-driven menu waits are added."""
import unittest
import sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent))
from run_player_soak import match_fences,webm_handles,growth_report
from test_menu_replay import assert_no_unimplemented_imports

class FenceTests(unittest.TestCase):
    def test_growth_uses_elapsed_window_and_keeps_in_window_failures(self):
        rows=[dict(seconds=t,private_bytes=100*1048576,handles=100,threads=10) for t in range(0,7200,60)]
        teardown=dict(seconds=7200.5,private_bytes=10**12,handles=100000,threads=1000)
        report,passed=growth_report(rows+[teardown],7200)
        self.assertTrue(passed)
        self.assertEqual(report['growth_window']['samples'],60)
        self.assertEqual(report['growth_window']['excluded_at_or_after_end'],[7200.5])
        # The same spike before expiry must still fail the unchanged limits.
        teardown['seconds']=7199
        self.assertFalse(growth_report(rows+[teardown],7200)[1])

    def test_growth_allowance_preserves_other_category_ceiling(self):
        rows=[dict(seconds=t,private_bytes=100*1048576,handles=200,threads=10,
            adjusted_handles=200,non_event_handles=100+20*t/3600,
            adjusted_event_handles=100-20*t/3600,event_handles=100-20*t/3600,webm_opens=0)
            for t in range(0,7200,60)]
        self.assertFalse(growth_report(rows,7200,True)[1])

    def test_interleaved_log_does_not_hide_real_unimplemented_imports(self):
        prefix='unimplemented imports reached:\n'
        interleaved='nvn_vk_frame: 1 draws 2\n'
        suffix='\nrun-time host functions: 690 handed out\n'
        assert_no_unimplemented_imports(prefix+interleaved+suffix)
        with self.assertRaises(AssertionError):
            assert_no_unimplemented_imports(prefix+interleaved+'         1  missing_import\n'+suffix)
        with self.assertRaises(AssertionError):assert_no_unimplemented_imports('truncated log')
    def test_menu_waits_never_count_as_matches(self):
        script='# title\nwait movie\n10+12:Z\nwait open:StrkrsSave:/strkrs.save\n20+12:Z\nwait movie\n'
        log='\n'.join(f'input wait: satisfied at frame {i*100}; block {i} begins (next inputs relative to this frame)' for i in (1,3,5))
        self.assertEqual(match_fences(script,log),1)
        self.assertEqual(match_fences(script,log.splitlines()[0]),0)
    def test_repeated_match_fences(self):
        script='wait open:StrkrsSave:/strkrs.save\n0+12:Z\nwait open:StrkrsSave:/strkrs.save\n'
        log='input wait: satisfied at frame 10; block 1 begins\ninput wait: satisfied at frame 100; block 3 begins\n'
        self.assertEqual(match_fences(script,log),2)
    def test_webm_allowance_cannot_hide_other_handle_types(self):
        row={'handles':130}
        census={'total':130,'types':{'Event':100,'Thread':20,'File':10}}
        log='movie: opened intro.webm: 20 bytes\nmovie: opened memory.webm: 30 bytes\nmovie: open failed bad.webm\nmovie: opened clip.mp4: 40 bytes\n'
        result=webm_handles(row,census,log)
        self.assertEqual(result['webm_opens'],2)
        self.assertEqual(result['adjusted_event_handles'],96)
        self.assertEqual(result['non_event_handles'],30)
        self.assertEqual(result['adjusted_handles'],126)
        grown=webm_handles({'handles':140},{'total':140,'types':{'Event':100,'Thread':30,'File':10}},log)
        self.assertEqual(grown['non_event_handles']-result['non_event_handles'],10)
        self.assertEqual(grown['adjusted_handles']-result['adjusted_handles'],10)

if __name__=='__main__':unittest.main(verbosity=2)
