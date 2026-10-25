"""The saves folder apart from the caches (launcher/bbport_launcher_win.py cache_folder): a saves
folder in a cloud folder keeps the shaders on this PC; and the mods' order after a drag (moved)."""
from paths import ROOT
import os
import sys
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(ROOT / 'launcher'))
sys.path.insert(0, str(ROOT / 'scripts'))
import bbport_launcher_win as L  # noqa: E402

ONEDRIVE = {'OneDrive': r'C:\Users\Hunter\OneDrive', 'OneDriveConsumer': '', 'OneDriveCommercial': ''}


class CacheFolderTests(unittest.TestCase):
    def setUp(self):
        patch = mock.patch.dict(os.environ, ONEDRIVE)
        patch.start()
        self.addCleanup(patch.stop)

    def test_cloud_folders_are_recognised(self):
        for path in (r'C:\Users\Hunter\OneDrive\Bloodborne', r'C:\Users\Hunter\OneDrive', r'D:\Dropbox\BB saves',
                     r'E:\Google Drive\saves', r'C:\Users\Hunter\OneDrive - Firma\BB', r'F:\Nextcloud\bb'):
            self.assertTrue(L.synced_folder(path), path)
        for path in (r'C:\Games\BBPort\user', r'C:\Users\Hunter\OneDriveBackup', r'D:\Drop\box'):
            self.assertFalse(L.synced_folder(path), path)

    def test_the_caches_beside_the_saves_as_before(self):
        settings = {**L.APP_DEFAULTS, 'user_dir': r'D:\BB\user'}
        self.assertEqual(L.cache_folder(settings), Path(r'D:\BB\user'))
        self.assertNotIn('BB_GPU_USER_DIR', self.environment(settings))

    def test_saves_in_a_cloud_folder_keep_the_shaders_on_this_pc(self):
        settings = {**L.APP_DEFAULTS, 'user_dir': r'C:\Users\Hunter\OneDrive\Bloodborne'}
        self.assertEqual(L.cache_folder(settings), L.DATA_DIR / 'user')
        env = self.environment(settings)
        self.assertEqual(env['BB_USER_DIR'], r'C:\Users\Hunter\OneDrive\Bloodborne')
        self.assertEqual(env['BB_GPU_USER_DIR'], str(L.DATA_DIR / 'user'))

    def test_a_cache_folder_of_the_players_choice(self):
        settings = {**L.APP_DEFAULTS, 'user_dir': r'C:\Users\Hunter\OneDrive\Bloodborne', 'cache_dir': r'D:\BB cache'}
        self.assertEqual(self.environment(settings)['BB_GPU_USER_DIR'], r'D:\BB cache')

    @staticmethod
    def environment(settings):
        with mock.patch.dict(os.environ, {'BB_GPU_USER_DIR': ''}):
            os.environ.pop('BB_GPU_USER_DIR')
            return L.game_environment(settings)


class FolderEntriesTests(unittest.TestCase):
    """What the Saves and Mods pages are drawn from: drawn again on a page change only when it changed."""

    def test_entries_change_with_the_folder(self):
        import tempfile
        with tempfile.TemporaryDirectory() as folder:
            before = L.folder_entries(folder)
            (Path(folder) / 'mods.json').write_text('{}')
            (Path(folder) / 'other').write_text('x')
            self.assertNotEqual(L.folder_entries(folder), before)
            self.assertEqual([name for name, _ in L.folder_entries(folder, ('mods.json',))], ['mods.json'])
        self.assertEqual(L.folder_entries(Path(folder) / 'gone'), ())


class ModOrderTests(unittest.TestCase):
    ORDER = ['a', 'b', 'c', 'd']

    def test_down_up_and_to_the_end(self):
        self.assertEqual(L.moved(self.ORDER, 0, 3), ['b', 'c', 'a', 'd'])
        self.assertEqual(L.moved(self.ORDER, 3, 0), ['d', 'a', 'b', 'c'])
        self.assertEqual(L.moved(self.ORDER, 1, 4), ['a', 'c', 'd', 'b'])

    def test_dropped_where_it_was(self):
        for source in range(4):
            self.assertEqual(L.moved(self.ORDER, source, source), self.ORDER)
            self.assertEqual(L.moved(self.ORDER, source, source + 1), self.ORDER)


if __name__ == '__main__':
    unittest.main()
