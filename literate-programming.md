# Code Documentation (Literate Programming)

This project embraces **literate programming principles** in source code comments.
Comments are not an afterthought — they are the externalized memory for a project
where the developer is learning unfamiliar domains (DSP, Core Audio, file formats,
music theory). The code explains the *how*; comments explain the *why*. This is
also useful for future readers of the code, including you.

## Donald Knuth's Philosophy of Literate Programming

Donald Knuth introduced **literate programming** in his 1984 paper "Literate Programming" and his 1992 book *Literate Programming*. His core insight was simple but profound:

> Programmers spend far more time reading code than writing it. Therefore, programs should be written to be read by humans first, with the computer as a secondary audience.

### What Literate Programming Actually Means

Knuth argued that the traditional approach — writing code in the order the computer needs to execute it, then adding documentation as an afterthought — inverts the natural reading order. Instead, he proposed that the programmer should tell the story of what the program does and why, in natural language, with code snippets woven throughout.

This involves two operations:
- **Weaving** — extracting the narrative and documentation into a readable document (for humans to read).
- **Tangling** — extracting the code fragments into a compilable source file (for the computer to execute).

### The Web Language

Knuth did not invent "the web" (the World Wide Web did not exist in 1984). **Web** (capitalized) is his own document preparation language, a variant of TeX designed specifically for literate programming. It was not a markup language for the internet — it was a typesetting system for embedding code within prose.

Key features of Web:
- Code and prose exist in a single source file.
- Sections are named and can be referenced out of order.
- The tangler reorders code fragments into compilable order.
- The weaver produces a nicely typeset document.

Web was written in Web itself (a form of bootstrapping), and it produced the source code for the TeX typesetting system — the very system it extended. This self-referential approach was a powerful demonstration of the philosophy.

### Why It Did Not Become Mainstream

Web required learning a new language and toolchain. Most programmers already knew C, C++, Java, or similar languages — learning Web on top of those was a significant barrier. The tools were also less accessible than the compilers they were documenting. As a result, literate programming remained a niche practice, valued by academics and a few practitioners but never adopted by the broader industry.

## Literate Programming in C++

C++ has no built-in support for literate programming. There is no tangler or weaver in the standard toolchain. However, the philosophy translates directly through **structured comments**:

- Comments are interleaved with code in the same source file.
- Documentation tools (Doxygen) extract the narrative into readable HTML, PDF, or other formats.
- The source file itself remains compilable C++ — no tangling is needed.
- The narrative order matches the source file order, which is the natural reading order for a programmer.

The key insight is that **literate programming is a philosophy, not a toolchain**. You can apply Knuth's principles — write for humans first, tell the story, explain the why — using any language's comment syntax. The tools merely automate the extraction.

## The Toolchain: Doxygen, JavaDoc, and Historical Precedents

### JavaDoc Came First

JavaDoc was introduced in 1997 as part of the JDK 1.0 documentation tool. It was the first widely-adopted tool to extract structured documentation from source code comments using a standardized markup format. JavaDoc established the convention of `/** ... */` block comments with `@`-prefixed tags (`@param`, `@return`, `@see`, etc.).

### Doxygen Extended the Idea

Doxygen (released in 2001) generalized JavaDoc's approach to support C, C++, Python, Fortran, IDL, and other languages. It was not the first — JavaDoc predates it — but it was the first to bring the concept to C++ in a broad, language-agnostic way. Doxygen can produce HTML, LaTeX, PDF, and man-page output from the same source.

### The Historical Lineage

The tools that extract documentation from comments did not emerge in a vacuum:

| Era | Tool | Contribution |
|-----|------|---------------|
| 1970s | **NROFF / TROFF** | Unix text formatting; introduced structured markup for documents |
| 1980s | **TeX** | Donald Knuth's typesetting system; the foundation for Web |
| 1984 | **Web** | Knuth's literate programming language (typesetting, not the web) |
| 1997 | **JavaDoc** | First mainstream structured comment extraction tool |
| 2001 | **Doxygen** | Multi-language documentation extraction; extended JavaDoc-style markup to C++ |
| 2020s | **AI-assisted tools** | AI models can keep comments and code in sync automatically |

The thread connecting all of these is the same idea: **documentation should be embedded in the source, not separated from it**.

### AI-Assisted Synchronization

One of the historical problems with literate programming was that comments and code would drift apart. As code changed, comments became stale. AI tools solve this problem: when code is modified, the AI can update the associated comments simultaneously, keeping the narrative and the implementation in sync. This removes the primary practical barrier to maintaining literate-style documentation.

## Source Code is for Humans

This project operates on a simple principle that has been true since the first assembly language was written:

> **Program source code is for humans. Humans write it. Humans read it. The fact that a machine can turn it into binary is almost incidental.**

Consider what happens when you write a program:
1. You (a human) think about a problem.
2. You express your thinking in a language you can write (source code).
3. A compiler or interpreter processes that text to produce executable instructions.
4. The machine executes those instructions.

Steps 3 and 4 are mechanical. Steps 1 and 2 are the hard parts — and they are entirely about human communication. The compiler is a translator, not the audience. The real purpose of source code is to communicate intent from one human to another (including future you).

