"""字体离线实验的数值和评估契约；不导入窗口、WGC 或输入模块。"""

import json
from pathlib import Path
import sys
from tempfile import TemporaryDirectory
import unittest

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "experiments/font_retrieval"))
from templates import (Raster, TemplateBank, binary_features, font_characters,
                       hamming_distances, render)
from benchmark import choose_threshold, edit_distance, load_samples, metrics, reuse_paddle_baseline


class BinaryFeatureTests(unittest.TestCase):
    def test_hamming_agrees_with_unpacked_bits(self):
        rng = np.random.default_rng(42)
        bank = rng.integers(0,256,size=(20,32),dtype=np.uint8)
        query = rng.integers(0,256,size=32,dtype=np.uint8)
        expected = np.unpackbits(bank ^ query,axis=1).sum(axis=1)
        np.testing.assert_array_equal(hamming_distances(query,bank),expected)

    def test_character_descriptor_uses_32_bytes(self):
        ink = np.zeros((20,13),dtype=np.float32)
        ink[5:15,4:8] = 1
        packed = binary_features([ink],(16,16))
        self.assertEqual(packed.shape,(1,32))
        self.assertEqual(packed.dtype,np.uint8)

    def test_font_cmap_and_exact_render_queries(self):
        font = ROOT.parent / "temp/FZYASHJW_Zhong.ttf"
        if not font.exists():
            self.skipTest("本机实验字体不随源码分发")
        chars,info = font_characters(font)
        self.assertEqual(info['glyphs'],8371)
        self.assertEqual(info['unicode_characters'],8765)
        self.assertIn(' ',info['mapped_whitespace'])
        self.assertIn('绾',chars)
        self.assertNotIn('図',chars)
        raster = Raster(13,1,0.5,3)
        texts = ['春风拂面','月下醒梦','圆圆丸子','绾丝垂','银灰羽絮']
        bank = TemplateBank(texts,font,raster)
        for text in texts:
            for method in ('gray','binary','hybrid'):
                with self.subTest(text=text,method=method):
                    result = bank.search(render(text,font,raster),method)
                    self.assertEqual(result['text'],text)

    def test_identical_templates_have_zero_margin(self):
        font = ROOT.parent / "temp/FZYASHJW_Zhong.ttf"
        if not font.exists():
            self.skipTest("本机实验字体不随源码分发")
        raster = Raster()
        bank = TemplateBank(['未','未','末'],font,raster)
        for method in ('gray','binary','hybrid'):
            result=bank.search(render('未',font,raster),method)
            self.assertAlmostEqual(result['margin'],0)


class EvaluationTests(unittest.TestCase):
    def test_edit_distance_counts_insert_delete_and_substitute(self):
        self.assertEqual(edit_distance('冉冉的新星·绿汀','冉冉的新星·绿江'),1)
        self.assertEqual(edit_distance('未',''),1)
        self.assertEqual(edit_distance('','末'),1)

    def test_evaluation_errors_do_not_change_threshold(self):
        samples=[{'split':'calibration','text':'甲'}, {'split':'calibration','text':'乙'},
                 {'split':'evaluation','text':'丙'}]
        predictions=[{'text':'甲','margin':0.3,'top5':['甲']},
                     {'text':'丁','margin':0.2,'top5':['丁','乙']},
                     {'text':'戊','margin':0.9,'top5':['戊']}]
        threshold=choose_threshold(samples,predictions)
        self.assertGreater(threshold,0.2)
        self.assertLess(threshold,0.3)
        result=metrics(samples,predictions,threshold,'evaluation')
        self.assertEqual(result['accepted_wrong'],1)
        self.assertEqual(result['exact_correct'],0)

    def test_labels_must_be_reviewed_and_name_disjoint(self):
        with TemporaryDirectory() as directory:
            path=Path(directory)/'samples.json'
            samples=[{'id':'a','text':'未','reviewed':False,'split':'calibration'},
                     {'id':'b','text':'末','reviewed':True,'split':'evaluation'}]
            path.write_text(json.dumps({'samples':samples}),encoding='utf8')
            with self.assertRaisesRegex(ValueError,'尚未逐图核对'):
                load_samples(path)
            samples[0]['reviewed']=True
            samples[1]['text']='未'
            path.write_text(json.dumps({'samples':samples}),encoding='utf8')
            with self.assertRaisesRegex(ValueError,'相同名称'):
                load_samples(path)

    def test_reuse_baseline_rescores_changed_labels_and_checks_inputs(self):
        samples=[{'id':'a','crop':'a.png','text':'暖阳缱绻','split':'evaluation'}]
        prediction={'text':'暖阳缱绻','margin':0.99,'top5':['暖阳缱绻']}
        previous={'predictions':[{'id':'a','crop':'a.png','predictions':{'paddle_current':prediction}}],
                  'methods':{'paddle_current':{'seconds':1.23,'evaluation':{'exact_correct':0}}}}
        measured,_=reuse_paddle_baseline(samples,previous)
        self.assertEqual(measured['evaluation']['exact_correct'],1)
        self.assertEqual(measured['seconds'],1.23)
        self.assertEqual(previous['methods']['paddle_current']['evaluation']['exact_correct'],0)
        samples[0]['crop']='changed.png'
        with self.assertRaisesRegex(ValueError,'输入及顺序不一致'):
            reuse_paddle_baseline(samples,previous)


if __name__ == '__main__':
    unittest.main()
