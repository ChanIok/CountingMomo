"""有界识别队列；共享字体模板，直接处理内存中的名称裁图。"""

import json
from dataclasses import dataclass
from queue import Full, Queue
from threading import Event, Thread
from time import perf_counter

from PIL import Image

from ownership import detect_ownership
from recognition import describe, load_recognizer, mark_for_review, recognize_names


@dataclass
class PageJob:
    frame_id: str
    image: Image.Image
    samples: list


class RecognitionWorker:
    def __init__(self, output, config, font_path, batch_size, queue_pages):
        self.output, self.config = output, config
        self.font_path, self.batch_size = font_path, batch_size
        self.recognition_info = None
        self.queue = Queue(maxsize=queue_pages)
        self.ready = Event()
        self.error = None
        self.completed = 0
        self.timings = {"name_recognition_seconds": 0.0,
                        "ownership_seconds": 0.0, "evidence_write_seconds": 0.0}
        self.thread = Thread(target=self.run, name="countingmomo-recognition", daemon=True)
        self.thread.start()

    def check(self):
        if self.error is not None:
            raise RuntimeError(f"识别工作线程失败：{self.error}") from self.error
        if self.ready.is_set() and not self.thread.is_alive():
            raise RuntimeError("识别工作线程意外退出。")

    def submit(self, job, check_stop):
        started = perf_counter()
        while True:
            check_stop()
            self.check()
            try:
                self.queue.put(job, timeout=0.1)
                return perf_counter() - started
            except Full:
                continue

    def finish(self):
        interrupted = False
        while self.thread.is_alive():
            try:
                self.queue.put(None, timeout=0.1)
                break
            except Full:
                continue
            except KeyboardInterrupt:
                interrupted = True
                print("正在保存已经采集的结果，请等待识别队列排空。", flush=True)
        while self.thread.is_alive():
            try:
                self.thread.join(0.2)
            except KeyboardInterrupt:
                interrupted = True
                print("正在保存已经采集的结果，请等待识别队列排空。", flush=True)
        return interrupted

    def process_page(self, recognizer, job, journal):
        started = perf_counter()
        job.image.save(self.output / f"frames/{job.frame_id}.jpg", quality=95)
        self.timings["evidence_write_seconds"] += perf_counter() - started
        for start in range(0, len(job.samples), self.batch_size):
            samples = job.samples[start:start + self.batch_size]
            records = [sample.record for sample in samples]
            timings = detect_ownership(records, (sample.star for sample in samples), self.config["ownership_star"])
            self.timings["ownership_seconds"] += timings["ownership_seconds"]
            started = perf_counter()
            for sample in samples:
                for kind in ("card", "name", "star"):
                    getattr(sample, kind).save(self.output / sample.record[f"{kind}_crop"])
            self.timings["evidence_write_seconds"] += perf_counter() - started
            if self.error is None:
                started = perf_counter()
                try:
                    recognize_names(recognizer, records, (sample.name for sample in samples))
                except Exception as exc:
                    self.error = exc
                finally:
                    self.timings["name_recognition_seconds"] += perf_counter() - started
            for record in records:
                if self.error is not None and not record["name_text"]:
                    record['name_issues'].append('识别中断：' + str(self.error))
                mark_for_review(record)
                journal.write(json.dumps(record, ensure_ascii=False) + "\n")
            journal.flush()
            self.completed += len(records)
            print(f"已处理 {self.completed} 件。", flush=True)

    def run(self):
        try:
            recognizer, timings = load_recognizer(self.font_path, self.config)
            self.recognition_info = describe(recognizer)
            self.timings.update(timings)
            self.ready.set()
            with (self.output / "items.jsonl").open("w", encoding="utf-8") as journal:
                while True:
                    job = self.queue.get()
                    try:
                        if job is None:
                            return
                        self.process_page(recognizer, job, journal)
                    except Exception as exc:
                        self.error = exc
                        # 保存其他已入队页面的证据，即使当前页无法继续识别。
                    finally:
                        self.queue.task_done()
        except Exception as exc:
            self.error = exc
        finally:
            self.ready.set()
