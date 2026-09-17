# LilyPond Reference for MIDI Generation and Music Notation

> A practical guide to using LilyPond for generating publication-quality sheet music and MIDI files, with a focus on piano and integration with Apple Logic Pro.

---

## 1. What Is LilyPond?

**LilyPond** is a **plain-text music engraving program** that takes human-readable music notation written in its own domain-specific language and produces **publication-quality sheet music** (PDF, SVG, PNG) and **playable MIDI files**.

Unlike GUI-based notation software (MuseScore, Finale, Sibelius), LilyPond separates **content** from **presentation**. You write the music; LilyPond handles the layout, beam grouping, slur placement, page breaks, and engraving rules.

### Key Characteristics

| Aspect | Detail |
|---|---|
| **Input** | Plain-text file (`.ly`) describing notes, rhythms, dynamics |
| **Output** | PDF, SVG, PNG (sheet music) + `.mid` (MIDI file) |
| **License** | GPL (free and open source) |
| **Platform** | macOS, Linux, Windows |
| **Learning curve** | Steep — but worth it for quality |
| **Engraving quality** | Widely considered the best available, open or closed source |

---

## 2. Installation

```bash
brew install lilypond
```

This installs:

| Command | Purpose |
|---|---|
| `lilypond` | Main compiler — converts `.ly` to PDF, MIDI, SVG, PNG |
| `midi2ly` | Converts `.mid` files into `.ly` source |
| `ly2dvi` | Legacy helper (rarely needed) |
| `lilybin` | Binary LilyPond format tool |

Verify installation:

```bash
lilypond --version
# Output: LilyPond 2.24.0 (or newer)
```

---

## 3. Basic Syntax

### A Minimal Score

```lilypond
\version "2.24.0"

\score {
  \relative c' {
    c4 d e f | g a b c' |
    d8 d c b | a g fis g |
  }
  \layout { }
  \midi { }
}
```

Compile and produce both PDF and MIDI:

```bash
lilypond music.ly
# Produces: music.pdf, music.mid, music.svg, music.png
```

### Key Syntax Elements

| Element | Syntax | Example |
|---|---|---|
| **Note names** | `c d e f g a b` (German notation: `h` = B natural, `b` = B flat) | `c' d' e'` |
| **Octaves** | Prime `'` = above middle C, apostrophe repeated for higher, comma `,` for lower | `c''` = C6, `c,` = C3 |
| **Rhythm** | Number after note = inverse of note value | `c4` = quarter note, `c8` = eighth, `c1` = whole |
| **Dots** | Add `.` = dotted note (adds half value) | `c4.` = dotted quarter |
| **Ties** | `-` connects notes across bars | `c4- c4` |
| **Slurs** | `()` = curved phrase | `c4( d e f)` |
| **Chords** | `{ }` = simultaneous notes | `{ c e g }` |
| **Rest** | `r` = rest | `r4` = quarter rest |
| **Repeat** | `\repeat volta 2 { ... }` | Repeat twice with 1st/2nd endings |

### Note Value Reference

| Number | Note Value |
|---|---|
| 1 | Whole (semibreve) |
| 2 | Half (minim) |
| 4 | Quarter (crotchet) |
| 8 | Eighth (quaver) |
| 16 | Sixteenth (semiquaver) |
| 32 | Thirty-second (demisemiquaver) |
| 64 | Sixty-fourth (hemidemisemiquaver) |

### Octave Notation

| Notation | Meaning | MIDI Note |
|---|---|---|
| `c,,,` | C1 (lowest piano: A0 = `a,,,`) | 21 |
| `c,,` | C2 | 36 |
| `c,` | C3 | 48 |
| `c` | C4 (middle C) | 60 |
| `c'` | C5 | 72 |
| `c''` | C6 | 84 |
| `c'''` | C7 | 96 |
| `c''''` | C8 (highest piano: C8 = `c''''''`) | 108 |

### Accidentals

| Symbol | Meaning |
|---|---|
| `cis` / `c#` | C sharp |
| `des` / `db` | D flat |
| `cises` / `c##` | C double sharp |
| `deses` / `cbb` | D double flat |
| `\accidentalSequence "german"` | Use German notation (cis, des, h, b) |

