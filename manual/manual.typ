// The melon manual. The general chapters are written here, the firmware
// chapters come from the README of every firmware folder and the build
// chapter from the module's page on wgdmodular.de, all collected into
// build/data.json by build.py.

#import "@preview/cmarker:0.1.10"

#let data = json("build/data.json")

// The panel: black with yellow stars, white labels and grey rules.
#let night = rgb("#0b0a09")
#let night-soft = rgb("#1d1b18")
#let melon = rgb("#cec74c")
#let melon-deep = rgb("#78721a")
#let flesh = rgb("#e4566e")
#let flesh-deep = rgb("#b03651")
#let rind = rgb("#5b9a43")
#let rind-deep = rgb("#3e7330")
#let sky = rgb("#4f8ec4")
#let sky-deep = rgb("#2f6795")
#let steel = rgb("#a3a3a3")
#let paper = rgb("#fbfaf4")
#let paper-deep = rgb("#f1efe2")
#let ink = rgb("#1d1b18")
#let ink-soft = rgb("#5e5a50")
#let accents = (flesh, rind, melon, sky)
#let deep-of = (
  (flesh.to-hex()): flesh-deep,
  (rind.to-hex()): rind-deep,
  (melon.to-hex()): melon-deep,
  (sky.to-hex()): sky-deep,
)
#let deep(colour) = deep-of.at(colour.to-hex(), default: colour)

#let accent = state("accent", flesh)
#let eyebrow = state("eyebrow", none)
#let display = "Montserrat"
// The variable font registers itself with its default optical size.
#let sans = "Nunito Sans 12pt"

// One star field the size of the page, so no pattern repeats. Covers use
// all of it, chapter heads its top.
#let starfield = image("assets/stars.svg", width: 148mm, height: 210mm)
#let stars = tiling(size: (148mm, 210mm), starfield)

#let mark(fill: melon, width: 16mm) = image(bytes(read("assets/mark.svg")
  .replace("#CEC74C", fill.to-hex())), format: "svg", width: width)

#let diamond(fill: steel, size: 4pt) = box(baseline: -0.15em,
  rotate(45deg, reflow: true, rect(width: size, height: size, fill: fill)))

#let sparkle(fill: melon, size: 7pt) = box(width: size, height: size,
  baseline: 12%, polygon(fill: fill,
    (size * 0.5, 0pt), (size * 0.62, size * 0.38), (size, size * 0.5),
    (size * 0.62, size * 0.62), (size * 0.5, size), (size * 0.38, size * 0.62),
    (0pt, size * 0.5), (size * 0.38, size * 0.38)))

#let swatch(colour) = box(baseline: 0.12em, circle(radius: 0.36em,
  fill: rgb(colour), stroke: 0.4pt + ink-soft.lighten(40%)))

#let month(date) = {
  let (y, m, d) = date.split("-").map(int)
  datetime(year: y, month: m, day: d).display(
    "[day padding:none] [month repr:long] [year]")
}

#let markdown(text, base: none) = cmarker.render(text, scope: (
  image: (path, alt: none) => align(center, image(
    if path.starts-with("/") or base == none { path }
    else { "/" + base + "/" + path }, alt: alt, width: 72%)),
  link: (dest, body) => if type(dest) == label {
    body
  } else if dest.starts-with("chapter:") {
    std.link(label(dest.slice(8)), body)
  } else {
    std.link(dest, body)
  },
  swatch: swatch,
))

// ---------------------------------------------------------------- styles

#set document(title: "Melon manual", author: "wgd modular")
#set text(font: sans, size: 9pt, fill: ink, lang: "en")
#set par(justify: false, leading: 0.66em, spacing: 0.95em)
#set page(
  paper: "a5",
  margin: (x: 15mm, top: 19mm, bottom: 16mm),
  fill: paper,
  header: context {
    let page-no = here().page()
    let openers = query(heading.where(level: 1))
    if openers.any(h => h.location().page() == page-no) { return }
    let before = query(heading.where(level: 1).before(here()))
    grid(columns: (1fr, auto), align: (left + bottom, right + bottom),
      text(font: display, style: "italic", weight: 600, size: 7pt,
        fill: ink-soft, if before.len() > 0 { before.last().body }),
      text(font: display, style: "italic", weight: 800, size: 8pt,
        fill: ink)[MELON])
    v(-1.2mm)
    line(length: 100%, stroke: 0.6pt + steel)
  },
  footer: context align(center, {
    diamond(size: 3.6pt)
    h(2.2mm)
    text(font: display, style: "italic", weight: 800, size: 7.5pt,
      counter(page).display())
    h(2.2mm)
    diamond(size: 3.6pt)
  }),
)

