#!/usr/bin/env python3
"""Tests for make_site_docs.py: each rule of the validator fires on a page that
breaks it and stays quiet on the page that does not (D210).

    python3 tools/package/test_make_site_docs.py
"""

import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import make_site_docs as site  # noqa: E402

FRONT = '---\ntitle: "T"\ndescription: "D"\norder: {order}\ngenerated: "wolang"\n---\n\n'


def validate(pages):
    with tempfile.TemporaryDirectory() as held:
        root = Path(held)
        for index, (name, body) in enumerate(pages.items(), start=1):
            path = root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(FRONT.format(order=index) + body, encoding="utf-8")
        return site.validate(root)


class ValidatorTest(unittest.TestCase):
    def test_a_clean_page_has_no_problems(self):
        self.assertEqual(validate({"wolang/a.mdx": "# Hello\n\nText with `{code}` and \\{escaped\\}.\n"}), [])

    def test_an_unescaped_brace_in_prose_fires(self):
        problems = validate({"wolang/a.mdx": "Text with {braces}.\n"})
        self.assertTrue(any("brace" in problem for problem in problems))

    def test_a_brace_in_a_fence_or_a_span_does_not_fire(self):
        self.assertEqual(validate({"wolang/a.mdx": "```wolang\nfun f() {}\n```\n\nand `{x}` here\n"}), [])

    def test_an_unescaped_angle_bracket_fires(self):
        problems = validate({"wolang/a.mdx": "A <b> tag.\n"})
        self.assertTrue(any("'<'" in problem for problem in problems))

    def test_an_unclosed_fence_fires(self):
        problems = validate({"wolang/a.mdx": "```wolang\nfun f() {}\n"})
        self.assertTrue(any("never closed" in problem for problem in problems))

    def test_a_link_to_a_missing_page_fires(self):
        problems = validate({"wolang/a.mdx": "[x](/docs/wolang/nope/)\n"})
        self.assertTrue(any("points at no page" in problem for problem in problems))

    def test_a_link_to_an_existing_page_does_not_fire(self):
        self.assertEqual(validate({"wolang/a.mdx": "[x](/docs/wolang/b/)\n", "wolang/b.mdx": "# B\n"}), [])

    def test_a_missing_anchor_fires_and_a_present_one_does_not(self):
        pages = {"wolang/a.mdx": "[x](/docs/wolang/b/#second-part)\n", "wolang/b.mdx": "## Second part\n"}
        self.assertEqual(validate(pages), [])
        pages["wolang/b.mdx"] = "## Other part\n"
        self.assertTrue(any("second-part" in problem for problem in validate(pages)))

    def test_a_relative_link_left_unconverted_fires(self):
        problems = validate({"wolang/a.mdx": "[x](other.md)\n"})
        self.assertTrue(any("relative link" in problem for problem in problems))

    def test_a_link_to_a_file_that_is_not_there_fires(self):
        problems = validate({"wolang/a.mdx": "[x]({}/blob/alpha/docs/not-a-file.md)\n".format(site.REPOSITORY)})
        self.assertTrue(any("not in the repository" in problem for problem in problems))

    def test_an_api_page_documenting_another_package_fires(self):
        problems = validate({"wolang-api/math.mdx": "## math\n\n## std.text\n"})
        self.assertTrue(any("not in the package" in problem for problem in problems))

    def test_an_api_page_documenting_its_own_modules_does_not_fire(self):
        self.assertEqual(validate({"wolang-api/std.mdx": "## std.text\n\n## std.map\n"}), [])

    def test_two_pages_with_one_order_fire_but_two_indexes_do_not(self):
        with tempfile.TemporaryDirectory() as held:
            root = Path(held)
            for name in ("wolang/a.mdx", "wolang/b.mdx", "wolang/index.mdx"):
                (root / name).parent.mkdir(parents=True, exist_ok=True)
                (root / name).write_text(FRONT.format(order=1) + "# X\n", encoding="utf-8")
            problems = site.validate(root)
        self.assertEqual(len(problems), 1)
        self.assertIn("order 1", problems[0])


class ConversionTest(unittest.TestCase):
    def test_a_code_span_may_wrap_across_lines(self):
        converted = site.convert_text("a `fun F()\n-> {int}` b {c}", "docs/x.md")
        self.assertIn("`fun F()\n-> {int}`", converted)
        self.assertIn("\\{c\\}", converted)

    def test_a_double_backtick_span_is_kept_whole(self):
        self.assertIn("``a`b``", site.convert_text("see ``a`b`` and {x}", "docs/x.md"))

    def test_an_angle_bracket_in_prose_is_made_literal(self):
        self.assertIn("&lt;", site.convert_text("Vec<T> is generic", "docs/x.md"))

    def test_a_link_to_a_guide_goes_to_its_page(self):
        self.assertIn("(/docs/wolang/tour/)", site.convert_text("[t](tour.md)", "docs/spec.md"))

    def test_every_package_has_a_page_link(self):
        for package in site.PACKAGES:
            self.assertIn("/docs/wolang-api/{}/".format(package), site.page_link("api/{}.md".format(package), "docs/stdlib.md"))


if __name__ == "__main__":
    unittest.main()