---

## 4. From Notation to MIDI: The Mapping

LilyPond translates every element of written notation into MIDI data. Understanding this mapping is crucial for generating usable MIDI for Logic Pro.

### Note-to-MIDI Mapping

| LilyPond | MIDI Note # | Frequency (Hz) |
|---|---|---|
| `c,,,` (A0) | 21 | 27.50 |
| `c,` (C3) | 48 | 130.81 |
| `c` (C4, middle C) | 60 | 261.63 |
| `c'` (C5) | 72 | 440.00 (A4 reference) |
| `c''` (C6) | 84 | 1046.50 |
| `c''''` (C8) | 108 | 4186.01 |

### Rhythm-to-MIDI Timing

LilyPond's timing is expressed in **measured tempo** (beats per minute). The conversion to MIDI ticks depends on the `timing` and `ragged-last-bottom-margin` settings, but the core principle is:

```
time (seconds) = (delta in ticks) / (ticks per quarter note) × (quarter note in beats) × (60 / BPM)
```

With LilyPond's default of **480 ticks per quarter note** (matching Logic Pro):

| Note Value | Ticks | At 120 BPM |
|---|---|---|
| Whole (1) | 1920 | 2.0 s |
| Half (2) | 960 | 1.0 s |
| Quarter (4) | 480 | 0.5 s |
| Eighth (8) | 240 | 0.25 s |
| Sixteenth (16) | 120 | 0.125 s |

### Dynamics-to-MIDI Velocity

| LilyPond | Approx. MIDI Velocity | Description |
|---|---|---|
| `pppp` | 16–32 | Extremely soft |
| `ppp` | 32–48 | Very very soft |
| `pp` | 48–64 | Very soft |
| `p` | 64–80 | Soft |
| `mp` | 80–96 | Medium-soft |
| `mf` | 96–112 | Medium-forte |
| `f` | 112–127 | Forte |
| `ff` | 127 | Very forte (capped) |
| `fff` | 127 | Extremely forte (capped) |
| `ffff` | 127 | Extremely very forte (capped) |

**Important**: LilyPond maps dynamics to velocity ranges. The mapping is configurable but defaults to a reasonable range. You can override with `\dynamicUp` or custom velocity mappings.

### Pedaling

LilyPond supports sustain pedal notation:

```lilypond
\score {
  \new PianoStaff <<
    \new Staff {
      \clef "treble"
      c4 d e f | g a b c' |
    }
    \new Staff {
      \clef "bass"
      \override PedalLineSpanner.stencil =
        #(lambda (grob)
           (if (eq? (ly:grob-property grob 'direction) UP)
               (ly:make-pedal-box grob UP)
               (ly:make-pedal-box grob DOWN)))
      c4 d e f | g a b c' |
    }
  >>
  \layout { }
  \midi { }
}
```

LilyPond generates **CC#64 (Sustain Pedal)** messages in the MIDI output, with values matching the pedal markings in the score.

---

## 5. Piano Scores

### Basic Piano Staff

```lilypond
\score {
  \new PianoStaff <<
    \new Staff = "upper" {
      \clef "treble"
      \relative c' {
        c4 d e f | g a b c' |
        d8 d c b | a g fis g |
      }
    }
    \new Staff = "lower" {
      \clef "bass"
      \relative c {
        c,4 g, | c,4 g, |
        a,4 fis, | g,4 g, |
      }
    }
  >>
  \layout { }
  \midi { }
}
```

This produces:
- A two-staff piano score (PDF/SVG)
- A MIDI file with **two tracks** (upper and lower staff on separate MIDI channels)

### Multi-voice Piano

```lilypond
\score {
  \new PianoStaff <<
    \new Staff {
      \new Voice = "right" \relative c' {
        \voiceOne c4 d e f | g a b c' |
      }
      \new Voice = "right2" \relative c' {
        \voiceTwo c8 c b b | a a g g |
      }
    }
    \new Staff {
      \new Voice = "left" \relative c {
        \voiceOne c,4 g, | c, g, |
      }
      \new Voice = "left2" \relative c {
        \voiceTwo c8 c b b | a a g g |
      }
    }
  >>
  \layout { }
  \midi { }
}
```

