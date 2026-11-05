#!/usr/bin/env python
#
# Builds the PDF manual. Every firmware folder with a README.md gets its own
# chapter, taken from that README: the first `#` heading is the firmware's
# name, any text before the first section heading is its introduction, and
# every heading of the shallowest level below the title starts a section of
# the chapter. Who worked on a firmware and when it last changed comes from
# the git history of its folder. The bill of materials and the build guide
# are read from the module's page on wgdmodular.de. The result is written
# to manual/build/melon-manual.pdf.
#
import datetime
import hashlib
import html.parser
import json
import os
import pathlib
import posixpath
import re
import subprocess
import sys
import urllib.parse
import urllib.request

ROOT = pathlib.Path(__file__).resolve().parent.parent
MANUAL = ROOT / "manual"
OUT = MANUAL / "build"
REPO = os.environ.get("GITHUB_REPOSITORY", "wgd-modular/melon-firmwares")

SITE = "https://wgdmodular.de"
BOM_PAGE = SITE + "/module/bom/77/"

HEADING = re.compile(r"^(#{1,6})\s+(.*?)\s*#*\s*$")
FENCE = re.compile(r"^\s*(```|~~~)")
LINK = re.compile(r"(?<!!)\[([^\]]*)\]\(([^)\s]+)\)")
IMAGE = re.compile(r"!\[([^\]]*)\]\((\S+?)\)")
# Sections for people who build or flash a firmware themselves, which the
# manual leaves out: the flashing and building chapter covers that.
BUILDER_SECTION = re.compile(
    r"^(build\w*(\s*(&|and)\s*(upload|flash\w*))?(\s+instructions)?|"
    r"flashing\b.*|board setup|hardware platform|cpu load|performance)"
    r"\s*:?$", re.I)

# LED colours that READMEs show as coloured emoji or GitHub math swatches.
EMOJI_COLOURS = {
    "🔴": "#e5484d", "🟠": "#f0883e", "🟡": "#f2c94c", "🟢": "#46a758",
    "🔵": "#3e8ed0", "🟣": "#9b59b6", "🟤": "#8d5b3a", "⚫": "#222222",
    "⚪": "#f2f2f2",
}
# Arrows written as math, which GitHub renders and the manual does not.
ARROWS = {r"$\leftarrow$": "←", r"$\rightarrow$": "→",
          r"$\leftrightarrow$": "↔"}
SWATCH = re.compile(r"\$\\color\{(#[0-9a-fA-F]{6})\}\{\\text\{[^}]*\}\}\$")
# A paragraph that only names who made the firmware, like
# "Creator / origin: **s0ca**.", goes into the chapter's credit line.
CREDIT_LINE = re.compile(r"^(?:creator\s*/\s*origin|created by|origin|"
                         r"authors?)\s*:\s*(.+?)\.?$", re.I | re.S)
EMOJI = re.compile("[\U0001F000-\U0001FAFF☀-➿️‍]")

ACRONYMS = {"fm": "FM", "cv": "CV", "lfo": "LFO", "vco": "VCO"}

LOGINS = {}
NOREPLY = re.compile(r"^(?:\d+\+)?([^@]+)@users\.noreply\.github\.com$")
USER_AGENT = "melon-manual (+https://github.com/{})".format(REPO)


def git(*args):
    return subprocess.run(["git", *args], cwd=ROOT, capture_output=True,
                          text=True, check=True).stdout


def fetch(url):
    request = urllib.request.Request(url, headers={
        "User-Agent": USER_AGENT, "Cookie": "django_language=en"})
    with urllib.request.urlopen(request, timeout=20) as response:
        return response.read()


def download(url, folder):
    """Saves a remote image under manual/build and returns its path."""
    data = fetch(url)
    suffix = pathlib.PurePosixPath(urllib.parse.urlparse(url).path).suffix
    name = hashlib.sha1(url.encode()).hexdigest()[:12] + (suffix or ".png")
    path = OUT / folder / name
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)
    return "/" + str(path.relative_to(ROOT))