This is why literate programming matters. If you cannot read your own code three months from now and understand what it does and why, then you have not written software — you have written a puzzle for your future self. The same is true for every other person who reads the code.

### Why This Matters Now

AI tools have shifted the balance again. If an AI can generate code for you, the most valuable skill becomes **reading and understanding** that code — not writing it from scratch. Literate programming, applied through structured comments, ensures that the AI-generated (or human-written) code remains readable, maintainable, and understandable.

## Other Literate Programming Tools

Knuth's Web was not the only literate programming tool developed. Several
different approaches have emerged over the decades, each with different
syntax, target languages, and philosophies.

### CWEB

**CWEB** is Donald Knuth's own C-language variant of Web, developed with
Silvio Levy. It is the most widely-used literate programming system for C
and C++ code.

- **URL**: https://tug.org/texinfohtml/web2c.html
- **Syntax**: Uses `%` for comments (like C) and `%O` for narrative text.
- **Toolchain**: `ctangle` (extract code), `cweave` (extract documentation).
- **Notable use**: The CWEB distribution itself was written in CWEB.

CWEB is available from the TeX Users Group (TUG) and is maintained by Knuth.

### Noweb

**Noweb** was developed by Ken Williams in the 1990s. It differs from Web
in its chunk syntax, which is more concise and shell-script-friendly.

- **URL**: https://github.com/nrnrnr/noweb
- **Syntax**: `<<chunk-name>>= ... =<` for defining chunks.
- **Toolchain**: `noweave` (extract documentation), `noweave` (extract code).
- **Notable feature**: Named chunks with forward references; chunks can be
  referenced before they are defined.

Noweb is particularly popular for shell scripting and system administration,
where its lightweight syntax is an advantage. It works well with any text
editor because the syntax is plain ASCII with angle brackets.

### Org-Mode (Emacs)

**Org-mode** is an Emacs mode that includes built-in literate programming
support through **Org-Babel**. It allows embedding executable code blocks
within plain text documents.

- **URL**: https://orgmode.org/
- **Approach**: Code blocks are embedded inline within the document using
  language-specific delimiters (e.g., `#+BEGIN_SRC python`).
- **Languages**: Supports Python, R, bash, shell, SQL, and many others.
- **Toolchain**: `org-babel` executes code blocks; `org-export` produces
  formatted documentation.

Org-mode has gained significant popularity in the scientific computing
community, particularly for reproducible research workflows.

### Other Notable Tools

| Tool | Language | Key Feature |
|------|----------|-------------|
| **Pweave** | Python | Python weave, GitHub: mpacer/pweave |
| **knitr** | R | R literate programming, yihui.name/knitr |
| **R Markdown** | R/Python/Julia | Web-based literate publishing, rmarkdown.rstudio.com |
| **Jupyter Notebooks** | Multi-language | Browser-based literate notebooks, jupyter.org |

Note: Jupyter Notebooks and R Markdown are modern descendants of the
literate programming idea, adapted for interactive and web-based workflows.
They represent a shift from the "tangle + weave" paradigm to a more
interactive, cell-based model.

### Comparison: Web vs. Noweb vs. CWEB

| Feature | Web | Noweb | CWEB |
|---------|-----|-------|------|
| Comment syntax | `%` (TeX-style) | `//` (C-style) | `%` (C-style) |
| Chunk syntax | `@chunk-name` | `<<chunk-name>>=` | `<<chunk-name>>=` |
| Forward references | Yes | Yes | Yes |
| Primary language | Pascal (TeX) | Any text | C / C++ |
| Maintainer | Knuth (TUG) | Ken Williams | Knuth (TUG) |

## Philosophy

- **Comments capture the "why"**: domain knowledge, tradeoffs, decisions, and
  reasoning that executable code cannot express. The code shows what it does;
  comments explain why it does it that way.
- **Comments are the externalized memory**: for a project where the user is
  learning unfamiliar domains, comments serve as a searchable reference that
  can be revisited months later.
- **AI-assisted synchronization**: since AI tools update comments alongside
  code changes, the synchronization problem that traditionally makes comments
  stale is largely solved. Comments stay current because they are updated
  together with the code.
- **Literate Programming**: Donald Knuth's vision — documentation and code
  are intertwined, with narrative explaining the structure and reasoning.
  This project applies that philosophy through structured comments.

## JavaDoc-Style Markup

Comments use structured markup inspired by JavaDoc and Doxygen. This allows
comments to be extracted into documentation by tools like Doxygen, even if
we don't use Doxygen today. The markup is also highly readable by AI tools.

**Header files** — use `/** ... */` block comments on public interfaces:

```cpp
/**
 * @file module.h
 * @brief Brief description of the module's purpose.
 *
 * Longer narrative explaining the module's role, design rationale,
 * and key decisions. This is the "why" that code alone cannot express.
 *
 * @section section-name Section Title
 *
 * A detailed explanation of a specific aspect:
 * - Why a particular approach was chosen
 * - What tradeoffs were considered
 * - What common pitfalls to avoid
 *
 * @param paramName Description of the parameter.
 * @return Description of the return value.
 * @brief One-line summary (optional).
 * @note Important caveats or gotchas.
 */
```