#show link: set text(fill: flesh-deep)
#set strong(delta: 300)
#show raw: set text(font: "DejaVu Sans Mono", size: 0.9em)
#show raw.where(block: true): it => block(width: 100%, fill: paper-deep,
  radius: 2mm, inset: (x: 3.5mm, y: 3mm), it)

#set list(body-indent: 0.6em, spacing: 0.65em)
#set enum(spacing: 0.65em)

// Tables on a black plaque take the colours of the panel.
#let dark = state("dark", false)
#set table(stroke: none, inset: (x: 5pt, y: 4.2pt))
#show table.cell.where(y: 0): set text(fill: white, weight: 700)
#show table: it => {
  // Markdown tables come with auto columns. A two column table lets the
  // second take the rest, wider ones are left to fit their content.
  if it.columns.len() == 2 and it.columns.last() == auto {
    table(columns: it.columns.slice(0, -1) + (1fr,), align: it.align,
      ..it.children)
  } else {
    // A table stays on one page unless it is too tall for that.
    context {
      let inner = { set text(size: 8pt); it }
      let height = measure(block(width: 118mm, inner)).height
      let edge = if dark.get() { steel.darken(45%) }
        else { accent.get().lighten(45%) }
      block(radius: 2mm, breakable: height > 110mm, stroke: 0.6pt + edge,
        clip: height <= 110mm, inner)
    }
  }
}

// Colours a chapter: headings, tables and list markers take its accent.
#let themed(colour, body) = {
  accent.update(colour)
  set table(fill: (_, y) => if y == 0 { deep(colour) }
    else if calc.even(y) { paper-deep })
  set list(marker: diamond(fill: colour, size: 3.4pt))
  body
}

// A chapter opens like the top of the panel: the title in white on the
// star field, a grey rule below it and a dot where the panel has its
// mounting hole.
#let head-pad = 12mm
#show heading.where(level: 1): it => {
  pagebreak(weak: true)
  context {
    let colour = accent.get()
    let kicker = eyebrow.get()
    // A black backing keeps the stars out from under the letters.
    let backed(body) = box(fill: night, outset: (x: 1.8mm, y: 1.4mm), body)
    let content = block(width: 148mm - 2 * head-pad, {
      if kicker != none {
        block(below: 1.6mm, backed(text(font: display, weight: 700,
          size: 6.5pt, tracking: 0.18em, fill: steel, upper(kicker))))
      }
      grid(columns: (1fr, 10mm), align: (left + bottom, right + horizon),
        backed(text(font: display, style: "italic", weight: 800, size: 21pt,
          fill: white, it.body)),
        circle(radius: 2.4mm, fill: colour, stroke: 1.6mm + night))
      v(2.6mm)
      line(length: 100%, stroke: 0.8pt + steel)
    })
    let head = 9mm + measure(content).height + 6mm
    place(top + left, dx: -15mm, dy: -19mm, block(width: 148mm,
      height: head, fill: stars, inset: (x: head-pad, top: 9mm), content))
    v(head - 19mm + 7mm)
  }
}
#show heading.where(level: 2): it => context block(above: 1.4em,
  below: 0.75em, sticky: true, {
    sparkle(fill: accent.get(), size: 7.5pt)
    h(2mm)
    text(font: display, weight: 700, size: 11.5pt, fill: deep(accent.get()),
      it.body)
  })
#show heading.where(level: 3): it => block(above: 1.3em, below: 0.6em,
  sticky: true, text(font: display, weight: 600, size: 9.5pt, fill: ink,
    it.body))
#show heading.where(level: 4): it => block(above: 1.1em, below: 0.5em,
  sticky: true, text(font: display, weight: 600, size: 8.5pt, fill: ink-soft,
    it.body))