def parse_readme(text):
    """Splits a README into its title, introduction and sections.

    The sections are the headings of the shallowest level below the title,
    so a README that only uses `###` headings is split just like one that
    uses `##`. Deeper headings stay inside their section.
    """
    lines = text.splitlines()
    marks = {}
    fenced = False
    for number, line in enumerate(lines):
        if FENCE.match(line):
            fenced = not fenced
        match = None if fenced else HEADING.match(line)
        if match:
            marks[number] = (len(match.group(1)), match.group(2).strip())

    title_line = next((number for number in sorted(marks)
                       if marks[number][0] == 1), None)
    title = marks[title_line][1] if title_line is not None else None
    below = [level for number, (level, _) in marks.items()
             if number != title_line and level > 1]
    split = min(below) if below else None

    intro, sections = [], []
    for number, line in enumerate(lines):
        if number == title_line:
            continue
        if number in marks and marks[number][0] == split:
            sections.append({"title": marks[number][1], "lines": []})
        elif sections:
            sections[-1]["lines"].append(line)
        else:
            intro.append(line)
    for section in sections:
        body = "\n".join(section.pop("lines")).strip()
        # A rule between sections is only a separator on GitHub.
        section["body"] = re.sub(r"\n\s*(---+|\*\*\*+)\s*$", "", body).strip()
    return title, "\n".join(intro).strip(), sections


def split_controls(body):
    """Splits a controls section into its overview and the detail below it.

    The overview is everything up to the first sub-heading, a table or a
    list of the controls. The manual puts it in a plaque.
    """
    lines = body.splitlines()
    fenced = False
    for number, line in enumerate(lines):
        if FENCE.match(line):
            fenced = not fenced
        if not fenced and HEADING.match(line):
            return ("\n".join(lines[:number]).strip(),
                    "\n".join(lines[number:]).strip())
    return body, ""


def plain(text):
    text = re.sub(r"<!--.*?-->", "", text)
    text = re.sub(r"!?\[([^\]]*)\]\([^)]*\)", r"\1", text)
    return re.sub(r"[*_`]", "", " ".join(text.split()))


def summary(intro, sections):
    """The first sentence of a firmware's description, as plain text.

    That is the first prose paragraph, skipping sections that only credit
    people.
    """
    candidates = [intro] + [section["body"] for section in sections
                            if not re.match(r"(authors?|credits?|license)\b",
                                            section["title"], re.I)]
    for text in candidates:
        for block in text.split("\n\n"):
            if block.strip() and block.lstrip()[0] not in "|-*>!<#`0123456789":
                sentence = first_sentence(plain(block))
                if sentence:
                    return sentence
    return ""


def first_sentence(text):
    match = re.match(r"(.+?[.!?:])(\s|$)", text)
    sentence = match.group(1) if match else text
    return sentence[:-1] + "." if sentence.endswith(":") else sentence


def display_title(title, folder):
    title = title or folder
    if title == title.lower():
        title = " ".join(ACRONYMS.get(word, word.capitalize())
                         for word in title.replace("_", " ").split())
    return title


def resolve_links(text, folder, ids):
    """Points links at a chapter or at the repository on GitHub.

    A link to the README or the folder of another firmware goes to its
    chapter. A link whose text is just the path of a README is shown as the
    name of that firmware instead.
    """
    def chapter_link(label, path):
        chapter = re.fullmatch(r"([^/]+)(/README\.md)?/?", path)
        if chapter and chapter.group(1) in ids:
            if label.endswith("README.md"):
                label = display_title(None, chapter.group(1))
            return "[{}](chapter:{})".format(label, chapter.group(1))
        return None

    def replace(match):
        label, url = match.groups()
        if label.startswith("!"):
            return match.group(0)
        own = re.match(r"https://github\.com/{}/(?:tree|blob)/main/(.*)$"
                       .format(re.escape(REPO)), url)
        if own:
            return chapter_link(label, own.group(1)) or match.group(0)
        if re.match(r"([a-z][a-z0-9+.-]*:|#)", url, re.I):
            return match.group(0)
        path = posixpath.normpath(posixpath.join(folder, url))
        path, _, anchor = path.partition("#")
        return chapter_link(label, path) or "[{}](https://github.com/{}/blob/main/{})".format(
            label, REPO, path)
    return LINK.sub(replace, text)


def local_images(text, folder):
    """Downloads remote images and drops badges.

    Typst only reads local files. Badges just repeat what the manual says
    around them.
    """
    def replace(match):
        alt, url = match.groups()
        if not re.match(r"https?://", url):
            return match.group(0)
        if "img.shields.io" in url:
            return ""
        try:
            return "![{}]({})".format(alt, download(url, "images"))
        except OSError as error:
            print("  warning: could not fetch {} for {}: {}".format(
                url, folder, error), file=sys.stderr)
            return alt
    return IMAGE.sub(replace, text)


