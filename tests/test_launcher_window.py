"""The launcher's window (launcher/bbport_launcher_win.py): opaque unless the player asks for the
see-through look, and the side bar's switch drawn on the side bar's colour."""
from paths import ROOT
import sys
import unittest

sys.path.insert(0, str(ROOT / 'launcher'))
sys.path.insert(0, str(ROOT / 'scripts'))
import bbport_launcher_win as L  # noqa: E402

SOURCE = (ROOT / 'launcher' / 'bbport_launcher_win.py').read_text(encoding='utf-8')


class WindowTests(unittest.TestCase):
    def test_opaque_by_default(self):
        self.assertIs(L.APP_DEFAULTS['translucent_window'], False)
        # the first test build saved 'translucent' on for everyone: that key must not count any more
        self.assertNotIn('translucent', L.APP_DEFAULTS)
        self.assertNotIn("get('translucent'", SOURCE)

    def test_the_switch_is_on_a_page_of_the_simple_view(self):
        page = SOURCE[SOURCE.index('    def build_language('):]
        page = page[:page.index('\n    def ', 10)]
        self.assertIn('self.appearance_check(f)', page)
        self.assertNotIn('language', L.EXPERT_PAGES)

    def test_the_game_folder_is_for_experts_and_links_open_it(self):
        self.assertIn('game', L.EXPERT_PAGES)
        show = SOURCE[SOURCE.index('    def show_page(self, name):'):]
        show = show[:show.index('\n    def ', 10)]
        self.assertIn('self.expert_var.set(True)', show)  # a check's link to the game folder turns the view on

    def test_the_launchers_title_bar_by_default(self):
        self.assertIs(L.APP_DEFAULTS['windows_title_bar'], False)
        self.assertIn("'windows_title_bar', 'app'", SOURCE)  # the way back, in Advanced

    def test_where_windows_finds_the_caption(self):
        import bbport_frame

        class User32:
            zoomed = False

            def ClientToScreen(self, _hwnd, point):
                point._obj.x, point._obj.y = 100, 50  # the client area's top left on the screen

            def GetClientRect(self, _hwnd, rect):
                rect._obj.right, rect._obj.bottom = 1200, 800

            def IsZoomed(self, _hwnd):
                return self.zoomed
        frame = bbport_frame.Frame(1, [])
        frame.user32 = User32()
        frame.bar, frame.buttons, frame.edge, frame.corner = 32, 138, 5, 10

        def hit(x, y):
            return frame.hit(((y & 0xFFFF) << 16) | (x & 0xFFFF))
        self.assertEqual(hit(100 + 600, 50 + 16), bbport_frame.HTCAPTION)
        self.assertEqual(hit(100 + 1200 - 20, 50 + 16), bbport_frame.HTCLIENT)  # the window's buttons
        self.assertEqual(hit(100 + 600, 50 + 2), bbport_frame.HTTOP)
        self.assertEqual(hit(100 + 3, 50 + 2), bbport_frame.HTTOPLEFT)
        self.assertEqual(hit(100 + 1197, 50 + 2), bbport_frame.HTTOPRIGHT)
        self.assertEqual(hit(100 + 600, 50 + 40), bbport_frame.HTCLIENT)  # below the bar: the page
        frame.user32.zoomed = True
        self.assertEqual(hit(100 + 600, 50 + 2), bbport_frame.HTCAPTION)  # maximized: no edge to resize

    def test_side_bar_switch_matches_the_side_bar(self):
        start = SOURCE.index('self.mode_switch = ttk.Checkbutton(')
        statement = SOURCE[start:SOURCE.index('self.mode_switch.pack', start)]
        self.assertIn("style='Bar.TCheckbutton'", statement)


if __name__ == '__main__':
    unittest.main()
