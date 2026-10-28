"""正式名称模块的离线回归；普通字与小字调用同一入口。"""

from pathlib import Path
import sys
import unittest

import numpy as np
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from font_templates import FontTemplates, font_characters, template_bank
from recognition import NameRecognizer, empty_name, mark_for_review, recognize_name


class NameRecognitionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.font = ROOT.parent / 'temp/FZYASHJW_Zhong.ttf'
        if not cls.font.is_file():
            raise unittest.SkipTest('游戏字体不随源码分发')
        cls.recognizer = NameRecognizer(FontTemplates(cls.font,
            tuple(sorted(set('春风晚霞甲乙于日落再相见·若重逢'))), {}), 114, (103, -4, 127, 16))

    def render(self, text, size, origin=0):
        image = Image.new('L', (114, 20), 255)
        font = ImageFont.truetype(str(self.font), size)
        ImageDraw.Draw(image).text((origin + .5, 3), text, font=font, fill=0)
        return image.convert('RGB')

    def test_same_entry_reads_both_sizes_and_column_offsets(self):
        for size, origin in ((13, 0), (13, 1), (11, 3)):
            with self.subTest(size=size, origin=origin):
                result = recognize_name(self.recognizer, self.render('于日落再相见', size, origin))
                self.assertEqual(result['name_text'], '于日落再相见')
                self.assertEqual(result['name_font_size'], size)
                self.assertEqual(result['name_origin'], origin)

    def test_game_character_set_excludes_bullet(self):
        chars, _ = font_characters(self.font)
        self.assertIn('·', chars)
        self.assertNotIn('•', chars)

    def test_font_banks_are_shared_and_cached_by_size(self):
        first = template_bank(self.recognizer.templates, 13)
        self.assertIs(first, template_bank(self.recognizer.templates, 13))
        self.assertEqual(first.shape[1], 14 * 20)

    def test_texture_and_badge_do_not_create_a_name(self):
        y, x = np.indices((20, 114))
        image = Image.fromarray((180 + x % 7 + y % 3).astype('uint8')).convert('RGB')
        ImageDraw.Draw(image).rectangle((103, 0, 113, 15), fill='black')
        result = recognize_name(self.recognizer, image)
        self.assertEqual(result['name_text'], '')
        self.assertTrue(result['name_issues'])

    def test_review_flags_name_and_ownership_independently(self):
        record = {**empty_name('字符同分'), 'ownership': 'owned'}
        mark_for_review(record)
        self.assertTrue(record['needs_review'])
        self.assertEqual(record['review_reasons'], ['名称需要核对：字符同分'])
        record['name_issues'], record['ownership'] = [], 'unknown'
        mark_for_review(record)
        self.assertEqual(record['review_reasons'], ['拥有状态需要核对'])

    def test_occluded_long_name_requires_review(self):
        image = self.render('于日落再相见·若重逢', 11, 3)
        ImageDraw.Draw(image).rectangle((103, 0, 113, 15), fill='#665599')
        result = recognize_name(self.recognizer, image)
        self.assertTrue(any('图标' in issue for issue in result['name_issues']))


if __name__ == '__main__':
    unittest.main()