def swatches(text):
    """Turns colour emoji and math colour swatches into coloured dots."""
    for math, arrow in ARROWS.items():
        text = text.replace(math, arrow)
    text = SWATCH.sub(r'<!--raw-typst #swatch("\1")-->', text)
    for emoji, colour in EMOJI_COLOURS.items():
        text = text.replace(emoji, '<!--raw-typst #swatch("{}")-->'.format(
            colour))
    return EMOJI.sub("", text)


def github_api(path):
    request = urllib.request.Request("https://api.github.com/" + path,
                                     headers={"Accept":
                                              "application/vnd.github+json"})
    token = os.environ.get("GITHUB_TOKEN") or os.environ.get("GH_TOKEN")
    if token:
        request.add_header("Authorization", "Bearer " + token)
    try:
        with urllib.request.urlopen(request, timeout=10) as response:
            return json.load(response)
    except (OSError, ValueError):
        return None


def github_login(email, sha=None):
    """The GitHub account behind a commit email, if GitHub knows it."""
    noreply = NOREPLY.match(email)
    if noreply:
        return noreply.group(1)
    if sha:
        commit = github_api("repos/{}/commits/{}".format(REPO, sha)) or {}
        login = (commit.get("author") or {}).get("login")
        if login:
            return login
    # Co-authors are not linked on the commit, look for any commit of theirs.
    found = github_api("search/commits?per_page=1&q=author-email:" +
                       urllib.parse.quote(email)) or {}
    for item in found.get("items", []):
        return (item.get("author") or {}).get("login")
    return None


def history(folder):
    """Authors, first and last change of a folder, oldest author first.

    The pathspec ignores case, as some folders were renamed to lower case.
    """
    log = git("log", "--reverse", "--format=%H%x1f%an%x1f%ae%x1f%as%x1f%s%x1f"
              "%(trailers:key=Co-authored-by,valueonly,separator=%x1e)%x1d",
              "--", ":(icase){}".format(folder.name))
    entries = [entry.strip("\n").split("\x1f")
               for entry in log.split("\x1d") if entry.strip()]
    if not entries:
        return None

    people = {}
    for sha, name, email, date, subject, coauthors in entries:
        candidates = [(name, email)]
        for trailer in filter(None, coauthors.split("\x1e")):
            match = re.match(r"\s*(.*?)\s*<(.*)>", trailer)
            if match:
                candidates.append(match.groups())
        for person_name, person_email in candidates:
            if person_name.endswith("[bot]"):
                continue
            key = person_email.lower()
            if key not in people:
                people[key] = {"name": person_name, "email": key}
            if person_email == email:
                people[key].setdefault("sha", sha)

    # The same person often commits under more than one name or address,
    # merge them by GitHub account and keep the most complete name.
    authors = {}
    for person in people.values():
        if person["email"] not in LOGINS:
            LOGINS[person["email"]] = github_login(person["email"],
                                                   person.get("sha"))
        login = LOGINS[person["email"]]
        key = login or person["email"]
        name = person["name"]
        if name == name.lower() and " " in name:
            name = name.title()
        if key in authors:
            known = authors[key]["name"]
            name = known if " " in known or " " not in name else name
        authors[key] = {"name": name, "login": login}

    first, last = entries[0], entries[-1]
    return {
        "authors": list(authors.values()),
        "added": first[3],
        "updated": last[3],
        "commit": last[0][:7],
        "subject": last[4],
    }


def same_person(short, full):
    """Whether two names are one person, the first name is just shortened."""
    first, last = short.lower().split(), full.lower().split()
    return (len(first) > 1 and first[1:] == last[1:] and first[0] != last[0]
            and last[0].startswith(first[0]))


def firmware_folders():
    """The folders CI builds a firmware from: `name/name.ino`, no .ci-skip."""
    return sorted(folder for folder in ROOT.iterdir()
                  if (folder / (folder.name + ".ino")).is_file()
                  and (folder / "README.md").is_file()
                  and not (folder / ".ci-skip").exists())