// A black plaque like a piece of the panel, for the controls of a firmware.
// It stays on one page unless it is too tall for that.
#let plaque(colour: flesh, body) = context {
  let inner = block(width: 100%, radius: 2.2mm, inset: 3.6mm,
    stroke: 0.5pt + steel.darken(35%), {
      set text(fill: paper)
      show link: set text(fill: melon)
      set table(fill: (_, y) => if y == 0 { deep(colour) }
        else if calc.even(y) { night-soft })
      set list(marker: diamond(fill: colour, size: 3.4pt))
      dark.update(true)
      body
      dark.update(false)
    })
  let height = measure(inner, width: 118mm - 2.8mm).height
  block(width: 100%, fill: night, radius: 3mm, inset: 1.4mm,
    breakable: height > 140mm, inner)
}

// A light note with a coloured edge.
#let note(colour: melon, body) = block(width: 100%, fill: paper-deep,
  radius: (right: 2mm), inset: (left: 4mm, rest: 3.2mm),
  stroke: (left: 2.2pt + colour), body)

// ----------------------------------------------------------------- cover

#let wgd(size: 13pt, fill: white) = {
  diamond(size: size * 0.42)
  h(size * 0.45)
  text(font: display, style: "italic", weight: 800, size: size, fill: fill,
    "WGD")
  h(size * 0.45)
  diamond(size: size * 0.42)
}

#let release = {
  if data.release [Release #data.version] else [Development build]
  [ · #month(data.date)]
}

#page(margin: 0pt, header: none, footer: none, fill: stars)[
  #place(top + left, dx: 104mm, dy: 14mm, block(height: 182mm,
    radius: 1.6mm, clip: true, stroke: 0.6pt + steel.darken(40%),
    image("assets/panel.png", height: 100%)))
  #place(top + left, dx: 13mm, dy: 44mm, block(width: 80mm, fill: night,
    radius: 4mm, inset: (x: 7mm, y: 8mm), stroke: 0.8pt + steel.darken(35%), {
      mark(width: 19mm)
      v(5mm)
      text(font: display, style: "italic", weight: 800, size: 38pt,
        fill: white, tracking: 0.02em)[MELON]
      v(-1mm)
      line(length: 100%, stroke: 0.8pt + steel)
      v(2.5mm)
      text(font: display, style: "italic", weight: 600, size: 12pt,
        fill: melon)[User manual]
      v(4mm)
      text(size: 8.5pt, fill: paper.darken(18%))[The melon Eurorack module
        by wgd modular, how to build it, and every firmware it runs.]
      v(6mm)
      box(radius: 2mm, inset: (x: 3mm, y: 2mm), stroke: 0.6pt + melon,
        text(font: display, weight: 700, size: 7pt, fill: melon, release))
    }))
  #place(bottom + left, dx: 13mm, dy: -12mm, block(width: 80mm, {
    line(length: 100%, stroke: 0.8pt + steel)
    v(3.5mm)
    align(center, wgd(size: 15pt))
  }))
]

#counter(page).update(1)

// -------------------------------------------------------------- contents

#let firmware-ids = data.firmwares.map(fw => fw.id)

#page(header: none)[
  #v(2mm)
  #text(font: display, style: "italic", weight: 800, size: 22pt)[Contents]
  #v(4mm)
  #show outline.entry.where(level: 1): it => {
    let el = it.element
    if el.has("label") and str(el.label) in firmware-ids {
      pad(left: 5mm, top: -0.2mm, text(fill: ink-soft, it))
    } else {
      v(1.6mm)
      text(weight: 700, it)
    }
  }
  #outline(title: none, depth: 1, indent: 0pt)
  #v(1fr)
  #note(colour: melon)[
    #set text(size: 7.5pt, fill: ink-soft)
    This manual is generated from the firmware descriptions in the
    #box(link("https://github.com/" + data.repo)[melon-firmwares])
    repository and the module's page on wgdmodular.de. It was built from
    commit #raw(data.commit) of #month(data.date).
  ]
]

// ------------------------------------------------------------ the module

#themed(flesh)[
= The module

The melon is a 4 HP Eurorack module around the Seeed Studio XIAO RP2350, a
thumb-sized board with a Raspberry Pi RP2350 microcontroller. It is a
reworked version of Hagiwo's MOD2: the layout is symmetrical, so the panel
can be flipped, it has its own 5 V regulator and a standard 2×5 power
header, and its RGB LED sits right behind the melon on the panel, which
lights up with it.

