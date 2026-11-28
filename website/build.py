#!/usr/bin/env python3
# Low Latency IPC Library for high-speed traffic
# Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>
#
# SPDX-License-Identifier: BSD-3-Clause

"""Builds the DataPath website into a static folder for GitHub Pages.

The site follows the repository: the landing page pulls its comparison table
and graphs from README.md, and the documentation pages are the Markdown files
under docs/ and the binding READMEs, rendered as they are. Only the landing
page layout and the getting-started guide live in website/.

    python website/build.py [--out _site]

Needs `markdown` and `pygments` (website/requirements.txt). Every URL in the
output is relative, so the site works under any base path
(https://xaymar.github.io/datapath/ or a custom domain).
"""

from __future__ import annotations

import argparse
import html
import json
import posixpath
import re
import shutil
import subprocess
from pathlib import Path

import markdown
from pygments import highlight
from pygments.formatters import HtmlFormatter
from pygments.lexers import get_lexer_by_name
from pygments.util import ClassNotFound

ROOT = Path(__file__).resolve().parent.parent
SITE = Path(__file__).resolve().parent
REPO_URL = "https://github.com/Xaymar/datapath"
BRANCH = "root"

# Repository Markdown rendered as site pages: source path -> (site path, nav section).
# Every docs/comparisons/*.md is added on its own (see collect_pages), so a new
# comparison shows up without touching this file.
DOC_PAGES = {
	"docs/semantics.md": ("docs/semantics/", "docs"),
	"CONTRIBUTING.md": ("docs/building/", "docs"),
	"bindings/python/README.md": ("docs/python/", "docs"),
	"bindings/node/README.md": ("docs/node/", "docs"),
	"bindings/rust/datapath/README.md": ("docs/rust/", "docs"),
	"docs/comparisons/README.md": ("benchmarks/methodology/", "benchmarks"),
}

# Short titles for the sidebar; pages not listed use their first heading.
NAV_TITLES = {
	"docs/semantics/": "Semantics and tuning",
	"docs/building/": "Building and testing",
	"docs/python/": "Python",
	"docs/node/": "Node.js",
	"docs/rust/": "Rust",
	"benchmarks/methodology/": "Methodology",
}
DOCS_ORDER = ["docs/semantics/", "docs/rust/", "docs/python/", "docs/node/", "docs/building/"]

TOP_NAV = [
	("", "Overview"),
	("start/", "Get started"),
	("benchmarks/", "Benchmarks"),
	("docs/", "Docs"),
]


# ---------------------------------------------------------------------------
# Helpers


def rel(src_page: str, target: str) -> str:
	"""Relative URL from site page `src_page` (a folder path like 'docs/x/', '' for the root) to `target`."""
	frag = ""
	if "#" in target:
		target, frag = target.split("#", 1)
		frag = "#" + frag
	base = "/" + src_page
	is_dir = target == "" or target.endswith("/")
	path = posixpath.relpath("/" + target, base)
	if is_dir:
		path = "./" if path == "." else path + "/"
	return path + frag


def slugify(text: str) -> str:
	text = re.sub(r"<[^>]+>", "", text)
	text = html.unescape(text).strip().lower()
	text = re.sub(r"[^\w\- ]", "", text)
	return re.sub(r"[\s]+", "-", text)


_FORMATTER = HtmlFormatter(nowrap=True)
_LANG_ALIASES = {"sh": "bash", "shell": "bash", "toml": "toml", "js": "javascript", "ts": "typescript", "": "text"}


def highlight_code(html_text: str) -> str:
	"""Syntax-highlight every <pre><code class="language-x"> block."""

	def repl(m: re.Match) -> str:
		lang = (m.group(1) or "").lower()
		code = html.unescape(m.group(2))
		try:
			lexer = get_lexer_by_name(_LANG_ALIASES.get(lang, lang))
		except ClassNotFound:
			lexer = get_lexer_by_name("text")
		body = highlight(code, lexer, _FORMATTER).rstrip("\n")
		label = f' data-lang="{html.escape(lang)}"' if lang else ""
		return f'<pre class="code"{label}><code>{body}</code></pre>'

	return re.sub(
		r'<pre><code(?: class="language-([\w+-]+)")?>(.*?)</code></pre>', repl, html_text, flags=re.S
	)