**Source files** — use `/** ... */` block comments at the top of files and
sections, with annotations for specific notes:

```cpp
/**
 * @file module.cpp
 * @brief Implementation of the module described in module.h.
 *
 * Domain context explaining implementation decisions:
 * - Why this pattern was chosen over alternatives
 * - What bugs were fixed and why
 * - What debugging information is available
 *
 */
```

## Comment Structure Checklist

When writing or reviewing comments, ensure each file has:

1. **File-level narrative** (`@file`, `@brief`): What this file does and why.
2. **Section blocks** (`@section`): Domain-specific explanations for key
   decisions (e.g., why a particular API pattern, why a conversion formula).
3. **Public interface docs** (`@param`, `@return`, `@brief`): On every
   public method in header files.
4. **Domain context notes** (`@note`): Gotchas, pitfalls, common mistakes,
   and the reasoning behind non-obvious code.

## What to Document

Priority order for adding domain context comments:

1. **API boundary decisions** — Why a particular function signature or
   class design was chosen.
2. **Format/protocol decisions** — Why a specific encoding, byte order,
   or format was selected (e.g., 32-bit integer vs 80-bit extended float).
3. **Bug fixes** — What the bug was, why it happened, and why the fix
   works (so it doesn't reappear).
4. **Debugging hooks** — What diagnostic values are available and how
   to interpret them (e.g., what ioCallbackCount == 0 means).
5. **Algorithmic choices** — Why one algorithm was chosen over alternatives
   (e.g., YINfft over YIN for piano transcription).

## What NOT to Document

- Trivial code that explains itself (e.g., `i++` — increment i).
- Code that is about to be deleted.

## Example: Float-to-Int16 Conversion

```cpp
// Domain context: The clamping and scaling process:
// 1. std::max(-1.0f, ...) clamps negative values (handles no overflow).
// 2. std::min(1.0f, ...) clamps positive values (prevents overflow).
// 3. Multiply by 32767.0f (not 32768.0f) — this is critical.
// 4. Cast to int16_t — this truncates the float to integer.
//
// Why 32767? int16_t ranges from -32768 to +32767 (asymmetric
// in two's complement). If we scaled to 32768, a float of 1.0
// would produce 32768, which overflows to -32768 (a loud,
// distorted sample). Using 32767 avoids this well-known gotcha.
```

This comment explains the *why* (asymmetric two's complement range),
the *what* (clamping and scaling steps), and the *gotcha* (32767 vs
32768) that a beginner would need to know.

## Further Reading

For those interested in the academic and historical background of literate
programming, here are verified references:

### Donald Knuth's Original Works

1. **"Literate Programming"** (1984) — *The Computer Journal*, Vol. 27, No. 2,
   pp. 97–111. This is the original paper that introduced the concept.
   - PDF: https://www.cs.tufts.edu/~nr/cs257/archive/literate-programming/01-knuth-lp.pdf

2. **"Literate Programming"** (1992) — CSLI Lecture Notes, Stanford.
   The expanded book-length treatment, published by Stanford's Center for
   the Study of Language and Information.
   - ISBN: 978-0-937073-82-4
   - https://www.amazon.com/Literate-Programming-Lecture-Notes-Donald/dp/0937073806/

### Academic and Historical References

3. **"The Web System"** — Knuth's original Web documentation, describing
   the system used to produce TeX itself.
   - https://www.tug.org/web/

4. **"CWEB: A Literate Programming System for C"** — Silvio Levy's
   documentation of CWEB, the C-language variant.
   - https://www.tug.org/cweb/

5. **"Noweb: A Literate Programming System for Unix Tools"** — Ken Williams.
   The nowweb documentation and source.
   - **URL**: https://github.com/nrnrnr/noweb

### Modern Approaches

6. **Org-mode** — The Emacs mode with Org-Babel for literate programming.
   - https://orgmode.org/

7. **"Reproducible Research with R Markdown"** — The R Markdown book,
   covering literate publishing workflows.
   - https://rmarkdown.rstudio.com/

### Key Concepts to Research Further

- **Weave** — Extracting the narrative and documentation into a readable
  document (for humans to read).
- **Tangle** — Extracting the code fragments into a compilable source file
  (for the computer to execute).
- **Chunk** — A named unit of code that can be referenced and reordered.
- **Forward reference** — Referencing a chunk before it is defined, allowing
  the narrative to flow naturally rather than following execution order.
- **Self-documenting code** — The opposing philosophy (associated with
  Donald Knuth's critic, Edsger Dijkstra) that code should be clear enough
  without narrative documentation.

## Verification Checklist

When reviewing new code for literate programming compliance, check:

- [ ] File has a `@file` / `@brief` narrative explaining what it does and why
- [ ] Header files have `@section` blocks for domain-specific decisions
- [ ] Every public method in header files has `@param` and `@return`
- [ ] Critical decisions have `@note` explaining gotchas and pitfalls
- [ ] No trivial self-explanatory code is commented (e.g., `i++`)