def collect_firmwares():
    folders = firmware_folders()
    ids = {folder.name for folder in folders}
    firmwares = []
    for folder in folders:
        text = (folder / "README.md").read_text()
        text = swatches(local_images(text, folder.name))
        title, intro, sections = parse_readme(resolve_links(
            text, folder.name, ids))
        sections = [section for section in sections
                    if not BUILDER_SECTION.match(section["title"])]
        credit = ""
        paragraphs = [part for part in intro.split("\n\n") if part.strip()]
        for paragraph in list(paragraphs):
            match = CREDIT_LINE.match(paragraph.strip())
            if match:
                credit = plain(match.group(1))
                paragraphs.remove(paragraph)
        intro = "\n\n".join(paragraphs)
        for section in list(sections):
            if re.fullmatch(r"authors?", section["title"], re.I):
                first, _, rest = section["body"].partition("\n\n")
                credit = plain(first)
                intro = "\n\n".join(part for part in (intro, rest.strip())
                                    if part)
                sections.remove(section)
        for section in sections:
            if section["title"] == section["title"].lower():
                section["title"] = section["title"].capitalize()
            section["controls"] = bool(re.fullmatch(
                r"controls?\s*:?", section["title"], re.I))
            if section["controls"]:
                section["body"], section["rest"] = split_controls(
                    section["body"])
        firmwares.append({
            "id": folder.name,
            "title": display_title(title, folder.name),
            "intro": intro,
            "summary": summary(intro, sections),
            "sections": sections,
            "credit": credit,
            "path": folder.name,
            "history": history(folder),
        })

    names = {}
    for firmware in firmwares:
        for author in (firmware["history"] or {}).get("authors", []):
            known = names.get(author["login"], "")
            if author["login"] and (" " in author["name"] or not known):
                names[author["login"]] = author["name"]
    for firmware in firmwares:
        for author in (firmware["history"] or {}).get("authors", []):
            author["name"] = names.get(author["login"], author["name"])

    # An address GitHub does not know may belong to a person who is on
    # GitHub under a longer form of the name.
    known = {a["login"]: a["name"] for fw in firmwares
             for a in (fw["history"] or {}).get("authors", [])
             if a["login"]}
    for firmware in firmwares:
        authors = (firmware["history"] or {}).get("authors", [])
        for author in authors:
            if author["login"] is None:
                for login, name in known.items():
                    if same_person(author["name"], name):
                        author.update(name=name, login=login)
        seen, unique = set(), []
        for author in authors:
            key = author["login"] or author["name"]
            if key not in seen:
                seen.add(key)
                unique.append(author)
        if firmware["history"]:
            firmware["history"]["authors"] = unique
    return firmwares


class GuideParser(html.parser.HTMLParser):
    """Turns the build guide on the module's page into Markdown.

    Every `<h5>` starts a step, what comes before the first one is the
    introduction.
    """

    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.depth = 0
        self.intro, self.steps = [], []
        self.text, self.href, self.heading = "", None, False

    def target(self):
        return self.steps[-1]["blocks"] if self.steps else self.intro

    def flush(self):
        text = " ".join(self.text.split())
        if text and not self.heading:
            self.target().append(text)
        self.text = ""

    def handle_starttag(self, tag, attrs):
        attrs = dict(attrs)
        if self.depth == 0:
            if tag == "div" and attrs.get("id") == "collapse-build-info":
                self.depth = 1
            return
        if tag == "div":
            self.depth += 1
        elif tag in ("p", "li", "h1", "h2", "h3", "h4", "h5", "h6"):
            self.flush()
            self.heading = tag[0] == "h"
            if tag == "li":
                self.text = "- "
        elif tag in ("strong", "b"):
            self.text += "**"
        elif tag in ("em", "i"):
            self.text += "*"
        elif tag == "br":
            self.text += " "
        elif tag == "a":
            self.href = urllib.parse.urljoin(BOM_PAGE, attrs.get("href", ""))
            self.text += "["
        elif tag == "img":
            self.flush()
            url = urllib.parse.urljoin(BOM_PAGE, attrs.get("src", ""))
            self.target().append("![{}]({})".format(
                attrs.get("alt", ""), download(url, "bom")))

    def handle_endtag(self, tag):
        if self.depth == 0:
            return
        if tag == "div":
            self.depth -= 1
            if self.depth == 0:
                self.flush()
        elif tag in ("h1", "h2", "h3", "h4", "h5", "h6"):
            title = re.sub(r"^\d+\.\s*", "", " ".join(self.text.split()))
            self.steps.append({"title": title, "blocks": []})
            self.text, self.heading = "", False
        elif tag in ("p", "li"):
            self.flush()
        elif tag in ("strong", "b"):
            self.text += "**"
        elif tag in ("em", "i"):
            self.text += "*"
        elif tag == "a":
            self.text += "]({})".format(self.href)

    def handle_data(self, data):
        if self.depth:
            self.text += data