`\\voiceOne` and `\\voiceTwo` control stem direction and voice independence.

---

## 6. MIDI Generation from LilyPond

### The `\midi` Block

The `\midi { }` block inside a `\score` tells LilyPond to generate a MIDI file. You can customize it:

```lilypond
\score {
  % ... music ...
  \layout { }
  \midi {
    \tempo 4 = 120          % Set tempo: 120 BPM (quarter note = 120)
    \set Staff.midiInstrument = #"acoustic grand"
    \set Staff.midiMinimumVelocity = #48
    \set Staff.midiMaximumVelocity = #127
  }
}
```

### Available Piano Instruments in LilyPond MIDI Output

| Instrument Name | GM Patch | Channel |
|---|---|---|
| `"acoustic grand"` | 1 (0) | 1 |
| `"bright acoustic"` | 2 (1) | 1 |
| `"electric grand"` | 3 (2) | 1 |
| `"honky-tonk"` | 4 (3) | 1 |
| `"rhodes"` | 5 (4) | 1 |
| `"chorused piano"` | 6 (5) | 1 |
| `"harpsichord"` | 7 (6) | 1 |
| `"clavinet"` | 8 (7) | 1 |
| `"cello"` | 42 (41) | 1 |
| `"violin"` | 40 (39) | 1 |
| `"flute"` | 73 (72) | 1 |
| `"organ"` | 19 (18) | 1 |
| `"guitar"` | 25 (24) | 1 |
| `"pizzicato"` | 48 (47) | 1 |
| `"strings ensemble"` | 47 (46) | 1 |

**Default**: `"acoustic grand"` (GM patch 1, channel 1).

### Multi-Track MIDI

```lilypond
\score {
  \new Score <<
    \new Staff {
      \set Staff.midiInstrument = #"acoustic grand"
      c4 d e f |
    }
    \new Staff {
      \set Staff.midiInstrument = #"cello"
      c,4 g, |
    }
  >>
  \layout { }
  \midi { }
}
```

This produces a **Type 1 MIDI file** with two tracks — one for piano, one for cello. Each track uses a separate MIDI channel.

### Tempo Control

```lilypond
\midi {
  \tempo 4 = 120        % Steady 120 BPM
  \tempo 4 = 100        % Change to 100 BPM
  \tempo 4 = 140        % Change to 140 BPM
}
```

LilyPond generates **Set Tempo** (`FF 51`) meta events at tempo changes, producing a proper tempo map in the MIDI file.

### Output Formats

```bash
# Generate PDF (sheet music)
lilypond score.ly

# Generate MIDI file
lilypond --midi score.ly

# Generate SVG (vector graphic)
lilypond --svg score.ly

# Generate PNG (raster image)
lilypond --png score.ly

# Generate all at once
lilypond score.ly
# Produces: score.pdf, score.mid, score.svg, score.png
```

---

## 7. Converting MIDI to LilyPond

### The `midi2ly` Tool

```bash
midi2ly input.mid -o output.ly
```

This converts a MIDI file into LilyPond source code. You can then edit the `.ly` file and recompile to get updated PDF and MIDI.

### What `midi2ly` Handles Well

| Feature | Quality |
|---|---|
| Single-voice piano music | ★★★★★ |
| Simple two-voice piano | ★★★★☆ |
| Steady tempo | ★★★★★ |
| Standard note values | ★★★★★ |
| Basic dynamics | ★★★☆☆ |
| Multi-voice counterpoint | ★★★☆☆ |
| Complex pedaling | ★★☆☆☆ |
| Expression / velocity nuance | ★★☆☆☆ |
| Tempo changes | ★★★☆☆ |
| Polyphonic textures | ★★☆☆☆ |

### Limitations

- **Velocity information** is preserved but not always mapped to dynamics correctly
- **Pedaling** is often lost or misinterpreted
- **Dotted rhythms** can sometimes be misaligned
- **Grace notes** and ornaments are not always converted accurately
- **Multiple independent voices** in one staff may merge incorrectly

### Post-Processing Tips