def render_markdown(text: str) -> str:
	md = markdown.Markdown(extensions=["tables", "fenced_code", "sane_lists"])
	out = md.convert(text)
	return highlight_code(out)


def add_heading_anchors(html_text: str) -> tuple[str, list[tuple[int, str, str]]]:
	"""Give h2/h3 ids (GitHub style) and return the table of contents."""
	toc: list[tuple[int, str, str]] = []
	seen: dict[str, int] = {}

	def repl(m: re.Match) -> str:
		level, inner = int(m.group(1)), m.group(2)
		slug = slugify(inner)
		if slug in seen:
			seen[slug] += 1
			slug = f"{slug}-{seen[slug]}"
		else:
			seen[slug] = 0
		if level in (2, 3):
			toc.append((level, slug, re.sub(r"<[^>]+>", "", inner)))
		return f'<h{level} id="{slug}"><a class="anchor" href="#{slug}" aria-hidden="true">#</a>{inner}</h{level}>'

	return re.sub(r"<h([1-4])>(.*?)</h\1>", repl, html_text), toc


def git_short_rev() -> str:
	try:
		return subprocess.check_output(["git", "rev-parse", "--short", "HEAD"], cwd=ROOT, text=True).strip()
	except Exception:
		return ""


# ---------------------------------------------------------------------------
# Pages


class Page:
	def __init__(self, path: str, title: str, body: str, section: str, source: str | None = None,
	             toc=None, description: str = "", wide: bool = False):
		self.path = path            # site folder path, '' for the root
		self.title = title
		self.body = body
		self.section = section      # 'home', 'start', 'benchmarks', 'docs'
		self.source = source        # repository path it was rendered from
		self.toc = toc or []
		self.description = description
		self.wide = wide


def collect_pages() -> dict[str, tuple[str, str]]:
	pages = dict(DOC_PAGES)
	for md in sorted((ROOT / "docs/comparisons").glob("*.md")):
		src = md.relative_to(ROOT).as_posix()
		if src not in pages:
			pages[src] = (f"benchmarks/{md.stem}/", "benchmarks")
	return pages


def rewrite_links(html_text: str, source: str, page_path: str, mapping: dict[str, str]) -> str:
	"""Point repository-relative links at site pages, or at GitHub when the target is not on the site."""
	src_dir = posixpath.dirname(source)

	def fix(url: str, is_img: bool) -> str:
		if re.match(r"^[a-z]+:|^#|^//", url):
			return url
		path, _, frag = url.partition("#")
		target = posixpath.normpath(posixpath.join(src_dir, path)) if path else source
		if target.endswith("/"):
			target = target.rstrip("/")
		if target in mapping:
			return rel(page_path, mapping[target] + (("#" + frag) if frag else ""))
		if target + "/README.md" in mapping:
			return rel(page_path, mapping[target + "/README.md"] + (("#" + frag) if frag else ""))
		if target.startswith("docs/images/"):
			return rel(page_path, "img/" + posixpath.basename(target))
		kind = "raw" if is_img else ("tree" if (ROOT / target).is_dir() else "blob")
		return f"{REPO_URL}/{kind}/{BRANCH}/{target}" + (("#" + frag) if frag else "")

	html_text = re.sub(r'(<a [^>]*href=")([^"]+)(")', lambda m: m.group(1) + fix(m.group(2), False) + m.group(3), html_text)
	html_text = re.sub(r'(<img [^>]*src=")([^"]+)(")', lambda m: m.group(1) + fix(m.group(2), True) + m.group(3), html_text)
	return html_text


def first_heading(text: str) -> str:
	m = re.search(r"^#\s+(.+)$", text, flags=re.M)
	return m.group(1).strip() if m else "DataPath"


def readme_section(readme: str, heading: str) -> str:
	m = re.search(rf"^## {re.escape(heading)}\s*$(.*?)(?=^## |\Z)", readme, flags=re.M | re.S)
	return m.group(1).strip() if m else ""


def wrap_tables(html_text: str) -> str:
	return re.sub(r"(<table>.*?</table>)", r'<div class="table-wrap">\1</div>', html_text, flags=re.S)