class PartsParser(html.parser.HTMLParser):
    """Reads the rows of the bill of materials on the module's page.

    A row has the ID, references, package, quantity, whether it comes
    pre-populated (a check icon), description and a link to a supplier.
    """

    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.rows, self.row, self.cell = [], None, None
        self.inside = False

    def handle_starttag(self, tag, attrs):
        attrs = dict(attrs)
        if tag == "tbody":
            self.inside = True
        elif self.inside and tag == "tr":
            self.row = []
        elif self.row is not None and tag == "td":
            self.cell = {"text": "", "check": False, "href": None}
        elif self.cell is not None:
            if tag == "svg" and "check" in attrs.get("class", ""):
                self.cell["check"] = True
            elif tag == "a":
                self.cell["href"] = attrs.get("href")

    def handle_endtag(self, tag):
        if tag == "tbody":
            self.inside = False
        elif tag == "td" and self.cell is not None:
            self.cell["text"] = " ".join(self.cell["text"].split())
            self.row.append(self.cell)
            self.cell = None
        elif tag == "tr" and self.row is not None:
            if len(self.row) >= 7:
                self.rows.append(self.row)
            self.row = None

    def handle_data(self, data):
        if self.cell is not None:
            self.cell["text"] += data


def supplier(url):
    if not url:
        return None
    host = urllib.parse.urlparse(url).hostname or url
    name = host.split(".")[-2] if host.count(".") else host
    return {"url": url, "name": {"aliexpress": "AliExpress"}.get(
        name, name.capitalize())}


def collect_build():
    """The build guide and the bill of materials from wgdmodular.de.

    Without a connection the manual still builds, pointing at the page.
    """
    try:
        page = fetch(BOM_PAGE).decode("utf-8")
        guide = GuideParser()
        guide.feed(page)
    except OSError as error:
        print("  warning: could not fetch the build guide: {}".format(error),
              file=sys.stderr)
        return {"page": BOM_PAGE, "guide": None}

    table = PartsParser()
    table.feed(page)
    parts = [{
        "references": row[1]["text"],
        "package": row[2]["text"],
        "quantity": row[3]["text"],
        "fitted": row[4]["check"],
        "part": row[5]["text"],
        "supplier": supplier(row[6]["href"]),
    } for row in table.rows]
    revision = re.search(r"Rev\.\s*\d+", page)
    return {
        "page": BOM_PAGE,
        "revision": revision.group(0) if revision else None,
        "guide": {
            "intro": "\n\n".join(guide.intro),
            "steps": [{"title": step["title"],
                       "body": "\n\n".join(step["blocks"])}
                      for step in guide.steps],
        },
        "parts": parts,
    }


def collect():
    manifest = json.loads((ROOT / ".release-please-manifest.json").read_text())
    head = git("log", "-1", "--format=%H%x1f%cs").strip().split("\x1f")
    version = manifest.get(".")
    tags = git("tag", "--points-at", "HEAD").split()
    return {
        "repo": REPO,
        "version": version,
        "release": "v{}".format(version) in tags,
        "commit": head[0][:7],
        "date": head[1],
        "built": datetime.date.today().isoformat(),
        "firmwares": collect_firmwares(),
        "build": collect_build(),
    }


def main():
    OUT.mkdir(exist_ok=True)
    data = collect()
    (OUT / "data.json").write_text(json.dumps(data, indent=2,
                                              ensure_ascii=False))
    print("Collected {} firmwares".format(len(data["firmwares"])))
    pdf = OUT / "melon-manual.pdf"
    result = subprocess.call([
        "typst", "compile", "--root", str(ROOT),
        "--font-path", str(MANUAL / "fonts"), "--ignore-system-fonts",
        str(MANUAL / "manual.typ"), str(pdf),
    ])
    if result == 0:
        print("  -> {}".format(pdf.relative_to(ROOT)))
    sys.exit(result)


if __name__ == "__main__":
    main()