After running `midi2ly`, you will typically need to:

1. **Add pedal markings** manually (pedal conversion is unreliable)
2. **Adjust dynamics** (velocity → dynamics mapping is approximate)
3. **Fix voice independence** (stems may merge or flip)
4. **Add bar lines** (sometimes `midi2ly` misses measure divisions)
5. **Add tempo markings** (if the MIDI has tempo changes)

---

## 8. Programmatic Generation of LilyPond Source

Since LilyPond is plain text, you can **generate `.ly` files programmatically** from your MIDI data, your own data, or any structured input.

### Python Example: Generate a Simple Piano Score

```python
#!/usr/bin/env python3

def generate_piano_score(filename, title, tempo, measures):
    """
    Generate a LilyPond .ly file from structured data.
    
    Args:
        filename: Output filename (without extension)
        title: Score title
        tempo: BPM (quarter notes per minute)
        measures: List of dicts with 'treble' and 'bass' keys
                  Each value is a list of note strings
    """
    ly = []
    ly.append(f'\\version "2.24.0"')
    ly.append('')
    ly.append(f'\\score {{')
    ly.append(f'  \\paper {{')
    ly.append(f'    title = "{title}"')
    ly.append(f'    tagline = ##f')  % Remove default tagline
    ly.append(f'    indent = 0')
    ly.append(f'    top-margin = 20')
    ly.append(f'    bottom-margin = 20')
    ly.append(f'    oddFooterMarkup = ##f')
    ly.append(f'    evenFooterMarkup = ##f')
    ly.append(f'    oddHeaderMarkup = ##f')
    ly.append(f'    evenHeaderMarkup = ##f')
    ly.append(f'  }}')
    ly.append(f'  \\new PianoStaff <<')
    ly.append(f'    \\new Staff = "upper" {{')
    ly.append(f'      \\clef "treble"')
    ly.append(f'      \\relative c\' {{')
    
    for measure in measures:
        notes = measure.get('treble', [])
        ly.append('        ' + ' '.join(notes) + ' |')
    
    ly.append('      }')
    ly.append('    }')
    ly.append('    \\new Staff = "lower" {')
    ly.append('      \\clef "bass"')
    ly.append('      \\relative c {')
    
    for measure in measures:
        notes = measure.get('bass', [])
        ly.append('        ' + ' '.join(notes) + ' |')
    
    ly.append('      }')
    ly.append('    }')
    ly.append('  >>')
    ly.append('  \\layout { }')
    ly.append(f'  \\midi {{')
    ly.append(f'    \\tempo 4 = {tempo}')
    ly.append(f'    \\set Staff.midiInstrument = #"acoustic grand"')
    ly.append(f'  }}')
    ly.append('}')
    
    with open(f'{filename}.ly', 'w') as f:
        f.write('\n'.join(ly) + '\n')


# Example: Generate "Twinkle Twinkle Little Star"
measures = [
    {'treble': ["c'4 c' g' g' a' a' g'"], 'bass': ["c,4 c, g, g, a, a, g,"]},
    {'treble': ["f'4 f' e' e' d' d' c'"], 'bass': ["f,4 f, c, c, d, d, c,"]},
    {'treble': ["g'4 g' f' f' e' e' d'"], 'bass': ["g,4 g, c, c, d, d, c,"]},
    {'treble': ["g'4 g' f' f' e' e' d'"], 'bass': ["g,4 g, c, c, d, d, c,"]},
    {'treble': ["f'4 f' e' e' d' d' c'"], 'bass': ["f,4 f, c, c, d, d, c,"]},
    {'treble': ["g'4 g' f' f' e' e' d'"], 'bass': ["g,4 g, c, c, d, d, c,"]},
    {'treble': ["f'4 f' e' e' d' d' c'"], 'bass': ["f,4 f, c, c, d, d, c,"]},
]

generate_piano_score("twinkle", "Twinkle Twinkle Little Star", 120, measures)
```

### Python Example: Convert MIDI Note Data to LilyPond