def build_doc_pages(mapping_src: dict[str, tuple[str, str]]) -> list[Page]:
	mapping = {src: dst for src, (dst, _) in mapping_src.items()}
	pages = []
	for src, (dst, section) in mapping_src.items():
		text = (ROOT / src).read_text(encoding="utf-8")
		title = first_heading(text)
		body = render_markdown(text)
		body = rewrite_links(body, src, dst, mapping)
		body, toc = add_heading_anchors(body)
		body = wrap_tables(body)
		pages.append(Page(dst, title, body, section, source=src, toc=toc))
	return pages


def build_home(readme: str, mapping: dict[str, str]) -> Page:
	body = (SITE / "content/index.html").read_text(encoding="utf-8")
	# The comparison block follows the README: its graphs and tables.
	compare = readme_section(readme, "How it compares")
	images = re.findall(r"!\[([^\]]*)\]\(([^)]+)\)", readme)
	figures = []
	for i, (alt, src) in enumerate(images):
		name = posixpath.basename(src)
		platform = "Linux" if "linux" in name else ("Windows" if "windows" in name else name)
		figures.append((platform, alt, "img/" + name))
	tabs = "".join(
		f'<button role="tab" class="tab" aria-selected="{"true" if i == 0 else "false"}" '
		f'aria-controls="graph-{i}" id="graph-tab-{i}">{html.escape(p)}</button>'
		for i, (p, _, _) in enumerate(figures)
	)
	# Inline the SVGs: hover titles work, and the theme toggle can recolour them.
	panels = "".join(
		f'<figure role="tabpanel" class="graph" id="graph-{i}" aria-labelledby="graph-tab-{i}"{"" if i == 0 else " hidden"}>'
		f'<div class="chart" data-chart="graph-data-{i}"></div>'
		f'<script type="application/json" id="graph-data-{i}">{chart_json(src)}</script>'
		f'<div class="static">{inline_svg(ROOT / "docs/images" / posixpath.basename(src))}</div>'
		f'<figcaption>{"<br>".join(html.escape(n) for n in graph_data((ROOT / "docs/images" / posixpath.basename(src)).read_text(encoding="utf-8"))["notes"])}'
		f'<br><a href="{src}">Static version of this graph</a></figcaption></figure>'
		for i, (_, alt, src) in enumerate(figures)
	)
	compare_html = render_markdown(compare)
	compare_html = rewrite_links(compare_html, "README.md", "", mapping)
	compare_html = wrap_tables(compare_html)
	body = body.replace("{{graph_tabs}}", tabs).replace("{{graph_panels}}", panels)
	body = body.replace("{{compare}}", compare_html)
	return Page("", "DataPath: fast, predictable IPC", highlight_code(body), "home",
	            description="DataPath is a C++ IPC library for processes on the same machine, built for low worst-case latency: "
	                        "1–3 µs p99.9 round trips over shared memory, with C, Rust, Python and Node.js bindings.",
	            wide=True)


def chart_json(src: str) -> str:
	data = graph_data((ROOT / "docs/images" / posixpath.basename(src)).read_text(encoding="utf-8"))
	return json.dumps(data, ensure_ascii=False, separators=(",", ":")).replace("</", "<\\/")


def inline_svg(path: Path) -> str:
	svg = path.read_text(encoding="utf-8")
	svg = re.sub(r"^<\?xml[^>]*>\s*", "", svg)
	# Drop the fixed size so it scales with the card; the viewBox keeps the aspect ratio.
	return re.sub(r'(<svg\b[^>]*?)\s(?:width|height)="[^"]*"', r"\1", re.sub(r'(<svg\b[^>]*?)\s(?:width|height)="[^"]*"', r"\1", svg, count=1), count=1)


_UNITS = {"B": 1, "KiB": 1024, "MiB": 1024 * 1024}


def _shape(el: str) -> str:
	if el.startswith("<circle"):
		return "circle"
	if el.startswith("<rect"):
		return "square"
	if "l6,-10" in el:
		return "triangle-down"
	if " h" in el and " v" in el:
		return "plus"
	if "l6,10" in el:
		return "triangle"
	if "l6,6" in el:
		return "diamond"
	return "circle"


def _palette(style: str) -> dict[str, dict[str, str]]:
	"""Series colours of a README graph, light and dark, keyed by marker class."""
	blocks = re.findall(r"svg\{([^}]*)\}", style)
	out = {}
	for name, block in zip(("light", "dark"), blocks):
		out[name] = dict(re.findall(r"--(\w+):(#[0-9a-fA-F]+)", block))
	return out