What the module does is entirely up to the firmware it runs, from drums and
oscillators to sample players and effects. You can flash a different one at
any time, and this manual has a chapter for each of them.

#note(colour: flesh)[
  The firmwares in this manual are written for melons with the *WS2812B*
  RGB LED. A melon with the plain 3 mm LED runs the original MOD2 firmwares,
  which you can flash from
  #link("https://dl.modulove.io/mod2/")[modulove's web flasher].
]

== Specifications

#table(columns: (auto, 1fr),
  table.header([Feature], [Details]),
  [Format], [Eurorack, 3 U, 4 HP, 35 mm deep],
  [Power], [+12 V 72 mA, −12 V 8 mA, 2×5 header, onboard 5 V regulator],
  [Platform], [Seeed Studio XIAO RP2350, Raspberry Pi RP2350 with two Arm
    Cortex-M33 cores at 150 MHz, 520 KB RAM, 2 MB flash],
  [Audio], [One output, PWM from the RP2350, filtered on the module],
  [Inputs], [Two gate inputs, one CV input],
  [Controls], [Three knobs, one button],
  [LED], [WS2812B RGB LED behind the melon],
  [Firmware], [Changed over USB, see the chapter on flashing],
)

#pagebreak(weak: true)

== The panel

#let panel-height = 150mm
// Heights on the panel, in millimetres from its top edge.
#let at(mm) = panel-height * mm / 128.5
#let callouts = (
  (19.0, 0.5, [POT1], [A0], [The top knob. What it sets depends on the
    firmware, often pitch, speed or a sound's main colour.]),
  (40.6, 0.5, [POT2], [A1], [The middle knob.]),
  (61.7, 0.5, [POT3], [A2], [The bottom knob, added to the CV input. Pitch
    for most firmwares that play notes.]),
  (76.6, 0.46, [LED], [D3], [Shines through the melon. Its colour shows the
    mode, model or page, and it flashes on triggers.]),
  (87.3, 0.5, [Button], [D4], [Switches modes or plays a sound.]),
  (96.6, 0.74, [IN1 and IN2], [D5, D6], [Inputs 1 and 2: triggers, gates
    and clocks.]),
  (109.3, 0.74, [CV and OUT], [A2, D7], [CV in on the left, adding to
    POT3. Audio out on the right.]),
)
#let panel-width = panel-height * 237 / 1518
#block(height: panel-height, width: 100%, {
  place(top + left, block(radius: 1mm, clip: true,
    image("assets/panel.png", height: panel-height)))
  for (mm, x, name, pin, body) in callouts {
    let y = at(mm)
    let from = panel-width * x
    place(top + left, dx: from, dy: y - 0.6mm, circle(radius: 0.6mm,
      fill: flesh))
    place(top + left, line(start: (from + 0.6mm, y),
      end: (panel-width + 9mm, y), stroke: 0.5pt + flesh))
    place(top + left, dx: panel-width + 11mm, dy: y - 3.2mm,
      block(width: 118mm - panel-width - 11mm, {
        text(font: display, weight: 700, size: 9pt, fill: flesh-deep, name)
        h(1.5mm)
        text(size: 7pt, fill: ink-soft, pin)
        linebreak()
        text(size: 8pt, body)
      }))
  }
})

#pagebreak(weak: true)

== Controls in general

Every firmware works with the same three knobs, one button, two inputs, a CV
input and one output. The table shows which pin of the XIAO each one is
wired to. The firmware chapters call the knobs POT1 to POT3, top to bottom,
and the jacks IN1, IN2, CV and OUT.

#table(columns: (auto, auto, 1fr),
  table.header([Control], [XIAO pin], [Used for]),
  [POT1], [A0], [Top knob],
  [POT2], [A1], [Middle knob],
  [POT3 and CV], [A2], [Bottom knob, added to the CV input],
  [IN1], [D5, GPIO 7], [Trigger, gate or clock input],
  [IN2], [D6, GPIO 0], [Second gate input],
  [Button], [D4, GPIO 6], [Pressed connects to ground],
  [LED], [D3, GPIO 5], [WS2812B RGB LED],
  [OUT], [D7, GPIO 1], [PWM audio output],
)