```python
def midi_note_to_ly(note_number, duration='4'):
    """Convert a MIDI note number to LilyPond notation."""
    note_names = ['c', 'cis', 'd', 'des', 'e', 'f', 'fis', 'g', 'ges', 'a', 'b', 'h']
    octave_map = {
        0: ',,,,,', 1: ',,,,', 2: ',,,', 3: ',,', 4: ',', 5: '',
        6: '\'', 7: '\'', 8: '\'', 9: '\'', 10: '\'', 11: '\''
    }
    
    note_name = note_names[note_number % 12]
    octave = (note_number // 12) - 2
    
    octave_chars = '\'' * max(0, octave) + ',' * max(0, -octave)
    
    return f'{note_name}{octave_chars}{duration}'


# Example: C4 (MIDI 60) → "c'4"
print(midi_note_to_ly(60, '4'))   # c'4
print(midi_note_to_ly(48, '8'))   # c,8
print(midi_note_to_ly(72, '16'))  # c''16
```

### Python Example: Full MIDI-to-LilyPond Pipeline

```python
import subprocess
import sys

def midi_to_lilypond_midi_and_pdf(midi_file, ly_file=None):
    """
    Convert a MIDI file to LilyPond source, then compile to PDF and MIDI.
    
    Args:
        midi_file: Path to input .mid file
        ly_file: Optional output .ly filename (defaults to stem of midi_file)
    
    Returns:
        Path to generated .ly file
    """
    if ly_file is None:
        ly_file = midi_file.replace('.mid', '.ly').replace('.Mid', '.ly')
    
    # Step 1: Convert MIDI to LilyPond source
    subprocess.run(['midi2ly', midi_file, '-o', ly_file], check=True)
    print(f"Generated: {ly_file}")
    
    # Step 2: Compile LilyPond source to PDF and MIDI
    result = subprocess.run(
        ['lilypond', ly_file],
        capture_output=True, text=True
    )
    
    if result.returncode != 0:
        print(f"Compilation errors:\n{result.stderr}")
        return None
    
    base = ly_file.replace('.ly', '')
    print(f"Generated: {base}.pdf")
    print(f"Generated: {base}.mid")
    print(f"Generated: {base}.svg")
    print(f"Generated: {base}.png")
    
    return ly_file


# Usage:
# midi_to_lilypond_midi_and_pdf("my_piano_piece.mid")
# → Produces: my_piano_piece.ly, my_piano_piece.pdf, my_piano_piece.mid, etc.
```

---

## 9. Integration with Apple Logic Pro

### The Full Workflow

```
┌─────────────────────────────────────────────────────────┐
│                    Your Pipeline                         │
├─────────────────────────────────────────────────────────┤
│                                                         │
│  Option A: Notation → MIDI → Logic Pro                 │
│  ┌──────────┐    ┌───────────┐    ┌───────────────┐    │
│  │ Write    │───→│ lilypond  │───→│ Import .mid   │    │
│  │ .ly file │    │ --midi    │    │ into Logic Pro│    │
│  └──────────┘    └───────────┘    └───────────────┘    │
│       │                                    │            │
│       │ (PDF for printing)                 │ (Edit,     │
│       │                                    |  produce)  │
│                                                         │
│  Option B: MIDI → Notation → Edit → MIDI → Logic Pro   │
│  ┌──────────┐    ┌───────────┐    ┌───────────────┐    │
│  │ Create   │───→│ midi2ly   │───→│ Edit .ly      │    │
│  │ MIDI in  │    │ → .ly     │───→│ (add pedaling,│    │
│  │ Logic Pro│    └───────────┘    │  dynamics)    │    │
│  └──────────┘            │        └───────┬───────┘    │
│              (Export .mid)│                │            │
│                          ▼                ▼            │
│                    ┌───────────┐    ┌───────────────┐  │
│                    │ lilypond  │───→│ Import .mid   │  │
│                    │ --midi    │    │ back into     │  │
│                    └───────────┘    │ Logic Pro     │  │
│                                     └───────────────┘  │
└─────────────────────────────────────────────────────────┘
```

### Importing LilyPond MIDI into Logic Pro