def graph_data(svg: str) -> dict:
	"""Pull the values of a README graph out of its hover titles.

	The README graphs (docs/images/latency_chart.py) carry every value in a
	<title>, and every series as a marker class with its own colour. Reading
	them back means any library added to the README graph shows up on the
	site without touching this script.
	"""
	style = re.search(r"<style>(.*?)</style>", svg, flags=re.S).group(1)
	title = re.search(r'<text class="t1"[^>]*>([^<]*)</text>', svg).group(1)
	marker_re = r'(<(?:circle|rect|path) class="(\w+) m"[^>]*>)(?:<title>([^<]*)</title>)?</(?:circle|rect|path)>'
	series, panels = [], {}
	known = set()
	for m in re.finditer(marker_re + r'(?:<text class="t3"[^>]*>([^<]*)</text>)?', svg):
		el, cls, tip, label = m.group(1), m.group(2), m.group(3), m.group(4)
		if tip is None:
			if label and cls not in known:
				known.add(cls)
				series.append({"id": cls, "label": html.unescape(label), "shape": _shape(el)})
			continue
		tm = re.match(r"(.*), ([\d.]+ (?:B|KiB|MiB)), (.*): ([\d.]+) µs \(runs ([\d.]+) to ([\d.]+)\)", html.unescape(tip))
		if not tm:
			continue
		panel, size = tm.group(1), tm.group(2)
		rows = panels.setdefault(panel, {})
		rows.setdefault(size, {})[cls] = [float(tm.group(4)), float(tm.group(5)), float(tm.group(6))]
	size_key = lambda s: float(s.split()[0]) * _UNITS[s.split()[1]]
	notes: list[str] = []
	for t in re.findall(r'<text class="t4" x="16"[^>]*>([^<]*)</text>', svg):
		t = html.unescape(t).strip()
		# A footnote wrapped onto two SVG lines continues without a full stop.
		if notes and not notes[-1].endswith("."):
			notes[-1] += " " + t
		else:
			notes.append(t)
	return {
		"title": html.unescape(title),
		"machine": html.unescape(title.split(":")[0]),
		"colors": _palette(style),
		"series": series,
		"panels": [
			{"name": name, "rows": [{"size": sz, "values": rows[sz]} for sz in sorted(rows, key=size_key)]}
			for name, rows in panels.items()
		],
		"notes": notes,
	}


def build_start() -> Page:
	body = (SITE / "content/start.html").read_text(encoding="utf-8")
	body, toc = add_heading_anchors(highlight_code(body))
	return Page("start/", "Get started", body, "start", toc=toc,
	            description="Install DataPath and send your first message from C++, C, Rust, Python or Node.js.")


def build_index(path: str, title: str, intro: str, items: list[Page], section: str) -> Page:
	cards = "".join(
		f'<a class="card" href="{rel(path, p.path)}"><strong>{html.escape(NAV_TITLES.get(p.path, p.title))}</strong>'
		f'<span>{html.escape(p.description or p.title)}</span></a>'
		for p in items
	)
	body = f"<h1>{html.escape(title)}</h1>{intro}<div class=\"cards\">{cards}</div>"
	return Page(path, title, body, section)


# ---------------------------------------------------------------------------
# Template


def render(page: Page, sidebar: dict[str, list[Page]], template: str, rev: str) -> str:
	nav = "".join(
		f'<a href="{rel(page.path, p)}"{" aria-current=\"page\"" if (page.section == ("home" if p == "" else p.rstrip("/"))) else ""}>{label}</a>'
		for p, label in TOP_NAV
	)
	side = ""
	if page.section in sidebar:
		links = []
		for p in sidebar[page.section]:
			cur = ' aria-current="page"' if p.path == page.path else ""
			links.append(f'<a href="{rel(page.path, p.path)}"{cur}>{html.escape(NAV_TITLES.get(p.path, p.title))}</a>')
		side = '<nav class="sidebar" aria-label="Section">' + "".join(links) + "</nav>"
	toc = ""
	if len(page.toc) >= 3 and page.section != "start":
		items = "".join(
			f'<a class="l{lvl}" href="#{slug}">{text}</a>' for lvl, slug, text in page.toc
		)
		toc = f'<nav class="toc" aria-label="On this page"><p>On this page</p>{items}</nav>'
	edit = ""
	if page.source:
		edit = f'<p class="edit"><a href="{REPO_URL}/blob/{BRANCH}/{page.source}">View this page on GitHub</a></p>'
	layout = "home" if page.wide else ("doc" if side else "plain")
	title = page.title if page.section == "home" else f"{page.title} · DataPath"
	desc = page.description or "DataPath: fast, predictable message passing between processes on the same machine."
	out = template
	for key, val in {
		"title": html.escape(re.sub(r"<[^>]+>", "", title)),
		"description": html.escape(desc),
		"root": rel(page.path, ""),
		"nav": nav,
		"sidebar": side,
		"toc": toc,
		"body": page.body,
		"edit": edit,
		"layout": layout,
		"repo": REPO_URL,
		"rev": rev,
	}.items():
		out = out.replace("{{" + key + "}}", val)
	return out