POT3 and the CV jack are added up before the XIAO reads them, so the knob
sets the starting point and the CV moves it from there. The stage that adds
them inverts the voltage, which every firmware corrects for, so turning the
knob clockwise or raising the CV always counts as up. The CV spans about
five volts, enough for five octaves where a firmware tracks 1 V/oct.
]

// ---------------------------------------------------------------- build

#let build = data.build
#let guide = build.guide

#themed(rind)[
= Building the module

#if guide == none [
  The build guide and the bill of materials are on the
  #link(build.page)[melon's page on wgdmodular.de].
] else [
  #markdown(guide.intro)

  == Bill of materials

  #let fitted = build.parts.any(p => p.fitted)
  #table(
    columns: if fitted { (auto, auto, 1fr, 1.3fr, auto, auto) }
      else { (auto, auto, 1fr, 1.3fr, auto) },
    table.header([Qty], [Reference], [Part], [Package],
      ..if fitted { ([Fitted],) } else { () }, [Source]),
    ..build.parts.map(p => (
      [#p.quantity], [#p.references], [#p.part],
      text(size: 7pt, p.package),
      ..if fitted { (if p.fitted [yes] else [],) } else { () },
      if p.supplier == none [] else { link(p.supplier.url, p.supplier.name) },
    )).flatten(),
  )

  The list names both LEDs. Fit only one of them, the WS2812B for the
  firmwares in this manual. The parts and their sources are also on the
  #link(build.page)[bill of materials#if build.revision != none [
  for #build.revision]] on wgdmodular.de.

  #pagebreak(weak: true)
  == Step by step

  #for (number, step) in guide.steps.enumerate(start: 1) {
    block(above: 4.5mm, below: 2mm, sticky: true, grid(
      columns: (7mm, 1fr), column-gutter: 3mm, align: horizon,
      circle(radius: 3.5mm, fill: night, align(center + horizon, text(
        font: display, style: "italic", weight: 800, size: 9pt,
        fill: melon, str(number)))),
      text(font: display, weight: 700, size: 10.5pt, fill: rind-deep,
        step.title)))
    pad(left: 10mm, markdown(step.body))
  }
]
]

// -------------------------------------------------------------- flashing

#let repo-url = "https://github.com/" + data.repo
#let flasher = "wgdmodular.de/melon/list"

#themed(sky)[
= Flashing and building

#note(colour: flesh)[
  *Always disconnect the Eurorack power before you connect the module to
  your computer over USB.*
]

The quickest way to change the firmware is the web flasher, which runs in
your browser and needs nothing installed.

+ Open #link("https://" + flasher + "/", flasher) in Google Chrome or
  Microsoft Edge. Safari and Firefox lack the Web Serial support the
  flasher needs.
+ Connect the XIAO RP2350 on the back of the module to your computer with a
  USB-C cable.
+ Press *Flash* on the firmware you want and pick the XIAO from the list of
  serial ports. The page restarts it into its bootloader.
+ When the page asks for the drive, choose the one called *RP2350*. The page
  writes the firmware to it and the module restarts with the new firmware.

== By hand

Every firmware is also attached as a `.uf2` file to each
#link(repo-url + "/releases/latest")[release] on GitHub, and the flasher has
a download button for each one.

+ Hold the *B* (boot) button on the XIAO while you connect it, or hold it and
  tap *R* (reset) if it is already connected. A drive called *RP2350*
  appears.
+ Copy the `.uf2` file onto that drive. The XIAO restarts with the new
  firmware as soon as the copy is done.

== Building from source

The firmwares are open source and live in the
#link(repo-url)[melon-firmwares] repository on GitHub, one folder per
firmware. They build with #link("https://arduino.github.io/arduino-cli/")[
arduino-cli] and Earle Philhower's RP2350 core:

```sh
git clone https://github.com/wgd-modular/melon-firmwares
cd melon-firmwares
URL=https://github.com/earlephilhower/arduino-pico/releases/download/global/package_rp2040_index.json
arduino-cli core update-index --additional-urls $URL
arduino-cli core install rp2040:rp2040 --additional-urls $URL
arduino-cli lib install "Adafruit NeoPixel" "Bounce2"
arduino-cli compile --fqbn rp2040:rp2040:seeed_xiao_rp2350 \
  --output-dir build braids
```

