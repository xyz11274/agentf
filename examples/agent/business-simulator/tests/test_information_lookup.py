"""查询只匹配业务内容；元数据不能把无关资料伪装成命中。"""

import copy
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "harness"))

from business_sim.services import lookup


class InformationLookupTests(unittest.TestCase):
    def test_dates_ids_sources_and_field_names_are_not_query_content(self):
        # 同一年份与 URL 中的单词，不是航班问题与天气资料之间的业务关联。
        weather = {"id": "flight-2026", "title": "北京天气", "topic": "weather",
                   "date": "2026-10-10", "temperature_c": 20,
                   "source": "https://flight.example/catalog", "keywords": ["北京", "天气"]}
        for query in ("2026年10月10日广州到昆明机票", "flight", "temperature_c", "2026"):
            with self.subTest(query=query):
                self.assertEqual(lookup(query, [weather]), [])

    def test_punctuation_does_not_form_a_new_chinese_word(self):
        self.assertEqual(lookup("天，气", [{"title": "天气"}]), [])

    def test_case_insensitive_words_do_not_match_substrings(self):
        laptop = {"title": "MacBook Air", "text": "Python 开发", "keywords": ["Laptop"]}
        self.assertEqual(lookup("laptop", [laptop]), [laptop])
        self.assertEqual(lookup("python", [laptop]), [laptop])
        self.assertEqual(lookup("book", [laptop]), [])

    def test_content_aliases_and_movie_fields_remain_searchable(self):
        rows = [{"title": "设备指南", "text": "先断开电源再清洁滤网"},
                {"title": "测试电影", "actors": ["张三"], "aliases": ["星际旅行"],
                 "type": "科幻", "highlight": "家庭一起观看"}]
        for query, expected in (("清洁滤网", rows[0]), ("张三", rows[1]),
                                ("星际旅行", rows[1]), ("科幻", rows[1])):
            with self.subTest(query=query):
                self.assertEqual(lookup(query, rows), [expected])

    def test_empty_keywords_do_not_match_every_query_or_mutate_catalog(self):
        rows = [{"title": "北京天气", "keywords": ["", "  "]}]
        original = copy.deepcopy(rows)
        self.assertEqual(lookup("广州机票", rows), [])
        self.assertEqual(lookup("", rows), [])
        self.assertEqual(rows, original)

    def test_ranking_is_stable_and_results_are_bounded(self):
        rows = [{"id": str(i), "title": "测试电影", "keywords": ["电影"]} for i in range(8)]
        self.assertEqual(lookup("电影", rows), rows[:5])


    def test_snapshot_query_respects_topic_and_location(self):
        from business_sim.evaluation_world import execute
        rows = [{"title": "北京医院", "topic": "health", "location": "北京"},
                {"title": "上海医院", "topic": "health", "location": "上海"},
                {"title": "医院附近餐饮", "topic": "food", "location": "上海"}]
        state = {"evaluation_world": {"catalog": {"information": rows}, "sha256": "test-profile"}}
        original = copy.deepcopy(state)
        reply = execute(state, "web-search", {
            "action": "query", "query": "医院", "topic": "health", "location": "上海"})
        self.assertEqual(reply["status"], "succeeded")
        self.assertEqual(reply["UserOutputObject"]["results"], [rows[1]])
        self.assertEqual(state, original)

    def test_snapshot_empty_query_is_distinct_from_unsupported_operation(self):
        from business_sim.evaluation_world import execute
        state = {"evaluation_world": {"catalog": {"information": []}, "sha256": "test-profile"}}
        empty = execute(state, "web-search", {"action": "query", "query": "机票"})
        unavailable = execute(state, "web-search", {
            "action": "browser_open", "url": "https://example.invalid"})
        self.assertEqual(empty["status"], "not_found")
        self.assertEqual(empty["UserOutputObject"]["results"], [])
        self.assertNotIn("error", empty)
        self.assertEqual(unavailable["status"], "unsupported")
        self.assertEqual(unavailable["error"], "snapshot_miss")


if __name__ == "__main__":
    unittest.main()