def main() -> None:
	ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
	ap.add_argument("--out", default=str(ROOT / "_site"))
	args = ap.parse_args()
	out = Path(args.out)
	if out.exists():
		shutil.rmtree(out)
	out.mkdir(parents=True)

	mapping_src = collect_pages()
	mapping = {src: dst for src, (dst, _) in mapping_src.items()}
	mapping["README.md"] = ""
	readme = (ROOT / "README.md").read_text(encoding="utf-8")

	docs = build_doc_pages(mapping_src)
	by_path = {p.path: p for p in docs}
	descriptions = {
		"docs/semantics/": "Threads, backpressure, scheduling, placement and the measurements behind the defaults.",
		"docs/building/": "Build the library, run the tests and benchmarks, contribute.",
		"docs/python/": "pip install com.xaymar.datapath: blocking and callback APIs, latency.",
		"docs/node/": "npm install @xaymar/datapath: Node-API addon with TypeScript typings.",
		"docs/rust/": "com-xaymar-datapath: safe Rust wrapper over the C API.",
		"benchmarks/methodology/": "How every number is measured: round trips, runs, worst-case placement, stress.",
	}
	for p in docs:
		p.description = descriptions.get(p.path, p.description)
		if not p.description:
			m = re.search(r"<p>(.*?)</p>", p.body, flags=re.S)
			p.description = re.sub(r"<[^>]+>", "", m.group(1))[:160].rsplit(" ", 1)[0] + "…" if m else p.title

	docs_pages = [by_path[p] for p in DOCS_ORDER if p in by_path]
	bench_pages = [by_path["benchmarks/methodology/"]] + sorted(
		(p for p in docs if p.section == "benchmarks" and p.path != "benchmarks/methodology/"),
		key=lambda p: p.path,
	)

	home = build_home(readme, mapping)
	start = build_start()
	bench_index = build_index(
		"benchmarks/", "Benchmarks",
		"<p class=\"lead\">Every number is a full round trip between two processes, reported at p99.9, with client and server "
		"pinned to the worst placement the machine allows. Each writeup has the full p50, p99 and p99.9 tables.</p>",
		bench_pages, "benchmarks")
	docs_index = build_index(
		"docs/", "Documentation",
		"<p class=\"lead\">The C++ API is documented in its headers under "
		f"<a href=\"{REPO_URL}/tree/{BRANCH}/include/datapath\"><code>include/datapath</code></a>; "
		"these pages cover behaviour, tuning and each binding.</p>",
		docs_pages, "docs")

	sidebar = {"benchmarks": [bench_index] + bench_pages, "docs": [docs_index] + docs_pages}
	NAV_TITLES.setdefault("benchmarks/", "Overview")
	NAV_TITLES["benchmarks/"] = "Overview"
	NAV_TITLES["docs/"] = "Overview"

	template = (SITE / "templates/page.html").read_text(encoding="utf-8")
	rev = git_short_rev()
	for page in [home, start, bench_index, docs_index] + docs:
		dest = out / page.path / "index.html"
		dest.parent.mkdir(parents=True, exist_ok=True)
		dest.write_text(render(page, sidebar, template, rev), encoding="utf-8")

	shutil.copytree(SITE / "static", out, dirs_exist_ok=True)
	(out / "img").mkdir(exist_ok=True)
	for svg in (ROOT / "docs/images").glob("*.svg"):
		shutil.copy2(svg, out / "img" / svg.name)
	(out / ".nojekyll").write_text("", encoding="utf-8")
	print(f"Built {len(docs) + 4} pages into {out}")


if __name__ == "__main__":
	main()