This builds `build/braids.ino.uf2`, ready to copy onto the module. A firmware
that needs more libraries lists them in a `libraries.txt` in its folder. In
the Arduino IDE, add the same URL under *Additional Boards Manager URLs*,
install *Raspberry Pi Pico/RP2040/RP2350* and pick the *Seeed XIAO RP2350*
board. `pixi run manual` builds this manual.
]

// ------------------------------------------------------------- firmwares

#let colour-of(fw) = accents.at(calc.rem(
  data.firmwares.position(other => other.id == fw.id), accents.len()))

#themed(melon)[
= Firmwares

The melon runs #data.firmwares.len() firmwares, each with its own chapter
that describes all of its controls.

#let entry(fw) = {
  let colour = colour-of(fw)
  block(below: 3.4mm, breakable: false, grid(
    columns: (5mm, 1fr, 8mm), column-gutter: 1.5mm,
    pad(top: 1.2mm, sparkle(fill: colour, size: 7pt)),
    {
      link(label(fw.id), text(font: display, weight: 700, size: 10pt,
        fill: deep(colour), fw.title))
      linebreak()
      text(size: 8.1pt, fill: ink-soft, fw.summary)
    },
    align(right, text(font: display, style: "italic", weight: 800,
      size: 10pt, fill: deep(colour),
      context str(counter(page).at(label(fw.id)).first()))),
  ))
}

#v(3mm)
#for fw in data.firmwares { entry(fw) }
]

#for fw in data.firmwares {
  let colour = colour-of(fw)
  themed(colour, {
    eyebrow.update([Firmware])
    [#heading(level: 1, fw.title) #label(fw.id)]

    if fw.history != none {
      let h = fw.history
      let people = h.authors.map(a => {
        if a.login == none { a.name } else if a.login == a.name {
          link("https://github.com/" + a.login, "@" + a.login)
        } else {
          [#a.name (#link("https://github.com/" + a.login, "@" + a.login))]
        }
      })
      let key(body) = text(font: display, weight: 700, size: 7pt,
        fill: deep(colour), body)
      block(below: 4mm, {
        set text(size: 7.5pt, fill: ink-soft)
        show raw: set text(size: 7.5pt)
        grid(columns: (auto, 1fr), column-gutter: 3mm, row-gutter: 1.1mm,
          key[By], people.join(", "),
          ..if fw.credit != "" { (key[Credit], fw.credit) } else { () },
          key[Added], month(h.added),
          key[Last change], [#month(h.updated) in #link(repo-url + "/commit/"
            + h.commit, raw(h.commit))],
          key[Source], link(repo-url + "/tree/main/" + fw.path, raw(fw.path)),
        )
      })
    }

    if fw.intro != "" { markdown(fw.intro, base: fw.path) }

    for section in fw.sections {
      heading(level: 2, section.title)
      if section.controls {
        plaque(colour: colour, markdown(section.body, base: fw.path))
        if section.rest != "" { markdown(section.rest, base: fw.path) }
      } else {
        markdown(section.body, base: fw.path)
      }
    }
  })
}

// ------------------------------------------------------------ back cover

#page(margin: 0pt, header: none, footer: none, fill: stars)[
  #place(center + horizon, dy: -8mm, block(width: 92mm, fill: night,
    radius: 4mm, inset: (x: 8mm, y: 9mm), stroke: 0.8pt + steel.darken(35%),
    align(center, {
      mark(width: 14mm)
      v(3mm)
      wgd(size: 17pt)
      v(1mm)
      text(font: display, style: "italic", weight: 600, size: 9pt,
        fill: melon)[wgd modular]
      v(4mm)
      line(length: 40%, stroke: 0.6pt + steel)
      v(4mm)
      set text(size: 7.5pt, fill: paper.darken(18%))
      [The melon builds on Hagiwo's MOD2. Each firmware chapter names the
        people behind that firmware. Montserrat and Nunito Sans are licensed
        under the SIL Open Font License.]
      v(4mm)
      text(size: 7.5pt, link(repo-url, text(fill: melon,
        "github.com/" + data.repo)))
      linebreak()
      text(size: 7.5pt, link("https://wgdmodular.de/module/melon/",
        text(fill: melon, "wgdmodular.de")))
    })))
]