1. **Export MIDI from LilyPond**: `lilypond --midi score.ly`
2. **Import into Logic Pro**: File → Import → MIDI File
3. **Logic Pro creates Software Instrument tracks** — one per staff
4. **Change the instrument** to Logic's built-in "Studio Grand Piano"
5. **Edit in the Piano Roll** — adjust notes, velocity, timing
6. **Re-export** if you want updated sheet music: File → Export → MIDI File
7. **Convert back** to LilyPond: `midi2ly exported.mid`

### Why This Workflow Works

| Strength | Explanation |
|---|---|
| **Publication-quality scores** | LilyPond produces better sheet music than any GUI tool |
| **Editable MIDI** | Logic Pro's Piano Roll lets you refine notes and expression |
| **Round-trip editing** | MIDI → LilyPond → edit → MIDI → Logic Pro → MIDI |
| **Multiple instruments** | Each staff becomes a separate track in Logic Pro |
| **Tempo maps** | LilyPond tempo changes become tempo tracks in Logic Pro |
| **Print + digital** | Get both PDF sheet music and editable MIDI |

### Recommended Instrument Mapping

When importing LilyPond MIDI into Logic Pro, consider swapping instruments:

| LilyPond Instrument | Logic Pro Equivalent |
|---|---|
| `"acoustic grand"` | **Studio Grand** (best concert grand) |
| `"bright acoustic"` | **Bright Acoustic** (Logic's bright piano) |
| `"electric grand"` | **Electric Grand** (Logic's electric grand) |
| `"rhodes"` | **Vintage E.P.** → **Stage E.P.** |
| `"harp"` | **Concert Harp** |
| `"cello"` | **Cello** (Logic's orchestral library) |
| `"violin"` | **Violin** (Logic's orchestral library) |
| `"flute"` | **Flute** (Logic's orchestral library) |
| `"organ"` | **Hammond B3** or **Farfisa** |
| `"guitar"` | **Acoustic Guitar** or **Electric Guitar** |
| `"strings ensemble"` | **Strings** (Logic's orchestral library) |
| `"pizzicato"` | **Pizzicato Strings** |

### Logic Pro Piano Instruments

| Instrument | Description | Best For |
|---|---|---|
| **Studio Grand** | Concert grand, 3 velocity layers, multiple mic positions | Classical, jazz, pop |
| **Vintage E.P.** | Rhodes, Wurlitzer, Clavinet | Funk, jazz, soul |
| **Player Piano** | Sampled upright piano | Novice, toy piano |
| **Prepared Piano** | Muted strings, experimental | Modern, avant-garde |
| **Upright** | Acoustic upright piano | Realistic, warm |
| **Tape Piano** | Tape-saturated piano | Lo-fi, ambient |
| **Soft Grand** | Muted concert grand | Ballads, intimate |
| **Clavinet** | Electric clavinet | Funk, disco |
| **Concert Grand** | Full concert grand | Orchestral, cinematic |

---

## 10. Advanced Topics

### Key Signature and Mode

```lilypond
\score {
  \new Staff {
    \key c \major          % C major (no sharps/flats)
    \key d \minor           % D minor (1 flat)
    \key g \mixolydian      % G Mixolydian
    \relative c' {
      c4 d e f | g a b c' |
    }
  }
  \layout { }
  \midi { }
}
```

LilyPond generates **key signature** meta events in the MIDI file when configured, though most MIDI players ignore key signatures.

### Time Signatures

```lilypond
\new Staff {
  \time 4/4               % Common time
  \time 3/8               % Waltz feel
  \time 7/8               % Irregular
  \time 12/16             % Compound
  c4 d e f | g a b c' |
}
```

LilyPond generates **Time Signature** (`FF 58`) meta events in the MIDI file.

### Dynamic Markings

```lilypond
\new PianoStaff <<
  \new Staff {
    c4 <c e>8 <e g> f4 <f a>
    \p                     % piano (soft)
    \mf                    % mezzo-forte (medium)
    \f                     % forte (loud)
    \croissant             % crescendo (getting louder)
    \decrescendo           % decrescendo (getting softer)
    \ff                    % fortissimo (very loud)
  }
>>
```

Dynamics are mapped to velocity ranges in the MIDI output.

### Articulations

| Marking | LilyPond Command | MIDI Equivalent |
|---|---|---|
| Staccato | `c'.'` | Shorter note duration |
| Tenuto | `c'~` | Full note duration |
| Accent | `c'>` | Higher velocity |
| Marcato | `c'>` | Higher velocity |
| Fermata | `c'~^` | Extended duration (variable) |
| Trill | `\trill` | Alternating notes |
| Mordent | `\mordent` | Quick ornament |
| Turn | `\turn` | Quick ornament |
| Arpeggio | `\arpeggio` | Broken chord |

---

## 11. Common Pitfalls

| Pitfall | Solution |
|---|---|
| **German vs. English note names** | Use `\accidentalSequence "english"` for C/D/E/F/G/A/B |
| **Octave confusion** | Remember: `c` = C4 (middle C), `c'` = C5, `c,` = C3 |
| **Missing MIDI output** | Always include `\midi { }` block inside `\score` |
| **Wrong instrument** | Set `\set Staff.midiInstrument = #"acoustic grand"` |
| **No pedal in MIDI** | Add pedal markings manually; `midi2ly` pedal conversion is unreliable |
| **Velocity too low** | Use `\set Staff.midiMinimumVelocity = #48` |
| **Dynamics not mapped** | Check velocity range; add `\dynamicUp` or custom mappings |
| **Multi-voice merging** | Use `\voiceOne` and `\voiceTwo` explicitly |
| **Tempo too fast/slow** | Set `\tempo 4 = <BPM>` explicitly |
| **Missing bar lines** | Ensure `|` separators between measures |
| **Long scores page breaks** | Use `\pageBreak` or `\break` commands |
| **Lyrics alignment** | Use `\addlyric` with proper syllable mapping |

---

## 12. Standards and Specifications

| Standard | Description | Link |
|---|---|---|
| **LilyPond Documentation** | Official user and reference manuals | [lilypond.org/doc](https://lilypond.org/doc) |
| **LilyPond Snippet Repository** | Community-contributed code snippets | [lilypond.org/snippets](https://lilypond.org/snippets) |
| **LilyPond Notation Reference** | Complete reference for all notation commands | [lilypond.org/doc/v2.24/notation](https://lilypond.org/doc/v2.24/notation) |
| **LilyPond MIDI Reference** | MIDI output configuration | [lilypond.org/doc/v2.24/midi](https://lilypond.org/doc/v2.24/midi) |
| **LilyPond Scheme API** | Extending LilyPond with Scheme | [lilypond.org/doc/v2.24/scheme-api](https://lilypond.org/doc/v2.24/scheme-api) |
| **MIDI 1.0 Spec** | Original specification (MMA, 1983) | [midi.org/specifications](https://www.midi.org/specifications) |
| **SMF 1.0 Spec** | Standard MIDI File format (MMA, 1996) | [midi.org/file-format-specifications](https://www.midi.org/file-format-specifications) |
| **General MIDI Spec** | GM standardization (MMA, 1991) | [midi.org/specifications-old/item-general-midi-1](https://www.midi.org/specifications-old/item-general-midi-1) |
| **MIDI File Format** | MMA reference documentation | [midi.org/file-format-specifications](https://www.midi.org/file-format-specifications) |

---

## 13. Comparison: LilyPond vs. Other Notation Tools

| Feature | LilyPond | MuseScore | Finale | Sibelius | Dorico |
|---|---|---|---|---|---|
| **Type** | Text-based | GUI WYSIWYG | GUI WYSIWYG | GUI WYSIWYG | GUI WYSIWYG |
| **Output quality** | ★★★★★ | ★★★★☆ | ★★★★☆ | ★★★★☆ | ★★★★★ |
| **Learning curve** | Very steep | Moderate | Steep | Steep | Moderate |
| **Cost** | Free | Free | $400+ | $250+/yr | $450+ |
| **MIDI export** | Excellent | Good | Excellent | Excellent | Excellent |
| **MIDI import** | Good (midi2ly) | Excellent | Excellent | Excellent | Excellent |
| **Scriptable** | Fully (Scheme) | Limited | Limited | Limited | Limited |
| **Version control** | Git-friendly | Poor | Poor | Poor | Poor |
| **Batch processing** | Excellent | Limited | Limited | Limited | Limited |
| **Best for** | Publication, | Quick notation, | Professional | Film/TV scoring | Modern professional |
| | education, | beginners, | publishers | | publishing |

---

## 14. Checklist: Generating LilyPond Scores for Logic Pro

- [ ] Use `\version "2.24.0"` (or latest stable)
- [ ] Include `\score { ... \layout { } \midi { } }` block
- [ ] Set `\set Staff.midiInstrument = #"acoustic grand"` (or desired instrument)
- [ ] Set `\tempo 4 = <BPM>` for tempo control
- [ ] Use `\relative c'` or `\relative c` for octave clarity
- [ ] Include `\clef "treble"` and `\clef "bass"` for piano scores
- [ ] Use `\new PianoStaff << ... >>` for two-staff piano
- [ ] Add pedal markings manually (pedal conversion is unreliable)
- [ ] Set `\set Staff.midiMinimumVelocity = #48` for reasonable dynamics
- [ ] Use `\key` for key signatures
- [ ] Use `\time` for time signatures
- [ ] Add `\dynamicUp` or `\dynamicDown` for dynamic direction
- [ ] Compile with `lilypond score.ly` (produces PDF + MIDI + SVG + PNG)
- [ ] Import the `.mid` file into Logic Pro: File → Import → MIDI File
- [ ] In Logic Pro, swap to "Studio Grand Piano" or another preferred instrument
- [ ] Edit notes in the Piano Roll as needed
- [ ] Re-export from Logic Pro if you want updated sheet music: `midi2ly exported.mid`

---

## 15. Workflow Summary

### Complete Pipeline: Notation → Print + MIDI → Logic Pro → Print + MIDI

```
Step 1: Write LilyPond source (.ly)
         │
         ▼
Step 2: Compile with lilypond
         │
         ├──→ Sheet music (PDF/SVG/PNG) — for printing
         │
         └──→ MIDI file (.mid)
                  │
                  ▼
Step 3: Import into Logic Pro
         │
         ├──→ Edit notes in Piano Roll
         ├──→ Change instruments (Studio Grand, etc.)
         ├──→ Add effects, automation
         ├──→ Mix and produce
         │
         └──→ Export MIDI back
                  │
                  ▼
Step 4: (Optional) midi2ly → Updated sheet music
```

This gives you:
- **Publication-quality printed music** (from LilyPond)
- **Editable, producible MIDI** (from Logic Pro)
- **Round-trip editing** (MIDI ↔ notation)
- **Version control friendly** (plain text `.ly` files)
- **Programmatic generation** (generate scores from data)

---

## 16. Example: Complete Piano Piece

```lilypond
\version "2.24.0"

\score {
  \paper {
    title = "Nocturne in C Minor"
    composer = "Generated by LilyPond"
    top-margin = 25
    bottom-margin = 25
    oddFooterMarkup = ##f
    evenFooterMarkup = ##f
  }
  
  \new PianoStaff <<
    \new Staff = "upper" {
      \clef "treble"
      \key c \minor
      \relative c' {
        \tempo 4 = 72
        \dynamicUp
        c4\p fis,8 g | as4 g fis8 g |
        as4 g fis8 g | as g fis4 |
        g'4 as fis8 g | as4 g fis8 g |
        as4 g fis8 g | g4 g, |
      }
    }
    \new Staff = "lower" {
      \clef "bass"
      \relative c {
        \tempo 4 = 72
        c,4 g, | c, g, |
        c, g, | c, g, |
        c, g, | c, g, |
        c, g, | c, g, |
      }
    }
  >>
  
  \layout { }
  
  \midi {
    \tempo 4 = 72
    \set Staff.midiInstrument = #"acoustic grand"
    \set Staff.midiMinimumVelocity = #48
    \set Staff.midiMaximumVelocity = #127
  }
}
```

Compile:

```bash
lilypond nocturne.ly
# Produces: nocturne.pdf, nocturne.mid, nocturne.svg, nocturne.png
```

Import `nocturne.mid` into Logic Pro and enjoy.
